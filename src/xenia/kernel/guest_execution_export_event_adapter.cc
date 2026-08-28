/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_execution_export_event_adapter.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>

#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace {

using Range = cpu::GuestExecutionCaptureExternalEventEffectRange;

// One contiguous span of X_KTHREAD a modeled cooperative wait writes.
struct KThreadEffectField {
  uint32_t offset;
  uint32_t byte_count;
};

// The complete set of guest thread writes on the modeled path: WaitEnter and
// WaitExit set the state, alertable flag, wait reason, processor mode, wait
// IRQL and wait result (xobject.cc); the park and the dispatch switch set the
// state again (guest_scheduler.cc); and the wake priority boost publishes the
// new level (xthread.cc). Adjacent fields share one span; separated ones stay
// apart so no untouched byte is claimed.
const KThreadEffectField kKThreadEffectFields[] = {
    {static_cast<uint32_t>(offsetof(X_KTHREAD, thread_state)), 1},
    {static_cast<uint32_t>(offsetof(X_KTHREAD, alertable)),
     static_cast<uint32_t>(offsetof(X_KTHREAD, priority) + 1 -
                           offsetof(X_KTHREAD, alertable))},
    {static_cast<uint32_t>(offsetof(X_KTHREAD, wait_result)),
     static_cast<uint32_t>(offsetof(X_KTHREAD, processor_mode) + 1 -
                           offsetof(X_KTHREAD, wait_result))},
    {static_cast<uint32_t>(offsetof(X_KTHREAD, wait_reason)), 1},
};

// Acquiring or signaling an event or a semaphore writes only signal_state.
// Every other dispatcher type is refused: a mutant acquire also takes ownership
// and links the mutant into the thread's owned list, which mutates guest list
// nodes outside both objects, and no other type's writes are established here.
bool IsEnumerableDispatcherType(uint8_t type) {
  return type == X_OBJECT_TYPES::EventNotificationObject ||
         type == X_OBJECT_TYPES::EventSynchronizationObject ||
         type == X_OBJECT_TYPES::SemaphoreObject;
}

bool IsMappedRange(const Memory& memory, uint32_t address,
                   uint32_t byte_count) {
  const uint64_t end = uint64_t(address) + byte_count;
  if (!byte_count || end > uint64_t(std::numeric_limits<uint32_t>::max()) + 1) {
    return false;
  }
  return memory.LookupHeap(address) &&
         memory.LookupHeap(static_cast<uint32_t>(end - 1));
}

bool AreCanonicalRanges(const std::vector<Range>& ranges) {
  uint64_t previous_end = 0;
  bool have_previous = false;
  for (const Range& range : ranges) {
    if (!range.byte_count) {
      return false;
    }
    if (have_previous && uint64_t(range.address) <= previous_end) {
      return false;
    }
    previous_end = uint64_t(range.address) + range.byte_count;
    have_previous = true;
  }
  return true;
}

void ReadEffectRanges(const Memory& memory, const std::vector<Range>& ranges,
                      std::vector<uint8_t>* image) {
  image->clear();
  for (const Range& range : ranges) {
    const uint8_t* bytes =
        memory.TranslateVirtual<const uint8_t*>(range.address);
    image->insert(image->end(), bytes, bytes + range.byte_count);
  }
}

cpu::GuestExecutionCaptureExternalEventVolatileRegisterState
CaptureVolatileRegisters(const cpu::ppc::PPCContext& context,
                         uint64_t stored_result) {
  cpu::GuestExecutionCaptureExternalEventVolatileRegisterState registers;
  registers.values[0] = context.r[0];
  for (size_t i = 3; i <= 12; ++i) {
    registers.values[i - 2] = context.r[i];
  }
  // The export trampoline stores the shim's result after the shim body has
  // returned, so the value the dispatch leaves in r3 is the one the caller is
  // about to store, not the one still live here.
  registers.values[1] = stored_result;
  registers.values[11] = context.ctr;
  registers.values[12] = context.cr();
  return registers;
}

}  // namespace

bool BuildGuestExecutionExportEventEffectRanges(
    const Memory& memory, const GuestExecutionExportEventDispatch& dispatch,
    std::vector<Range>* ranges) {
  ranges->clear();
  if (!dispatch.kthread_address || (dispatch.kthread_address & 3) ||
      !dispatch.object_count ||
      dispatch.object_count > dispatch.object_addresses.size()) {
    return false;
  }
  // Only through the last field the wait writes; the rest of the structure is
  // never claimed, so an object at the end of a heap is not falsely refused.
  if (!IsMappedRange(
          memory, dispatch.kthread_address,
          static_cast<uint32_t>(offsetof(X_KTHREAD, wait_reason) + 1))) {
    return false;
  }
  std::vector<Range> built;
  built.reserve(std::size(kKThreadEffectFields) + dispatch.object_count);
  for (const KThreadEffectField& field : kKThreadEffectFields) {
    built.push_back(
        {dispatch.kthread_address + field.offset, field.byte_count});
  }
  for (uint32_t i = 0; i < dispatch.object_count; ++i) {
    const uint32_t address = dispatch.object_addresses[i];
    if (!address || (address & 3) ||
        !IsMappedRange(memory, address,
                       static_cast<uint32_t>(sizeof(X_DISPATCH_HEADER)))) {
      return false;
    }
    const auto* header =
        memory.TranslateVirtual<const X_DISPATCH_HEADER*>(address);
    if (!IsEnumerableDispatcherType(static_cast<uint8_t>(header->type))) {
      return false;
    }
    built.push_back({address + static_cast<uint32_t>(
                                   offsetof(X_DISPATCH_HEADER, signal_state)),
                     static_cast<uint32_t>(sizeof(header->signal_state))});
  }
  std::sort(built.begin(), built.end(),
            [](const Range& left, const Range& right) {
              return left.address < right.address;
            });
  // A repeated handle, an object aliasing the waiting thread, or any other
  // collision leaves a non-canonical list, which the log would latch as a
  // permanent rejection. Refuse it here so one unrepresentable wait cannot end
  // the whole log.
  if (!AreCanonicalRanges(built)) {
    return false;
  }
  *ranges = std::move(built);
  return true;
}

uint32_t DecodeGuestExecutionExportEventThunkAddress(
    const Memory& memory, uint32_t call_site_address) {
  if (call_site_address < 8 || (call_site_address & 3)) {
    return 0;
  }
  const uint32_t branch_address = call_site_address - 4;
  if (!IsMappedRange(memory, branch_address,
                     static_cast<uint32_t>(sizeof(uint32_t)))) {
    return 0;
  }
  const uint32_t instruction = xe::load_and_swap<uint32_t>(
      memory.TranslateVirtual<const uint8_t*>(branch_address));
  // Primary opcode 18 with LK set and AA clear: a relative branch-and-link.
  if ((instruction >> 26) != 18 || !(instruction & 0x1) ||
      (instruction & 0x2)) {
    return 0;
  }
  // LI is a signed 26-bit displacement; extend it without relying on a signed
  // shift.
  const uint32_t field = instruction & 0x03FFFFFCu;
  const uint32_t displacement =
      (field & 0x02000000u) ? (field | 0xFC000000u) : field;
  const uint32_t target = branch_address + displacement;
  return (target && !(target & 3)) ? target : 0;
}

uint32_t GuestExecutionExportEventObjectAddress(KernelState* kernel_state,
                                                uint32_t handle) {
  if (!kernel_state) {
    return 0;
  }
  auto object = kernel_state->object_table()->LookupObject<XObject>(handle);
  return object ? object->guest_object() : 0;
}

bool IsGuestExecutionExportEventRecording() {
  if (!GuestScheduler::enabled() || !XThread::IsInThread()) {
    return false;
  }
  XThread* self = XThread::GetCurrentThread();
  cpu::ThreadState* thread_state = self ? self->thread_state() : nullptr;
  cpu::Processor* processor =
      thread_state ? thread_state->processor() : nullptr;
  return processor &&
         processor->guest_execution_capture_external_event_log_installed();
}

GuestExecutionExportEventScope::GuestExecutionExportEventScope(
    uint32_t export_ordinal, GuestSchedulerCaptureWaitKind wait_kind,
    bool alertable, std::span<const uint32_t> object_addresses) {
  // An alertable wait can return on a user APC, and the export then runs the
  // APC as guest code on the waiting thread's stack. Resuming at the return
  // address would skip a nested guest invocation, so the class is refused
  // outright rather than approximated.
  if (alertable || !IsModeledBlockingExportWaitKind(wait_kind)) {
    return;
  }
  if (object_addresses.empty() ||
      object_addresses.size() > kGuestExecutionExportEventMaximumObjects) {
    return;
  }
  // Only the cooperative path parks a fiber inside the export; a host block
  // produces no modeled scheduler state to resume from.
  if (!GuestScheduler::enabled() || !XThread::GetCurrentFiberThread() ||
      !XThread::IsInThread()) {
    return;
  }
  XThread* self = XThread::GetCurrentThread();
  if (!self || !self->is_guest_thread()) {
    return;
  }
  cpu::ThreadState* thread_state = self->thread_state();
  if (!thread_state) {
    return;
  }
  cpu::Processor* processor = thread_state->processor();
  if (!processor ||
      !processor->guest_execution_capture_external_event_log_installed()) {
    return;
  }
  auto log = processor->guest_execution_capture_external_event_log();
  const cpu::ppc::PPCContext* context = thread_state->context();
  Memory* memory = thread_state->memory();
  if (!log || !context || !memory) {
    return;
  }

  GuestExecutionExportEventDispatch dispatch;
  dispatch.participant = {thread_state->guest_execution_capture_instance_id(),
                          thread_state->thread_id()};
  dispatch.wait_kind = wait_kind;
  dispatch.export_ordinal = export_ordinal;
  dispatch.call_site_address = static_cast<uint32_t>(context->lr);
  dispatch.guest_address = DecodeGuestExecutionExportEventThunkAddress(
      *memory, dispatch.call_site_address);
  dispatch.kthread_address = self->guest_object();
  if (!dispatch.kthread_address) {
    return;
  }
  // A queued APC is guest code the wait would run on this thread's stack
  // before the export returns, which the return-address resume cannot replay.
  auto* kthread =
      memory->TranslateVirtual<X_KTHREAD*>(dispatch.kthread_address);
  if (kthread->user_apc_pending || !kthread->apc_lists[0].empty(memory)) {
    return;
  }
  dispatch.object_count = static_cast<uint32_t>(object_addresses.size());
  std::copy(object_addresses.begin(), object_addresses.end(),
            dispatch.object_addresses.begin());
  Open(std::move(log), memory, context, dispatch);
}

GuestExecutionExportEventScope::GuestExecutionExportEventScope(
    std::shared_ptr<cpu::GuestExecutionCaptureExternalEventLog> log,
    const Memory* memory, const cpu::ppc::PPCContext* context,
    const GuestExecutionExportEventDispatch& dispatch) {
  Open(std::move(log), memory, context, dispatch);
}

GuestExecutionExportEventScope::~GuestExecutionExportEventScope() {
  // The dispatch left through a path the model does not describe, so the log
  // records what it has and marks the session non-replayable rather than
  // stranding an open call or claiming a result it never saw.
  Close(cpu::GuestExecutionCaptureExternalEventDisposition::kRejectSession,
        false, X_STATUS_SUCCESS);
}

void GuestExecutionExportEventScope::Complete(X_STATUS status) {
  Close(cpu::GuestExecutionCaptureExternalEventDisposition::kReplayCaptured,
        true, status);
}

void GuestExecutionExportEventScope::Open(
    std::shared_ptr<cpu::GuestExecutionCaptureExternalEventLog> log,
    const Memory* memory, const cpu::ppc::PPCContext* context,
    const GuestExecutionExportEventDispatch& dispatch) {
  if (!log || !memory || !context ||
      !IsModeledBlockingExportWaitKind(dispatch.wait_kind) ||
      !dispatch.participant.capture_instance_id) {
    return;
  }
  // Both addresses are load-bearing for the replay route: the call site is
  // where the thread resumes and the thunk address is what the bundle binds
  // the pending export to.
  if (!dispatch.call_site_address || (dispatch.call_site_address & 3) ||
      !dispatch.guest_address || (dispatch.guest_address & 3)) {
    return;
  }
  std::vector<Range> ranges;
  if (!BuildGuestExecutionExportEventEffectRanges(*memory, dispatch, &ranges)) {
    return;
  }
  std::vector<uint8_t> preimage;
  ReadEffectRanges(*memory, ranges, &preimage);

  cpu::GuestExecutionCaptureExternalEventBegin begin;
  begin.participant = dispatch.participant;
  begin.kind = cpu::GuestExecutionCaptureExternalEventKind::kKernelExport;
  begin.export_ordinal = dispatch.export_ordinal;
  begin.guest_address = dispatch.guest_address;
  begin.call_site_address = dispatch.call_site_address;
  begin.effect_ranges = ranges;
  const cpu::GuestExecutionCaptureExternalEventToken token =
      log->OnExternalEventBegin(begin, preimage);
  if (!token) {
    return;
  }
  log_ = std::move(log);
  memory_ = memory;
  context_ = context;
  ranges_ = std::move(ranges);
  token_ = token;
}

void GuestExecutionExportEventScope::Close(
    cpu::GuestExecutionCaptureExternalEventDisposition disposition,
    bool has_result, X_STATUS status) {
  if (!token_) {
    return;
  }
  std::vector<uint8_t> postimage;
  ReadEffectRanges(*memory_, ranges_, &postimage);

  cpu::GuestExecutionCaptureExternalEventEnd end;
  end.disposition = disposition;
  end.mutation_source =
      cpu::GuestExecutionCaptureExternalEventMutationSource::kActiveGuestThread;
  if (has_result) {
    end.has_returned_value = true;
    // Exactly what shim::ResultBase<T>::Store writes into r3.
    end.returned_value = uint64_t(int32_t(status));
    end.has_volatile_registers = true;
    end.volatile_registers =
        CaptureVolatileRegisters(*context_, end.returned_value);
  }
  log_->OnExternalEventEnd(token_, end, postimage);

  token_ = {};
  log_.reset();
  ranges_.clear();
  memory_ = nullptr;
  context_ = nullptr;
}

}  // namespace kernel
}  // namespace xe

#endif
