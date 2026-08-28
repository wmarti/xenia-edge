/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_EXECUTION_EXPORT_EVENT_ADAPTER_H_
#define XENIA_KERNEL_GUEST_EXECUTION_EXPORT_EVENT_ADAPTER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/xbox.h"

namespace xe {
class Memory;
namespace kernel {

class KernelState;

// The wait kinds whose link register is the export's single return point, so a
// checkpoint taken inside one resumes by applying a recorded result rather than
// by re-entering the export. A timed delay, a fence or spin poll, offloaded or
// I/O-completion work and a socket wait all either return void from the
// scheduler primitive or leave their status to the enclosing export's own host
// code after the wait, so their link register means "one poll finished" and
// nothing here may record them.
constexpr bool IsModeledBlockingExportWaitKind(
    GuestSchedulerCaptureWaitKind kind) {
  return kind == GuestSchedulerCaptureWaitKind::kSingle ||
         kind == GuestSchedulerCaptureWaitKind::kMultiAny ||
         kind == GuestSchedulerCaptureWaitKind::kMultiAll;
}

// Matches the scheduler's own wait-handle bound, so a wait this adapter records
// is always one the blocked-wait binding can describe as well.
inline constexpr size_t kGuestExecutionExportEventMaximumObjects =
    kGuestSchedulerCaptureMaximumWaitHandles;

// One modeled blocking export dispatch, as the shim gathers it. Nothing here
// reaches back into the kernel, so the recorded shape is exercisable without a
// running title.
struct GuestExecutionExportEventDispatch {
  cpu::GuestExecutionCaptureParticipantIdentity participant;
  GuestSchedulerCaptureWaitKind wait_kind =
      GuestSchedulerCaptureWaitKind::kNone;
  uint32_t export_ordinal = 0;
  // Export thunk the call site branched to.
  uint32_t guest_address = 0;
  // Link register at the dispatch boundary: the export's single return point.
  uint32_t call_site_address = 0;
  // Guest X_KTHREAD of the waiting thread.
  uint32_t kthread_address = 0;
  // Guest dispatcher headers the export acquires or signals, in call order.
  uint32_t object_count = 0;
  std::array<uint32_t, kGuestExecutionExportEventMaximumObjects>
      object_addresses = {};
};

// The guest-memory regions a modeled blocking wait writes, in canonical
// ascending order: the waiting thread's X_KTHREAD wait fields, then every
// dispatcher header the wait acquires or signals. Returns false when the
// dispatch is not enumerable, which is every case where a complete list cannot
// be proven: an unmapped region, an object whose acquire mutates guest list
// structures this cannot bound, a duplicate or overlapping object, or more
// objects than the range budget holds.
bool BuildGuestExecutionExportEventEffectRanges(
    const Memory& memory, const GuestExecutionExportEventDispatch& dispatch,
    std::vector<cpu::GuestExecutionCaptureExternalEventEffectRange>* ranges);

// Guest address of the export thunk the call site's branch-and-link targeted,
// or zero. Guest code enters an import through a direct relative bl at the
// instruction before the return address; a computed call, an absolute branch or
// an unreadable call site leaves the dispatch unattributed.
uint32_t DecodeGuestExecutionExportEventThunkAddress(
    const Memory& memory, uint32_t call_site_address);

// Guest dispatcher-header address behind a wait handle, or zero when the handle
// names no object or the object has no guest structure.
uint32_t GuestExecutionExportEventObjectAddress(KernelState* kernel_state,
                                                uint32_t handle);

// True only while a capture has installed an external-event log on the running
// thread's processor. Lets a high-frequency export skip resolving its own wait
// objects when nothing would record them.
bool IsGuestExecutionExportEventRecording();

// Records one modeled blocking export dispatch as a kKernelExport external
// event. Construction snapshots the declared effect preimage before the export
// body runs, and Complete closes the event with the status the shim is about to
// store into r3 plus the post-return volatile register set.
//
// Every admission predicate is fail-closed: an alertable wait, a wait kind
// outside the modeled class, a pending APC, a non-cooperative block, an
// unattributable call site, an object whose writes cannot be enumerated, or no
// installed log all leave the scope inactive and record nothing. Such a wait
// stays exactly as unrepresented as it is today; nothing partial is ever
// emitted.
class GuestExecutionExportEventScope {
 public:
  // Gathers the dispatch from the running fiber and opens nothing unless every
  // predicate holds.
  GuestExecutionExportEventScope(uint32_t export_ordinal,
                                 GuestSchedulerCaptureWaitKind wait_kind,
                                 bool alertable,
                                 std::span<const uint32_t> object_addresses);
  // Explicit form, for a caller that has already gathered the dispatch.
  GuestExecutionExportEventScope(
      std::shared_ptr<cpu::GuestExecutionCaptureExternalEventLog> log,
      const Memory* memory, const cpu::ppc::PPCContext* context,
      const GuestExecutionExportEventDispatch& dispatch);
  ~GuestExecutionExportEventScope();

  GuestExecutionExportEventScope(const GuestExecutionExportEventScope&) =
      delete;
  GuestExecutionExportEventScope& operator=(
      const GuestExecutionExportEventScope&) = delete;

  bool active() const { return static_cast<bool>(token_); }

  // Closes the event with the status the export returns.
  void Complete(X_STATUS status);

 private:
  void Open(std::shared_ptr<cpu::GuestExecutionCaptureExternalEventLog> log,
            const Memory* memory, const cpu::ppc::PPCContext* context,
            const GuestExecutionExportEventDispatch& dispatch);
  void Close(cpu::GuestExecutionCaptureExternalEventDisposition disposition,
             bool has_result, X_STATUS status);

  std::shared_ptr<cpu::GuestExecutionCaptureExternalEventLog> log_;
  const Memory* memory_ = nullptr;
  const cpu::ppc::PPCContext* context_ = nullptr;
  std::vector<cpu::GuestExecutionCaptureExternalEventEffectRange> ranges_;
  cpu::GuestExecutionCaptureExternalEventToken token_;
};

}  // namespace kernel
}  // namespace xe

#endif

#endif  // XENIA_KERNEL_GUEST_EXECUTION_EXPORT_EVENT_ADAPTER_H_
