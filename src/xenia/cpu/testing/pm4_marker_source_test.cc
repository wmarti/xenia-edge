/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"
#include "xenia/cpu/guest_execution_marker_controller.h"
#include "xenia/gpu/pm4_marker_sink.h"

#include "third_party/catch/include/catch.hpp"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xe {
namespace gpu {

class Pm4MarkerDispatcherTestAccess {
 public:
  static void SetPostOrdinalAssignmentHook(Pm4MarkerDispatcher& dispatcher,
                                           void (*hook)(void*), void* context) {
    dispatcher.SetPostOrdinalAssignmentHookForTesting(hook, context);
  }
  static void SetPostTicketAssignmentHook(Pm4MarkerDispatcher& dispatcher,
                                          void (*hook)(void*, uint64_t),
                                          void* context) {
    dispatcher.SetPostTicketAssignmentHookForTesting(hook, context);
  }
};

namespace testing {
namespace {

using namespace std::chrono_literals;

class RecordingSink final : public Pm4MarkerSink {
 public:
  bool OnPm4Marker(const Pm4MarkerEvent& event) noexcept override {
    events.push_back(event);
    return accept;
  }

  void OnPm4MarkerSourceShutdown() noexcept override { ++shutdown_count; }

  std::vector<Pm4MarkerEvent> events;
  bool accept = true;
  uint32_t shutdown_count = 0;
};

class ReentrantDetachSink final
    : public Pm4MarkerSink,
      public std::enable_shared_from_this<ReentrantDetachSink> {
 public:
  explicit ReentrantDetachSink(Pm4MarkerDispatcher& dispatcher)
      : dispatcher_(dispatcher) {}

  bool OnPm4Marker(const Pm4MarkerEvent&) noexcept override {
    ++event_count;
    detach_result = dispatcher_.DetachSink(shared_from_this());
    return true;
  }

  void OnPm4MarkerSourceShutdown() noexcept override { ++shutdown_count; }

  Pm4MarkerDispatcher& dispatcher_;
  uint32_t event_count = 0;
  uint32_t shutdown_count = 0;
  bool detach_result = true;
};

class BlockingShutdownSink final : public Pm4MarkerSink {
 public:
  bool OnPm4Marker(const Pm4MarkerEvent&) noexcept override { return true; }

  void OnPm4MarkerSourceShutdown() noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return released_; });
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class AtomicMarkerClock final : public cpu::GuestExecutionMarkerClock {
 public:
  uint64_t NowTicks() const noexcept override { return now.load(); }

  std::atomic<uint64_t> now{0};
};

class ConcurrentBoundarySink final
    : public cpu::GuestExecutionMarkerBoundarySink {
 public:
  bool OnMarkerBoundary(
      const cpu::GuestExecutionMarkerBoundary& boundary) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (boundary_count_ == boundaries_.size()) {
      return false;
    }
    boundaries_[boundary_count_++] = boundary;
    condition_.notify_all();
    return true;
  }

  bool OnArmedMarker(const Pm4MarkerEvent& event) noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (armed_marker_count_ == armed_markers_.size()) {
      return false;
    }
    armed_markers_[armed_marker_count_++] = event;
    armed_marker_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return !block_armed_marker_; });
    return true;
  }

  void BlockArmedMarker() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_armed_marker_ = true;
    armed_marker_entered_ = false;
  }

  bool WaitForArmedMarker(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this]() { return armed_marker_entered_; });
  }

  void ReleaseArmedMarker() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_armed_marker_ = false;
    condition_.notify_all();
  }

  size_t boundary_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return boundary_count_;
  }

  size_t armed_marker_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_marker_count_;
  }

  cpu::GuestExecutionMarkerBoundary boundary(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return boundaries_[index];
  }

  Pm4MarkerEvent armed_marker(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_markers_[index];
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::array<cpu::GuestExecutionMarkerBoundary, 4> boundaries_{};
  std::array<Pm4MarkerEvent, 8> armed_markers_{};
  size_t boundary_count_ = 0;
  size_t armed_marker_count_ = 0;
  bool block_armed_marker_ = false;
  bool armed_marker_entered_ = false;
};

class OrdinalAssignmentBlocker {
 public:
  static void Hook(void* context) {
    auto& self = *static_cast<OrdinalAssignmentBlocker*>(context);
    std::unique_lock<std::mutex> lock(self.mutex_);
    self.entered_ = true;
    self.condition_.notify_all();
    self.condition_.wait(lock, [&self]() { return self.released_; });
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class TicketAssignmentBlocker {
 public:
  explicit TicketAssignmentBlocker(uint64_t target_ticket)
      : target_ticket_(target_ticket) {}

  static void Hook(void* context, uint64_t ticket) {
    auto& self = *static_cast<TicketAssignmentBlocker*>(context);
    std::unique_lock<std::mutex> lock(self.mutex_);
    self.seen_ = true;
    self.last_ticket_seen_ = std::max(self.last_ticket_seen_, ticket);
    self.condition_.notify_all();
    if (ticket != self.target_ticket_) {
      return;
    }
    self.entered_ = true;
    self.condition_.notify_all();
    self.condition_.wait(lock, [&self]() { return self.released_; });
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  bool WaitUntilSeen(uint64_t ticket, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this, ticket]() {
      return seen_ && last_ticket_seen_ >= ticket;
    });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  const uint64_t target_ticket_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
  uint64_t last_ticket_seen_ = 0;
  bool seen_ = false;
};

struct RealControllerHarness {
  explicit RealControllerHarness(uint64_t stop_marker_count) {
    cpu::GuestExecutionMarkerControllerConfig config;
    config.warmup_ticks = 1;
    config.stop_marker_count = stop_marker_count;
    std::string error;
    controller = cpu::GuestExecutionMarkerController::Create(
        config, clock, boundary_sink, &error);
    REQUIRE(controller);
    REQUIRE(error.empty());
    REQUIRE(dispatcher.AttachSink(controller));
  }

  void BeginWarmup() {
    clock.now.store(10);
    REQUIRE(controller->Begin());
    dispatcher.NotifyPm4Swap(100);
    REQUIRE(controller->status().state ==
            cpu::GuestExecutionMarkerControllerState::kWarming);
  }

  void Arm() {
    BeginWarmup();
    clock.now.store(11);
    dispatcher.NotifyPm4Swap(110);
    REQUIRE(controller->status().state ==
            cpu::GuestExecutionMarkerControllerState::kArmed);
    REQUIRE(boundary_sink.boundary_count() == 1);
  }

  Pm4MarkerHoldToken HoldAcknowledgeAndResume() {
    Arm();
    Pm4MarkerHoldToken token;
    REQUIRE(dispatcher.HoldSink(controller, &token));
    REQUIRE(dispatcher.status().sink_held);
    REQUIRE(
        controller->AcknowledgeBoundary(boundary_sink.boundary(0).sequence));
    // The acknowledgement happens while dispatcher admission is still held.
    REQUIRE(dispatcher.status().sink_held);
    REQUIRE(dispatcher.ResumeSink(controller, token));
    REQUIRE(dispatcher.status().sink_attached);
    return token;
  }

  Pm4MarkerDispatcher dispatcher;
  AtomicMarkerClock clock;
  ConcurrentBoundarySink boundary_sink;
  std::shared_ptr<cpu::GuestExecutionMarkerController> controller;
};

}  // namespace

TEST_CASE("PM4 marker source keeps source-local ordinals",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  REQUIRE(dispatcher.marker_count() == 0);

  dispatcher.NotifyPm4Swap(10);
  REQUIRE(dispatcher.marker_count() == 1);

  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  dispatcher.NotifyPm4Swap(20);
  dispatcher.NotifyPm4Swap(30);

  REQUIRE(dispatcher.marker_count() == 3);
  REQUIRE(sink->events.size() == 2);
  const Pm4MarkerEvent expected_first{Pm4MarkerSource::kPm4Swap,
                                      kPm4SwapMarkerOpcode, 2, 20, 1};
  const Pm4MarkerEvent expected_second{Pm4MarkerSource::kPm4Swap,
                                       kPm4SwapMarkerOpcode, 3, 30, 1};
  REQUIRE(sink->events[0] == expected_first);
  REQUIRE(sink->events[1] == expected_second);
  REQUIRE_FALSE(dispatcher.sink_failed());

  REQUIRE(dispatcher.DetachSink(sink));
  dispatcher.NotifyPm4Swap(40);
  REQUIRE(dispatcher.marker_count() == 4);
  REQUIRE(sink->events.size() == 2);
}

TEST_CASE("PM4 marker source owns one sink and shuts it down once",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto first = std::make_shared<RecordingSink>();
  auto second = std::make_shared<RecordingSink>();

  REQUIRE_FALSE(dispatcher.AttachSink({}));
  REQUIRE(dispatcher.AttachSink(first));
  REQUIRE_FALSE(dispatcher.AttachSink(second));
  REQUIRE_FALSE(dispatcher.DetachSink(second));
  REQUIRE(dispatcher.DetachSink(first));
  REQUIRE(dispatcher.AttachSink(second));

  dispatcher.Shutdown();
  dispatcher.Shutdown();
  REQUIRE(second->shutdown_count == 1);
  REQUIRE_FALSE(dispatcher.AttachSink(first));
  REQUIRE_FALSE(dispatcher.DetachSink(second));

  dispatcher.NotifyPm4Swap(50);
  REQUIRE(dispatcher.marker_count() == 1);
  REQUIRE(second->events.empty());
}

TEST_CASE("PM4 marker source fails delivery closed", "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  sink->accept = false;
  REQUIRE(dispatcher.AttachSink(sink));

  dispatcher.NotifyPm4Swap(60);
  REQUIRE(dispatcher.sink_failed());
  REQUIRE(sink->events.size() == 1);

  dispatcher.NotifyPm4Swap(70);
  REQUIRE(dispatcher.marker_count() == 2);
  REQUIRE(sink->events.size() == 1);

  REQUIRE(dispatcher.DetachSink(sink));
  REQUIRE_FALSE(dispatcher.AttachSink(std::make_shared<RecordingSink>()));
}

TEST_CASE("PM4 resume requires the exact hold attestation",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  auto other = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));

  Pm4MarkerHoldToken ignored;
  REQUIRE_FALSE(dispatcher.HoldSink(other, &ignored));
  REQUIRE(dispatcher.status().sink_attached);
  Pm4MarkerHoldToken token;
  REQUIRE(dispatcher.HoldSink(sink, &token));
  REQUIRE(dispatcher.status().sink_held);
  Pm4MarkerHoldToken wrong = token;
  ++wrong.last_ordinal;
  REQUIRE_FALSE(dispatcher.ResumeSink(sink, wrong));
  REQUIRE(dispatcher.status().sink_held);
  REQUIRE(dispatcher.ResumeSink(sink, token));
  REQUIRE(dispatcher.status().sink_attached);
  REQUIRE(dispatcher.status().sink_generation == 2);
  REQUIRE_FALSE(dispatcher.ResumeSink(sink, token));
  REQUIRE(dispatcher.status().sink_attached);
}

TEST_CASE("PM4 ticket service admits a marker after an overlapping attach",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  TicketAssignmentBlocker blocker(0);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);

  bool attach_result = false;
  std::thread attach([&]() { attach_result = dispatcher.AttachSink(sink); });
  const bool attach_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!attach_ticket_entered) {
    blocker.Release();
    attach.join();
  }
  REQUIRE(attach_ticket_entered);

  std::thread producer([&]() { dispatcher.NotifyPm4Swap(10); });
  const bool marker_ticket_seen = blocker.WaitUntilSeen(1, 2s);
  if (!marker_ticket_seen) {
    blocker.Release();
    attach.join();
    producer.join();
  }
  REQUIRE(marker_ticket_seen);
  blocker.Release();
  attach.join();
  producer.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE(attach_result);
  REQUIRE_FALSE(dispatcher.sink_failed());
  REQUIRE(sink->events.size() == 1);
  REQUIRE(sink->events[0].ordinal == 1);
  REQUIRE(sink->events[0].sink_generation == 1);
}

TEST_CASE("PM4 ticket service admits a marker after an overlapping resume",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  Pm4MarkerHoldToken token;
  REQUIRE(dispatcher.HoldSink(sink, &token));

  TicketAssignmentBlocker blocker(2);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);
  bool resume_result = false;
  std::thread resume(
      [&]() { resume_result = dispatcher.ResumeSink(sink, token); });
  const bool resume_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!resume_ticket_entered) {
    blocker.Release();
    resume.join();
  }
  REQUIRE(resume_ticket_entered);

  std::thread producer([&]() { dispatcher.NotifyPm4Swap(20); });
  const bool marker_ticket_seen = blocker.WaitUntilSeen(3, 2s);
  if (!marker_ticket_seen) {
    blocker.Release();
    resume.join();
    producer.join();
  }
  REQUIRE(marker_ticket_seen);
  blocker.Release();
  resume.join();
  producer.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE(resume_result);
  REQUIRE_FALSE(dispatcher.sink_failed());
  REQUIRE(sink->events.size() == 1);
  REQUIRE(sink->events[0].ordinal == 1);
  REQUIRE(sink->events[0].sink_generation == 2);
}

TEST_CASE("PM4 semantic occurrence lease cannot be overtaken by hold",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));

  TicketAssignmentBlocker blocker(1);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);
  std::atomic<bool> lease_acquired{false};
  std::atomic<bool> complete_lease{false};
  std::thread producer([&]() {
    auto lease = dispatcher.BeginPm4Swap();
    lease_acquired.store(true, std::memory_order_release);
    while (!complete_lease.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    dispatcher.CompletePm4Swap(std::move(lease), 30);
  });
  const bool marker_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!marker_ticket_entered) {
    blocker.Release();
    complete_lease.store(true, std::memory_order_release);
    producer.join();
  }
  REQUIRE(marker_ticket_entered);

  Pm4MarkerHoldToken token;
  std::atomic<bool> hold_returned{false};
  bool hold_result = false;
  std::thread holder([&]() {
    hold_result = dispatcher.HoldSink(sink, &token);
    hold_returned.store(true, std::memory_order_release);
  });
  const bool hold_ticket_seen = blocker.WaitUntilSeen(2, 2s);
  if (!hold_ticket_seen) {
    blocker.Release();
    complete_lease.store(true, std::memory_order_release);
    producer.join();
    holder.join();
  }
  REQUIRE(hold_ticket_seen);
  blocker.Release();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!lease_acquired.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool acquired_before_timeout =
      lease_acquired.load(std::memory_order_acquire);
  if (!acquired_before_timeout) {
    complete_lease.store(true, std::memory_order_release);
    producer.join();
    holder.join();
  }
  REQUIRE(acquired_before_timeout);
  REQUIRE_FALSE(hold_returned.load(std::memory_order_acquire));
  complete_lease.store(true, std::memory_order_release);
  producer.join();
  holder.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE(hold_result);
  REQUIRE(token.last_ordinal == 1);
  REQUIRE(sink->events.size() == 1);
  REQUIRE(sink->events[0].host_tick == 30);
  REQUIRE_FALSE(dispatcher.sink_failed());
}

TEST_CASE("PM4 nested semantic occurrence fails closed without deadlocking",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));

  auto outer = dispatcher.BeginPm4Swap();
  auto nested = dispatcher.BeginPm4Swap();
  dispatcher.CompletePm4Swap(std::move(nested), 35);
  dispatcher.CompletePm4Swap(std::move(outer), 36);

  REQUIRE(dispatcher.marker_count() == 2);
  REQUIRE(dispatcher.sink_failed());
  REQUIRE(sink->events.size() == 1);
  REQUIRE(sink->events[0].ordinal == 1);
  REQUIRE(sink->events[0].host_tick == 36);
}

TEST_CASE("PM4 marker source rejects detach from its callback",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<ReentrantDetachSink>(dispatcher);
  REQUIRE(dispatcher.AttachSink(sink));

  dispatcher.NotifyPm4Swap(80);
  REQUIRE(sink->event_count == 1);
  REQUIRE_FALSE(sink->detach_result);
  REQUIRE(dispatcher.DetachSink(sink));
  dispatcher.Shutdown();
  REQUIRE(sink->shutdown_count == 0);
}

TEST_CASE("PM4 hold drains a pre-hold ordinal before a new generation",
          "[pm4-marker-source][concurrency]") {
  RealControllerHarness harness(100);
  harness.BeginWarmup();
  harness.clock.now.store(11);

  OrdinalAssignmentBlocker blocker;
  Pm4MarkerDispatcherTestAccess::SetPostOrdinalAssignmentHook(
      harness.dispatcher, &OrdinalAssignmentBlocker::Hook, &blocker);
  std::thread producer([&harness]() { harness.dispatcher.NotifyPm4Swap(110); });
  const bool ordinal_hook_entered = blocker.WaitUntilEntered(2s);
  if (!ordinal_hook_entered) {
    blocker.Release();
    producer.join();
    Pm4MarkerDispatcherTestAccess::SetPostOrdinalAssignmentHook(
        harness.dispatcher, nullptr, nullptr);
  }
  REQUIRE(ordinal_hook_entered);

  Pm4MarkerHoldToken token;
  std::atomic<bool> hold_started{false};
  std::atomic<bool> hold_returned{false};
  bool hold_result = false;
  std::thread holder([&]() {
    hold_started.store(true);
    hold_result = harness.dispatcher.HoldSink(harness.controller, &token);
    hold_returned.store(true);
  });
  while (!hold_started.load()) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(10ms);
  const bool returned_before_ordinal_release = hold_returned.load();
  blocker.Release();
  producer.join();
  holder.join();
  Pm4MarkerDispatcherTestAccess::SetPostOrdinalAssignmentHook(
      harness.dispatcher, nullptr, nullptr);

  REQUIRE_FALSE(returned_before_ordinal_release);
  REQUIRE(hold_result);
  const Pm4MarkerHoldToken expected_start_token{1, 1, 2};
  REQUIRE(token == expected_start_token);
  REQUIRE(harness.controller->status().state ==
          cpu::GuestExecutionMarkerControllerState::kArmed);
  REQUIRE(harness.boundary_sink.boundary_count() == 1);
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE(harness.dispatcher.status().sink_held);
  REQUIRE(harness.dispatcher.ResumeSink(harness.controller, token));
  REQUIRE(harness.dispatcher.status().sink_generation == 2);

  harness.clock.now.store(12);
  harness.dispatcher.NotifyPm4Swap(120);
  REQUIRE(harness.boundary_sink.armed_marker_count() == 1);
  const Pm4MarkerEvent resumed = harness.boundary_sink.armed_marker(0);
  REQUIRE(resumed.ordinal == 3);
  REQUIRE(resumed.sink_generation == 2);
}

TEST_CASE("PM4 production while held permanently prevents resume",
          "[pm4-marker-source][concurrency]") {
  RealControllerHarness harness(100);
  harness.Arm();
  Pm4MarkerHoldToken token;
  REQUIRE(harness.dispatcher.HoldSink(harness.controller, &token));
  Pm4MarkerHoldToken repeated;
  REQUIRE(harness.dispatcher.HoldSink(harness.controller, &repeated));
  REQUIRE(repeated == token);

  harness.dispatcher.NotifyPm4Swap(120);
  const Pm4MarkerDispatcherStatus failed = harness.dispatcher.status();
  REQUIRE(failed.marker_count == 3);
  REQUIRE(failed.sink_held);
  REQUIRE(failed.sink_failed);
  REQUIRE(failed.source_advanced_while_held);
  REQUIRE(harness.boundary_sink.armed_marker_count() == 0);
  REQUIRE_FALSE(harness.dispatcher.HoldSink(harness.controller, &repeated));
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE_FALSE(harness.dispatcher.ResumeSink(harness.controller, token));

  harness.dispatcher.NotifyPm4Swap(130);
  REQUIRE(harness.dispatcher.marker_count() == 4);
  REQUIRE(harness.dispatcher.status().sink_held);
  REQUIRE(harness.boundary_sink.armed_marker_count() == 0);
}

TEST_CASE("PM4 terminal seal orders the next swap outside the session",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  Pm4MarkerHoldToken token;
  REQUIRE(dispatcher.HoldSink(sink, &token));

  TicketAssignmentBlocker blocker(2);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);
  bool seal_result = false;
  std::thread sealer(
      [&]() { seal_result = dispatcher.SealAndDetachHeldSink(sink, token); });
  const bool seal_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!seal_ticket_entered) {
    blocker.Release();
    sealer.join();
  }
  REQUIRE(seal_ticket_entered);

  std::thread producer([&]() { dispatcher.NotifyPm4Swap(40); });
  const bool marker_ticket_seen = blocker.WaitUntilSeen(3, 2s);
  if (!marker_ticket_seen) {
    blocker.Release();
    sealer.join();
    producer.join();
  }
  REQUIRE(marker_ticket_seen);
  blocker.Release();
  sealer.join();
  producer.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE(seal_result);
  REQUIRE(dispatcher.marker_count() == 1);
  REQUIRE_FALSE(dispatcher.sink_failed());
  REQUIRE_FALSE(dispatcher.status().sink_attached);
  REQUIRE_FALSE(dispatcher.status().sink_held);
  REQUIRE(sink->events.empty());
}

TEST_CASE("PM4 swap ordered before terminal seal rejects publication",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  Pm4MarkerHoldToken token;
  REQUIRE(dispatcher.HoldSink(sink, &token));

  TicketAssignmentBlocker blocker(2);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);
  std::thread producer([&]() { dispatcher.NotifyPm4Swap(50); });
  const bool marker_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!marker_ticket_entered) {
    blocker.Release();
    producer.join();
  }
  REQUIRE(marker_ticket_entered);

  bool seal_result = true;
  std::thread sealer(
      [&]() { seal_result = dispatcher.SealAndDetachHeldSink(sink, token); });
  const bool seal_ticket_seen = blocker.WaitUntilSeen(3, 2s);
  if (!seal_ticket_seen) {
    blocker.Release();
    producer.join();
    sealer.join();
  }
  REQUIRE(seal_ticket_seen);
  blocker.Release();
  producer.join();
  sealer.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE_FALSE(seal_result);
  const auto status = dispatcher.status();
  REQUIRE(status.sink_held);
  REQUIRE(status.sink_failed);
  REQUIRE(status.source_advanced_while_held);
  REQUIRE(status.marker_count == 1);
  REQUIRE(sink->events.empty());
}

TEST_CASE("PM4 terminal seal fences source shutdown",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  Pm4MarkerHoldToken token;
  REQUIRE(dispatcher.HoldSink(sink, &token));

  TicketAssignmentBlocker blocker(2);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);
  bool seal_result = false;
  std::thread sealer(
      [&]() { seal_result = dispatcher.SealAndDetachHeldSink(sink, token); });
  const bool seal_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!seal_ticket_entered) {
    blocker.Release();
    sealer.join();
  }
  REQUIRE(seal_ticket_entered);

  std::thread shutdown([&]() { dispatcher.Shutdown(); });
  const bool shutdown_ticket_seen = blocker.WaitUntilSeen(3, 2s);
  if (!shutdown_ticket_seen) {
    blocker.Release();
    sealer.join();
    shutdown.join();
  }
  REQUIRE(shutdown_ticket_seen);
  blocker.Release();
  sealer.join();
  shutdown.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE(seal_result);
  REQUIRE(dispatcher.status().shut_down);
  REQUIRE(sink->shutdown_count == 0);
  REQUIRE_FALSE(dispatcher.sink_failed());
}

TEST_CASE("PM4 source shutdown callback drains before terminal seal fails",
          "[pm4-marker-source][concurrency]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<BlockingShutdownSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  Pm4MarkerHoldToken token;
  REQUIRE(dispatcher.HoldSink(sink, &token));

  std::thread shutdown([&]() { dispatcher.Shutdown(); });
  const bool shutdown_entered = sink->WaitUntilEntered(2s);
  if (!shutdown_entered) {
    sink->Release();
    shutdown.join();
  }
  REQUIRE(shutdown_entered);

  TicketAssignmentBlocker blocker(3);
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(
      dispatcher, &TicketAssignmentBlocker::Hook, &blocker);
  std::atomic<bool> seal_returned{false};
  bool seal_result = true;
  std::thread sealer([&]() {
    seal_result = dispatcher.SealAndDetachHeldSink(sink, token);
    seal_returned.store(true, std::memory_order_release);
  });
  const bool seal_ticket_entered = blocker.WaitUntilEntered(2s);
  if (!seal_ticket_entered) {
    blocker.Release();
    sink->Release();
    shutdown.join();
    sealer.join();
  }
  REQUIRE(seal_ticket_entered);
  blocker.Release();
  std::this_thread::sleep_for(10ms);
  const bool seal_returned_before_shutdown_release =
      seal_returned.load(std::memory_order_acquire);
  sink->Release();
  shutdown.join();
  sealer.join();
  Pm4MarkerDispatcherTestAccess::SetPostTicketAssignmentHook(dispatcher,
                                                             nullptr, nullptr);

  REQUIRE_FALSE(seal_returned_before_shutdown_release);
  REQUIRE_FALSE(seal_result);
  REQUIRE(dispatcher.status().shut_down);
  REQUIRE_FALSE(dispatcher.status().sink_attached);
  REQUIRE_FALSE(dispatcher.status().sink_held);
}

TEST_CASE("PM4 hold drains the callback and its stop boundary",
          "[pm4-marker-source][concurrency]") {
  RealControllerHarness harness(1);
  harness.HoldAcknowledgeAndResume();
  harness.boundary_sink.BlockArmedMarker();
  harness.clock.now.store(12);

  std::thread producer([&harness]() { harness.dispatcher.NotifyPm4Swap(120); });
  const bool armed_marker_entered =
      harness.boundary_sink.WaitForArmedMarker(2s);
  if (!armed_marker_entered) {
    harness.boundary_sink.ReleaseArmedMarker();
    producer.join();
  }
  REQUIRE(armed_marker_entered);
  Pm4MarkerHoldToken stop_token;
  std::atomic<bool> hold_returned{false};
  bool hold_result = false;
  std::thread holder([&]() {
    hold_result = harness.dispatcher.HoldSink(harness.controller, &stop_token);
    hold_returned.store(true);
  });
  std::this_thread::sleep_for(10ms);
  const bool returned_before_callback_release = hold_returned.load();
  harness.boundary_sink.ReleaseArmedMarker();
  producer.join();
  holder.join();

  REQUIRE_FALSE(returned_before_callback_release);
  REQUIRE(hold_result);
  const Pm4MarkerHoldToken expected_stop_token{2, 2, 3};
  REQUIRE(stop_token == expected_stop_token);
  REQUIRE(harness.boundary_sink.armed_marker_count() == 1);
  REQUIRE(harness.boundary_sink.boundary_count() == 2);
  REQUIRE(harness.boundary_sink.boundary(1).kind ==
          cpu::GuestExecutionMarkerBoundaryKind::kStop);
  REQUIRE(harness.controller->status().state ==
          cpu::GuestExecutionMarkerControllerState::kStopped);
  REQUIRE(harness.dispatcher.status().sink_held);
}

TEST_CASE("PM4 shutdown rejects an attached armed controller",
          "[pm4-marker-source]") {
  RealControllerHarness harness(100);
  harness.HoldAcknowledgeAndResume();
  harness.dispatcher.Shutdown();

  const auto controller_status = harness.controller->status();
  REQUIRE(controller_status.state ==
          cpu::GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(controller_status.rejection ==
          cpu::GuestExecutionMarkerControllerRejection::kMarkerSourceLost);
  REQUIRE(harness.dispatcher.status().shut_down);
  REQUIRE_FALSE(harness.dispatcher.status().sink_attached);
}

}  // namespace testing
}  // namespace gpu
}  // namespace xe

#else

TEST_CASE("PM4 marker source compiles out of normal builds",
          "[pm4-marker-source]") {
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE)
  REQUIRE(XE_ENABLE_GUEST_INVOCATION_CAPTURE == 0);
#else
  SUCCEED();
#endif
}

#endif
