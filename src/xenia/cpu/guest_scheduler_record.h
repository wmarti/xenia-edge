/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_SCHEDULER_RECORD_H_
#define XENIA_CPU_GUEST_SCHEDULER_RECORD_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace xe {
namespace cpu {

// The durable scheduler tape vocabulary and its payload decoder. Everything
// here is compiled unconditionally: a capture-disabled replay binary still has
// to read a recorded scheduler tape, so none of it may sit behind
// XE_ENABLE_GUEST_INVOCATION_CAPTURE. The capture-side kernel observer aliases
// these declarations rather than redeclaring them.

// One scheduler transition that changes, or decides, which participant runs
// next on a dispatch CPU. Values are durable tape identifiers.
enum class GuestSchedulerCaptureEventKind : uint8_t {
  kNone = 0,
  kEnqueueReady = 1,
  kDequeueReady = 2,
  kDispatch = 3,
  kSwitchOut = 4,
  kYield = 5,
  kPreemptRequest = 6,
  kSafepoint = 7,
  kBlock = 8,
  kReready = 9,
  kParkSuspended = 10,
  kResume = 11,
  kPriorityChange = 12,
  kMigrate = 13,
  kExit = 14,
  kTerminate = 15,
  kForget = 16,
  kShutdown = 17,
};

enum class GuestSchedulerCaptureReason : uint8_t {
  kNone = 0,
  // kPreemptRequest: who raised the running participant's preempt flag.
  kPriority = 1,
  kWake = 2,
  kTimeslice = 3,
  kTerminate = 4,
  kShutdown = 5,
  // kSafepoint: what the handler did with the raised flag.
  kDeferredLock = 6,
  kDeferredIrql = 7,
  kForcedIrql = 8,
  kYielded = 9,
  // kReready: which gate released the parked participant.
  kPolled = 10,
  kSignalEpoch = 11,
  kDeadline = 12,
  kUserApc = 13,
  kBackstop = 14,
  // kTerminate: what an external terminate did to the participant.
  kDetached = 15,
  kPreemptRequested = 16,
  kReadied = 17,
  kNeverRan = 18,
  kDeferredToDispatcher = 19,
};

// Kind-specific bits of the record's flags field.
enum GuestSchedulerCaptureEventFlags : uint16_t {
  // kEnqueueReady, kReready, kMigrate, kTerminate: linked at its level head.
  kGuestSchedulerCaptureFlagAtHead = 1u << 0,
  // kEnqueueReady: a voluntary yield that prefers any other ready thread.
  kGuestSchedulerCaptureFlagYieldToOther = 1u << 1,
  // kDequeueReady: the selection passed over the yielder.
  kGuestSchedulerCaptureFlagHonoredYield = 1u << 2,
  // kDispatch: the participant's first dispatch.
  kGuestSchedulerCaptureFlagFirstRun = 1u << 3,
  // kDispatch: a fresh slice was granted rather than a preempted remainder.
  kGuestSchedulerCaptureFlagFreshQuantum = 1u << 4,
  // kYield: YieldCurrentThread arguments and the preempted link state.
  kGuestSchedulerCaptureFlagQuantumEnd = 1u << 5,
  kGuestSchedulerCaptureFlagToLower = 1u << 6,
  kGuestSchedulerCaptureFlagPreempted = 1u << 7,
  // kSafepoint: which requests were pending when the handler ran.
  kGuestSchedulerCaptureFlagSchedulerRequested = 1u << 8,
  kGuestSchedulerCaptureFlagCaptureRequested = 1u << 9,
  // kBlock: BlockCurrentThread gating and arguments.
  kGuestSchedulerCaptureFlagGated = 1u << 10,
  kGuestSchedulerCaptureFlagAlertable = 1u << 11,
  kGuestSchedulerCaptureFlagInterruptible = 1u << 12,
  kGuestSchedulerCaptureFlagHasDeadline = 1u << 13,
};

inline constexpr uint32_t kGuestSchedulerCaptureForcedIrqlMinimumDeclines =
    4096;

constexpr size_t kGuestSchedulerCaptureMaximumWaitHandles = 8;

// Durable values carried by the record's value field for kBlock and kReready.
// Keep these synchronized with XThread::CooperativeWaitKind.
enum class GuestSchedulerCaptureWaitKind : uint8_t {
  kNone = 0,
  kSingle = 1,
  kMultiAny = 2,
  kMultiAll = 3,
  kDelay = 4,
  kFence = 5,
  kIoOffload = 6,
  kSpinBackoff = 7,
  kIoCompletion = 8,
  kSocketIo = 9,
};

enum GuestSchedulerCaptureWaitFlags : uint8_t {
  kGuestSchedulerCaptureWaitFlagGated = 1u << 0,
  kGuestSchedulerCaptureWaitFlagAlertable = 1u << 1,
  kGuestSchedulerCaptureWaitFlagInterruptible = 1u << 2,
  kGuestSchedulerCaptureWaitFlagUserApcPending = 1u << 3,
};

// Authenticated state of the cooperative wait at kBlock or kReready. The
// epoch arrays identify which tracked wait object moved without retaining an
// XObject pointer. A wait naming more handles than fit remains visible through
// handle_count and is rejected as non-replayable by the session bridge.
struct GuestSchedulerCaptureWaitState {
  uint64_t deadline_ms = 0;
  uint64_t observed_uptime_ms = 0;
  uint32_t wait_epoch = 0;
  uint32_t observed_wait_epoch = 0;
  uint8_t handle_count = 0;
  uint8_t flags = 0;
  std::array<uint32_t, kGuestSchedulerCaptureMaximumWaitHandles> handles = {};
  std::array<uint32_t, kGuestSchedulerCaptureMaximumWaitHandles>
      signal_epochs_before = {};
  std::array<uint32_t, kGuestSchedulerCaptureMaximumWaitHandles>
      signal_epochs_observed = {};

  bool operator==(const GuestSchedulerCaptureWaitState&) const = default;
};

// One decoded XEGSCE1 scheduler payload. This is the whole durable record:
// the capture-side event additionally carries a transport-only executed
// instruction delta, which the payload deliberately never serializes.
struct DecodedSchedulerRecord {
  uint64_t sequence = 0;
  uint64_t capture_instance_id = 0;
  uint32_t guest_thread_id = 0;
  // kSafepoint kForcedIrql and kYielded: safepoints declined since the
  // participant's previous terminal outcome, both lock and IRQL episodes.
  uint32_t count = 0;
  // Exact PPC address of kSafepoint. Zero for every other event kind.
  uint32_t guest_pc = 0;
  uint16_t flags = 0;
  GuestSchedulerCaptureEventKind kind = GuestSchedulerCaptureEventKind::kNone;
  GuestSchedulerCaptureReason reason = GuestSchedulerCaptureReason::kNone;
  int8_t cpu = -1;
  int8_t target_cpu = -1;
  // Effective level of the participant at the event. For kPriorityChange this
  // is the new level and value is the previous level.
  uint8_t priority = 0;
  // kSafepoint: guest IRQL. kBlock and kReready: XThread::CooperativeWaitKind.
  // kPriorityChange: the previous level.
  uint8_t value = 0;
  GuestSchedulerCaptureWaitState wait;

  bool operator==(const DecodedSchedulerRecord&) const = default;
};

// Little-endian XEGSCE1 payload, version 2:
//   0 magic, 8 version, 12 kind, 16 sequence, 24 capture_instance_id,
//   32 guest_thread_id, 36 count, 40 flags, 42 reason, 43 cpu, 44 target_cpu,
//   45 priority, 46 value, 47 reserved, 48 guest_pc, 52 wait_epoch,
//   56 observed_wait_epoch, 60 handle_count, 61 wait flags, 62 reserved,
//   64 deadline_ms, 72 observed_uptime_ms, 80 handles, 112 signal_epochs_
//   before, 144 signal_epochs_observed, 176 reserved tail.
class GuestSchedulerRecordCodec {
 public:
  static constexpr uint32_t kPayloadVersion = 2;
  static constexpr size_t kPayloadV1Size = 48;
  static constexpr size_t kPayloadSize = 192;

  // Envelope, size and enum decode. Unknown versions, wrong sizes, nonzero
  // reserved bytes and unmodeled kind or reason identifiers all fail closed.
  // Per-kind provenance is Validate's job, so a consumer that must reject a
  // well-formed but impossible record calls both.
  static bool Decode(std::span<const uint8_t> payload,
                     DecodedSchedulerRecord* record,
                     std::string* error = nullptr) noexcept;

  // Full per-kind provenance validation, shared by the capture encoder and by
  // every decoded-record consumer.
  static bool Validate(const DecodedSchedulerRecord& record,
                       std::string* error = nullptr) noexcept;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_SCHEDULER_RECORD_H_
