/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_SCHEDULER_CAPTURE_OBSERVER_H_
#define XENIA_KERNEL_GUEST_SCHEDULER_CAPTURE_OBSERVER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xe {
namespace kernel {

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

// Kind-specific bits of GuestSchedulerCaptureEvent::flags.
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

// Durable values carried by GuestSchedulerCaptureEvent::value for kBlock and
// kReready. Keep these synchronized with XThread::CooperativeWaitKind.
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

// Fixed-size, pointer-free record of one scheduler transition. The sequence
// is the single global order of scheduler state mutation. Participants are
// identified by their ThreadState capture instance ID; the guest thread ID is
// diagnostic only. cpu is the dispatch CPU whose state the event mutates, or
// the caller's CPU for a decision, and -1 off a dispatch thread.
struct GuestSchedulerCaptureEvent {
  uint64_t sequence = 0;
  uint64_t capture_instance_id = 0;
  // Capture-transport-only executed-basic-block instruction weight accumulated
  // by this participant since its previous actor-owned scheduler cut. The
  // session bridge emits
  // this as a separate instruction-coverage event immediately before the
  // scheduler event; it is not duplicated in the durable scheduler payload.
  uint64_t guest_instruction_delta = 0;
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

  bool operator==(const GuestSchedulerCaptureEvent&) const = default;
};

// Capture-build-only observer of GuestScheduler transitions. Install one
// continuous observer before the first fiber dispatch and arm or disarm
// capture inside it; GuestScheduler permanently rejects later attachment.
//
// Every callback runs while GuestScheduler holds its scheduler lock, at the
// point of the state mutation the event describes, so the sequence is a total
// order across every dispatch CPU, the watchdog and external callers. The
// callback must therefore be bounded and nonblocking, must not call any
// GuestScheduler, XThread, KernelState or Processor API, and must not retain
// anything beyond the event value. Returning false latches a sticky rejection
// and no further event, including kShutdown, is delivered.
class GuestSchedulerCaptureObserver {
 public:
  virtual ~GuestSchedulerCaptureObserver() = default;

  virtual bool OnSchedulerEvent(
      const GuestSchedulerCaptureEvent& event) noexcept = 0;

  // Queried at attachment and at a detachment request made before the first
  // dispatch.
  virtual bool CanDetach() const noexcept = 0;
};

enum class GuestSchedulerCaptureRecorderRejection : uint8_t {
  kNone,
  kInvalidEvent,
  kSequenceGap,
  kSequenceRegression,
  kEventAfterShutdown,
  kOverflow,
};

struct GuestSchedulerCaptureRecorderSnapshot {
  GuestSchedulerCaptureRecorderRejection rejection =
      GuestSchedulerCaptureRecorderRejection::kNone;
  bool armed = false;
  bool shutdown_seen = false;
  // Every delivered event, whether or not it was recorded.
  uint64_t delivered_count = 0;
  uint64_t last_sequence = 0;
  std::vector<GuestSchedulerCaptureEvent> events;
};

// Bounded, nonblocking event recorder satisfying the observer contract. The
// buffer is reserved at construction so no callback allocates. Sequence
// continuity is validated for every delivered event, armed or not, and an
// overflow while armed fails closed instead of dropping events.
class GuestSchedulerCaptureEventRecorder final
    : public GuestSchedulerCaptureObserver {
 public:
  explicit GuestSchedulerCaptureEventRecorder(size_t capacity);
  ~GuestSchedulerCaptureEventRecorder() override;
  GuestSchedulerCaptureEventRecorder(
      const GuestSchedulerCaptureEventRecorder&) = delete;
  GuestSchedulerCaptureEventRecorder& operator=(
      const GuestSchedulerCaptureEventRecorder&) = delete;

  // Begins recording at the next delivered event. Fails after a rejection or
  // shutdown, or while already armed.
  bool Arm();
  // Stops recording; delivered events keep validating continuity.
  bool Disarm();

  bool OnSchedulerEvent(
      const GuestSchedulerCaptureEvent& event) noexcept override;
  bool CanDetach() const noexcept override;

  GuestSchedulerCaptureRecorderSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  const size_t capacity_;
  std::vector<GuestSchedulerCaptureEvent> events_;
  bool armed_ = false;
  bool shutdown_seen_ = false;
  uint64_t delivered_count_ = 0;
  uint64_t last_sequence_ = 0;
  GuestSchedulerCaptureRecorderRejection rejection_ =
      GuestSchedulerCaptureRecorderRejection::kNone;
};

}  // namespace kernel
}  // namespace xe

#endif

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_CAPTURE_OBSERVER_H_
