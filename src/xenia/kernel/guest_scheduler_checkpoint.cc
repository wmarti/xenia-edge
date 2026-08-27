/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler_checkpoint.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <limits>

namespace xe {
namespace kernel {

bool GuestSchedulerCheckpointBarrier::Begin(
    uint8_t dispatch_cpu_mask,
    std::span<const GuestSchedulerCheckpointParticipant> participants,
    uint64_t* out_generation) {
  if (out_generation) {
    *out_generation = 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!dispatch_cpu_mask) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
    return false;
  }

  uint8_t running_cpu_mask = 0;
  for (size_t i = 0; i < participants.size(); ++i) {
    const auto& participant = participants[i];
    // Passive participants do not rendezvous, but they still belong to a
    // scheduler queue whose owning dispatch CPU must be part of this barrier.
    if (!participant.thread_id || participant.cpu < 0 ||
        participant.cpu >= std::numeric_limits<uint8_t>::digits) {
      rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
      return false;
    }
    if (!(dispatch_cpu_mask & (uint8_t{1} << participant.cpu))) {
      rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
      return false;
    }
    if (std::find_if(participants.begin(), participants.begin() + i,
                     [&participant](const auto& other) {
                       return other.thread_id == participant.thread_id;
                     }) != participants.begin() + i) {
      rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
      return false;
    }
    if (participant.state ==
        GuestSchedulerCheckpointParticipantState::kRunning) {
      const uint8_t cpu_bit = uint8_t{1} << participant.cpu;
      if (running_cpu_mask & cpu_bit) {
        rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
        return false;
      }
      running_cpu_mask |= cpu_bit;
    }
  }

  if (generation_ == std::numeric_limits<uint64_t>::max()) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
    return false;
  }
  ++generation_;
  rejection_ = GuestSchedulerCheckpointBarrierRejection::kNone;
  dispatch_cpu_mask_ = dispatch_cpu_mask;
  quiesced_cpu_mask_ = 0;
  participants_.assign(participants.begin(), participants.end());
  arrivals_.assign(participants_.size(), ArrivalState{});
  if (out_generation) {
    *out_generation = generation_;
  }
  active_.store(true, std::memory_order_release);
  return true;
}

bool GuestSchedulerCheckpointBarrier::ArriveAtSafepoint(uint32_t thread_id,
                                                        int cpu,
                                                        uint32_t guest_pc) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_.load(std::memory_order_relaxed) ||
      rejection_ != GuestSchedulerCheckpointBarrierRejection::kNone) {
    return false;
  }
  if (!guest_pc || (guest_pc & 3) || cpu < 0 ||
      cpu >= std::numeric_limits<uint8_t>::digits) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidGuestPc;
    NotifyIfTerminalLocked();
    return false;
  }
  auto it = std::find_if(
      participants_.begin(), participants_.end(),
      [thread_id](const auto& participant) {
        return participant.thread_id == thread_id &&
               participant.state ==
                   GuestSchedulerCheckpointParticipantState::kRunning;
      });
  if (it == participants_.end() || it->cpu != cpu) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kUnexpectedSafepoint;
    NotifyIfTerminalLocked();
    return false;
  }
  const size_t index = static_cast<size_t>(it - participants_.begin());
  if (arrivals_[index].arrived) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kDuplicateSafepoint;
    NotifyIfTerminalLocked();
    return false;
  }
  arrivals_[index].arrived = true;
  it->guest_pc = guest_pc;
  it->resume_kind = GuestSchedulerCheckpointResumeKind::kJitSafepoint;
  it->restorable = true;
  return true;
}

bool GuestSchedulerCheckpointBarrier::ConfirmSwitchOut(uint32_t thread_id,
                                                       int cpu) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_.load(std::memory_order_relaxed)) {
    return false;
  }
  auto it = std::find_if(
      participants_.begin(), participants_.end(),
      [thread_id](const auto& participant) {
        return participant.thread_id == thread_id &&
               participant.state ==
                   GuestSchedulerCheckpointParticipantState::kRunning;
      });
  if (it == participants_.end() || it->cpu != cpu) {
    if (rejection_ == GuestSchedulerCheckpointBarrierRejection::kNone) {
      rejection_ =
          GuestSchedulerCheckpointBarrierRejection::kUnexpectedSwitchOut;
      condition_.notify_all();
    }
    return false;
  }
  const size_t index = static_cast<size_t>(it - participants_.begin());
  if (!arrivals_[index].arrived || arrivals_[index].switched_out) {
    if (rejection_ == GuestSchedulerCheckpointBarrierRejection::kNone) {
      rejection_ =
          GuestSchedulerCheckpointBarrierRejection::kUnexpectedSwitchOut;
      condition_.notify_all();
    }
    return false;
  }
  arrivals_[index].switched_out = true;
  NotifyIfTerminalLocked();
  return true;
}

bool GuestSchedulerCheckpointBarrier::AcknowledgeDispatchQuiesced(int cpu) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_.load(std::memory_order_relaxed) || cpu < 0 ||
      cpu >= std::numeric_limits<uint8_t>::digits) {
    return false;
  }
  const uint8_t cpu_bit = uint8_t{1} << cpu;
  if (!(dispatch_cpu_mask_ & cpu_bit)) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
    NotifyIfTerminalLocked();
    return false;
  }
  quiesced_cpu_mask_ |= cpu_bit;
  NotifyIfTerminalLocked();
  return true;
}

void GuestSchedulerCheckpointBarrier::Reject(
    GuestSchedulerCheckpointBarrierRejection rejection) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_.load(std::memory_order_relaxed) ||
      rejection == GuestSchedulerCheckpointBarrierRejection::kNone ||
      rejection_ != GuestSchedulerCheckpointBarrierRejection::kNone) {
    return;
  }
  rejection_ = rejection;
  condition_.notify_all();
}

bool GuestSchedulerCheckpointBarrier::WaitUntilQuiesced(
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!active_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!condition_.wait_for(lock, timeout, [this]() {
        return !active_.load(std::memory_order_relaxed) ||
               rejection_ != GuestSchedulerCheckpointBarrierRejection::kNone ||
               IsQuiescedLocked();
      })) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kTimedOut;
    return false;
  }
  return active_.load(std::memory_order_relaxed) &&
         rejection_ == GuestSchedulerCheckpointBarrierRejection::kNone &&
         IsQuiescedLocked();
}

bool GuestSchedulerCheckpointBarrier::Finalize(
    uint64_t expected_generation,
    GuestSchedulerCheckpointBarrierSnapshot* out_final_snapshot,
    GuestSchedulerCheckpointBarrierRejection* out_rejection) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (out_final_snapshot) {
    *out_final_snapshot = {};
    out_final_snapshot->generation = expected_generation;
  }
  if (!out_rejection) {
    return false;
  }
  if (!active_.load(std::memory_order_relaxed)) {
    if (expected_generation == terminal_generation_ &&
        terminal_rejection_ !=
            GuestSchedulerCheckpointBarrierRejection::kNone) {
      *out_rejection = terminal_rejection_;
      if (out_final_snapshot) {
        *out_final_snapshot = SnapshotLocked(terminal_quiesced_);
        out_final_snapshot->active = false;
        out_final_snapshot->rejection = terminal_rejection_;
      }
      return false;
    }
    *out_rejection = GuestSchedulerCheckpointBarrierRejection::kNotActive;
    if (out_final_snapshot) {
      out_final_snapshot->rejection = *out_rejection;
    }
    return false;
  }
  if (expected_generation != generation_) {
    *out_rejection = GuestSchedulerCheckpointBarrierRejection::kStaleGeneration;
    if (out_final_snapshot) {
      out_final_snapshot->rejection = *out_rejection;
    }
    return false;
  }

  const bool quiesced = IsQuiescedLocked();
  if (rejection_ == GuestSchedulerCheckpointBarrierRejection::kNone &&
      !quiesced) {
    rejection_ = GuestSchedulerCheckpointBarrierRejection::kInvalidTopology;
  }
  *out_rejection = rejection_;
  terminal_generation_ = generation_;
  terminal_rejection_ = rejection_;
  terminal_quiesced_ = quiesced;
  if (out_final_snapshot) {
    *out_final_snapshot = SnapshotLocked(quiesced);
    out_final_snapshot->active = false;
  }
  active_.store(false, std::memory_order_release);
  condition_.notify_all();
  return true;
}

GuestSchedulerCheckpointBarrierSnapshot
GuestSchedulerCheckpointBarrier::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

GuestSchedulerCheckpointBarrierSnapshot
GuestSchedulerCheckpointBarrier::SnapshotLocked(bool final_quiesced) const {
  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  snapshot.generation = generation_;
  snapshot.rejection = rejection_;
  snapshot.dispatch_cpu_mask = dispatch_cpu_mask_;
  snapshot.quiesced_cpu_mask = quiesced_cpu_mask_;
  snapshot.active = active_.load(std::memory_order_relaxed);
  snapshot.quiesced = final_quiesced || (snapshot.active && IsQuiescedLocked());
  snapshot.roster_scope = GuestSchedulerCheckpointRosterScope::kSchedulerOwned;
  snapshot.release_policy =
      GuestSchedulerCheckpointReleasePolicy::kRunningSafepointsRequeueAtHead;
  snapshot.participants = participants_;
  return snapshot;
}

bool GuestSchedulerCheckpointBarrier::IsQuiescedLocked() const {
  if ((quiesced_cpu_mask_ & dispatch_cpu_mask_) != dispatch_cpu_mask_) {
    return false;
  }
  for (size_t i = 0; i < participants_.size(); ++i) {
    if (participants_[i].state ==
            GuestSchedulerCheckpointParticipantState::kRunning &&
        !arrivals_[i].switched_out) {
      return false;
    }
  }
  return true;
}

void GuestSchedulerCheckpointBarrier::NotifyIfTerminalLocked() {
  if (rejection_ != GuestSchedulerCheckpointBarrierRejection::kNone ||
      IsQuiescedLocked()) {
    condition_.notify_all();
  }
}

bool GuestSchedulerCheckpointHeldState::Arrive(uint64_t generation) {
  if (!generation || phase_ != GuestSchedulerCheckpointHeldPhase::kEmpty) {
    return false;
  }
  generation_ = generation;
  phase_ = GuestSchedulerCheckpointHeldPhase::kArrived;
  discard_on_release_ = false;
  return true;
}

bool GuestSchedulerCheckpointHeldState::ConfirmSwitchOut() {
  if (phase_ == GuestSchedulerCheckpointHeldPhase::kArrived) {
    phase_ = GuestSchedulerCheckpointHeldPhase::kSwitchedOut;
    return true;
  }
  if (phase_ == GuestSchedulerCheckpointHeldPhase::kReleasePending) {
    phase_ = GuestSchedulerCheckpointHeldPhase::kReadyToRequeue;
    return true;
  }
  return false;
}

bool GuestSchedulerCheckpointHeldState::RequestRelease(uint64_t generation) {
  if (!generation || generation != generation_) {
    return false;
  }
  if (phase_ == GuestSchedulerCheckpointHeldPhase::kArrived) {
    phase_ = GuestSchedulerCheckpointHeldPhase::kReleasePending;
    return true;
  }
  if (phase_ == GuestSchedulerCheckpointHeldPhase::kSwitchedOut) {
    phase_ = GuestSchedulerCheckpointHeldPhase::kReadyToRequeue;
    return true;
  }
  return phase_ == GuestSchedulerCheckpointHeldPhase::kReleasePending ||
         phase_ == GuestSchedulerCheckpointHeldPhase::kReadyToRequeue;
}

bool GuestSchedulerCheckpointHeldState::DiscardOnRelease() {
  if (phase_ == GuestSchedulerCheckpointHeldPhase::kEmpty) {
    return false;
  }
  discard_on_release_ = true;
  return true;
}

bool GuestSchedulerCheckpointHeldState::ConsumeReady(uint64_t generation) {
  if (!generation || generation != generation_ ||
      phase_ != GuestSchedulerCheckpointHeldPhase::kReadyToRequeue) {
    return false;
  }
  generation_ = 0;
  phase_ = GuestSchedulerCheckpointHeldPhase::kEmpty;
  discard_on_release_ = false;
  return true;
}

}  // namespace kernel
}  // namespace xe

#endif
