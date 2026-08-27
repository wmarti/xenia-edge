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
  uint32_t guest_pc = 0;
  int8_t cpu = -1;
  GuestSchedulerCheckpointParticipantState state =
      GuestSchedulerCheckpointParticipantState::kReady;
  GuestSchedulerCheckpointResumeKind resume_kind =
      GuestSchedulerCheckpointResumeKind::kNativeContinuation;
  bool restorable = false;

  bool operator==(const GuestSchedulerCheckpointParticipant&) const = default;
};

enum class GuestSchedulerCheckpointBarrierRejection : uint8_t {
  kNone,
  kAlreadyActive,
  kNotStarted,
  kCalledFromDispatchThread,
  kInvalidTopology,
  kUnexpectedSafepoint,
  kDuplicateSafepoint,
  kInvalidGuestPc,
  kUnexpectedSwitchOut,
  kTopologyChanged,
  kShutdown,
  kTimedOut,
};

struct GuestSchedulerCheckpointBarrierSnapshot {
  uint64_t generation = 0;
  GuestSchedulerCheckpointBarrierRejection rejection =
      GuestSchedulerCheckpointBarrierRejection::kNone;
  uint8_t dispatch_cpu_mask = 0;
  uint8_t quiesced_cpu_mask = 0;
  bool active = false;
  bool quiesced = false;
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
             std::span<const GuestSchedulerCheckpointParticipant> participants);
  bool ArriveAtSafepoint(uint32_t thread_id, int cpu, uint32_t guest_pc);
  bool ConfirmSwitchOut(uint32_t thread_id, int cpu);
  bool AcknowledgeDispatchQuiesced(int cpu);
  void Reject(GuestSchedulerCheckpointBarrierRejection rejection);
  bool WaitUntilQuiesced(std::chrono::milliseconds timeout);
  bool Release();

  bool active() const { return active_.load(std::memory_order_acquire); }
  GuestSchedulerCheckpointBarrierSnapshot snapshot() const;

 private:
  struct ArrivalState {
    bool arrived = false;
    bool switched_out = false;
  };

  bool IsQuiescedLocked() const;
  void NotifyIfTerminalLocked();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::atomic<bool> active_{false};
  uint64_t generation_ = 0;
  GuestSchedulerCheckpointBarrierRejection rejection_ =
      GuestSchedulerCheckpointBarrierRejection::kNone;
  uint8_t dispatch_cpu_mask_ = 0;
  uint8_t quiesced_cpu_mask_ = 0;
  std::vector<GuestSchedulerCheckpointParticipant> participants_;
  std::vector<ArrivalState> arrivals_;
};

}  // namespace kernel
}  // namespace xe

#endif

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_CHECKPOINT_H_
