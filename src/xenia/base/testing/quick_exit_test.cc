/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace testing {
namespace {

using namespace std::chrono_literals;

void CountPrepare(void* context) noexcept {
  static_cast<std::atomic<uint32_t>*>(context)->fetch_add(
      1, std::memory_order_relaxed);
}

class BlockingPrepare {
 public:
  static void Run(void* context) noexcept {
    auto& self = *static_cast<BlockingPrepare*>(context);
    std::unique_lock<std::mutex> lock(self.mutex_);
    self.entered_ = true;
    self.condition_.notify_all();
    self.condition_.wait(lock, [&self]() { return self.released_; });
  }

  bool WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 2s, [this]() { return entered_; });
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

}  // namespace

TEST_CASE("quick exit preparation supports an idempotent owner and relaunch",
          "[quick-exit]") {
  std::atomic<uint32_t> first_count{0};
  std::atomic<uint32_t> second_count{0};
  REQUIRE(RegisterQuickExitPrepareCallback(&CountPrepare, &first_count));
  REQUIRE(RegisterQuickExitPrepareCallback(&CountPrepare, &first_count));
  REQUIRE_FALSE(RegisterQuickExitPrepareCallback(&CountPrepare, &second_count));

  PrepareForQuickExit();
  PrepareForQuickExit();
  REQUIRE(first_count.load(std::memory_order_relaxed) == 2);
  REQUIRE(UnregisterQuickExitPrepareCallback(&CountPrepare, &first_count));
  PrepareForQuickExit();
  REQUIRE(first_count.load(std::memory_order_relaxed) == 2);

  REQUIRE(RegisterQuickExitPrepareCallback(&CountPrepare, &second_count));
  PrepareForQuickExit();
  REQUIRE(second_count.load(std::memory_order_relaxed) == 1);
  REQUIRE(UnregisterQuickExitPrepareCallback(&CountPrepare, &second_count));
}

TEST_CASE("quick exit owner removal drains an admitted callback",
          "[quick-exit][concurrency]") {
  BlockingPrepare prepare;
  REQUIRE(RegisterQuickExitPrepareCallback(&BlockingPrepare::Run, &prepare));

  std::thread caller([]() { PrepareForQuickExit(); });
  const bool callback_entered = prepare.WaitUntilEntered();
  std::atomic<bool> unregister_returned{false};
  bool unregister_result = false;
  std::thread unregister([&]() {
    unregister_result =
        UnregisterQuickExitPrepareCallback(&BlockingPrepare::Run, &prepare);
    unregister_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(10ms);
  const bool returned_before_release =
      unregister_returned.load(std::memory_order_acquire);
  prepare.Release();
  caller.join();
  unregister.join();

  REQUIRE(callback_entered);
  REQUIRE_FALSE(returned_before_release);
  REQUIRE(unregister_result);
}

}  // namespace testing
}  // namespace xe
