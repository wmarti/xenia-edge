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
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class GuestExecutionCaptureRegistryTestAccess final {
 public:
  static void SetThreadStateDestructionGateSignal(Processor& processor,
                                                  std::atomic<bool>* signal) {
    processor
        .guest_execution_capture_thread_state_destruction_gate_test_signal_ =
        signal;
  }

  static void SetThreadStateRegistrationGate(Processor& processor,
                                             std::atomic<bool>* signal,
                                             std::atomic<bool>* release) {
    processor
        .guest_execution_capture_thread_state_registration_gate_test_signal_ =
        signal;
    processor
        .guest_execution_capture_thread_state_registration_gate_test_release_ =
        release;
  }
};

namespace testing {
namespace {

class RegistryInvocationCaptureSink final
    : public GuestInvocationCaptureEventSink {
 public:
  uint32_t root_address() const override { return root; }
  uint8_t initial_event_mask() const override { return mask; }
  bool Poll() override { return true; }
  bool OnFunctionDependency(uint32_t, uint32_t) override { return true; }
  bool OnFunctionDefined(uint32_t, uint32_t) override { return true; }
  bool OnFunctionEntry(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                       uint32_t, const ppc::GuestPPCRegisterState&) override {
    return true;
  }
  bool OnFunctionExit(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                      uint32_t, const ppc::GuestPPCRegisterState&) override {
    return true;
  }
  bool OnMemoryAccess(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                      uint32_t,
                      ppc::GuestInvocationRecorderMemoryAccess) override {
    return true;
  }
  bool OnUnsupportedDependency(const ppc::GuestInvocationRecorderIdentity&,
                               uint32_t) override {
    return true;
  }
  bool OnTailCall(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                  uint32_t) override {
    return true;
  }
  bool OnUnwindOrLongjmp(const ppc::GuestInvocationRecorderIdentity&) override {
    return true;
  }
  bool OnAsyncReentry(const ppc::GuestInvocationRecorderIdentity&) override {
    return true;
  }

  uint32_t root = 0;
  uint8_t mask = 0;
};

class StubBackend final : public backend::Backend {
 public:
  void* AllocThreadData() override { return this; }

  void FreeThreadData(void* thread_data) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block_next_free_) {
      block_next_free_ = false;
      free_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return release_free_; });
    }
  }

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

  void BlockNextFree() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_next_free_ = true;
    free_entered_ = false;
    release_free_ = false;
  }

  bool WaitUntilFreeEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return free_entered_; });
  }

  void ReleaseFree() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_free_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool block_next_free_ = false;
  bool free_entered_ = false;
  bool release_free_ = false;
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
    std::unique_ptr<StubBackend> owned_backend =
        std::make_unique<StubBackend>();
    backend = owned_backend.get();
    if (!processor->Setup(std::move(owned_backend))) {
      throw std::runtime_error("test processor initialization failed");
    }
  }

  std::unique_ptr<ThreadState> MakeThread(uint32_t guest_thread_id) {
    std::unique_ptr<ThreadState> thread_state =
        MakePendingThread(guest_thread_id);
    if (thread_state->PublishGuestExecutionCaptureReady() !=
        GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept) {
      throw std::runtime_error("test ThreadState publication failed");
    }
    return thread_state;
  }

  std::unique_ptr<ThreadState> MakePendingThread(uint32_t guest_thread_id) {
    return std::make_unique<ThreadState>(processor.get(), guest_thread_id);
  }

  std::unique_ptr<Memory> memory;
  std::unique_ptr<Processor> processor;
  StubBackend* backend = nullptr;
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

class RecordingLifecycleObserver final
    : public GuestExecutionCaptureHostCallObserver {
 public:
  GuestExecutionCaptureThreadStateLifecycleDisposition OnThreadStateSeed(
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          seed) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++seed_call_count_;
    if (reject_seed_ && reject_seed_index_ < seed.size()) {
      reject_seed_ = false;
      return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
    }
    events_.insert(events_.end(), seed.begin(), seed.end());
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
  }

  GuestExecutionCaptureThreadStateLifecycleDisposition OnThreadStateLifecycle(
      GuestExecutionCaptureThreadStateLifecycleEvent event) noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    events_.push_back(event);
    const bool reject =
        reject_next_lifecycle_ && reject_lifecycle_state_ == event.state;
    if (reject) {
      reject_next_lifecycle_ = false;
    }
    if (block_next_lifecycle_ && block_lifecycle_state_ == event.state) {
      block_next_lifecycle_ = false;
      lifecycle_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return release_lifecycle_; });
    }
    return reject
               ? GuestExecutionCaptureThreadStateLifecycleDisposition::kReject
               : GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
  }

  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override {
    return {};
  }

  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override {
    return false;
  }

  bool CanDetach() const noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block_next_can_detach_) {
      block_next_can_detach_ = false;
      can_detach_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return release_can_detach_; });
    }
    return true;
  }

  void RejectNextLifecycle(
      GuestExecutionCaptureThreadStateLifecycleState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    reject_next_lifecycle_ = true;
    reject_lifecycle_state_ = state;
  }

  void RejectSeedAt(size_t participant_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    reject_seed_ = true;
    reject_seed_index_ = participant_index;
  }

  void BlockNextLifecycle(
      GuestExecutionCaptureThreadStateLifecycleState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_next_lifecycle_ = true;
    block_lifecycle_state_ = state;
    lifecycle_entered_ = false;
    release_lifecycle_ = false;
  }

  bool WaitUntilLifecycleEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return lifecycle_entered_; });
  }

  void ReleaseLifecycle() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_lifecycle_ = true;
    condition_.notify_all();
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

  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

  size_t seed_call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seed_call_count_;
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> events_;
  bool reject_seed_ = false;
  size_t reject_seed_index_ = 0;
  size_t seed_call_count_ = 0;
  bool reject_next_lifecycle_ = false;
  GuestExecutionCaptureThreadStateLifecycleState reject_lifecycle_state_ =
      GuestExecutionCaptureThreadStateLifecycleState::kPending;
  bool block_next_lifecycle_ = false;
  GuestExecutionCaptureThreadStateLifecycleState block_lifecycle_state_ =
      GuestExecutionCaptureThreadStateLifecycleState::kPending;
  bool lifecycle_entered_ = false;
  bool release_lifecycle_ = false;
  mutable bool block_next_can_detach_ = false;
  mutable bool can_detach_entered_ = false;
  mutable bool release_can_detach_ = false;
};

class CollectingThreadStateVisitor final
    : public GuestExecutionCaptureThreadStateVisitor {
 public:
  explicit CollectingThreadStateVisitor(bool stop_after_first = false)
      : stop_after_first_(stop_after_first) {}

  bool VisitThreadState(const ThreadState& thread_state) noexcept override {
    participants_.push_back({
        thread_state.guest_execution_capture_instance_id(),
        thread_state.thread_id(),
    });
    contexts_valid_ &= thread_state.context() &&
                       thread_state.context()->thread_state == &thread_state;
    register_31_values_.push_back(thread_state.context()->r[31]);
    return !(stop_after_first_ && participants_.size() == 1);
  }

  const std::vector<GuestExecutionCaptureParticipantIdentity>& participants()
      const {
    return participants_;
  }
  bool contexts_valid() const { return contexts_valid_; }
  const std::vector<uint64_t>& register_31_values() const {
    return register_31_values_;
  }

 private:
  bool stop_after_first_ = false;
  bool contexts_valid_ = true;
  std::vector<GuestExecutionCaptureParticipantIdentity> participants_;
  std::vector<uint64_t> register_31_values_;
};

class ReentrantObserver final : public GuestExecutionCaptureHostCallObserver {
 public:
  explicit ReentrantObserver(Processor& processor) : processor_(processor) {}

  GuestExecutionCaptureThreadStateLifecycleDisposition OnThreadStateSeed(
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          seed) noexcept override {
    RecordReentry();
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
  }

  GuestExecutionCaptureThreadStateLifecycleDisposition OnThreadStateLifecycle(
      GuestExecutionCaptureThreadStateLifecycleEvent event) noexcept override {
    RecordReentry();
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
  }

  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override {
    RecordReentry();
    return {1};
  }

  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override {
    RecordReentry();
    return token.value == 1;
  }

  bool CanDetach() const noexcept override {
    RecordReentry();
    return true;
  }

  std::vector<GuestExecutionCaptureThreadStateRegistryRejection> rejections()
      const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejections_;
  }

 private:
  void RecordReentry() const {
    const GuestExecutionCaptureThreadStateRegistrySnapshot snapshot =
        processor_.QueryGuestExecutionCaptureParticipants();
    std::lock_guard<std::mutex> lock(mutex_);
    rejections_.push_back(snapshot.rejection);
  }

  Processor& processor_;
  mutable std::mutex mutex_;
  mutable std::vector<GuestExecutionCaptureThreadStateRegistryRejection>
      rejections_;
};

class ReentrantThreadStateVisitor final
    : public GuestExecutionCaptureThreadStateVisitor {
 public:
  explicit ReentrantThreadStateVisitor(Processor& processor)
      : processor_(processor) {}

  bool VisitThreadState(const ThreadState& thread_state) noexcept override {
    rejection_ = processor_.QueryGuestExecutionCaptureParticipants().rejection;
    return true;
  }

  GuestExecutionCaptureThreadStateRegistryRejection rejection() const {
    return rejection_;
  }

 private:
  Processor& processor_;
  GuestExecutionCaptureThreadStateRegistryRejection rejection_ =
      GuestExecutionCaptureThreadStateRegistryRejection::kNone;
};

class BlockingThreadStateVisitor final
    : public GuestExecutionCaptureThreadStateVisitor {
 public:
  bool VisitThreadState(const ThreadState& thread_state) noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return release_; });
    context_valid_ = thread_state.context() &&
                     thread_state.context()->thread_state == &thread_state;
    return true;
  }

  bool WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_ = true;
    condition_.notify_all();
  }

  bool context_valid() const { return context_valid_.load(); }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool release_ = false;
  std::atomic<bool> context_valid_ = false;
};

class RequestJitSafepointVisitor final
    : public GuestExecutionCaptureThreadStateVisitor {
 public:
  bool VisitThreadState(const ThreadState& thread_state) noexcept override {
    thread_state.RequestGuestExecutionCaptureJitSafepoint();
    ++request_count_;
    return true;
  }

  size_t request_count() const { return request_count_; }

 private:
  size_t request_count_ = 0;
};

class JitSafepointObserver final
    : public GuestExecutionCaptureHostCallObserver {
 public:
  explicit JitSafepointObserver(uint64_t blocked_instance_id = 0)
      : blocked_instance_id_(blocked_instance_id) {}

  GuestExecutionCaptureJitSafepointDisposition OnJitSafepoint(
      const ThreadState& thread_state,
      uint32_t guest_address) noexcept override {
    std::unique_lock<std::mutex> lock(mutex_);
    guest_addresses_.push_back(guest_address);
    requests_consumed_.push_back(
        !thread_state.IsGuestExecutionCaptureJitSafepointRequested());
    if (reenter_next_) {
      reenter_next_ = false;
      thread_state.RequestGuestExecutionCaptureJitSafepoint();
      reentry_result_ = HandleGuestExecutionCaptureJitSafepoint(
          thread_state.context(), guest_address + 4);
      reentry_request_retained_ =
          thread_state.IsGuestExecutionCaptureJitSafepointRequested();
    }
    condition_.notify_all();
    if (blocked_instance_id_ &&
        blocked_instance_id_ ==
            thread_state.guest_execution_capture_instance_id()) {
      blocked_instance_id_ = 0;
      blocked_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return release_blocked_; });
    }
    if (reject_next_) {
      reject_next_ = false;
      return GuestExecutionCaptureJitSafepointDisposition::kReject;
    }
    return GuestExecutionCaptureJitSafepointDisposition::kAccept;
  }

  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override {
    return {1};
  }

  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override {
    return token.value == 1;
  }

  bool CanDetach() const noexcept override { return true; }

  void RejectNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    reject_next_ = true;
  }

  void ReenterNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    reenter_next_ = true;
  }

  bool WaitUntilBlocked() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return blocked_; });
  }

  bool WaitForCallbackCount(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return guest_addresses_.size() >= count;
    });
  }

  void ReleaseBlocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_blocked_ = true;
    condition_.notify_all();
  }

  std::vector<uint32_t> guest_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return guest_addresses_;
  }

  std::vector<bool> requests_consumed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_consumed_;
  }

  GuestExecutionCaptureJitSafepointResult reentry_result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reentry_result_;
  }

  bool reentry_request_retained() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reentry_request_retained_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<uint32_t> guest_addresses_;
  std::vector<bool> requests_consumed_;
  uint64_t blocked_instance_id_ = 0;
  bool blocked_ = false;
  bool release_blocked_ = false;
  bool reject_next_ = false;
  bool reenter_next_ = false;
  GuestExecutionCaptureJitSafepointResult reentry_result_ =
      GuestExecutionCaptureJitSafepointResult::kNotRequested;
  bool reentry_request_retained_ = false;
};

}  // namespace

TEST_CASE("Guest ThreadState registry rejects pending snapshots atomically",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto ready_thread = environment.MakeThread(0x101);
  auto pending_thread = environment.MakePendingThread(0x202);
  const GuestExecutionCaptureParticipantIdentity ready_identity = {
      ready_thread->guest_execution_capture_instance_id(),
      ready_thread->thread_id(),
  };
  const GuestExecutionCaptureParticipantIdentity pending_identity = {
      pending_thread->guest_execution_capture_instance_id(),
      pending_thread->thread_id(),
  };
  const GuestExecutionCaptureThreadStateRegistrySnapshot snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  REQUIRE(snapshot.rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::kNone);
  REQUIRE_FALSE(snapshot.all_ready());
  REQUIRE(snapshot.participants ==
          std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
              {ready_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kReady},
              {pending_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kPending},
          });

  auto observer = std::make_shared<RecordingLifecycleObserver>();
  REQUIRE_FALSE(
      environment.processor->AttachGuestExecutionCaptureHostCallObserver(
          observer));
  REQUIRE(observer->seed_call_count() == 0);
  REQUIRE(observer->events().empty());

  CollectingThreadStateVisitor visitor;
  REQUIRE(
      environment.processor->VisitGuestExecutionCaptureThreadStates(visitor) ==
      GuestExecutionCaptureThreadStateVisitResult::kParticipantNotReady);
  REQUIRE(visitor.participants().empty());

  pending_thread.reset();
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      observer));
  REQUIRE(observer->seed_call_count() == 1);
  REQUIRE(observer->events() ==
          std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
              {ready_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kReady},
          });
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      observer));
  ready_thread.reset();
}

TEST_CASE("Guest host-call dispatch permanently closes observer attachment",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x212);
  CallbackGuestFunction function(0x82000100, 0x8200011C,
                                 [](ThreadState* call_thread_state,
                                    uint32_t return_address) { return true; });

  REQUIRE(function.Call(thread_state.get(), 0x82000200));

  auto observer = std::make_shared<RecordingLifecycleObserver>();
  REQUIRE_FALSE(
      environment.processor->AttachGuestExecutionCaptureHostCallObserver(
          observer));
  REQUIRE(observer->seed_call_count() == 0);
  REQUIRE(observer->events().empty());
}

TEST_CASE("Guest host-call dispatch racing attachment fails closed",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x213);
  auto observer = std::make_shared<RecordingLifecycleObserver>();
  observer->BlockNextCanDetach();

  bool attach_result = true;
  std::thread attach_worker([&] {
    attach_result =
        environment.processor->AttachGuestExecutionCaptureHostCallObserver(
            observer);
  });
  const bool can_detach_entered = observer->WaitUntilCanDetachEntered();

  CallbackGuestFunction function(0x82000300, 0x8200031C,
                                 [](ThreadState* call_thread_state,
                                    uint32_t return_address) { return true; });
  const bool call_result = function.Call(thread_state.get(), 0x82000400);

  observer->ReleaseCanDetach();
  attach_worker.join();

  REQUIRE(can_detach_entered);
  REQUIRE(call_result);
  REQUIRE_FALSE(attach_result);
  REQUIRE(observer->seed_call_count() == 0);
  REQUIRE(observer->events().empty());
}

TEST_CASE("Guest ThreadState publishes initialized PPC state",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakePendingThread(0x303);
  constexpr uint64_t kSentinel = 0x123456789ABCDEF0ull;
  thread_state->context()->r[31] = kSentinel;

  CollectingThreadStateVisitor pending_visitor;
  REQUIRE(environment.processor->VisitGuestExecutionCaptureThreadStates(
              pending_visitor) ==
          GuestExecutionCaptureThreadStateVisitResult::kParticipantNotReady);
  REQUIRE(pending_visitor.participants().empty());

  REQUIRE(thread_state->PublishGuestExecutionCaptureReady() ==
          GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
  const GuestExecutionCaptureThreadStateRegistrySnapshot snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  REQUIRE(snapshot.all_ready());
  REQUIRE(snapshot.participants.size() == 1);
  REQUIRE(snapshot.participants[0].state ==
          GuestExecutionCaptureThreadStateLifecycleState::kReady);

  CollectingThreadStateVisitor visitor;
  REQUIRE(
      environment.processor->VisitGuestExecutionCaptureThreadStates(visitor) ==
      GuestExecutionCaptureThreadStateVisitResult::kCompleted);
  REQUIRE(visitor.contexts_valid());
  REQUIRE(visitor.register_31_values() == std::vector<uint64_t>{kSentinel});

  CollectingThreadStateVisitor stopping_visitor(true);
  REQUIRE(environment.processor->VisitGuestExecutionCaptureThreadStates(
              stopping_visitor) ==
          GuestExecutionCaptureThreadStateVisitResult::kStoppedByVisitor);
  REQUIRE(stopping_visitor.participants().size() == 1);
}

TEST_CASE("Guest ThreadState lifecycle exposes exact state transitions",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto observer = std::make_shared<RecordingLifecycleObserver>();
  ScopedHostCallObserver attachment(*environment.processor, observer);

  auto pending_thread = environment.MakePendingThread(0x404);
  const GuestExecutionCaptureParticipantIdentity pending_identity = {
      pending_thread->guest_execution_capture_instance_id(),
      pending_thread->thread_id(),
  };
  pending_thread.reset();

  auto ready_thread = environment.MakePendingThread(0x404);
  const GuestExecutionCaptureParticipantIdentity ready_identity = {
      ready_thread->guest_execution_capture_instance_id(),
      ready_thread->thread_id(),
  };
  REQUIRE(ready_identity.guest_thread_id == pending_identity.guest_thread_id);
  REQUIRE(ready_identity.capture_instance_id >
          pending_identity.capture_instance_id);
  REQUIRE(ready_thread->PublishGuestExecutionCaptureReady() ==
          GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
  ready_thread.reset();

  REQUIRE(observer->events() ==
          std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
              {pending_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kPending},
              {pending_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kDestroying},
              {ready_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kPending},
              {ready_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kReady},
              {ready_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kDestroying},
          });
  const GuestExecutionCaptureThreadStateRegistrySnapshot snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  REQUIRE(snapshot.rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::kNone);
  REQUIRE(snapshot.participants.empty());
  REQUIRE(attachment.Detach());
}

TEST_CASE("Guest ThreadState remains registered throughout destruction",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x405);
  const GuestExecutionCaptureParticipantIdentity identity = {
      thread_state->guest_execution_capture_instance_id(),
      thread_state->thread_id(),
  };
  environment.backend->BlockNextFree();

  std::thread destroy_worker([&] { thread_state.reset(); });
  const bool free_entered = environment.backend->WaitUntilFreeEntered();

  const GuestExecutionCaptureThreadStateRegistrySnapshot destroying_snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  CollectingThreadStateVisitor visitor;
  const GuestExecutionCaptureThreadStateVisitResult visit_result =
      environment.processor->VisitGuestExecutionCaptureThreadStates(visitor);
  auto observer = std::make_shared<RecordingLifecycleObserver>();
  const bool attach_result =
      environment.processor->AttachGuestExecutionCaptureHostCallObserver(
          observer);

  environment.backend->ReleaseFree();
  destroy_worker.join();

  REQUIRE(free_entered);
  REQUIRE_FALSE(destroying_snapshot.all_ready());
  REQUIRE(destroying_snapshot.participants ==
          std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
              {identity,
               GuestExecutionCaptureThreadStateLifecycleState::kDestroying},
          });
  REQUIRE(visit_result ==
          GuestExecutionCaptureThreadStateVisitResult::kParticipantNotReady);
  REQUIRE(visitor.participants().empty());
  REQUIRE_FALSE(attach_result);
  REQUIRE(observer->events().empty());
  REQUIRE(environment.processor->QueryGuestExecutionCaptureParticipants()
              .participants.empty());
}

TEST_CASE("Guest ThreadState observer seed rejection prevents installation",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto first_thread = environment.MakeThread(0x505);
  auto second_thread = environment.MakeThread(0x506);
  const GuestExecutionCaptureParticipantIdentity first_identity = {
      first_thread->guest_execution_capture_instance_id(),
      first_thread->thread_id(),
  };
  const GuestExecutionCaptureParticipantIdentity second_identity = {
      second_thread->guest_execution_capture_instance_id(),
      second_thread->thread_id(),
  };
  auto rejecting_observer = std::make_shared<RecordingLifecycleObserver>();
  rejecting_observer->RejectSeedAt(1);
  REQUIRE_FALSE(
      environment.processor->AttachGuestExecutionCaptureHostCallObserver(
          rejecting_observer));
  REQUIRE(rejecting_observer->seed_call_count() == 1);
  REQUIRE(rejecting_observer->events().empty());

  auto accepting_observer = std::make_shared<RecordingLifecycleObserver>();
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      accepting_observer));
  REQUIRE(accepting_observer->seed_call_count() == 1);
  REQUIRE(accepting_observer->events() ==
          std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
              {first_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kReady},
              {second_identity,
               GuestExecutionCaptureThreadStateLifecycleState::kReady},
          });
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      accepting_observer));
}

TEST_CASE("Guest ThreadState runtime rejection is sticky and detachable",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto rejecting_observer = std::make_shared<RecordingLifecycleObserver>();
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      rejecting_observer));
  rejecting_observer->RejectNextLifecycle(
      GuestExecutionCaptureThreadStateLifecycleState::kPending);

  auto thread_state = environment.MakePendingThread(0x607);
  const GuestExecutionCaptureThreadStateRegistrySnapshot rejected_snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  REQUIRE(rejected_snapshot.rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::
              kObserverRejectedRuntimeEvent);
  REQUIRE_FALSE(rejected_snapshot.all_ready());
  REQUIRE(thread_state->PublishGuestExecutionCaptureReady() ==
          GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
  thread_state.reset();
  REQUIRE(rejecting_observer->events().size() == 3);

  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      rejecting_observer));
  REQUIRE(environment.processor->QueryGuestExecutionCaptureParticipants()
              .rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::kNone);

  auto retry_observer = std::make_shared<RecordingLifecycleObserver>();
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      retry_observer));
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      retry_observer));
}

TEST_CASE("Guest ThreadState observer attachment snapshots a concurrent ready",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto observer = std::make_shared<RecordingLifecycleObserver>();
  observer->BlockNextCanDetach();

  bool attach_result = false;
  std::thread attach_worker([&] {
    attach_result =
        environment.processor->AttachGuestExecutionCaptureHostCallObserver(
            observer);
  });
  const bool can_detach_entered = observer->WaitUntilCanDetachEntered();

  auto thread_state = environment.MakeThread(0x708);
  const GuestExecutionCaptureParticipantIdentity identity = {
      thread_state->guest_execution_capture_instance_id(),
      thread_state->thread_id(),
  };
  observer->ReleaseCanDetach();
  attach_worker.join();

  REQUIRE(can_detach_entered);
  REQUIRE(attach_result);
  REQUIRE(observer->seed_call_count() == 1);
  REQUIRE(
      observer->events() ==
      std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
          {identity, GuestExecutionCaptureThreadStateLifecycleState::kReady},
      });
  thread_state.reset();
  REQUIRE(
      observer->events() ==
      std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>{
          {identity, GuestExecutionCaptureThreadStateLifecycleState::kReady},
          {identity,
           GuestExecutionCaptureThreadStateLifecycleState::kDestroying},
      });
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      observer));
}

TEST_CASE("Guest ThreadState lifecycle callback retains observer through race",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto observer = std::make_shared<RecordingLifecycleObserver>();
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      observer));
  observer->BlockNextLifecycle(
      GuestExecutionCaptureThreadStateLifecycleState::kPending);

  std::unique_ptr<ThreadState> thread_state;
  std::thread create_worker(
      [&] { thread_state = environment.MakePendingThread(0x809); });
  const bool create_entered = observer->WaitUntilLifecycleEntered();
  const bool detached_during_create =
      environment.processor->DetachGuestExecutionCaptureHostCallObserver(
          observer);

  std::weak_ptr<RecordingLifecycleObserver> observer_lifetime = observer;
  std::shared_ptr<RecordingLifecycleObserver> retained_observer = observer;
  observer.reset();
  retained_observer->ReleaseLifecycle();
  create_worker.join();

  REQUIRE(create_entered);
  REQUIRE_FALSE(detached_during_create);
  REQUIRE(observer_lifetime.lock());
  REQUIRE(thread_state);
  REQUIRE(thread_state->PublishGuestExecutionCaptureReady() ==
          GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
  thread_state.reset();
  REQUIRE(retained_observer->events().size() == 3);
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      retained_observer));
  retained_observer.reset();
  REQUIRE(observer_lifetime.expired());
}

TEST_CASE("Guest ThreadState rejects duplicate ready publication",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakePendingThread(0x90A);
  REQUIRE(thread_state->PublishGuestExecutionCaptureReady() ==
          GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
  REQUIRE(thread_state->PublishGuestExecutionCaptureReady() ==
          GuestExecutionCaptureThreadStateLifecycleDisposition::kReject);
  const GuestExecutionCaptureThreadStateRegistrySnapshot snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  REQUIRE(snapshot.rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::
              kInvalidReadyTransition);
  REQUIRE(snapshot.participants.size() == 1);
  REQUIRE(snapshot.participants[0].state ==
          GuestExecutionCaptureThreadStateLifecycleState::kReady);
  CollectingThreadStateVisitor visitor;
  REQUIRE(
      environment.processor->VisitGuestExecutionCaptureThreadStates(visitor) ==
      GuestExecutionCaptureThreadStateVisitResult::kRegistryRejected);
}

TEST_CASE("Guest capture callbacks reject Processor reentry without deadlock",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto observer = std::make_shared<ReentrantObserver>(*environment.processor);
  ScopedHostCallObserver attachment(*environment.processor, observer);
  auto thread_state = environment.MakeThread(0xA0B);

  ReentrantThreadStateVisitor visitor(*environment.processor);
  REQUIRE(
      environment.processor->VisitGuestExecutionCaptureThreadStates(visitor) ==
      GuestExecutionCaptureThreadStateVisitResult::kCompleted);
  REQUIRE(visitor.rejection() ==
          GuestExecutionCaptureThreadStateRegistryRejection::
              kObserverCallbackReentry);

  CallbackGuestFunction function(0x82000000, 0x8200001C,
                                 [](ThreadState* call_thread_state,
                                    uint32_t return_address) { return true; });
  REQUIRE(function.Call(thread_state.get(), 0x82001000));
  thread_state.reset();
  REQUIRE_FALSE(attachment.Detach());

  const auto rejections = observer->rejections();
  REQUIRE(rejections.size() == 7);
  for (GuestExecutionCaptureThreadStateRegistryRejection rejection :
       rejections) {
    REQUIRE(rejection == GuestExecutionCaptureThreadStateRegistryRejection::
                             kObserverCallbackReentry);
  }
}

TEST_CASE("Guest ThreadState registry tolerates concurrent lifecycle stress",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto observer = std::make_shared<RecordingLifecycleObserver>();
  ScopedHostCallObserver attachment(*environment.processor, observer);
  constexpr uint32_t kWorkerCount = 4;
  constexpr uint32_t kLifetimesPerWorker = 16;
  std::atomic<bool> publication_failed = false;
  std::vector<std::thread> workers;
  for (uint32_t worker_index = 0; worker_index < kWorkerCount; ++worker_index) {
    workers.emplace_back([&, worker_index] {
      for (uint32_t lifetime_index = 0; lifetime_index < kLifetimesPerWorker;
           ++lifetime_index) {
        auto thread_state =
            environment.MakePendingThread(0xB000 + worker_index);
        thread_state->context()->r[31] = lifetime_index;
        if (thread_state->PublishGuestExecutionCaptureReady() !=
            GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept) {
          publication_failed = true;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  REQUIRE_FALSE(publication_failed.load());
  const GuestExecutionCaptureThreadStateRegistrySnapshot snapshot =
      environment.processor->QueryGuestExecutionCaptureParticipants();
  REQUIRE(snapshot.rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::kNone);
  REQUIRE(snapshot.participants.empty());

  const auto events = observer->events();
  REQUIRE(events.size() == kWorkerCount * kLifetimesPerWorker * 3);
  std::map<uint64_t,
           std::vector<GuestExecutionCaptureThreadStateLifecycleState>>
      states_by_instance;
  for (const GuestExecutionCaptureThreadStateLifecycleEvent& event : events) {
    states_by_instance[event.participant.capture_instance_id].push_back(
        event.state);
  }
  REQUIRE(states_by_instance.size() == kWorkerCount * kLifetimesPerWorker);
  for (const auto& instance : states_by_instance) {
    REQUIRE(instance.second ==
            std::vector<GuestExecutionCaptureThreadStateLifecycleState>{
                GuestExecutionCaptureThreadStateLifecycleState::kPending,
                GuestExecutionCaptureThreadStateLifecycleState::kReady,
                GuestExecutionCaptureThreadStateLifecycleState::kDestroying,
            });
  }
  REQUIRE(attachment.Detach());
}

TEST_CASE("Guest ThreadState locked visitor excludes destruction",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x606);
  BlockingThreadStateVisitor visitor;
  GuestExecutionCaptureThreadStateVisitResult visit_result =
      GuestExecutionCaptureThreadStateVisitResult::kRegistryRejected;
  std::thread visit_worker([&] {
    visit_result =
        environment.processor->VisitGuestExecutionCaptureThreadStates(visitor);
  });
  const bool visitor_entered = visitor.WaitUntilEntered();

  std::atomic<bool> destroyer_reached_registry_gate = false;
  std::atomic<bool> destroy_finished = false;
  GuestExecutionCaptureRegistryTestAccess::SetThreadStateDestructionGateSignal(
      *environment.processor, &destroyer_reached_registry_gate);
  std::thread destroy_worker([&] {
    thread_state.reset();
    destroy_finished.store(true, std::memory_order_release);
  });

  const auto gate_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!destroyer_reached_registry_gate.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < gate_deadline) {
    std::this_thread::yield();
  }
  const bool destroyer_reached_registry_gate_observed =
      destroyer_reached_registry_gate.load(std::memory_order_acquire);
  const bool destroy_finished_before_visitor_release =
      destroy_finished.load(std::memory_order_acquire);

  visitor.Release();
  visit_worker.join();
  destroy_worker.join();
  GuestExecutionCaptureRegistryTestAccess::SetThreadStateDestructionGateSignal(
      *environment.processor, nullptr);

  REQUIRE(visitor_entered);
  REQUIRE(destroyer_reached_registry_gate_observed);
  REQUIRE_FALSE(destroy_finished_before_visitor_release);
  REQUIRE(visit_result ==
          GuestExecutionCaptureThreadStateVisitResult::kCompleted);
  REQUIRE(visitor.context_valid());
  REQUIRE(destroy_finished.load(std::memory_order_acquire));
  REQUIRE(environment.processor->QueryGuestExecutionCaptureParticipants()
              .participants.empty());
}

TEST_CASE("Guest ThreadState registration publishes current capture control",
          "[guest-execution-capture]") {
  CaptureTestEnvironment environment;
  constexpr uint32_t kOldRoot = 0x82060600;
  constexpr uint32_t kPublishedRoot = 0x82060700;
  constexpr uint8_t kPublishedEventMask =
      kGuestInvocationCaptureRootEvent | kGuestInvocationCaptureWriteEvent;
  RegistryInvocationCaptureSink old_sink;
  old_sink.root = kOldRoot;
  old_sink.mask = kGuestInvocationCaptureRootEvent;
  RegistryInvocationCaptureSink new_sink;
  new_sink.root = kPublishedRoot;
  new_sink.mask = kPublishedEventMask;
  environment.processor->set_guest_invocation_capture_sink(&old_sink);
  auto existing_thread = environment.MakeThread(0x606);
  const uint64_t old_control =
      std::atomic_ref<uint64_t>(
          existing_thread->context()->guest_invocation_capture_control)
          .load(std::memory_order_acquire);
  std::unique_ptr<ThreadState> thread_state;
  std::atomic<bool> replacement_finished = false;
  bool replacement_succeeded = false;
  bool masks_disabled_during_transition = false;
  bool replacement_finished_while_leased = false;
  uint64_t registered_control_during_transition = 0;
  std::thread replacement;
  {
    auto lease = environment.processor->AcquireGuestInvocationCaptureSink(
        old_control, kGuestInvocationCaptureRootEvent,
        existing_thread->context());
    REQUIRE(lease);

    std::atomic<bool> registration_reached = false;
    std::atomic<bool> registration_release = false;
    GuestExecutionCaptureRegistryTestAccess::SetThreadStateRegistrationGate(
        *environment.processor, &registration_reached, &registration_release);
    std::thread constructor([&] {
      thread_state =
          std::make_unique<ThreadState>(environment.processor.get(), 0x607);
    });
    const auto gate_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!registration_reached.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < gate_deadline) {
      std::this_thread::yield();
    }
    const bool registration_reached_observed =
        registration_reached.load(std::memory_order_acquire);
    if (!registration_reached_observed) {
      registration_release.store(true, std::memory_order_release);
      constructor.join();
      GuestExecutionCaptureRegistryTestAccess::SetThreadStateRegistrationGate(
          *environment.processor, nullptr, nullptr);
    }
    REQUIRE(registration_reached_observed);

    replacement = std::thread([&] {
      replacement_succeeded =
          environment.processor->TrySetGuestInvocationCaptureSink(&old_sink,
                                                                  &new_sink);
      replacement_finished.store(true, std::memory_order_release);
    });
    const auto disable_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (GuestInvocationCaptureControlEventMask(
               std::atomic_ref<uint64_t>(
                   existing_thread->context()->guest_invocation_capture_control)
                   .load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < disable_deadline) {
      std::this_thread::yield();
    }
    masks_disabled_during_transition = !GuestInvocationCaptureControlEventMask(
        std::atomic_ref<uint64_t>(
            existing_thread->context()->guest_invocation_capture_control)
            .load(std::memory_order_acquire));
    registration_release.store(true, std::memory_order_release);
    constructor.join();
    registered_control_during_transition =
        std::atomic_ref<uint64_t>(
            thread_state->context()->guest_invocation_capture_control)
            .load(std::memory_order_acquire);
    replacement_finished_while_leased =
        replacement_finished.load(std::memory_order_acquire);
    GuestExecutionCaptureRegistryTestAccess::SetThreadStateRegistrationGate(
        *environment.processor, nullptr, nullptr);
  }
  replacement.join();

  REQUIRE(thread_state);
  REQUIRE(masks_disabled_during_transition);
  REQUIRE(GuestInvocationCaptureControlEventMask(
              registered_control_during_transition) == 0);
  REQUIRE_FALSE(replacement_finished_while_leased);
  REQUIRE(replacement_finished.load(std::memory_order_acquire));
  REQUIRE(replacement_succeeded);
  const uint64_t published_control =
      std::atomic_ref<uint64_t>(
          thread_state->context()->guest_invocation_capture_control)
          .load(std::memory_order_acquire);
  REQUIRE(GuestInvocationCaptureControlRoot(published_control) ==
          kPublishedRoot);
  REQUIRE(GuestInvocationCaptureControlEventMask(published_control) ==
          kPublishedEventMask);
  REQUIRE(GuestInvocationCaptureControlGeneration(published_control) !=
          GuestInvocationCaptureControlGeneration(old_control));
  REQUIRE(std::atomic_ref<uint64_t>(
              existing_thread->context()->guest_invocation_capture_control)
              .load(std::memory_order_acquire) == published_control);
  REQUIRE(environment.processor->TrySetGuestInvocationCaptureSink(&new_sink,
                                                                  nullptr));
  thread_state.reset();
  existing_thread.reset();
}

TEST_CASE("Guest JIT safepoint requests are independent and consumed once",
          "[guest-execution-capture][jit-safepoint]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x707);
  thread_state->context()->preempt_requested = 0x5A;
  auto observer = std::make_shared<JitSafepointObserver>();
  ScopedHostCallObserver attachment(*environment.processor, observer);

  RequestJitSafepointVisitor visitor;
  REQUIRE(
      environment.processor->VisitGuestExecutionCaptureThreadStates(visitor) ==
      GuestExecutionCaptureThreadStateVisitResult::kCompleted);
  REQUIRE(visitor.request_count() == 1);
  REQUIRE(thread_state->IsGuestExecutionCaptureJitSafepointRequested());
  REQUIRE(thread_state->context()->preempt_requested == 0x5A);

  REQUIRE(HandleGuestExecutionCaptureJitSafepoint(thread_state->context(),
                                                  0x82000100) ==
          GuestExecutionCaptureJitSafepointResult::kDelivered);
  REQUIRE_FALSE(thread_state->IsGuestExecutionCaptureJitSafepointRequested());
  REQUIRE(thread_state->context()->preempt_requested == 0x5A);
  REQUIRE(HandleGuestExecutionCaptureJitSafepoint(thread_state->context(),
                                                  0x82000104) ==
          GuestExecutionCaptureJitSafepointResult::kNotRequested);
  REQUIRE(observer->guest_addresses() == std::vector<uint32_t>{0x82000100});
  REQUIRE(observer->requests_consumed() == std::vector<bool>{true});

  REQUIRE(attachment.Detach());
  thread_state.reset();
}

TEST_CASE("Guest JIT safepoint rejection is sticky and detachable",
          "[guest-execution-capture][jit-safepoint]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x708);
  auto observer = std::make_shared<JitSafepointObserver>();
  observer->RejectNext();
  ScopedHostCallObserver attachment(*environment.processor, observer);

  thread_state->RequestGuestExecutionCaptureJitSafepoint();
  REQUIRE(HandleGuestExecutionCaptureJitSafepoint(thread_state->context(),
                                                  0x82000200) ==
          GuestExecutionCaptureJitSafepointResult::kObserverRejected);
  REQUIRE(environment.processor->QueryGuestExecutionCaptureParticipants()
              .rejection == GuestExecutionCaptureThreadStateRegistryRejection::
                                kObserverRejectedJitSafepoint);

  thread_state->RequestGuestExecutionCaptureJitSafepoint();
  REQUIRE(HandleGuestExecutionCaptureJitSafepoint(thread_state->context(),
                                                  0x82000204) ==
          GuestExecutionCaptureJitSafepointResult::kRegistryRejected);
  REQUIRE_FALSE(thread_state->IsGuestExecutionCaptureJitSafepointRequested());
  REQUIRE(observer->guest_addresses() == std::vector<uint32_t>{0x82000200});

  REQUIRE(attachment.Detach());
  REQUIRE(environment.processor->QueryGuestExecutionCaptureParticipants()
              .rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::kNone);
  thread_state.reset();
}

TEST_CASE("Guest JIT safepoint callback reentry preserves the nested request",
          "[guest-execution-capture][jit-safepoint]") {
  CaptureTestEnvironment environment;
  auto thread_state = environment.MakeThread(0x709);
  auto observer = std::make_shared<JitSafepointObserver>();
  observer->ReenterNext();
  ScopedHostCallObserver attachment(*environment.processor, observer);

  thread_state->RequestGuestExecutionCaptureJitSafepoint();
  REQUIRE(HandleGuestExecutionCaptureJitSafepoint(thread_state->context(),
                                                  0x82000300) ==
          GuestExecutionCaptureJitSafepointResult::kDelivered);
  REQUIRE(observer->reentry_result() ==
          GuestExecutionCaptureJitSafepointResult::kObserverCallbackReentry);
  REQUIRE(observer->reentry_request_retained());
  REQUIRE(thread_state->IsGuestExecutionCaptureJitSafepointRequested());

  REQUIRE(HandleGuestExecutionCaptureJitSafepoint(thread_state->context(),
                                                  0x82000308) ==
          GuestExecutionCaptureJitSafepointResult::kDelivered);
  REQUIRE_FALSE(thread_state->IsGuestExecutionCaptureJitSafepointRequested());
  REQUIRE(observer->guest_addresses() ==
          std::vector<uint32_t>{0x82000300, 0x82000308});

  REQUIRE(attachment.Detach());
  thread_state.reset();
}

TEST_CASE("Guest JIT safepoint lease blocks only its ThreadState destruction",
          "[guest-execution-capture][jit-safepoint]") {
  CaptureTestEnvironment environment;
  auto blocked_thread = environment.MakeThread(0x70A);
  auto other_thread = environment.MakeThread(0x70B);
  ppc::PPCContext* blocked_context = blocked_thread->context();
  auto observer = std::make_shared<JitSafepointObserver>(
      blocked_thread->guest_execution_capture_instance_id());
  ScopedHostCallObserver attachment(*environment.processor, observer);

  blocked_thread->RequestGuestExecutionCaptureJitSafepoint();
  GuestExecutionCaptureJitSafepointResult blocked_result =
      GuestExecutionCaptureJitSafepointResult::kInvalidContext;
  std::thread callback_worker([&] {
    blocked_result =
        HandleGuestExecutionCaptureJitSafepoint(blocked_context, 0x82000400);
  });
  const bool callback_blocked = observer->WaitUntilBlocked();

  std::atomic<bool> destroyer_reached_registry_gate = false;
  std::atomic<bool> destroy_finished = false;
  GuestExecutionCaptureRegistryTestAccess::SetThreadStateDestructionGateSignal(
      *environment.processor, &destroyer_reached_registry_gate);
  std::thread destroy_worker;
  if (callback_blocked) {
    destroy_worker = std::thread([&] {
      blocked_thread.reset();
      destroy_finished.store(true, std::memory_order_release);
    });
  }

  const auto gate_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (callback_blocked &&
         !destroyer_reached_registry_gate.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < gate_deadline) {
    std::this_thread::yield();
  }
  const bool destroyer_reached_registry_gate_observed =
      destroyer_reached_registry_gate.load(std::memory_order_acquire);
  const bool destroy_finished_while_callback_blocked =
      destroy_finished.load(std::memory_order_acquire);

  other_thread->RequestGuestExecutionCaptureJitSafepoint();
  const GuestExecutionCaptureJitSafepointResult other_result =
      HandleGuestExecutionCaptureJitSafepoint(other_thread->context(),
                                              0x82000404);
  const bool both_callbacks_arrived = observer->WaitForCallbackCount(2);

  observer->ReleaseBlocked();
  callback_worker.join();
  if (destroy_worker.joinable()) {
    destroy_worker.join();
  }
  GuestExecutionCaptureRegistryTestAccess::SetThreadStateDestructionGateSignal(
      *environment.processor, nullptr);

  REQUIRE(callback_blocked);
  REQUIRE(destroyer_reached_registry_gate_observed);
  REQUIRE_FALSE(destroy_finished_while_callback_blocked);
  REQUIRE(other_result == GuestExecutionCaptureJitSafepointResult::kDelivered);
  REQUIRE(both_callbacks_arrived);
  REQUIRE(blocked_result ==
          GuestExecutionCaptureJitSafepointResult::kDelivered);
  REQUIRE(destroy_finished.load(std::memory_order_acquire));
  REQUIRE(observer->guest_addresses() ==
          std::vector<uint32_t>{0x82000400, 0x82000404});

  REQUIRE(attachment.Detach());
  other_thread.reset();
}

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
  REQUIRE_FALSE(attachment.Detach());
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
  REQUIRE_FALSE(attachment.Detach());
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
  REQUIRE_FALSE(attachment.Detach());
}

TEST_CASE("Guest host-call observer is permanent after dispatch",
          "[guest-execution-capture]") {
  std::weak_ptr<BlockingBeginObserver> observer_lifetime;
  {
    CaptureTestEnvironment environment;
    auto thread_state = environment.MakeThread(0x606);
    auto observer = std::make_shared<BlockingBeginObserver>();
    auto stale_observer =
        std::make_shared<GuestExecutionCaptureHostCallRoster>();
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
        [](ThreadState* call_thread_state, uint32_t return_address) {
          return true;
        });
    bool call_result = false;
    std::thread worker(
        [&] { call_result = function.Call(thread_state.get(), 0x82031000); });
    const bool begin_entered = observer->WaitUntilBeginEntered();
    const bool detached_during_begin =
        environment.processor->DetachGuestExecutionCaptureHostCallObserver(
            observer);
    observer_lifetime = observer;
    std::shared_ptr<BlockingBeginObserver> retained_observer = observer;
    observer.reset();
    retained_observer->ReleaseBegin();
    worker.join();

    REQUIRE(begin_entered);
    REQUIRE_FALSE(detached_during_begin);
    REQUIRE(call_result);
    REQUIRE(retained_observer->outcome() ==
            GuestExecutionCaptureHostCallOutcome::kReturnedToHost);
    REQUIRE_FALSE(
        environment.processor->DetachGuestExecutionCaptureHostCallObserver(
            retained_observer));
    retained_observer.reset();
    REQUIRE_FALSE(observer_lifetime.expired());
  }
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
    auto thread_state = environment.MakeThread(0x909);
    auto observer = std::make_shared<GuestExecutionCaptureHostCallRoster>();
    observer_lifetime = observer;
    REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
        observer));
    observer.reset();
    REQUIRE_FALSE(observer_lifetime.expired());
    thread_state.reset();
    REQUIRE(environment.processor->QueryGuestExecutionCaptureParticipants()
                .participants.empty());
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
  REQUIRE_FALSE(attachment.Detach());
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
