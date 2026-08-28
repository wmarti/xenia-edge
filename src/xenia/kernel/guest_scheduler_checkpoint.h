/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_SCHEDULER_CHECKPOINT_H_
#define XENIA_KERNEL_GUEST_SCHEDULER_CHECKPOINT_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "xenia/kernel/guest_scheduler_capture_observer.h"

namespace xe {
namespace kernel {

enum class GuestSchedulerCheckpointParticipantState : uint8_t {
  kRunning,
  kReady,
  kBlocked,
  kSuspended,
};

enum class GuestSchedulerCheckpointResumeKind : uint8_t {
  kJitSafepoint,
  kNativeContinuation,
  kAfterBlockingExport,
  kNotYetRun,
};

struct GuestSchedulerCheckpointParticipant {
  uint32_t thread_id = 0;
  uint64_t capture_instance_id = 0;
  uint32_t guest_pc = 0;
  // Entry address of the function whose emitted code holds guest_pc. Two
  // functions can share a block head, so it is carried, not derived.
  uint32_t owning_function_address = 0;
  int8_t cpu = -1;
  uint8_t effective_priority = 0;
  int8_t ready_queue_level = -1;
  uint32_t ready_queue_fifo_ordinal = UINT32_MAX;
  GuestSchedulerCheckpointParticipantState state =
      GuestSchedulerCheckpointParticipantState::kReady;
  GuestSchedulerCheckpointResumeKind resume_kind =
      GuestSchedulerCheckpointResumeKind::kNativeContinuation;
  bool restorable = false;
  uint32_t preempt_defers_irql = 0;
  uint32_t preempt_defers_lock = 0;
  uint32_t capture_declined_safepoints = 0;
  // Cooperative-scheduler decay floor, ordered with effective_priority under
  // the scheduler lock.
  uint8_t base_priority = 0;
  uint8_t suspension_count = 0;
  // Remaining cooperative-scheduler slice in microseconds. A zero quantum
  // configuration and an expired slice both have a zero remainder.
  uint32_t quantum_remaining_us = 0;
  GuestSchedulerCaptureWaitKind blocked_wait_kind =
      GuestSchedulerCaptureWaitKind::kNone;
  GuestSchedulerCaptureWaitState blocked_wait;

  bool operator==(const GuestSchedulerCheckpointParticipant&) const = default;
};

enum class GuestSchedulerCheckpointBarrierRejection : uint8_t {
  kNone,
  kAlreadyActive,
  kNotActive,
  kStaleGeneration,
  kReleasePending,
  kNotStarted,
  kCalledFromDispatchThread,
  kInvalidTopology,
  kUnexpectedSafepoint,
  kDuplicateSafepoint,
  kInvalidGuestPc,
  kUnexpectedSwitchOut,
  kTopologyChanged,
  kShutdown,
  kCancelled,
  kTimedOut,
};

enum class GuestSchedulerCheckpointRosterScope : uint8_t {
  kNone,
  // Threads currently owned by a scheduler CPU: running or linked in a ready,
  // blocked or suspended queue. A created-suspended thread that has never been
  // queued is deliberately absent and must be reconciled from the Processor
  // ThreadState lifecycle roster by the checkpoint publisher.
  kSchedulerOwned,
};

enum class GuestSchedulerCheckpointReleasePolicy : uint8_t {
  kNone,
  // A participant reported as running remains the exact boundary state in the
  // final snapshot. Once its switch-out is confirmed, the scheduler makes it
  // ready at the head of its CPU queue to resume the JIT safepoint.
  kRunningSafepointsRequeueAtHead,
};

struct GuestSchedulerCheckpointBarrierSnapshot {
  uint64_t generation = 0;
  GuestSchedulerCheckpointBarrierRejection rejection =
      GuestSchedulerCheckpointBarrierRejection::kNone;
  uint8_t dispatch_cpu_mask = 0;
  uint8_t quiesced_cpu_mask = 0;
  bool active = false;
  bool quiesced = false;
  GuestSchedulerCheckpointRosterScope roster_scope =
      GuestSchedulerCheckpointRosterScope::kNone;
  GuestSchedulerCheckpointReleasePolicy release_policy =
      GuestSchedulerCheckpointReleasePolicy::kNone;
  std::vector<GuestSchedulerCheckpointParticipant> participants;
};

// Thread-safe state machine for a scheduler checkpoint rendezvous. The
// scheduler owns queue mutation and uses this only to account for exact-PC
// arrivals and idle-dispatch acknowledgements. Passive ready, blocked and
// suspended participants remain in their scheduler queues throughout. The
// barrier deliberately accepts passive participants with restorable=false:
// quiescence preserves topology, but a later checkpoint publisher must reject
// any participant for which it cannot encode a durable continuation.
class GuestSchedulerCheckpointBarrier {
 public:
  GuestSchedulerCheckpointBarrier() = default;
  GuestSchedulerCheckpointBarrier(const GuestSchedulerCheckpointBarrier&) =
      delete;
  GuestSchedulerCheckpointBarrier& operator=(
      const GuestSchedulerCheckpointBarrier&) = delete;

  bool Begin(uint8_t dispatch_cpu_mask,
             std::span<const GuestSchedulerCheckpointParticipant> participants,
             uint64_t* out_generation = nullptr);
  bool ArriveAtSafepoint(uint32_t thread_id, int cpu, uint32_t guest_pc,
                         uint32_t preempt_defers_irql = 0,
                         uint32_t preempt_defers_lock = 0,
                         uint32_t capture_declined_safepoints = 0,
                         uint32_t quantum_remaining_us = 0);
  bool ConfirmSwitchOut(uint32_t thread_id, int cpu);
  bool AcknowledgeDispatchQuiesced(int cpu);
  void Reject(GuestSchedulerCheckpointBarrierRejection rejection);
  bool WaitUntilQuiesced(std::chrono::milliseconds timeout);

  // Atomically validates the generation and final barrier state, snapshots it,
  // and releases a matching generation. Returns true only when the supplied
  // generation owned the active barrier and was released. A true result may
  // still carry a rejection in out_rejection, in which case the checkpoint
  // must be discarded. A stale generation never releases the active barrier.
  bool Finalize(uint64_t expected_generation,
                GuestSchedulerCheckpointBarrierSnapshot* out_final_snapshot,
                GuestSchedulerCheckpointBarrierRejection* out_rejection);

  bool active() const { return active_.load(std::memory_order_acquire); }
  GuestSchedulerCheckpointBarrierSnapshot snapshot() const;

 private:
  struct ArrivalState {
    bool arrived = false;
    bool switched_out = false;
  };

  bool IsQuiescedLocked() const;
  GuestSchedulerCheckpointBarrierSnapshot SnapshotLocked(
      bool final_quiesced = false) const;
  void NotifyIfTerminalLocked();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::atomic<bool> active_{false};
  uint64_t generation_ = 0;
  GuestSchedulerCheckpointBarrierRejection rejection_ =
      GuestSchedulerCheckpointBarrierRejection::kNone;
  uint8_t dispatch_cpu_mask_ = 0;
  uint8_t quiesced_cpu_mask_ = 0;
  uint64_t terminal_generation_ = 0;
  GuestSchedulerCheckpointBarrierRejection terminal_rejection_ =
      GuestSchedulerCheckpointBarrierRejection::kNone;
  bool terminal_quiesced_ = false;
  std::vector<GuestSchedulerCheckpointParticipant> participants_;
  std::vector<ArrivalState> arrivals_;
};

enum class GuestSchedulerCheckpointHeldPhase : uint8_t {
  kEmpty,
  kArrived,
  kSwitchedOut,
  kReleasePending,
  kReadyToRequeue,
};

// Scheduler-lock-protected two-phase ownership for a fiber that has reached a
// checkpoint safepoint. Release may race before the fiber reaches the idle
// dispatcher; in that ordering requeue is deferred until switch-out confirms
// that the guest context and fiber stack are no longer running.
class GuestSchedulerCheckpointHeldState {
 public:
  bool Arrive(uint64_t generation);
  bool ConfirmSwitchOut();
  bool RequestRelease(uint64_t generation);
  bool DiscardOnRelease();
  bool ConsumeReady(uint64_t generation);

  uint64_t generation() const { return generation_; }
  GuestSchedulerCheckpointHeldPhase phase() const { return phase_; }
  bool discard_on_release() const { return discard_on_release_; }
  bool empty() const {
    return phase_ == GuestSchedulerCheckpointHeldPhase::kEmpty;
  }

 private:
  uint64_t generation_ = 0;
  GuestSchedulerCheckpointHeldPhase phase_ =
      GuestSchedulerCheckpointHeldPhase::kEmpty;
  bool discard_on_release_ = false;
};

}  // namespace kernel
}  // namespace xe

#endif

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_CHECKPOINT_H_
