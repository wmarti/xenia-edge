/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/memory.h"
#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_execution_export_event_adapter.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
namespace testing {
namespace {

using xe::kernel::BuildGuestExecutionExportEventEffectRanges;
using xe::kernel::DecodeGuestExecutionExportEventThunkAddress;
using xe::kernel::GuestExecutionExportEventDispatch;
using xe::kernel::GuestExecutionExportEventScope;
using xe::kernel::GuestSchedulerCaptureWaitKind;
using xe::kernel::IsModeledBlockingExportWaitKind;
using xe::kernel::X_DISPATCH_HEADER;
using xe::kernel::X_KTHREAD;
using xe::kernel::X_OBJECT_TYPES;

using Log = GuestExecutionCaptureExternalEventLog;
using Range = GuestExecutionCaptureExternalEventEffectRange;

constexpr uint32_t kExportOrdinal = 0xB0;

uint64_t DecodeLittleEndian(const uint8_t* bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= uint64_t(bytes[i]) << (i * 8);
  }
  return value;
}

// A guest image with one thread structure, a code page carrying the call site's
// branch-and-link, and as many dispatcher objects as a case needs.
class ExportEventHarness {
 public:
  ExportEventHarness() {
    memory = std::make_unique<Memory>();
    if (!memory->Initialize()) {
      throw std::runtime_error("test memory initialization failed");
    }
    log = std::make_shared<Log>();
    kthread_address =
        memory->SystemHeapAlloc(static_cast<uint32_t>(sizeof(X_KTHREAD)), 128);
    code_address = memory->SystemHeapAlloc(64, 16);
    if (!kthread_address || !code_address) {
      throw std::runtime_error("test guest allocation failed");
    }
    thunk_address = code_address + 32;
    call_site_address = code_address + 8;
    StoreBranchAndLink(call_site_address - 4, thunk_address);
    context.lr = call_site_address;
  }

  uint32_t AllocateObject(X_OBJECT_TYPES type, uint32_t signal_state) {
    const uint32_t address = memory->SystemHeapAlloc(
        static_cast<uint32_t>(sizeof(X_DISPATCH_HEADER)), 16);
    if (!address) {
      throw std::runtime_error("test guest allocation failed");
    }
    auto* header = memory->TranslateVirtual<X_DISPATCH_HEADER*>(address);
    header->type = type;
    header->signal_state = signal_state;
    return address;
  }

  void StoreBranchAndLink(uint32_t branch_address, uint32_t target) {
    const uint32_t displacement = target - branch_address;
    xe::store_and_swap<uint32_t>(
        memory->TranslateVirtual(branch_address),
        (18u << 26) | (displacement & 0x03FFFFFCu) | 1u);
  }

  X_KTHREAD* kthread() {
    return memory->TranslateVirtual<X_KTHREAD*>(kthread_address);
  }

  X_DISPATCH_HEADER* object(uint32_t address) {
    return memory->TranslateVirtual<X_DISPATCH_HEADER*>(address);
  }

  GuestExecutionExportEventDispatch Dispatch(
      GuestSchedulerCaptureWaitKind wait_kind,
      std::span<const uint32_t> object_addresses) {
    GuestExecutionExportEventDispatch dispatch;
    dispatch.participant = {0x51, 0x6};
    dispatch.wait_kind = wait_kind;
    dispatch.export_ordinal = kExportOrdinal;
    dispatch.guest_address = thunk_address;
    dispatch.call_site_address = call_site_address;
    dispatch.kthread_address = kthread_address;
    dispatch.object_count = static_cast<uint32_t>(object_addresses.size());
    std::copy(object_addresses.begin(), object_addresses.end(),
              dispatch.object_addresses.begin());
    return dispatch;
  }

  std::unique_ptr<Memory> memory;
  std::shared_ptr<Log> log;
  ppc::PPCContext context = {};
  uint32_t kthread_address = 0;
  uint32_t code_address = 0;
  uint32_t thunk_address = 0;
  uint32_t call_site_address = 0;
};

}  // namespace

TEST_CASE("Modeled export adapter records an allowlisted single-object wait",
          "[guest-export-event]") {
  ExportEventHarness harness;
  const uint32_t event_address =
      harness.AllocateObject(X_OBJECT_TYPES::EventSynchronizationObject, 1);
  const uint32_t objects[] = {event_address};

  harness.kthread()->thread_state = 2;
  harness.kthread()->wait_result = X_STATUS_PENDING;
  harness.context.r[0] = 0x1111111111111111ull;
  harness.context.r[3] = 0x2222222222222222ull;
  harness.context.r[12] = 0x3333333333333333ull;
  harness.context.ctr = 0x4444444444444444ull;
  harness.context.set_cr(0x55555555ull);

  {
    GuestExecutionExportEventScope scope(
        harness.log, harness.memory.get(), &harness.context,
        harness.Dispatch(GuestSchedulerCaptureWaitKind::kSingle, objects));
    REQUIRE(scope.active());
    // What the wait leaves behind: the thread runs again with a result, and the
    // auto-reset event was consumed.
    harness.kthread()->thread_state = 5;
    harness.kthread()->wait_result = X_STATUS_SUCCESS;
    harness.object(event_address)->signal_state = 0;
    scope.Complete(X_STATUS_SUCCESS);
    REQUIRE_FALSE(scope.active());
  }

  const GuestExecutionCaptureExternalEventSnapshot snapshot =
      harness.log->snapshot();
  REQUIRE(snapshot.replayable());
  REQUIRE(snapshot.active_calls.empty());
  REQUIRE(snapshot.events.size() == 1);
  const auto& record = snapshot.events[0];
  REQUIRE(record.kind == GuestExecutionCaptureExternalEventKind::kKernelExport);
  REQUIRE(record.disposition ==
          GuestExecutionCaptureExternalEventDisposition::kReplayCaptured);
  REQUIRE(record.mutation_source ==
          GuestExecutionCaptureExternalEventMutationSource::kActiveGuestThread);
  REQUIRE(record.export_ordinal == kExportOrdinal);
  REQUIRE(record.guest_address == harness.thunk_address);
  REQUIRE(record.call_site_address == harness.call_site_address);
  // Four thread spans plus the one dispatcher header.
  REQUIRE(record.effect_ranges.size() == 5);
  REQUIRE(record.effect_byte_count == 14);
  REQUIRE(record.preimage.size() == record.effect_byte_count);
  REQUIRE(record.postimage.size() == record.effect_byte_count);
  REQUIRE(record.preimage != record.postimage);
  REQUIRE(record.has_returned_value);
  REQUIRE(DecodeLittleEndian(record.returned_value_le.data()) ==
          uint64_t(int32_t(X_STATUS_SUCCESS)));
  REQUIRE(record.has_volatile_registers);
  const uint8_t* registers = record.volatile_registers_le.data();
  REQUIRE(DecodeLittleEndian(registers) == 0x1111111111111111ull);
  // r3 is the value the trampoline is about to store, not the live one.
  REQUIRE(DecodeLittleEndian(registers + 8) ==
          uint64_t(int32_t(X_STATUS_SUCCESS)));
  REQUIRE(DecodeLittleEndian(registers + 10 * 8) == 0x3333333333333333ull);
  REQUIRE(DecodeLittleEndian(registers + 11 * 8) == 0x4444444444444444ull);
  REQUIRE(DecodeLittleEndian(registers + 12 * 8) == harness.context.cr());
}

TEST_CASE(
    "Modeled export adapter orders multi-object effect ranges canonically",
    "[guest-export-event]") {
  ExportEventHarness harness;
  const uint32_t event_address =
      harness.AllocateObject(X_OBJECT_TYPES::EventNotificationObject, 1);
  const uint32_t semaphore_address =
      harness.AllocateObject(X_OBJECT_TYPES::SemaphoreObject, 3);
  // Declared highest address first, so a producer that simply appended in call
  // order would emit a descending list.
  const uint32_t objects[] = {std::max(event_address, semaphore_address),
                              std::min(event_address, semaphore_address)};

  std::vector<Range> ranges;
  REQUIRE(BuildGuestExecutionExportEventEffectRanges(
      *harness.memory,
      harness.Dispatch(GuestSchedulerCaptureWaitKind::kMultiAny, objects),
      &ranges));
  REQUIRE(ranges.size() == 6);
  uint32_t total = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    REQUIRE(ranges[i].byte_count != 0);
    if (i) {
      // Ascending, and separated by at least one byte.
      REQUIRE(ranges[i].address >
              ranges[i - 1].address + ranges[i - 1].byte_count);
    }
    total += ranges[i].byte_count;
  }
  REQUIRE(total == 18);
  // The thread spans are exactly the fields the modeled wait writes.
  const uint32_t base = harness.kthread_address;
  const std::vector<Range> thread_ranges = {
      {base + uint32_t(offsetof(X_KTHREAD, thread_state)), 1},
      {base + uint32_t(offsetof(X_KTHREAD, alertable)), 2},
      {base + uint32_t(offsetof(X_KTHREAD, wait_result)), 6},
      {base + uint32_t(offsetof(X_KTHREAD, wait_reason)), 1}};
  for (const Range& expected : thread_ranges) {
    REQUIRE(std::find(ranges.begin(), ranges.end(), expected) != ranges.end());
  }
  for (uint32_t address : objects) {
    const Range expected = {
        address + uint32_t(offsetof(X_DISPATCH_HEADER, signal_state)), 4};
    REQUIRE(std::find(ranges.begin(), ranges.end(), expected) != ranges.end());
  }
}

TEST_CASE("Modeled export adapter refuses a repeated wait object",
          "[guest-export-event]") {
  ExportEventHarness harness;
  const uint32_t event_address =
      harness.AllocateObject(X_OBJECT_TYPES::EventSynchronizationObject, 1);
  const uint32_t objects[] = {event_address, event_address};

  std::vector<Range> ranges;
  REQUIRE_FALSE(BuildGuestExecutionExportEventEffectRanges(
      *harness.memory,
      harness.Dispatch(GuestSchedulerCaptureWaitKind::kMultiAll, objects),
      &ranges));
  REQUIRE(ranges.empty());

  {
    GuestExecutionExportEventScope scope(
        harness.log, harness.memory.get(), &harness.context,
        harness.Dispatch(GuestSchedulerCaptureWaitKind::kMultiAll, objects));
    REQUIRE_FALSE(scope.active());
  }
  // Nothing was offered to the log, so one unrepresentable wait cannot latch a
  // rejection that ends the whole recording.
  const GuestExecutionCaptureExternalEventSnapshot snapshot =
      harness.log->snapshot();
  REQUIRE(snapshot.events.empty());
  REQUIRE(snapshot.replayable());
}

TEST_CASE("Modeled export adapter refuses an unenumerable dispatcher type",
          "[guest-export-event]") {
  ExportEventHarness harness;
  // A mutant acquire also takes ownership and links the mutant into the
  // thread's owned list, which these ranges cannot bound.
  const uint32_t mutant_address =
      harness.AllocateObject(X_OBJECT_TYPES::MutantObject, 1);
  const uint32_t objects[] = {mutant_address};

  std::vector<Range> ranges;
  REQUIRE_FALSE(BuildGuestExecutionExportEventEffectRanges(
      *harness.memory,
      harness.Dispatch(GuestSchedulerCaptureWaitKind::kSingle, objects),
      &ranges));

  GuestExecutionExportEventScope scope(
      harness.log, harness.memory.get(), &harness.context,
      harness.Dispatch(GuestSchedulerCaptureWaitKind::kSingle, objects));
  REQUIRE_FALSE(scope.active());
  REQUIRE(harness.log->snapshot().events.empty());
  REQUIRE(harness.log->snapshot().replayable());
}

TEST_CASE("Modeled export adapter refuses wait kinds outside the model",
          "[guest-export-event]") {
  REQUIRE(
      IsModeledBlockingExportWaitKind(GuestSchedulerCaptureWaitKind::kSingle));
  REQUIRE(IsModeledBlockingExportWaitKind(
      GuestSchedulerCaptureWaitKind::kMultiAny));
  REQUIRE(IsModeledBlockingExportWaitKind(
      GuestSchedulerCaptureWaitKind::kMultiAll));
  for (GuestSchedulerCaptureWaitKind kind :
       {GuestSchedulerCaptureWaitKind::kNone,
        GuestSchedulerCaptureWaitKind::kDelay,
        GuestSchedulerCaptureWaitKind::kFence,
        GuestSchedulerCaptureWaitKind::kIoOffload,
        GuestSchedulerCaptureWaitKind::kSpinBackoff,
        GuestSchedulerCaptureWaitKind::kIoCompletion,
        GuestSchedulerCaptureWaitKind::kSocketIo}) {
    REQUIRE_FALSE(IsModeledBlockingExportWaitKind(kind));
  }

  ExportEventHarness harness;
  const uint32_t event_address =
      harness.AllocateObject(X_OBJECT_TYPES::EventSynchronizationObject, 1);
  const uint32_t objects[] = {event_address};
  GuestExecutionExportEventScope scope(
      harness.log, harness.memory.get(), &harness.context,
      harness.Dispatch(GuestSchedulerCaptureWaitKind::kIoCompletion, objects));
  REQUIRE_FALSE(scope.active());
  REQUIRE(harness.log->snapshot().events.empty());
  REQUIRE(harness.log->snapshot().replayable());
}

TEST_CASE("Modeled export adapter decodes an export thunk from the call site",
          "[guest-export-event]") {
  ExportEventHarness harness;
  REQUIRE(DecodeGuestExecutionExportEventThunkAddress(
              *harness.memory, harness.call_site_address) ==
          harness.thunk_address);

  // A backward call reaches the same answer through the sign-extended field.
  const uint32_t backward_site = harness.code_address + 48;
  harness.StoreBranchAndLink(backward_site - 4, harness.code_address);
  REQUIRE(DecodeGuestExecutionExportEventThunkAddress(
              *harness.memory, backward_site) == harness.code_address);

  // A computed call through the count register names no thunk.
  xe::store_and_swap<uint32_t>(
      harness.memory->TranslateVirtual(harness.call_site_address - 4),
      0x4E800421u);
  REQUIRE(DecodeGuestExecutionExportEventThunkAddress(
              *harness.memory, harness.call_site_address) == 0);

  // Neither does an absolute branch or a misaligned return address.
  xe::store_and_swap<uint32_t>(
      harness.memory->TranslateVirtual(harness.call_site_address - 4),
      (18u << 26) | 0x20u | 0x3u);
  REQUIRE(DecodeGuestExecutionExportEventThunkAddress(
              *harness.memory, harness.call_site_address) == 0);
  REQUIRE(DecodeGuestExecutionExportEventThunkAddress(
              *harness.memory, harness.call_site_address + 2) == 0);
}

TEST_CASE("Modeled export adapter refuses an unattributed call site",
          "[guest-export-event]") {
  ExportEventHarness harness;
  const uint32_t event_address =
      harness.AllocateObject(X_OBJECT_TYPES::EventSynchronizationObject, 1);
  const uint32_t objects[] = {event_address};

  GuestExecutionExportEventDispatch dispatch =
      harness.Dispatch(GuestSchedulerCaptureWaitKind::kSingle, objects);
  dispatch.guest_address = 0;
  GuestExecutionExportEventScope scope(harness.log, harness.memory.get(),
                                       &harness.context, dispatch);
  REQUIRE_FALSE(scope.active());
  REQUIRE(harness.log->snapshot().events.empty());
  REQUIRE(harness.log->snapshot().replayable());
}

TEST_CASE("Modeled export adapter rejects the session on an abandoned dispatch",
          "[guest-export-event]") {
  ExportEventHarness harness;
  const uint32_t event_address =
      harness.AllocateObject(X_OBJECT_TYPES::EventSynchronizationObject, 1);
  const uint32_t objects[] = {event_address};

  {
    GuestExecutionExportEventScope scope(
        harness.log, harness.memory.get(), &harness.context,
        harness.Dispatch(GuestSchedulerCaptureWaitKind::kSingle, objects));
    REQUIRE(scope.active());
  }

  const GuestExecutionCaptureExternalEventSnapshot snapshot =
      harness.log->snapshot();
  REQUIRE(snapshot.events.size() == 1);
  REQUIRE(snapshot.events[0].disposition ==
          GuestExecutionCaptureExternalEventDisposition::kRejectSession);
  REQUIRE_FALSE(snapshot.events[0].has_returned_value);
  REQUIRE_FALSE(snapshot.events[0].has_volatile_registers);
  REQUIRE(snapshot.active_calls.empty());
  REQUIRE_FALSE(snapshot.replayable());
  REQUIRE(harness.log->CanDetach());
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
