/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <array>
#include <chrono>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/guest_scheduler_checkpoint.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace testing {
namespace {

using Participant = GuestSchedulerCheckpointParticipant;
using ParticipantState = GuestSchedulerCheckpointParticipantState;
using Rejection = GuestSchedulerCheckpointBarrierRejection;
using ReleasePolicy = GuestSchedulerCheckpointReleasePolicy;
using ResumeKind = GuestSchedulerCheckpointResumeKind;
using RosterScope = GuestSchedulerCheckpointRosterScope;

Participant MakeParticipant(
    uint32_t thread_id, int cpu, ParticipantState state, uint32_t guest_pc = 0,
    ResumeKind resume_kind = ResumeKind::kNativeContinuation) {
  Participant participant;
  participant.thread_id = thread_id;
  participant.guest_pc = guest_pc;
  participant.cpu = static_cast<int8_t>(cpu);
  participant.state = state;
  participant.resume_kind = resume_kind;
  return participant;
}

const Participant& FindParticipant(
    const GuestSchedulerCheckpointBarrierSnapshot& snapshot,
    uint32_t thread_id) {
  auto it =
      std::find_if(snapshot.participants.begin(), snapshot.participants.end(),
                   [thread_id](const Participant& participant) {
                     return participant.thread_id == thread_id;
                   });
  REQUIRE(it != snapshot.participants.end());
  return *it;
}

}  // namespace

TEST_CASE("Scheduler checkpoint barrier holds running and preserves passive",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kRunning),
      MakeParticipant(0x202, 1, ParticipantState::kRunning),
      MakeParticipant(0x303, 0, ParticipantState::kReady),
      MakeParticipant(0x404, 1, ParticipantState::kBlocked, 0x82001000,
                      ResumeKind::kAfterBlockingExport),
      MakeParticipant(0x505, 1, ParticipantState::kSuspended),
  };

  REQUIRE(barrier.Begin(0b11, participants));
  REQUIRE(barrier.ArriveAtSafepoint(0x101, 0, 0x82002000));
  REQUIRE(barrier.ArriveAtSafepoint(0x202, 1, 0x82003000));
  REQUIRE(barrier.ConfirmSwitchOut(0x101, 0));
  REQUIRE(barrier.ConfirmSwitchOut(0x202, 1));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(0));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(1));
  REQUIRE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));

  const auto snapshot = barrier.snapshot();
  REQUIRE(snapshot.active);
  REQUIRE(snapshot.quiesced);
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.dispatch_cpu_mask == 0b11);
  REQUIRE(snapshot.quiesced_cpu_mask == 0b11);
  REQUIRE(snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);

  const auto& running_a = FindParticipant(snapshot, 0x101);
  REQUIRE(running_a.guest_pc == 0x82002000);
  REQUIRE(running_a.resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(running_a.restorable);
  const auto& running_b = FindParticipant(snapshot, 0x202);
  REQUIRE(running_b.guest_pc == 0x82003000);
  REQUIRE(running_b.resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(running_b.restorable);

  const auto& ready = FindParticipant(snapshot, 0x303);
  REQUIRE(ready.state == ParticipantState::kReady);
  REQUIRE(ready.guest_pc == 0);
  REQUIRE(ready.resume_kind == ResumeKind::kNativeContinuation);
  REQUIRE_FALSE(ready.restorable);
  const auto& blocked = FindParticipant(snapshot, 0x404);
  REQUIRE(blocked.state == ParticipantState::kBlocked);
  REQUIRE(blocked.guest_pc == 0x82001000);
  REQUIRE(blocked.resume_kind == ResumeKind::kAfterBlockingExport);
  REQUIRE_FALSE(blocked.restorable);
  const auto& suspended = FindParticipant(snapshot, 0x505);
  REQUIRE(suspended.state == ParticipantState::kSuspended);
  REQUIRE_FALSE(suspended.restorable);

  GuestSchedulerCheckpointBarrierSnapshot final_snapshot;
  Rejection final_rejection = Rejection::kInvalidTopology;
  REQUIRE(
      barrier.Finalize(snapshot.generation, &final_snapshot, &final_rejection));
  REQUIRE(final_rejection == Rejection::kNone);
  REQUIRE_FALSE(final_snapshot.active);
  REQUIRE(final_snapshot.quiesced);
  REQUIRE(final_snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(final_snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
  REQUIRE(final_snapshot.participants == snapshot.participants);
  REQUIRE_FALSE(barrier.active());
}

TEST_CASE("Scheduler checkpoint barrier waits for switch-out and every CPU",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kRunning),
  };
  REQUIRE(barrier.Begin(0b11, participants));
  REQUIRE(barrier.ArriveAtSafepoint(0x101, 0, 0x82002000));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(0));

  auto snapshot = barrier.snapshot();
  REQUIRE_FALSE(snapshot.quiesced);
  REQUIRE(barrier.ConfirmSwitchOut(0x101, 0));
  snapshot = barrier.snapshot();
  REQUIRE_FALSE(snapshot.quiesced);
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(1));
  REQUIRE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  snapshot = barrier.snapshot();
  Rejection final_rejection = Rejection::kInvalidTopology;
  REQUIRE(barrier.Finalize(snapshot.generation, nullptr, &final_rejection));
  REQUIRE(final_rejection == Rejection::kNone);
}

TEST_CASE("Scheduler checkpoint barrier rejects invalid safepoints",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kRunning),
      MakeParticipant(0x202, 1, ParticipantState::kBlocked, 0x82001000,
                      ResumeKind::kAfterBlockingExport),
  };
  REQUIRE(barrier.Begin(0b11, participants));
  REQUIRE_FALSE(barrier.ArriveAtSafepoint(0x202, 1, 0x82002000));
  REQUIRE(barrier.snapshot().rejection == Rejection::kUnexpectedSafepoint);
  REQUIRE_FALSE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  const auto snapshot = barrier.snapshot();
  Rejection final_rejection = Rejection::kNone;
  REQUIRE(barrier.Finalize(snapshot.generation, nullptr, &final_rejection));
  REQUIRE(final_rejection == Rejection::kUnexpectedSafepoint);
}

TEST_CASE("Scheduler checkpoint barrier rejects duplicate running CPUs",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  REQUIRE_FALSE(barrier.Begin(0, {}));
  REQUIRE(barrier.snapshot().rejection == Rejection::kInvalidTopology);
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kRunning),
      MakeParticipant(0x202, 0, ParticipantState::kRunning),
  };
  REQUIRE_FALSE(barrier.Begin(0b1, participants));
  REQUIRE(barrier.snapshot().rejection == Rejection::kInvalidTopology);
  REQUIRE_FALSE(barrier.active());
}

TEST_CASE("Scheduler checkpoint barrier validates passive ownership and PC",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array passive_wrong_cpu = {
      MakeParticipant(0x101, 1, ParticipantState::kBlocked, 0x82001000,
                      ResumeKind::kAfterBlockingExport),
  };
  REQUIRE_FALSE(barrier.Begin(0b1, passive_wrong_cpu));
  REQUIRE(barrier.snapshot().rejection == Rejection::kInvalidTopology);

  const std::array running = {
      MakeParticipant(0x202, 0, ParticipantState::kRunning),
  };
  REQUIRE(barrier.Begin(0b1, running));
  REQUIRE_FALSE(barrier.ArriveAtSafepoint(0x202, 0, 0x82002002));
  const auto snapshot = barrier.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kInvalidGuestPc);
  Rejection final_rejection = Rejection::kNone;
  REQUIRE(barrier.Finalize(snapshot.generation, nullptr, &final_rejection));
  REQUIRE(final_rejection == Rejection::kInvalidGuestPc);
}

TEST_CASE("Scheduler checkpoint barrier timeout fails closed and can restart",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kRunning),
  };
  REQUIRE(barrier.Begin(0b1, participants));
  REQUIRE_FALSE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  auto snapshot = barrier.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kTimedOut);
  const uint64_t first_generation = snapshot.generation;
  Rejection final_rejection = Rejection::kNone;
  REQUIRE(barrier.Finalize(first_generation, nullptr, &final_rejection));
  REQUIRE(final_rejection == Rejection::kTimedOut);

  const std::array passive = {
      MakeParticipant(0x202, 0, ParticipantState::kBlocked, 0x82001000,
                      ResumeKind::kAfterBlockingExport),
  };
  REQUIRE(barrier.Begin(0b1, passive));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(0));
  REQUIRE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  snapshot = barrier.snapshot();
  REQUIRE(snapshot.generation == first_generation + 1);
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(barrier.Finalize(snapshot.generation, nullptr, &final_rejection));
  REQUIRE(final_rejection == Rejection::kNone);
}

TEST_CASE("Scheduler checkpoint finalization rejects late topology changes",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kReady),
  };
  REQUIRE(barrier.Begin(0b1, participants));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(0));
  REQUIRE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  const auto provisional = barrier.snapshot();
  REQUIRE(provisional.rejection == Rejection::kNone);

  // GuestScheduler calls Reject before a late MarkReady, new participant or
  // other queue mutation while holding the same scheduler lock as finalize.
  barrier.Reject(Rejection::kTopologyChanged);
  GuestSchedulerCheckpointBarrierSnapshot final_snapshot;
  Rejection final_rejection = Rejection::kNone;
  REQUIRE(barrier.Finalize(provisional.generation, &final_snapshot,
                           &final_rejection));
  REQUIRE(final_rejection == Rejection::kTopologyChanged);
  REQUIRE(final_snapshot.rejection == Rejection::kTopologyChanged);
  REQUIRE(final_snapshot.generation == provisional.generation);
  REQUIRE(final_snapshot.participants == provisional.participants);
  REQUIRE_FALSE(final_snapshot.active);
  REQUIRE(final_snapshot.quiesced);
  REQUIRE_FALSE(barrier.active());
}

TEST_CASE("Scheduler checkpoint stale generation cannot release or snapshot",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  REQUIRE(barrier.Begin(0b1, {}));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(0));
  REQUIRE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  const auto first = barrier.snapshot();
  Rejection first_rejection = Rejection::kInvalidTopology;
  REQUIRE(barrier.Finalize(first.generation, nullptr, &first_rejection));
  REQUIRE(first_rejection == Rejection::kNone);

  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kReady),
  };
  REQUIRE(barrier.Begin(0b1, participants));
  REQUIRE(barrier.AcknowledgeDispatchQuiesced(0));
  REQUIRE(barrier.WaitUntilQuiesced(std::chrono::milliseconds(1)));
  const auto current = barrier.snapshot();
  REQUIRE(current.generation == first.generation + 1);

  GuestSchedulerCheckpointBarrierSnapshot stale_snapshot;
  Rejection stale_rejection = Rejection::kNone;
  REQUIRE_FALSE(
      barrier.Finalize(first.generation, &stale_snapshot, &stale_rejection));
  REQUIRE(stale_rejection == Rejection::kStaleGeneration);
  REQUIRE(stale_snapshot.rejection == Rejection::kStaleGeneration);
  REQUIRE(stale_snapshot.generation == first.generation);
  REQUIRE(stale_snapshot.participants.empty());
  REQUIRE_FALSE(stale_snapshot.active);
  REQUIRE(barrier.active());
  REQUIRE(barrier.snapshot().generation == current.generation);
  REQUIRE(barrier.snapshot().participants == current.participants);

  GuestSchedulerCheckpointBarrierSnapshot final_snapshot;
  Rejection final_rejection = Rejection::kInvalidTopology;
  REQUIRE(
      barrier.Finalize(current.generation, &final_snapshot, &final_rejection));
  REQUIRE(final_rejection == Rejection::kNone);
  REQUIRE_FALSE(final_snapshot.active);
}

TEST_CASE("Scheduler checkpoint held fiber releases in either race order",
          "[guest_scheduler_checkpoint]") {
  SECTION("normal switch-out before release") {
    GuestSchedulerCheckpointHeldState held;
    REQUIRE(held.Arrive(7));
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kArrived);
    REQUIRE(held.ConfirmSwitchOut());
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kSwitchedOut);
    REQUIRE(held.RequestRelease(7));
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kReadyToRequeue);
    REQUIRE(held.ConsumeReady(7));
    REQUIRE(held.empty());
    REQUIRE_FALSE(held.ConsumeReady(7));
  }

  SECTION("timeout release before fiber yield") {
    GuestSchedulerCheckpointHeldState held;
    REQUIRE(held.Arrive(8));
    REQUIRE(held.RequestRelease(8));
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kReleasePending);
    REQUIRE_FALSE(held.ConsumeReady(8));
    REQUIRE(held.ConfirmSwitchOut());
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kReadyToRequeue);
    REQUIRE(held.ConsumeReady(8));
    REQUIRE(held.empty());
  }

  SECTION("shutdown release before fiber yield is generation bound") {
    GuestSchedulerCheckpointHeldState held;
    REQUIRE(held.Arrive(9));
    REQUIRE_FALSE(held.RequestRelease(10));
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kArrived);
    REQUIRE(held.RequestRelease(9));
    REQUIRE(held.ConfirmSwitchOut());
    REQUIRE_FALSE(held.ConsumeReady(10));
    REQUIRE(held.ConsumeReady(9));
    REQUIRE(held.empty());
  }

  SECTION("forgotten fiber is consumed without requeue") {
    GuestSchedulerCheckpointHeldState held;
    REQUIRE(held.Arrive(10));
    REQUIRE(held.DiscardOnRelease());
    REQUIRE(held.discard_on_release());
    REQUIRE(held.RequestRelease(10));
    REQUIRE(held.ConfirmSwitchOut());
    REQUIRE(held.phase() == GuestSchedulerCheckpointHeldPhase::kReadyToRequeue);
    REQUIRE(held.discard_on_release());
    REQUIRE(held.ConsumeReady(10));
    REQUIRE(held.empty());
    REQUIRE_FALSE(held.discard_on_release());
    REQUIRE_FALSE(held.DiscardOnRelease());
  }
}

TEST_CASE("Scheduler checkpoint preserves a terminal cancellation diagnostic",
          "[guest_scheduler_checkpoint]") {
  GuestSchedulerCheckpointBarrier barrier;
  const std::array participants = {
      MakeParticipant(0x101, 0, ParticipantState::kRunning),
  };
  REQUIRE(barrier.Begin(0b1, participants));
  const uint64_t generation = barrier.snapshot().generation;
  barrier.Reject(Rejection::kCancelled);

  GuestSchedulerCheckpointBarrierSnapshot first_snapshot;
  Rejection first_rejection = Rejection::kNone;
  REQUIRE(barrier.Finalize(generation, &first_snapshot, &first_rejection));
  REQUIRE(first_rejection == Rejection::kCancelled);
  REQUIRE_FALSE(first_snapshot.quiesced);

  GuestSchedulerCheckpointBarrierSnapshot replayed_snapshot;
  Rejection replayed_rejection = Rejection::kNone;
  REQUIRE_FALSE(
      barrier.Finalize(generation, &replayed_snapshot, &replayed_rejection));
  REQUIRE(replayed_rejection == Rejection::kCancelled);
  REQUIRE(replayed_snapshot.rejection == Rejection::kCancelled);
  REQUIRE(replayed_snapshot.generation == generation);
  REQUIRE(replayed_snapshot.participants == first_snapshot.participants);
  REQUIRE_FALSE(replayed_snapshot.active);
  REQUIRE_FALSE(replayed_snapshot.quiesced);
  REQUIRE(replayed_snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(replayed_snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
}

TEST_CASE("Scheduler checkpoint route accepts only exact PPC block heads",
          "[guest_scheduler_checkpoint]") {
  XThread::SchedulerLinks links;
  links.has_run = true;

  REQUIRE(links.SetCheckpointJitSafepoint(4));
  REQUIRE(links.RestorableCheckpointJitSafepointPc(ParticipantState::kReady) ==
          4);
  REQUIRE(links.RestorableCheckpointJitSafepointPc(
              ParticipantState::kSuspended) == 4);
  REQUIRE(links.RestorableCheckpointJitSafepointPc(
              ParticipantState::kRunning) == 0);
  REQUIRE(links.RestorableCheckpointJitSafepointPc(
              ParticipantState::kBlocked) == 0);

  REQUIRE(links.SetCheckpointJitSafepoint(0xFFFFFFFCull));
  REQUIRE(links.RestorableCheckpointJitSafepointPc(ParticipantState::kReady) ==
          0xFFFFFFFCu);

  for (uint64_t invalid_pc :
       {uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{3},
        uint64_t{0x82001002}, uint64_t{0x100000000}}) {
    REQUIRE(links.SetCheckpointJitSafepoint(0x82001000));
    REQUIRE_FALSE(links.SetCheckpointJitSafepoint(invalid_pc));
    REQUIRE(links.RestorableCheckpointJitSafepointPc(
                ParticipantState::kReady) == 0);
  }
}

TEST_CASE("Scheduler checkpoint route clears and never claims unrun fibers",
          "[guest_scheduler_checkpoint]") {
  XThread::SchedulerLinks links;
  REQUIRE(links.SetCheckpointJitSafepoint(0x82001000));

  REQUIRE(links.RestorableCheckpointJitSafepointPc(ParticipantState::kReady) ==
          0);
  links.has_run = true;
  REQUIRE(links.RestorableCheckpointJitSafepointPc(ParticipantState::kReady) ==
          0x82001000);

  // Dispatch, export/host yields, waits, exit and termination all use this
  // same fail-closed transition before changing scheduler ownership.
  links.ClearCheckpointResumeRoute();
  REQUIRE(links.RestorableCheckpointJitSafepointPc(ParticipantState::kReady) ==
          0);
  REQUIRE(links.RestorableCheckpointJitSafepointPc(
              ParticipantState::kSuspended) == 0);
}

}  // namespace testing
}  // namespace kernel
}  // namespace xe

#endif
