/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/guest_invocation_capture_poller.h"

namespace xe {
namespace cpu {
namespace test {

namespace {

constexpr uint32_t kRootAddress = 0x82040000u;
constexpr uint32_t kRootEndAddress = kRootAddress + 0xFC;
constexpr uint32_t kReturnAddress = 0x83000000u;
constexpr uint32_t kDataPage = 0x10000000u;
constexpr ppc::GuestInvocationRecorderIdentity kOwner = {0x1111, 0x2222};

class FakeClock final : public ppc::GuestInvocationRecorderClock {
 public:
  uint64_t NowTicks() const override {
    call_count.fetch_add(1, std::memory_order_relaxed);
    return now.load(std::memory_order_relaxed);
  }
  std::atomic<uint64_t> now{100};
  mutable std::atomic<uint64_t> call_count{0};
};

class FakePageReader final : public ppc::GuestInvocationRecorderPageReader {
 public:
  bool ReadPage(uint32_t page_address,
                std::array<uint8_t, 4096>* output) override {
    const auto it = pages.find(page_address);
    if (it == pages.cend()) {
      return false;
    }
    *output = it->second;
    return true;
  }

  std::map<uint32_t, std::array<uint8_t, 4096>> pages;
};

ppc::GuestInvocationRecorderSelection MakeSelection() {
  ppc::GuestInvocationRecorderSelection selection;
  selection.root_address = kRootAddress;
  selection.root_end_address = kRootEndAddress;
  selection.occurrence = 1;
  return selection;
}

ppc::GuestInvocationRecorderLimits MakeLimits() {
  ppc::GuestInvocationRecorderLimits limits;
  limits.max_attempts = 8;
  limits.max_duration_ticks = 1000;
  limits.max_page_count = 16;
  limits.max_access_count = 128;
  limits.max_call_depth = 8;
  limits.max_event_count = 1024;
  limits.max_function_count = 16;
  return limits;
}

ppc::GuestPPCRegisterState MakeState(uint64_t seed) {
  ppc::GuestPPCRegisterState state;
  state.link_register = kReturnAddress;
  state.gpr[3] = seed;
  return state;
}

void Attempt(GuestInvocationCaptureCoordinator& coordinator, uint64_t seed) {
  REQUIRE(coordinator.OnFunctionEntry(kOwner, kRootAddress, kRootEndAddress,
                                      MakeState(seed)));
  REQUIRE(coordinator.OnMemoryAccess(
      kOwner, kDataPage + 16, 8,
      ppc::GuestInvocationRecorderMemoryAccess::kReadWrite));
  REQUIRE(coordinator.OnFunctionExit(kOwner, kRootAddress, kReturnAddress,
                                     MakeState(seed + 1)));
}

std::unique_ptr<GuestInvocationCaptureCoordinator> MakeCoordinator(
    FakePageReader& reader, ppc::GuestInvocationRecorderClock& clock,
    GuestInvocationCaptureCoordinator::SegmentHandler handler) {
  std::string error;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      GuestInvocationCaptureCoordinator::Create(7, MakeSelection(),
                                                MakeLimits(), reader, clock,
                                                std::move(handler), &error);
  REQUIRE(coordinator);
  REQUIRE(error.empty());
  REQUIRE(coordinator->OnFunctionDefined(kRootAddress, kRootEndAddress));
  return coordinator;
}

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

class BlockingClock final : public ppc::GuestInvocationRecorderClock {
 public:
  uint64_t NowTicks() const override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++call_count_;
    if (block_) {
      blocked_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this] { return released_; });
    }
    return now_;
  }

  void BeginBlocking() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_ = true;
    blocked_ = false;
    released_ = false;
  }

  bool WaitUntilBlocked() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [this] { return blocked_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

  uint64_t call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return call_count_;
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable uint64_t call_count_ = 0;
  uint64_t now_ = 100;
  bool block_ = false;
  mutable bool blocked_ = false;
  mutable bool released_ = false;
};

#endif

void CompleteCapture(GuestInvocationCaptureCoordinator& coordinator,
                     FakePageReader& reader) {
  Attempt(coordinator, 10);
  Attempt(coordinator, 20);

  REQUIRE(coordinator.OnFunctionEntry(kOwner, kRootAddress, kRootEndAddress,
                                      MakeState(30)));
  REQUIRE(coordinator.OnMemoryAccess(
      kOwner, kDataPage + 16, 8,
      ppc::GuestInvocationRecorderMemoryAccess::kReadWrite));
  reader.pages[kDataPage][42] ^= 0xFF;
  coordinator.OnFunctionExit(kOwner, kRootAddress, kReturnAddress,
                             MakeState(31));
}

}  // namespace

TEST_CASE("guest invocation capture publishes one ordered segment",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  reader.pages[kDataPage][42] = 0x12;
  FakeClock clock;
  std::optional<ppc::GuestInvocationRecorderResult> published_result;
  uint64_t published_ordinal = 0;
  uint64_t published_start = 0;
  uint64_t published_end = 0;
  uint32_t handler_count = 0;

  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      MakeCoordinator(
          reader, clock,
          [&](uint64_t ordinal, uint64_t start_tick, uint64_t end_tick,
              const ppc::GuestInvocationRecorderResult& result, std::string*) {
            ++handler_count;
            published_ordinal = ordinal;
            published_start = start_tick;
            published_end = end_tick;
            published_result = result;
            return true;
          });
  clock.now = 200;
  CompleteCapture(*coordinator, reader);

  REQUIRE(handler_count == 1);
  REQUIRE(published_result);
  REQUIRE(published_result->attempt_count == 3);
  REQUIRE(published_ordinal == 7);
  REQUIRE(published_start == 100);
  REQUIRE(published_end == 200);
  const GuestInvocationCaptureStatus status = coordinator->status();
  REQUIRE(status.state == GuestInvocationCaptureState::kPublished);
  REQUIRE(status.recorder_state ==
          ppc::GuestInvocationRecorderState::kComplete);
  REQUIRE(status.segment_ordinal == 7);
  REQUIRE(status.accepted_segment_count == 1);
  REQUIRE(status.rejected_segment_count == 0);
  REQUIRE(status.capture_start_tick == 100);
  REQUIRE(status.capture_end_tick == 200);
  REQUIRE(status.message.empty());

  REQUIRE(coordinator->Poll());
  REQUIRE(handler_count == 1);
}

TEST_CASE("guest invocation capture fails closed on publication failure",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  reader.pages[kDataPage] = {};
  FakeClock clock;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      MakeCoordinator(
          reader, clock,
          [](uint64_t, uint64_t, uint64_t,
             const ppc::GuestInvocationRecorderResult&, std::string* error) {
            error->assign("injected publication failure");
            return false;
          });
  CompleteCapture(*coordinator, reader);

  const GuestInvocationCaptureStatus status = coordinator->status();
  REQUIRE(status.state == GuestInvocationCaptureState::kPublicationFailed);
  REQUIRE(status.accepted_segment_count == 0);
  REQUIRE(status.rejected_segment_count == 1);
  REQUIRE(status.message == "injected publication failure");
  REQUIRE_FALSE(coordinator->Poll());
}

TEST_CASE("guest invocation capture surfaces rejection and incomplete stop",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  reader.pages[kDataPage] = {};
  FakeClock clock;

  SECTION("recorder rejection") {
    std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
        MakeCoordinator(reader, clock,
                        [](uint64_t, uint64_t, uint64_t,
                           const ppc::GuestInvocationRecorderResult&,
                           std::string*) { return true; });
    REQUIRE(coordinator->OnFunctionEntry(kOwner, kRootAddress, kRootEndAddress,
                                         MakeState(1)));
    REQUIRE_FALSE(coordinator->OnUnsupportedDependency(
        kOwner, ppc::kGuestInvocationDependencyMmio));
    const GuestInvocationCaptureStatus status = coordinator->status();
    REQUIRE(status.state == GuestInvocationCaptureState::kRejected);
    REQUIRE(status.rejection ==
            ppc::GuestInvocationRecorderRejection::kUnsupportedDependency);
    REQUIRE(status.rejected_dependency_flags ==
            ppc::kGuestInvocationDependencyMmio);
    REQUIRE(status.rejected_segment_count == 1);
    REQUIRE_FALSE(status.message.empty());
  }

  SECTION("incomplete stop") {
    std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
        MakeCoordinator(reader, clock,
                        [](uint64_t, uint64_t, uint64_t,
                           const ppc::GuestInvocationRecorderResult&,
                           std::string*) { return true; });
    clock.now = 250;
    coordinator->Stop();
    const GuestInvocationCaptureStatus status = coordinator->status();
    REQUIRE(status.state == GuestInvocationCaptureState::kStopped);
    REQUIRE(status.rejected_segment_count == 1);
    REQUIRE(status.capture_end_tick == 250);
    REQUIRE(status.message.find("before") != std::string::npos);
    REQUIRE_FALSE(coordinator->Poll());
  }
}

TEST_CASE("guest invocation capture requires a segment handler",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  FakeClock clock;
  std::string error;
  REQUIRE_FALSE(GuestInvocationCaptureCoordinator::Create(
      0, MakeSelection(), MakeLimits(), reader, clock, {}, &error));
  REQUIRE(error == "capture segment handler is missing");
}

TEST_CASE("guest invocation capture publication is reentrancy-safe",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  reader.pages[kDataPage] = {};
  FakeClock clock;
  GuestInvocationCaptureCoordinator* coordinator_pointer = nullptr;
  uint32_t handler_count = 0;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      MakeCoordinator(
          reader, clock,
          [&](uint64_t, uint64_t, uint64_t,
              const ppc::GuestInvocationRecorderResult&, std::string*) {
            ++handler_count;
            REQUIRE(coordinator_pointer);
            REQUIRE(coordinator_pointer->status().state ==
                    GuestInvocationCaptureState::kPublishing);
            REQUIRE_FALSE(coordinator_pointer->Poll());
            return true;
          });
  coordinator_pointer = coordinator.get();

  CompleteCapture(*coordinator, reader);

  REQUIRE(handler_count == 1);
  REQUIRE(coordinator->status().state ==
          GuestInvocationCaptureState::kPublished);
  REQUIRE(coordinator->Poll());
  REQUIRE(handler_count == 1);
}

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

TEST_CASE("guest invocation capture deadline poller rejects a quiet attempt",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  reader.pages[kDataPage] = {};
  FakeClock clock;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      MakeCoordinator(reader, clock,
                      [](uint64_t, uint64_t, uint64_t,
                         const ppc::GuestInvocationRecorderResult&,
                         std::string*) { return true; });
  REQUIRE(coordinator->OnFunctionEntry(kOwner, kRootAddress, kRootEndAddress,
                                       MakeState(1)));
  clock.now = 1100;

  std::string error;
  std::unique_ptr<GuestInvocationCaptureDeadlinePoller> poller =
      GuestInvocationCaptureDeadlinePoller::Create(
          *coordinator, std::chrono::milliseconds(1), &error);
  REQUIRE(poller);
  REQUIRE(error.empty());
  REQUIRE(WaitUntil([&] {
    return coordinator->status().state ==
           GuestInvocationCaptureState::kRejected;
  }));
  poller->StopAndJoin();

  const GuestInvocationCaptureStatus status = coordinator->status();
  REQUIRE(status.rejection ==
          ppc::GuestInvocationRecorderRejection::kDeadlineExceeded);
  REQUIRE(status.rejected_segment_count == 1);
  REQUIRE(status.capture_end_tick == 1100);
}

TEST_CASE("guest invocation capture deadline polling is owner bounded",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  FakeClock clock;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      MakeCoordinator(reader, clock,
                      [](uint64_t, uint64_t, uint64_t,
                         const ppc::GuestInvocationRecorderResult&,
                         std::string*) { return true; });
  const uint64_t calls_before_attach = clock.call_count.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(clock.call_count.load() == calls_before_attach);

  std::string error;
  std::unique_ptr<GuestInvocationCaptureDeadlinePoller> poller =
      GuestInvocationCaptureDeadlinePoller::Create(
          *coordinator, std::chrono::milliseconds(1), &error);
  REQUIRE(poller);
  REQUIRE(
      WaitUntil([&] { return clock.call_count.load() > calls_before_attach; }));
  poller->StopAndJoin();
  const uint64_t calls_after_detach = clock.call_count.load();

  clock.now = 1100;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(clock.call_count.load() == calls_after_detach);
  REQUIRE(coordinator->status().state ==
          GuestInvocationCaptureState::kRecording);
  REQUIRE_FALSE(coordinator->Poll());
  REQUIRE(coordinator->status().rejection ==
          ppc::GuestInvocationRecorderRejection::kDeadlineExceeded);
}

TEST_CASE("guest invocation capture deadline poller drains shutdown",
          "[guest-invocation-capture]") {
  FakePageReader reader;
  BlockingClock clock;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      MakeCoordinator(reader, clock,
                      [](uint64_t, uint64_t, uint64_t,
                         const ppc::GuestInvocationRecorderResult&,
                         std::string*) { return true; });
  clock.BeginBlocking();

  std::string error;
  std::unique_ptr<GuestInvocationCaptureDeadlinePoller> poller =
      GuestInvocationCaptureDeadlinePoller::Create(
          *coordinator, std::chrono::milliseconds(1), &error);
  REQUIRE(poller);
  REQUIRE(clock.WaitUntilBlocked());

  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool stop_started = false;
  bool stop_complete = false;
  std::thread stopper([&] {
    {
      std::lock_guard<std::mutex> lock(stop_mutex);
      stop_started = true;
    }
    stop_condition.notify_all();
    poller->StopAndJoin();
    {
      std::lock_guard<std::mutex> lock(stop_mutex);
      stop_complete = true;
    }
    stop_condition.notify_all();
  });
  bool stopped_before_release = false;
  {
    std::unique_lock<std::mutex> lock(stop_mutex);
    REQUIRE(stop_condition.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return stop_started; }));
    stopped_before_release = stop_condition.wait_for(
        lock, std::chrono::milliseconds(25), [&] { return stop_complete; });
  }

  clock.Release();
  stopper.join();
  REQUIRE_FALSE(stopped_before_release);
  REQUIRE(stop_complete);
  const uint64_t calls_after_join = clock.call_count();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(clock.call_count() == calls_after_join);
  REQUIRE(coordinator->status().state ==
          GuestInvocationCaptureState::kRecording);
}

#endif

}  // namespace test
}  // namespace cpu
}  // namespace xe
