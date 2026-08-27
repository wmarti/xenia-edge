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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
namespace testing {
namespace {

class StubBackend final : public backend::Backend {
 public:
  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override {
  }

  std::unique_ptr<backend::Assembler> CreateAssembler() override {
    return nullptr;
  }

  std::unique_ptr<GuestFunction> CreateGuestFunction(
      Module* module, uint32_t address) override {
    return nullptr;
  }

  uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                        uint64_t current_pc) override {
    return current_pc;
  }
};

class CallbackGuestFunction final : public GuestFunction {
 public:
  using Callback = std::function<bool(ThreadState*, uint32_t)>;

  CallbackGuestFunction(uint32_t address, uint32_t end_address,
                        Callback callback)
      : GuestFunction(nullptr, address), callback_(std::move(callback)) {
    set_end_address(end_address);
  }

  uint8_t* machine_code() const override { return nullptr; }
  size_t machine_code_length() const override { return 0; }

 protected:
  bool CallImpl(ThreadState* thread_state, uint32_t return_address) override {
    return callback_(thread_state, return_address);
  }

 private:
  Callback callback_;
};

class CaptureTestEnvironment {
 public:
  CaptureTestEnvironment() {
    memory = std::make_unique<Memory>();
    if (!memory->Initialize()) {
      throw std::runtime_error("test memory initialization failed");
    }
    processor = std::make_unique<Processor>(memory.get(), nullptr);
    if (!processor->Setup(std::make_unique<StubBackend>())) {
      throw std::runtime_error("test processor initialization failed");
    }
  }

  std::unique_ptr<ThreadState> MakeThread(uint32_t guest_thread_id) {
    return std::make_unique<ThreadState>(processor.get(), guest_thread_id);
  }

  std::unique_ptr<Memory> memory;
  std::unique_ptr<Processor> processor;
};

class ScopedHostCallObserver final {
 public:
  ScopedHostCallObserver(
      Processor& processor,
      std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer)
      : processor_(processor), observer_(std::move(observer)) {
    if (!processor_.AttachGuestExecutionCaptureHostCallObserver(observer_)) {
      throw std::runtime_error("test observer attachment failed");
    }
  }

  ~ScopedHostCallObserver() {
    if (attached_) {
      processor_.DetachGuestExecutionCaptureHostCallObserver(observer_);
    }
  }

  bool Detach() {
    if (!attached_) {
      return false;
    }
    attached_ =
        !processor_.DetachGuestExecutionCaptureHostCallObserver(observer_);
    return !attached_;
  }

 private:
  Processor& processor_;
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer_;
  bool attached_ = true;
};

class ScopedThreadStateBinding final {
 public:
  explicit ScopedThreadStateBinding(ThreadState* thread_state)
      : original_thread_state_(ThreadState::Get()) {
    ThreadState::Bind(thread_state);
  }

  ~ScopedThreadStateBinding() { ThreadState::Bind(original_thread_state_); }

  ScopedThreadStateBinding(const ScopedThreadStateBinding&) = delete;
  ScopedThreadStateBinding& operator=(const ScopedThreadStateBinding&) = delete;

 private:
  ThreadState* original_thread_state_ = nullptr;
};

class BlockingBeginObserver final
    : public GuestExecutionCaptureHostCallObserver {
 public:
  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    begin_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return release_begin_; });
    active_ = true;
    return {1};
  }

  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token.value != 1 || !active_) {
      return false;
    }
    active_ = false;
    outcome_ = outcome;
    return true;
  }

  bool CanDetach() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_;
  }

  bool WaitUntilBeginEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return begin_entered_; });
  }

  void ReleaseBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_begin_ = true;
    condition_.notify_all();
  }

  GuestExecutionCaptureHostCallOutcome outcome() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outcome_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool begin_entered_ = false;
  bool release_begin_ = false;
  bool active_ = false;
  GuestExecutionCaptureHostCallOutcome outcome_ =
      GuestExecutionCaptureHostCallOutcome::kAbortedByHostUnwind;
};

class BlockingDetachObserver final
    : public GuestExecutionCaptureHostCallObserver {
 public:
  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override {
    return roster_.OnHostGuestCallBegin(thread_state, function, return_address);
  }

  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override {
    return roster_.OnHostGuestCallEnd(token, thread_state, function, outcome);
  }

  bool CanDetach() const noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block_next_can_detach_) {
      block_next_can_detach_ = false;
      can_detach_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return release_can_detach_; });
    }
    lock.unlock();
    return roster_.CanDetach();
  }

  void BlockNextCanDetach() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_next_can_detach_ = true;
    can_detach_entered_ = false;
    release_can_detach_ = false;
  }

  bool WaitUntilCanDetachEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return can_detach_entered_; });
  }

  void ReleaseCanDetach() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_can_detach_ = true;
    condition_.notify_all();
  }

 private:
  GuestExecutionCaptureHostCallRoster roster_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable bool block_next_can_detach_ = false;
  mutable bool can_detach_entered_ = false;
  mutable bool release_can_detach_ = false;
};

}  // namespace

TEST_CASE("Guest host-call roster records every terminal outcome",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto requested_thread = environment.MakeThread(0x101);
  auto original_thread = environment.MakeThread(0x202);
  auto roster = std::make_shared<GuestExecutionCaptureHostCallRoster>();
  ScopedHostCallObserver attachment(*environment.processor, roster);
  ScopedThreadStateBinding thread_binding(original_thread.get());

  GuestExecutionCaptureHostCallRosterSnapshot active_snapshot;
  CallbackGuestFunction returned_function(
      0x82001000, 0x8200103C,
      [&](ThreadState* thread_state, uint32_t return_address) {
        active_snapshot = roster->snapshot();
        return true;
      });
  REQUIRE(returned_function.Call(requested_thread.get(), 0x82002000));
  REQUIRE(ThreadState::Get() == original_thread.get());
  REQUIRE(active_snapshot.active_calls.size() == 1);
  const GuestExecutionCaptureActiveHostCall& active_call =
      active_snapshot.active_calls[0];
  REQUIRE(active_call.participant.capture_instance_id ==
          requested_thread->guest_execution_capture_instance_id());
  REQUIRE(active_call.participant.guest_thread_id ==
          requested_thread->thread_id());
  REQUIRE(active_call.function_address == 0x82001000);
  REQUIRE(active_call.function_end_address == 0x8200103C);
  REQUIRE(active_call.return_address == 0x82002000);
  REQUIRE(active_call.participant_depth == 1);
  REQUIRE(active_call.is_outermost());

  CallbackGuestFunction failed_function(
      0x82003000, 0x8200301C,
      [](ThreadState* thread_state, uint32_t return_address) { return false; });
  REQUIRE_FALSE(failed_function.Call(requested_thread.get(), 0x82004000));
  REQUIRE(ThreadState::Get() == original_thread.get());

  CallbackGuestFunction aborted_function(
      0x82005000, 0x8200501C,
      [](ThreadState* thread_state, uint32_t return_address) -> bool {
        throw std::runtime_error("synthetic host unwind");
      });
  REQUIRE_THROWS_AS(aborted_function.Call(requested_thread.get(), 0x82006000),
                    std::runtime_error);
  REQUIRE(ThreadState::Get() == original_thread.get());
  const GuestExecutionCaptureHostCallRosterSnapshot final_snapshot =
      roster->snapshot();
  REQUIRE(final_snapshot.returned_host_call_count == 1);
  REQUIRE(final_snapshot.failed_to_enter_host_call_count == 1);
  REQUIRE(final_snapshot.aborted_host_call_count == 1);
  REQUIRE(final_snapshot.rejection ==
          GuestExecutionCaptureHostCallRosterRejection::kNone);
  REQUIRE(final_snapshot.active_calls.empty());
  REQUIRE(roster->CanDetach());
  REQUIRE(attachment.Detach());
}

TEST_CASE(
    "Guest host-call roster separates nested host reentry from guest flow",
    "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x303);
  auto roster = std::make_shared<GuestExecutionCaptureHostCallRoster>();
  ScopedHostCallObserver attachment(*environment.processor, roster);

  GuestExecutionCaptureHostCallRosterSnapshot nested_snapshot;
  CallbackGuestFunction nested_function(
      0x82011000, 0x8201102C,
      [&](ThreadState* call_thread_state, uint32_t return_address) {
        nested_snapshot = roster->snapshot();
        return true;
      });
  GuestExecutionCaptureHostCallRosterSnapshot outer_before_nested;
  GuestExecutionCaptureHostCallRosterSnapshot outer_after_nested;
  CallbackGuestFunction outer_function(
      0x82010000, 0x8201004C,
      [&](ThreadState* call_thread_state, uint32_t return_address) {
        outer_before_nested = roster->snapshot();
        const bool nested_result =
            nested_function.Call(call_thread_state, 0x82012000);
        outer_after_nested = roster->snapshot();
        return nested_result;
      });

  REQUIRE(outer_function.Call(thread_state.get(), 0x82013000));
  REQUIRE(outer_before_nested.active_calls.size() == 1);
  REQUIRE(nested_snapshot.active_calls.size() == 2);
  REQUIRE(nested_snapshot.active_calls[0].participant_depth == 1);
  REQUIRE(nested_snapshot.active_calls[0].is_outermost());
  REQUIRE(nested_snapshot.active_calls[0].return_address == 0x82013000);
  REQUIRE(nested_snapshot.active_calls[1].participant_depth == 2);
  REQUIRE_FALSE(nested_snapshot.active_calls[1].is_outermost());
  REQUIRE(nested_snapshot.active_calls[1].return_address == 0x82012000);
  REQUIRE(outer_after_nested.active_calls.size() == 1);
  REQUIRE(outer_after_nested.active_calls[0].participant_depth == 1);

  const GuestExecutionCaptureHostCallRosterSnapshot final_snapshot =
      roster->snapshot();
  REQUIRE(final_snapshot.returned_host_call_count == 2);
  REQUIRE(final_snapshot.active_calls.empty());
  REQUIRE(final_snapshot.rejection ==
          GuestExecutionCaptureHostCallRosterRejection::kNone);
  REQUIRE(attachment.Detach());
}

TEST_CASE("Guest host-call roster serializes concurrent participants",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto first_thread = environment.MakeThread(0x404);
  auto second_thread = environment.MakeThread(0x505);
  REQUIRE(first_thread->guest_execution_capture_instance_id() !=
          second_thread->guest_execution_capture_instance_id());
  auto roster = std::make_shared<GuestExecutionCaptureHostCallRoster>();
  ScopedHostCallObserver attachment(*environment.processor, roster);

  std::mutex rendezvous_mutex;
  std::condition_variable rendezvous_condition;
  uint32_t arrived = 0;
  bool release = false;
  CallbackGuestFunction function(
      0x82020000, 0x8202003C,
      [&](ThreadState* thread_state, uint32_t return_address) {
        std::unique_lock<std::mutex> lock(rendezvous_mutex);
        ++arrived;
        rendezvous_condition.notify_all();
        rendezvous_condition.wait(lock, [&] { return release; });
        return true;
      });

  bool first_result = false;
  bool second_result = false;
  std::thread first_worker(
      [&] { first_result = function.Call(first_thread.get(), 0x82021000); });
  std::thread second_worker(
      [&] { second_result = function.Call(second_thread.get(), 0x82022000); });

  bool both_arrived = false;
  {
    std::unique_lock<std::mutex> lock(rendezvous_mutex);
    both_arrived = rendezvous_condition.wait_for(lock, std::chrono::seconds(5),
                                                 [&] { return arrived == 2; });
  }
  const GuestExecutionCaptureHostCallRosterSnapshot active_snapshot =
      roster->snapshot();
  {
    std::lock_guard<std::mutex> lock(rendezvous_mutex);
    release = true;
    rendezvous_condition.notify_all();
  }
  first_worker.join();
  second_worker.join();

  REQUIRE(both_arrived);
  REQUIRE(active_snapshot.active_calls.size() == 2);
  REQUIRE(active_snapshot.active_calls[0].token.value !=
          active_snapshot.active_calls[1].token.value);
  REQUIRE(first_result);
  REQUIRE(second_result);
  const GuestExecutionCaptureHostCallRosterSnapshot final_snapshot =
      roster->snapshot();
  REQUIRE(final_snapshot.returned_host_call_count == 2);
  REQUIRE(final_snapshot.active_calls.empty());
  REQUIRE(final_snapshot.rejection ==
          GuestExecutionCaptureHostCallRosterRejection::kNone);
  REQUIRE(attachment.Detach());
}

TEST_CASE("Guest host-call observer registration drains before detach",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x606);
  auto observer = std::make_shared<BlockingBeginObserver>();
  auto stale_observer = std::make_shared<GuestExecutionCaptureHostCallRoster>();
  REQUIRE_FALSE(
      environment.processor->AttachGuestExecutionCaptureHostCallObserver(
          nullptr));
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      observer));
  REQUIRE_FALSE(
      environment.processor->AttachGuestExecutionCaptureHostCallObserver(
          stale_observer));
  REQUIRE_FALSE(
      environment.processor->DetachGuestExecutionCaptureHostCallObserver(
          stale_observer));

  CallbackGuestFunction function(
      0x82030000, 0x8203001C,
      [](ThreadState* thread_state, uint32_t return_address) { return true; });
  bool call_result = false;
  std::thread worker(
      [&] { call_result = function.Call(thread_state.get(), 0x82031000); });
  const bool begin_entered = observer->WaitUntilBeginEntered();
  const bool detached_during_begin =
      environment.processor->DetachGuestExecutionCaptureHostCallObserver(
          observer);
  std::weak_ptr<BlockingBeginObserver> observer_lifetime = observer;
  observer.reset();
  std::shared_ptr<BlockingBeginObserver> retained_observer =
      observer_lifetime.lock();
  REQUIRE(retained_observer);
  retained_observer->ReleaseBegin();
  worker.join();

  REQUIRE(begin_entered);
  REQUIRE_FALSE(detached_during_begin);
  REQUIRE(call_result);
  REQUIRE(retained_observer->outcome() ==
          GuestExecutionCaptureHostCallOutcome::kReturnedToHost);
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      retained_observer));
  REQUIRE_FALSE(
      environment.processor->DetachGuestExecutionCaptureHostCallObserver(
          retained_observer));
  retained_observer.reset();
  REQUIRE(observer_lifetime.expired());
}

TEST_CASE("Guest thread capture identity is never reused",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto first_thread = environment.MakeThread(0x707);
  const uint64_t first_instance_id =
      first_thread->guest_execution_capture_instance_id();
  first_thread.reset();
  auto second_thread = environment.MakeThread(0x707);
  REQUIRE(second_thread->guest_execution_capture_instance_id() >
          first_instance_id);
}

TEST_CASE("Processor owns an attached guest host-call observer",
          "[guest-execution-capture]") {
  std::weak_ptr<GuestExecutionCaptureHostCallRoster> observer_lifetime;
  {
    CaptureTestEnvironment environment;
    auto observer = std::make_shared<GuestExecutionCaptureHostCallRoster>();
    observer_lifetime = observer;
    REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
        observer));
    observer.reset();
    REQUIRE_FALSE(observer_lifetime.expired());
  }
  REQUIRE(observer_lifetime.expired());
}

TEST_CASE("Guest host-call detach query never holds the Processor lock",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x808);
  auto observer = std::make_shared<BlockingDetachObserver>();
  ScopedHostCallObserver attachment(*environment.processor, observer);

  observer->BlockNextCanDetach();
  bool detach_result = true;
  std::thread detach_worker([&] { detach_result = attachment.Detach(); });
  const bool detach_query_entered = observer->WaitUntilCanDetachEntered();

  std::mutex call_mutex;
  std::condition_variable call_condition;
  bool call_entered = false;
  bool release_call = false;
  CallbackGuestFunction function(
      0x82040000, 0x8204001C,
      [&](ThreadState* call_thread_state, uint32_t return_address) {
        std::unique_lock<std::mutex> lock(call_mutex);
        call_entered = true;
        call_condition.notify_all();
        call_condition.wait(lock, [&] { return release_call; });
        return true;
      });
  bool call_result = false;
  std::thread call_worker(
      [&] { call_result = function.Call(thread_state.get(), 0x82041000); });
  bool call_reached_guest = false;
  {
    std::unique_lock<std::mutex> lock(call_mutex);
    call_reached_guest = call_condition.wait_for(lock, std::chrono::seconds(5),
                                                 [&] { return call_entered; });
  }

  observer->ReleaseCanDetach();
  detach_worker.join();
  {
    std::lock_guard<std::mutex> lock(call_mutex);
    release_call = true;
    call_condition.notify_all();
  }
  call_worker.join();

  REQUIRE(detach_query_entered);
  REQUIRE(call_reached_guest);
  REQUIRE_FALSE(detach_result);
  REQUIRE(call_result);
  REQUIRE(attachment.Detach());
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
