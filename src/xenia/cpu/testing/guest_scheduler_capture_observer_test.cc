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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"

namespace xe {
namespace kernel {
namespace testing {
namespace {

using Kind = GuestSchedulerCaptureEventKind;
using Reason = GuestSchedulerCaptureReason;
using Rejection = GuestSchedulerCaptureRecorderRejection;

constexpr uint64_t kParticipantA = 0x1001;
constexpr uint64_t kParticipantB = 0x1002;
constexpr uint8_t kWaitSingle = 1;  // XThread::CooperativeWaitKind::kSingle

struct EventSpec {
  Kind kind = Kind::kNone;
  uint64_t participant = 0;
  int cpu = 0;
  int target_cpu = -1;
  Reason reason = Reason::kNone;
  uint16_t flags = 0;
  uint8_t priority = 8;
  uint8_t value = 0;
  uint32_t count = 0;
};

GuestSchedulerCaptureEvent MakeEvent(uint64_t sequence, const EventSpec& spec) {
  GuestSchedulerCaptureEvent event;
  event.sequence = sequence;
  event.capture_instance_id = spec.participant;
  event.guest_thread_id = static_cast<uint32_t>(spec.participant & 0xFFFF);
  event.flags = spec.flags;
  event.kind = spec.kind;
  event.reason = spec.reason;
  event.cpu = static_cast<int8_t>(spec.cpu);
  event.target_cpu = static_cast<int8_t>(spec.target_cpu);
  event.priority = spec.priority;
  event.value = spec.value;
  event.count = spec.count;
  return event;
}

// Stands in for GuestScheduler's locked emission: one lock, one sequence,
// and a sticky latch once the observer rejects.
class FakeSchedulerDriver {
 public:
  explicit FakeSchedulerDriver(
      std::shared_ptr<GuestSchedulerCaptureObserver> observer)
      : observer_(std::move(observer)) {}

  bool Emit(const EventSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeliverLocked(MakeEvent(sequence_ + 1, spec));
  }

  bool EmitWithSequence(uint64_t sequence, const EventSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeliverLocked(MakeEvent(sequence, spec));
  }

  bool rejected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejected_;
  }

 private:
  bool DeliverLocked(const GuestSchedulerCaptureEvent& event) {
    if (rejected_) {
      return false;
    }
    sequence_ = event.sequence;
    if (!observer_->OnSchedulerEvent(event)) {
      rejected_ = true;
      return false;
    }
    return true;
  }

  mutable std::mutex mutex_;
  std::shared_ptr<GuestSchedulerCaptureObserver> observer_;
  uint64_t sequence_ = 0;
  bool rejected_ = false;
};

void RequireContiguous(const std::vector<GuestSchedulerCaptureEvent>& events,
                       uint64_t first_sequence) {
  uint64_t expected = first_sequence;
  for (const auto& event : events) {
    REQUIRE(event.sequence == expected);
    ++expected;
  }
}

std::vector<Kind> KindsOf(
    const std::vector<GuestSchedulerCaptureEvent>& events) {
  std::vector<Kind> kinds;
  kinds.reserve(events.size());
  for (const auto& event : events) {
    kinds.push_back(event.kind);
  }
  return kinds;
}

}  // namespace

TEST_CASE("Guest scheduler capture recorder orders two participants",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantA, -1, 0}));
  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantB, -1, 0}));
  REQUIRE(driver.Emit({Kind::kDequeueReady, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagFirstRun |
                           kGuestSchedulerCaptureFlagFreshQuantum}));
  REQUIRE(driver.Emit({Kind::kYield, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagToLower}));
  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantA, 0, 0, Reason::kNone,
                       kGuestSchedulerCaptureFlagYieldToOther}));
  REQUIRE(driver.Emit({Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kDequeueReady, kParticipantB, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagHonoredYield}));
  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantB, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagFirstRun |
                           kGuestSchedulerCaptureFlagFreshQuantum}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.delivered_count == 9);
  REQUIRE(snapshot.events.size() == 9);
  RequireContiguous(snapshot.events, 1);
  REQUIRE(KindsOf(snapshot.events) ==
          std::vector<Kind>{Kind::kEnqueueReady, Kind::kEnqueueReady,
                            Kind::kDequeueReady, Kind::kDispatch, Kind::kYield,
                            Kind::kEnqueueReady, Kind::kSwitchOut,
                            Kind::kDequeueReady, Kind::kDispatch});
  REQUIRE(snapshot.events[0].capture_instance_id == kParticipantA);
  REQUIRE(snapshot.events[0].cpu == -1);
  REQUIRE(snapshot.events[0].target_cpu == 0);
  REQUIRE(snapshot.events[1].capture_instance_id == kParticipantB);
  REQUIRE(snapshot.events[5].flags == kGuestSchedulerCaptureFlagYieldToOther);
  REQUIRE(snapshot.events[7].capture_instance_id == kParticipantB);
  REQUIRE(snapshot.events[7].flags == kGuestSchedulerCaptureFlagHonoredYield);
  REQUIRE(snapshot.events[8].flags == (kGuestSchedulerCaptureFlagFirstRun |
                                       kGuestSchedulerCaptureFlagFreshQuantum));
}

TEST_CASE("Guest scheduler capture recorder records block and wake",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagFreshQuantum}));
  REQUIRE(driver.Emit({Kind::kBlock, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagGated |
                           kGuestSchedulerCaptureFlagInterruptible |
                           kGuestSchedulerCaptureFlagHasDeadline,
                       8, kWaitSingle}));
  REQUIRE(driver.Emit({Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kReready, kParticipantA, 0, 0,
                       Reason::kSignalEpoch, 0, 9, kWaitSingle}));
  REQUIRE(driver.Emit(
      {Kind::kDequeueReady, kParticipantA, 0, -1, Reason::kNone, 0, 9}));
  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagFreshQuantum, 9}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 6);
  RequireContiguous(snapshot.events, 1);
  const auto& block = snapshot.events[1];
  REQUIRE(block.kind == Kind::kBlock);
  REQUIRE(block.value == kWaitSingle);
  REQUIRE((block.flags & kGuestSchedulerCaptureFlagGated) != 0);
  REQUIRE((block.flags & kGuestSchedulerCaptureFlagAlertable) == 0);
  const auto& reready = snapshot.events[3];
  REQUIRE(reready.kind == Kind::kReready);
  REQUIRE(reready.reason == Reason::kSignalEpoch);
  REQUIRE(reready.target_cpu == 0);
  REQUIRE(reready.priority == 9);
}

TEST_CASE("Guest scheduler capture recorder records preemption and safepoints",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagFreshQuantum}));
  REQUIRE(driver.Emit(
      {Kind::kEnqueueReady, kParticipantB, -1, 0, Reason::kNone, 0, 12}));
  REQUIRE(driver.Emit(
      {Kind::kPreemptRequest, kParticipantA, 0, -1, Reason::kPriority}));
  REQUIRE(driver.Emit({Kind::kSafepoint, kParticipantA, 0, -1,
                       Reason::kDeferredIrql,
                       kGuestSchedulerCaptureFlagSchedulerRequested, 8, 2}));
  REQUIRE(driver.Emit({Kind::kSafepoint, kParticipantA, 0, -1, Reason::kYielded,
                       kGuestSchedulerCaptureFlagSchedulerRequested, 8, 0}));
  REQUIRE(driver.Emit({Kind::kYield, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagQuantumEnd |
                           kGuestSchedulerCaptureFlagPreempted}));
  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantA, 0, 0, Reason::kNone,
                       kGuestSchedulerCaptureFlagAtHead}));
  REQUIRE(driver.Emit({Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE(driver.Emit(
      {Kind::kDequeueReady, kParticipantB, 0, -1, Reason::kNone, 0, 12}));
  REQUIRE(driver.Emit({Kind::kPreemptRequest, kParticipantB, 0, -1,
                       Reason::kTimeslice, 0, 12}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 10);
  RequireContiguous(snapshot.events, 1);
  REQUIRE(snapshot.events[2].reason == Reason::kPriority);
  REQUIRE(snapshot.events[3].reason == Reason::kDeferredIrql);
  REQUIRE(snapshot.events[3].value == 2);
  REQUIRE(snapshot.events[4].reason == Reason::kYielded);
  REQUIRE((snapshot.events[5].flags & kGuestSchedulerCaptureFlagPreempted) !=
          0);
  REQUIRE(snapshot.events[6].flags == kGuestSchedulerCaptureFlagAtHead);
  REQUIRE(snapshot.events[9].reason == Reason::kTimeslice);
}

TEST_CASE("Guest scheduler capture recorder keeps simultaneous requests",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  constexpr uint16_t kBoth = kGuestSchedulerCaptureFlagSchedulerRequested |
                             kGuestSchedulerCaptureFlagCaptureRequested;
  REQUIRE(driver.Emit(
      {Kind::kPreemptRequest, kParticipantA, 0, -1, Reason::kWake}));
  REQUIRE(driver.Emit(
      {Kind::kSafepoint, kParticipantA, 0, -1, Reason::kYielded, kBoth}));
  REQUIRE(driver.Emit({Kind::kYield, kParticipantA, 0, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagQuantumEnd |
                           kGuestSchedulerCaptureFlagPreempted}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 3);
  RequireContiguous(snapshot.events, 1);
  size_t safepoints = 0;
  for (const auto& event : snapshot.events) {
    if (event.kind == Kind::kSafepoint) {
      ++safepoints;
      REQUIRE(event.flags == kBoth);
    }
  }
  REQUIRE(safepoints == 1);
  REQUIRE(snapshot.events[2].kind == Kind::kYield);
}

TEST_CASE("Guest scheduler capture recorder records migration",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDequeueReady, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kMigrate, kParticipantA, 0, 3, Reason::kNone,
                       kGuestSchedulerCaptureFlagAtHead}));
  REQUIRE(driver.Emit({Kind::kDequeueReady, kParticipantA, 3}));
  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 3, -1, Reason::kNone,
                       kGuestSchedulerCaptureFlagFirstRun}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 4);
  RequireContiguous(snapshot.events, 1);
  REQUIRE(snapshot.events[1].kind == Kind::kMigrate);
  REQUIRE(snapshot.events[1].cpu == 0);
  REQUIRE(snapshot.events[1].target_cpu == 3);
  REQUIRE(snapshot.events[3].cpu == 3);
}

TEST_CASE("Guest scheduler capture recorder rejects a missing callback",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
  REQUIRE_FALSE(
      driver.EmitWithSequence(3, {Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE(driver.rejected());
  REQUIRE_FALSE(driver.Emit({Kind::kDispatch, kParticipantB, 0}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kSequenceGap);
  REQUIRE(snapshot.events.size() == 1);
  REQUIRE(snapshot.last_sequence == 1);
  REQUIRE_FALSE(recorder->Arm());
  REQUIRE(recorder->Disarm());
  REQUIRE_FALSE(recorder->Arm());
}

TEST_CASE("Guest scheduler capture recorder rejects out-of-order callbacks",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE_FALSE(
      driver.EmitWithSequence(2, {Kind::kSwitchOut, kParticipantA, 0}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kSequenceRegression);
  REQUIRE(snapshot.events.size() == 2);
  REQUIRE(snapshot.delivered_count == 3);
}

TEST_CASE("Guest scheduler capture recorder fails closed on overflow",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(3);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kYield, kParticipantA, 0}));
  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantA, 0, 0}));
  REQUIRE_FALSE(driver.Emit({Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE(driver.rejected());

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kOverflow);
  REQUIRE(snapshot.events.size() == 3);
  REQUIRE(snapshot.delivered_count == 4);
  RequireContiguous(snapshot.events, 1);
  REQUIRE_FALSE(driver.Emit({Kind::kShutdown, 0}));
  REQUIRE(recorder->snapshot().events.size() == 3);
}

TEST_CASE(
    "Guest scheduler capture recorder validates continuity while disarmed",
    "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantA, -1, 0}));
  REQUIRE(driver.Emit({Kind::kDequeueReady, kParticipantA, 0}));
  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.delivered_count == 2);
  REQUIRE(snapshot.events.empty());
  REQUIRE(snapshot.last_sequence == 2);

  REQUIRE(recorder->Arm());
  REQUIRE_FALSE(recorder->Arm());
  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
  REQUIRE(recorder->Disarm());
  REQUIRE_FALSE(recorder->Disarm());
  REQUIRE(driver.Emit({Kind::kYield, kParticipantA, 0}));
  REQUIRE(recorder->Arm());
  REQUIRE(driver.Emit({Kind::kEnqueueReady, kParticipantA, 0, 0}));

  snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.delivered_count == 5);
  REQUIRE(snapshot.events.size() == 2);
  REQUIRE(snapshot.events[0].sequence == 3);
  REQUIRE(snapshot.events[1].sequence == 5);

  REQUIRE(recorder->Disarm());
  REQUIRE_FALSE(
      driver.EmitWithSequence(7, {Kind::kSwitchOut, kParticipantA, 0}));
  REQUIRE(recorder->snapshot().rejection == Rejection::kSequenceGap);
}

TEST_CASE("Guest scheduler capture recorder rejects invalid events",
          "[guest_scheduler_capture]") {
  {
    auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(8);
    REQUIRE(recorder->Arm());
    FakeSchedulerDriver driver(recorder);
    REQUIRE_FALSE(driver.Emit({Kind::kNone, kParticipantA, 0}));
    REQUIRE(recorder->snapshot().rejection == Rejection::kInvalidEvent);
  }
  {
    auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(8);
    REQUIRE(recorder->Arm());
    FakeSchedulerDriver driver(recorder);
    REQUIRE_FALSE(driver.Emit({Kind::kDispatch, 0, 0}));
    REQUIRE(recorder->snapshot().rejection == Rejection::kInvalidEvent);
  }
  {
    auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(8);
    REQUIRE(recorder->Arm());
    FakeSchedulerDriver driver(recorder);
    REQUIRE(driver.Emit({Kind::kShutdown, 0, -1}));
    REQUIRE(recorder->snapshot().rejection == Rejection::kNone);
  }
}

TEST_CASE("Guest scheduler capture recorder closes at shutdown",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  REQUIRE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
  REQUIRE(
      driver.Emit({Kind::kForget, kParticipantA, 0, -1, Reason::kShutdown}));
  REQUIRE(driver.Emit({Kind::kShutdown, 0, -1}));
  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.shutdown_seen);
  REQUIRE_FALSE(snapshot.armed);
  REQUIRE(snapshot.events.size() == 3);
  REQUIRE(snapshot.events.back().kind == Kind::kShutdown);
  REQUIRE_FALSE(recorder->Arm());

  REQUIRE_FALSE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
  REQUIRE(recorder->snapshot().rejection == Rejection::kEventAfterShutdown);
}

TEST_CASE("Guest scheduler capture recorder survives a teardown race",
          "[guest_scheduler_capture]") {
  constexpr uint64_t kEventCount = 2000;
  auto recorder =
      std::make_shared<GuestSchedulerCaptureEventRecorder>(kEventCount + 1);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);
  std::atomic<bool> done{false};

  std::thread emitter([&] {
    for (uint64_t i = 0; i < kEventCount; ++i) {
      driver.Emit({i & 1 ? Kind::kSwitchOut : Kind::kDispatch,
                   i & 2 ? kParticipantB : kParticipantA,
                   static_cast<int>(i % 6)});
    }
    driver.Emit({Kind::kShutdown, 0, -1});
    done.store(true, std::memory_order_release);
  });
  // Catch assertions are main-thread only, so the poller only records.
  std::atomic<bool> observer_ok{true};
  std::atomic<uint64_t> polls{0};
  std::thread observer([&] {
    uint64_t last_seen = 0;
    while (!done.load(std::memory_order_acquire)) {
      auto snapshot = recorder->snapshot();
      polls.fetch_add(1, std::memory_order_relaxed);
      bool ok = snapshot.rejection == Rejection::kNone &&
                snapshot.last_sequence >= last_seen;
      last_seen = snapshot.last_sequence;
      uint64_t expected = 1;
      for (const auto& event : snapshot.events) {
        ok = ok && event.sequence == expected++;
      }
      // Once shutdown has disarmed the recorder it must agree to detach.
      if (snapshot.shutdown_seen) {
        ok = ok && recorder->CanDetach();
      }
      if (!ok) {
        observer_ok.store(false, std::memory_order_relaxed);
      }
    }
  });
  emitter.join();
  observer.join();

  REQUIRE(observer_ok.load());
  REQUIRE(polls.load() > 0);
  REQUIRE_FALSE(driver.rejected());
  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.shutdown_seen);
  REQUIRE_FALSE(snapshot.armed);
  REQUIRE(snapshot.delivered_count == kEventCount + 1);
  REQUIRE(snapshot.events.size() == kEventCount + 1);
  RequireContiguous(snapshot.events, 1);
  REQUIRE(snapshot.events.back().kind == Kind::kShutdown);
  REQUIRE(recorder->CanDetach());
  REQUIRE_FALSE(driver.Emit({Kind::kDispatch, kParticipantA, 0}));
}

TEST_CASE("Guest scheduler capture recorder records safepoint episodes",
          "[guest_scheduler_capture]") {
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(64);
  REQUIRE(recorder->Arm());
  FakeSchedulerDriver driver(recorder);

  // A lock episode nested in an IRQL episode: one opening event each, then
  // the terminal outcome carries every decline.
  REQUIRE(driver.Emit({Kind::kSafepoint, kParticipantA, 0, -1,
                       Reason::kDeferredIrql,
                       kGuestSchedulerCaptureFlagSchedulerRequested, 8, 2}));
  REQUIRE(driver.Emit({Kind::kSafepoint, kParticipantA, 0, -1,
                       Reason::kDeferredLock,
                       kGuestSchedulerCaptureFlagSchedulerRequested, 8, 2}));
  REQUIRE(driver.Emit(
      {Kind::kSafepoint, kParticipantA, 0, -1, Reason::kForcedIrql,
       kGuestSchedulerCaptureFlagSchedulerRequested, 8, 2, 4096 + 17}));

  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 3);
  RequireContiguous(snapshot.events, 1);
  REQUIRE(snapshot.events[0].count == 0);
  REQUIRE(snapshot.events[1].count == 0);
  REQUIRE(snapshot.events[2].reason == Reason::kForcedIrql);
  REQUIRE(snapshot.events[2].count == 4096 + 17);
  REQUIRE(snapshot.events[2].value == 2);
}

TEST_CASE("Guest scheduler attaches one capture observer before dispatch",
          "[guest_scheduler_capture]") {
  // A never-started scheduler exercises attachment, detachment and the
  // shutdown release without any fiber, KernelState or title.
  GuestScheduler scheduler(nullptr);
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(8);
  auto other = std::make_shared<GuestSchedulerCaptureEventRecorder>(8);

  REQUIRE_FALSE(scheduler.AttachCaptureObserver(nullptr));
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE_FALSE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE_FALSE(scheduler.AttachCaptureObserver(other));
  REQUIRE_FALSE(scheduler.DetachCaptureObserver(other));
  REQUIRE_FALSE(scheduler.DetachCaptureObserver(nullptr));
  REQUIRE(scheduler.DetachCaptureObserver(recorder));
  REQUIRE_FALSE(scheduler.DetachCaptureObserver(recorder));

  // An armed observer refuses both attachment and detachment.
  REQUIRE(other->Arm());
  REQUIRE_FALSE(scheduler.AttachCaptureObserver(other));
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());
  REQUIRE_FALSE(scheduler.DetachCaptureObserver(recorder));

  scheduler.Shutdown();
  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.shutdown_seen);
  REQUIRE(snapshot.delivered_count == 1);
  REQUIRE(snapshot.events.size() == 1);
  REQUIRE(snapshot.events[0].kind == Kind::kShutdown);
  REQUIRE(snapshot.events[0].sequence == 1);
  REQUIRE(snapshot.events[0].capture_instance_id == 0);
  REQUIRE(recorder->CanDetach());

  // Attachment is closed for the rest of the scheduler's lifetime.
  REQUIRE_FALSE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(other->Disarm());
  REQUIRE_FALSE(scheduler.AttachCaptureObserver(other));
  scheduler.Shutdown();
  REQUIRE(recorder->snapshot().delivered_count == 1);
  REQUIRE_FALSE(scheduler.capture_rejected());
}

TEST_CASE("Guest scheduler latches a rejecting capture observer",
          "[guest_scheduler_capture]") {
  GuestScheduler scheduler(nullptr);
  // Zero capacity: the first recorded event overflows.
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(0);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());
  REQUIRE_FALSE(scheduler.capture_rejected());

  scheduler.Shutdown();
  REQUIRE(scheduler.capture_rejected());
  auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == Rejection::kOverflow);
  REQUIRE(snapshot.delivered_count == 1);
  REQUIRE(snapshot.events.empty());
  REQUIRE_FALSE(scheduler.AttachCaptureObserver(recorder));
}

}  // namespace testing
}  // namespace kernel
}  // namespace xe

#endif
