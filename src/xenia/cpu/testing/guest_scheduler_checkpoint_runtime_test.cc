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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <string>
#include <thread>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xiocompletion.h"
#include "xenia/kernel/xsocket.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

struct GuestSchedulerCheckpointRuntimeInspection {
  GuestSchedulerCheckpointHeldPhase phase =
      GuestSchedulerCheckpointHeldPhase::kEmpty;
  int cpu = -1;
  bool found = false;
  bool queued = false;
  bool suspended = false;
  bool running = false;
  uint32_t checkpoint_jit_safepoint_pc = 0;
};

class GuestSchedulerCheckpointRuntimeTestAccess final {
 public:
  static void SetArrivalHook(GuestScheduler& scheduler,
                             GuestScheduler::CheckpointTestHook hook,
                             void* context) {
    scheduler.checkpoint_arrival_test_context_.store(context,
                                                     std::memory_order_release);
    scheduler.checkpoint_arrival_test_hook_.store(hook,
                                                  std::memory_order_release);
  }

  static void SetStartupHook(GuestScheduler& scheduler,
                             GuestScheduler::CheckpointTestHook hook,
                             void* context) {
    scheduler.checkpoint_startup_test_context_.store(context,
                                                     std::memory_order_release);
    scheduler.checkpoint_startup_test_hook_.store(hook,
                                                  std::memory_order_release);
  }

  static void SetSnapshotHook(GuestScheduler& scheduler,
                              GuestScheduler::CheckpointTestHook hook,
                              void* context) {
    scheduler.checkpoint_snapshot_test_context_.store(
        context, std::memory_order_release);
    scheduler.checkpoint_snapshot_test_hook_.store(hook,
                                                   std::memory_order_release);
  }

  static void SetDiscardHook(GuestScheduler& scheduler,
                             GuestScheduler::CheckpointTestHook hook,
                             void* context) {
    scheduler.checkpoint_discard_test_context_.store(context,
                                                     std::memory_order_release);
    scheduler.checkpoint_discard_test_hook_.store(hook,
                                                  std::memory_order_release);
  }

  static void SetTerminateHook(GuestScheduler& scheduler,
                               GuestScheduler::CheckpointTestHook hook,
                               void* context) {
    scheduler.checkpoint_terminate_test_context_.store(
        context, std::memory_order_release);
    scheduler.checkpoint_terminate_test_hook_.store(hook,
                                                    std::memory_order_release);
  }

  static void SetYieldHook(GuestScheduler& scheduler,
                           GuestScheduler::CheckpointTestHook hook,
                           void* context) {
    scheduler.checkpoint_yield_test_context_.store(context,
                                                   std::memory_order_release);
    scheduler.checkpoint_yield_test_hook_.store(hook,
                                                std::memory_order_release);
  }

  static GuestSchedulerCheckpointBarrierSnapshot Snapshot(
      GuestScheduler& scheduler) {
    return scheduler.checkpoint_barrier_.snapshot();
  }

  static GuestSchedulerCheckpointRuntimeInspection InspectHeld(
      GuestScheduler& scheduler, XThread* thread) {
    GuestSchedulerCheckpointRuntimeInspection result;
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    for (int cpu = 0; cpu < GuestScheduler::kMaxCpus; ++cpu) {
      const auto& held = scheduler.checkpoint_held_[cpu];
      if (held.thread != thread) {
        continue;
      }
      result.phase = held.state.phase();
      result.cpu = cpu;
      result.found = true;
      result.queued = thread->scheduler_links().queued;
      result.suspended = thread->scheduler_links().suspended;
      result.running = thread->scheduler_links().running;
      result.checkpoint_jit_safepoint_pc =
          thread->scheduler_links().checkpoint_jit_safepoint_pc;
      break;
    }
    return result;
  }

  static GuestSchedulerCheckpointRuntimeInspection InspectThread(
      GuestScheduler& scheduler, XThread* thread) {
    GuestSchedulerCheckpointRuntimeInspection result;
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    result.found = true;
    result.cpu = thread->scheduler_links().cpu;
    result.queued = thread->scheduler_links().queued;
    result.suspended = thread->scheduler_links().suspended;
    result.running = thread->scheduler_links().running;
    result.checkpoint_jit_safepoint_pc =
        thread->scheduler_links().checkpoint_jit_safepoint_pc;
    return result;
  }

  static int QueuedPriority(GuestScheduler& scheduler, XThread* thread) {
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    return thread->scheduler_links().queued
               ? thread->scheduler_links().queued_prio
               : -1;
  }

  static int BlockedMaxPriority(GuestScheduler& scheduler, int cpu) {
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    return cpu >= 0 && cpu < GuestScheduler::kMaxCpus
               ? scheduler.cpus_[cpu].max_blocked_prio
               : -1;
  }

  static uint8_t DispatchThreadMask(const GuestScheduler& scheduler) {
    uint8_t mask = 0;
    for (int cpu = 0; cpu < GuestScheduler::kMaxCpus; ++cpu) {
      if (scheduler.cpus_[cpu].host_thread) {
        mask |= uint8_t{1} << cpu;
      }
    }
    return mask;
  }

  static bool DispatchReady(const GuestScheduler& scheduler) {
    return scheduler.checkpoint_dispatch_ready_.load(std::memory_order_acquire);
  }

  static void RequestSchedulerSafepoint(GuestScheduler& scheduler,
                                        XThread* thread, bool raise_wake) {
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    auto& links = thread->scheduler_links();
    links.scheduler_safepoint_requested.store(true, std::memory_order_release);
    if (raise_wake) {
      std::atomic_ref<uint8_t>(
          thread->thread_state()->context()->preempt_requested)
          .store(1, std::memory_order_release);
    }
    scheduler.EmitCaptureLocked(GuestSchedulerCaptureEventKind::kPreemptRequest,
                                thread, links.cpu, -1,
                                GuestSchedulerCaptureReason::kWake, 0, 0);
  }

  static bool SchedulerSafepointPending(XThread* thread) {
    return thread->scheduler_links().scheduler_safepoint_requested.load(
        std::memory_order_acquire);
  }
};

class GuestSchedulerCaptureWaitRuntimeTestAccess final {
 public:
  static void SetRereadyDecisionHook(
      GuestScheduler& scheduler, GuestScheduler::RereadyDecisionTestHook hook,
      void* context) {
    scheduler.reready_decision_test_context_.store(context,
                                                   std::memory_order_release);
    scheduler.reready_decision_test_hook_.store(hook,
                                                std::memory_order_release);
  }

  static bool IsBlocked(GuestScheduler& scheduler, XThread* thread) {
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    return thread->scheduler_links().blocked;
  }

  static bool ArmRereadyDecisionHookAndForce(
      GuestScheduler& scheduler, XThread* thread,
      GuestScheduler::RereadyDecisionTestHook hook, void* context) {
    int cpu_index = -1;
    {
      std::lock_guard<std::mutex> lock(scheduler.lock_);
      if (!thread->scheduler_links().blocked) {
        return false;
      }
      scheduler.reready_decision_test_context_.store(context,
                                                     std::memory_order_release);
      scheduler.reready_decision_test_hook_.store(hook,
                                                  std::memory_order_release);
      cpu_index = thread->scheduler_links().cpu;
      auto& cpu = scheduler.cpus_[cpu_index];
      cpu.next_force_repoll_ms = 0;
      cpu.next_timed_repoll_ms = 0;
      cpu.repoll_now.store(true, std::memory_order_relaxed);
    }
    if (scheduler.cpus_[cpu_index].ready_event) {
      scheduler.cpus_[cpu_index].ready_event->Set();
    }
    return true;
  }
};

namespace testing {
namespace {

using namespace std::chrono_literals;

using HeldPhase = GuestSchedulerCheckpointHeldPhase;
using Kind = GuestSchedulerCaptureEventKind;
using Rejection = GuestSchedulerCheckpointBarrierRejection;
using ReleasePolicy = GuestSchedulerCheckpointReleasePolicy;
using ResumeKind = GuestSchedulerCheckpointResumeKind;
using RosterScope = GuestSchedulerCheckpointRosterScope;

constexpr uint32_t kSafepointPc = 0x82001000;
// The emitter names the function holding the safepoint; a PC cannot name it.
constexpr uint32_t kSafepointFunction = 0x82000000;
constexpr uint32_t kCpu0CreationFlags = uint32_t{1} << 24;
constexpr uint32_t kCpu1CreationFlags = uint32_t{2} << 24;

constexpr uint32_t kReplayCodeBase = 0x82050000;
constexpr uint32_t kReplayResumeA = kReplayCodeBase + 0x20;
constexpr uint32_t kReplayCallerA = kReplayCodeBase + 0x100;
constexpr uint32_t kReplayFinalA = kReplayCallerA + 20;
constexpr uint32_t kReplayResumeB = kReplayCodeBase + 0x220;
constexpr uint32_t kReplayCallerB = kReplayCodeBase + 0x300;
constexpr uint32_t kReplayFinalB = kReplayCallerB + 28;
constexpr uint32_t kReplayOuterReturnA = 0x83001000;
constexpr uint32_t kReplayOuterReturnB = 0x83002000;

constexpr uint32_t kSharedTokenA = 0;
constexpr uint32_t kSharedTokenB = 4;
constexpr uint32_t kSharedReachedA = 8;
constexpr uint32_t kSharedReachedB = 12;
constexpr uint32_t kSharedPoisonGate = 16;
constexpr uint32_t kSharedPoisonA = 20;
constexpr uint32_t kSharedPoisonB = 24;

uint32_t EncodeConditionalBranch(uint32_t opcode, uint32_t from, uint32_t to) {
  const int32_t displacement = static_cast<int32_t>(to - from);
  return opcode | (static_cast<uint32_t>(displacement) & 0x0000FFFCu);
}

void StoreGuestInstruction(Memory* memory, uint32_t address,
                           uint32_t instruction) {
  store_and_swap<uint32_t>(memory->TranslateVirtual(address), instruction);
}

bool WaitForGuestWord(Memory* memory, uint32_t address, uint32_t expected,
                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (load_and_swap<uint32_t>(memory->TranslateVirtual(address)) ==
        expected) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool InstallReplayGuestCode(Emulator* emulator) {
  Memory* memory = emulator->memory();

  StoreGuestInstruction(memory, kReplayResumeA, 0x38630001u);
  StoreGuestInstruction(memory, kReplayResumeA + 4, 0x90640000u);
  StoreGuestInstruction(memory, kReplayResumeA + 8, 0x4E800020u);

  StoreGuestInstruction(memory, kReplayCallerA, 0x80A40004u);
  StoreGuestInstruction(memory, kReplayCallerA + 4, 0x2C050001u);
  StoreGuestInstruction(
      memory, kReplayCallerA + 8,
      EncodeConditionalBranch(0x40820000u, kReplayCallerA + 8, kReplayCallerA));
  StoreGuestInstruction(memory, kReplayCallerA + 12, 0x38C00001u);
  StoreGuestInstruction(memory, kReplayCallerA + 16, 0x90C40008u);
  StoreGuestInstruction(memory, kReplayFinalA, 0x80E40010u);
  StoreGuestInstruction(memory, kReplayFinalA + 4, 0x2C070000u);
  StoreGuestInstruction(
      memory, kReplayFinalA + 8,
      EncodeConditionalBranch(0x41820000u, kReplayFinalA + 8, kReplayFinalA));
  StoreGuestInstruction(memory, kReplayFinalA + 12, 0x390000A1u);
  StoreGuestInstruction(memory, kReplayFinalA + 16, 0x91040014u);
  StoreGuestInstruction(memory, kReplayFinalA + 20, 0x7D6803A6u);
  StoreGuestInstruction(memory, kReplayFinalA + 24, 0x4E800020u);

  StoreGuestInstruction(memory, kReplayResumeB, 0x38630002u);
  StoreGuestInstruction(memory, kReplayResumeB + 4, 0x4E800020u);

  StoreGuestInstruction(memory, kReplayCallerB, 0x80A40000u);
  StoreGuestInstruction(memory, kReplayCallerB + 4, 0x2C050001u);
  StoreGuestInstruction(
      memory, kReplayCallerB + 8,
      EncodeConditionalBranch(0x40820000u, kReplayCallerB + 8, kReplayCallerB));
  StoreGuestInstruction(memory, kReplayCallerB + 12, 0x38C00001u);
  StoreGuestInstruction(memory, kReplayCallerB + 16, 0x90C40004u);
  StoreGuestInstruction(memory, kReplayCallerB + 20, 0x38C00001u);
  StoreGuestInstruction(memory, kReplayCallerB + 24, 0x90C4000Cu);
  StoreGuestInstruction(memory, kReplayFinalB, 0x80E40010u);
  StoreGuestInstruction(memory, kReplayFinalB + 4, 0x2C070000u);
  StoreGuestInstruction(
      memory, kReplayFinalB + 8,
      EncodeConditionalBranch(0x41820000u, kReplayFinalB + 8, kReplayFinalB));
  StoreGuestInstruction(memory, kReplayFinalB + 12, 0x390000B2u);
  StoreGuestInstruction(memory, kReplayFinalB + 16, 0x91040018u);
  StoreGuestInstruction(memory, kReplayFinalB + 20, 0x7D6803A6u);
  StoreGuestInstruction(memory, kReplayFinalB + 24, 0x4E800020u);

  auto module = std::make_unique<cpu::RawModule>(emulator->processor());
  module->set_name("scheduler-backed-continuous-replay-test");
  module->SetAddressRange(kReplayCodeBase, 0x1000);
  return emulator->processor()->AddModule(std::move(module));
}

class BlockingGate final {
 public:
  static void Hook(void* context) {
    static_cast<BlockingGate*>(context)->Enter();
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  void Enter() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return released_; });
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const std::string prefix =
        "xenia-scheduler-checkpoint-runtime-test-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint32_t suffix = 0; suffix < 100; ++suffix) {
      std::error_code error;
      auto candidate = std::filesystem::temp_directory_path() /
                       (prefix + "-" + std::to_string(suffix));
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        std::filesystem::create_directories(path_ / "storage", error);
        std::filesystem::create_directories(path_ / "content", error);
        std::filesystem::create_directories(path_ / "cache", error);
        return;
      }
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class SchedulerEnvironment final {
 public:
  SchedulerEnvironment()
      : old_guest_scheduler_(cvars::guest_scheduler),
        old_quantum_us_(cvars::guest_scheduler_quantum_us) {
    cvars::guest_scheduler = true;
    cvars::guest_scheduler_quantum_us = 0;
    if (directory_.path().empty()) {
      return;
    }
    emulator_ = std::make_unique<Emulator>(
        "xenia-cpu-tests", directory_.path() / "storage",
        directory_.path() / "content", directory_.path() / "cache");
    setup_status_ = emulator_->Setup(nullptr, nullptr, false, {}, {}, {});
  }

  ~SchedulerEnvironment() {
    if (emulator_) {
      emulator_->Shutdown();
      emulator_.reset();
    }
    cvars::guest_scheduler_quantum_us = old_quantum_us_;
    cvars::guest_scheduler = old_guest_scheduler_;
  }

  bool ready() const {
    return emulator_ && setup_status_ == X_STATUS_SUCCESS &&
           emulator_->kernel_state();
  }

  Emulator* emulator() const { return emulator_.get(); }
  GuestScheduler* scheduler() const {
    return ready() ? emulator_->kernel_state()->guest_scheduler() : nullptr;
  }

 private:
  bool old_guest_scheduler_ = false;
  uint32_t old_quantum_us_ = 0;
  TemporaryDirectory directory_;
  std::unique_ptr<Emulator> emulator_;
  X_STATUS setup_status_ = X_STATUS_UNSUCCESSFUL;
};

struct FiberControl {
  bool WaitForStart(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout,
                              [this]() { return started.load(); });
  }

  bool WaitForSafepointReturn(std::chrono::milliseconds timeout) {
    return WaitForSafepointReturns(1, timeout);
  }

  bool WaitForSafepointReturns(uint32_t count,
                               std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [this, count]() {
      return safepoint_returns.load(std::memory_order_acquire) >= count;
    });
  }

  bool WaitForExit(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout,
                              [this]() { return exit_called.load(); });
  }

  void Notify() { condition.notify_all(); }

  std::atomic<bool> started{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> exit_called{false};
  std::atomic<bool> missing_handler{false};
  std::atomic<uint32_t> safepoint_returns{0};
  std::mutex mutex;
  std::condition_variable condition;
};

struct WaitInventoryControl {
  bool WaitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [this]() {
      return completed.load(std::memory_order_acquire);
    });
  }

  void Complete() {
    completed.store(true, std::memory_order_release);
    condition.notify_all();
  }

  std::atomic<bool> completed{false};
  bool single_wait_timed_out = false;
  bool multi_any_timed_out = false;
  bool multi_all_timed_out = false;
  bool signal_and_wait_timed_out = false;
  bool delay_completed = false;
  bool io_completion_timed_out = false;
  bool socket_initialized = false;
  bool socket_bound = false;
  bool socket_received = false;
  std::mutex mutex;
  std::condition_variable condition;
};

struct InstructionCoverageControl {
  bool WaitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [this]() { return completed; });
  }

  void Complete() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      completed = true;
    }
    condition.notify_all();
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
};

class SwitchOutGateObserver final : public GuestSchedulerCaptureObserver {
 public:
  bool OnSchedulerEvent(
      const GuestSchedulerCaptureEvent& event) noexcept override {
    const uint32_t target_thread_id =
        target_thread_id_.load(std::memory_order_acquire);
    if (event.kind != Kind::kSwitchOut || !target_thread_id ||
        event.guest_thread_id != target_thread_id || entered_.exchange(true)) {
      return true;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_visible_ = true;
    }
    condition_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return released_; });
    return true;
  }

  bool OnSchedulerSignalWitness(
      const GuestSchedulerCaptureSignalWitness&) noexcept override {
    return true;
  }

  bool CanDetach() const noexcept override { return true; }

  void SetTargetThread(uint32_t thread_id) {
    target_thread_id_.store(thread_id, std::memory_order_release);
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this]() { return entered_visible_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::atomic<uint32_t> target_thread_id_{0};
  std::atomic<bool> entered_{false};
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_visible_ = false;
  bool released_ = false;
};

class InstructionCoverageRuntimeThread final : public XThread {
 public:
  InstructionCoverageRuntimeThread(KernelState* kernel_state,
                                   InstructionCoverageControl* control)
      : XThread(kernel_state, 64 * 1024, 0, kSafepointPc, 0, kCpu0CreationFlags,
                true, false, kernel_state->GetSystemProcess()),
        control_(control) {}

  void Execute() override {
    auto* context = thread_state()->context();
    std::atomic_ref<uint64_t*>(
        context->guest_execution_session_instruction_counter)
        .store(&context->guest_execution_session_instruction_count,
               std::memory_order_release);
    std::atomic_ref<uint64_t>(
        context->guest_execution_session_instruction_count)
        .store(7, std::memory_order_relaxed);
    kernel_state()->guest_scheduler()->YieldCurrentThread(false);
    std::atomic_ref<uint64_t>(
        context->guest_execution_session_instruction_count)
        .store(11, std::memory_order_relaxed);
    kernel_state()->guest_scheduler()->YieldCurrentThread(false);
    std::atomic_ref<uint64_t>(
        context->guest_execution_session_instruction_count)
        .store(13, std::memory_order_relaxed);
    control_->Complete();
    Exit(0);
  }

 private:
  InstructionCoverageControl* control_;
};

class DeferredInstructionCoverageRuntimeThread final : public XThread {
 public:
  DeferredInstructionCoverageRuntimeThread(KernelState* kernel_state,
                                           InstructionCoverageControl* control)
      : XThread(kernel_state, 64 * 1024, 0, kSafepointPc, 0, kCpu0CreationFlags,
                true, false, kernel_state->GetSystemProcess()),
        control_(control) {}

  void Execute() override {
    auto* context = thread_state()->context();
    std::atomic_ref<uint64_t*>(
        context->guest_execution_session_instruction_counter)
        .store(&context->guest_execution_session_instruction_count,
               std::memory_order_release);
    std::atomic_ref<uint64_t>(
        context->guest_execution_session_instruction_count)
        .store(5, std::memory_order_relaxed);
    scheduler_links().scheduler_safepoint_requested.store(
        true, std::memory_order_release);
    {
      auto global_lock = xe::global_critical_region::AcquireDirect();
      cpu::backend::preempt_yield_handler(context, kSafepointPc,
                                          kSafepointFunction);
    }

    std::atomic_ref<uint64_t>(
        context->guest_execution_session_instruction_count)
        .fetch_add(7, std::memory_order_relaxed);
    std::atomic_ref<uint8_t>(context->preempt_requested)
        .store(0, std::memory_order_release);
    cpu::backend::preempt_yield_handler(context, kSafepointPc,
                                        kSafepointFunction);
    control_->Complete();
    Exit(0);
  }

 private:
  InstructionCoverageControl* control_;
};

class WaitInventoryRuntimeThread final : public XThread {
 public:
  WaitInventoryRuntimeThread(KernelState* kernel_state,
                             WaitInventoryControl* control)
      : XThread(kernel_state, 64 * 1024, 0, kSafepointPc, 0, kCpu0CreationFlags,
                true, false, kernel_state->GetSystemProcess()),
        control_(control) {}

  void Execute() override {
    constexpr int64_t kRelativeTimeoutTicks = -50'000;
    uint64_t timeout = static_cast<uint64_t>(kRelativeTimeoutTicks);

    auto single = object_ref<XEvent>(new XEvent(kernel_state()));
    single->Initialize(false, false);
    control_->single_wait_timed_out =
        single->Wait(0, 0, 0, &timeout) == X_STATUS_TIMEOUT;

    auto multi_a = object_ref<XEvent>(new XEvent(kernel_state()));
    auto multi_b = object_ref<XEvent>(new XEvent(kernel_state()));
    multi_a->Initialize(false, false);
    multi_b->Initialize(false, false);
    XObject* multi_objects[] = {multi_a.get(), multi_b.get()};
    timeout = static_cast<uint64_t>(kRelativeTimeoutTicks);
    control_->multi_any_timed_out =
        XObject::WaitMultiple(2, multi_objects, 1, 0, 0, 0, &timeout) ==
        X_STATUS_TIMEOUT;
    timeout = static_cast<uint64_t>(kRelativeTimeoutTicks);
    control_->multi_all_timed_out =
        XObject::WaitMultiple(2, multi_objects, 0, 0, 0, 0, &timeout) ==
        X_STATUS_TIMEOUT;

    auto signal = object_ref<XEvent>(new XEvent(kernel_state()));
    auto signal_wait = object_ref<XEvent>(new XEvent(kernel_state()));
    signal->Initialize(false, false);
    signal_wait->Initialize(false, false);
    timeout = static_cast<uint64_t>(kRelativeTimeoutTicks);
    control_->signal_and_wait_timed_out =
        XObject::SignalAndWait(signal.get(), signal_wait.get(), 0, 0, 0,
                               &timeout) == X_STATUS_TIMEOUT;

    control_->delay_completed =
        Delay(0, 0, static_cast<uint64_t>(kRelativeTimeoutTicks)) ==
        X_STATUS_SUCCESS;
    GuestScheduler::SpinYield(1ms);

    xe::threading::Fence fence;
    std::thread fence_signaler([&fence]() {
      std::this_thread::sleep_for(5ms);
      fence.Signal();
    });
    GuestScheduler::WaitOnFence(fence);
    fence_signaler.join();

    std::atomic<bool> release_io{false};
    std::thread io_releaser([&release_io]() {
      std::this_thread::sleep_for(5ms);
      release_io.store(true, std::memory_order_release);
    });
    kernel_state()->guest_scheduler()->RunBlockingHostCall([&release_io]() {
      while (!release_io.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });
    io_releaser.join();

    auto completion =
        object_ref<XIOCompletion>(new XIOCompletion(kernel_state()));
    XIOCompletion::IONotification notification = {};
    control_->io_completion_timed_out =
        !completion->WaitForNotification(5, &notification);

    auto socket = object_ref<XSocket>(new XSocket(kernel_state()));
    control_->socket_initialized =
        socket->Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_DGRAM,
                           XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS;
    if (control_->socket_initialized) {
      N_XSOCKADDR_IN bind_address = {};
      bind_address.sin_family = XSocket::X_AF_INET;
      bind_address.sin_port = 0;
      bind_address.sin_addr = 0;
      control_->socket_bound =
          socket->Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS;
      if (control_->socket_bound) {
        const uint16_t port = socket->bound_port();
        std::thread sender([port]() {
          std::this_thread::sleep_for(5ms);
          asio::io_context context;
          asio::ip::udp::socket host_socket(context);
          asio::error_code error;
          host_socket.open(asio::ip::udp::v4(), error);
          if (!error) {
            const uint8_t byte = 0x5A;
            host_socket.send_to(
                asio::buffer(&byte, sizeof(byte)),
                asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port),
                0, error);
          }
        });
        uint8_t byte = 0;
        N_XSOCKADDR_IN source = {};
        uint32_t source_size = sizeof(source);
        control_->socket_received =
            socket->RecvFrom(&byte, 1, 0, &source, &source_size) == 1;
        sender.join();
      }
    }

    control_->Complete();
    Exit(0);
  }

 private:
  WaitInventoryControl* control_;
};

struct RereadyRaceControl {
  static void Hook(void* opaque, XThread* thread,
                   const GuestSchedulerCaptureWaitState& wait) {
    auto* control = static_cast<RereadyRaceControl*>(opaque);
    bool unclaimed = false;
    if (thread != control->thread ||
        !control->claimed.compare_exchange_strong(unclaimed, true)) {
      return;
    }
    control->decision_wait = wait;
    control->entered.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (control->event->cooperative_signal_epoch() ==
               wait.observed_wait_epoch &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    control->signal_observed.store(
        control->event->cooperative_signal_epoch() != wait.observed_wait_epoch,
        std::memory_order_release);
  }

  // A parked signaller would have to be woken from inside the very window it is
  // racing, so it announces itself and then spins already on-CPU.
  bool WaitForHook(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    signaler_ready.store(true, std::memory_order_release);
    while (!entered.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return entered.load(std::memory_order_acquire);
      }
      std::this_thread::yield();
    }
    return true;
  }

  bool WaitForSignaler(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!signaler_ready.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

  XThread* thread = nullptr;
  XEvent* event = nullptr;
  GuestSchedulerCaptureWaitState decision_wait;
  std::atomic<bool> claimed{false};
  std::atomic<bool> signaler_ready{false};
  std::atomic<bool> entered{false};
  std::atomic<bool> signal_observed{false};
};

struct RereadyWaitControl {
  bool WaitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [this]() { return completed; });
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
};

class RereadyWaitRuntimeThread final : public XThread {
 public:
  RereadyWaitRuntimeThread(KernelState* kernel_state,
                           RereadyWaitControl* control, XEvent* event,
                           uint32_t blocked_continuation_pc = 0)
      : XThread(kernel_state, 64 * 1024, 0, kSafepointPc, 0, kCpu0CreationFlags,
                true, false, kernel_state->GetSystemProcess()),
        control_(control),
        event_(event),
        blocked_continuation_pc_(blocked_continuation_pc) {}

  void Execute() override {
    if (blocked_continuation_pc_) {
      thread_state()->context()->lr = blocked_continuation_pc_;
    }
    event_->Wait(0, 0, 0, nullptr);
    {
      std::lock_guard<std::mutex> lock(control_->mutex);
      control_->completed = true;
    }
    control_->condition.notify_all();
    Exit(0);
  }

 private:
  RereadyWaitControl* control_;
  XEvent* event_;
  uint32_t blocked_continuation_pc_;
};

class CheckpointRuntimeThread final : public XThread {
 public:
  CheckpointRuntimeThread(KernelState* kernel_state, FiberControl* control,
                          uint32_t creation_flags)
      : XThread(kernel_state, 64 * 1024, 0, kSafepointPc, 0, creation_flags,
                true, false, kernel_state->GetSystemProcess()),
        control_(control) {}

  void Execute() override {
    control_->started.store(true, std::memory_order_release);
    control_->Notify();
    while (!control_->stop.load(std::memory_order_acquire)) {
      auto* context = thread_state()->context();
      if (!context->preempt_requested) {
        std::this_thread::yield();
        continue;
      }
      context->preempt_requested = 0;
      auto handler = cpu::backend::preempt_yield_handler;
      if (!handler) {
        control_->missing_handler.store(true, std::memory_order_release);
        break;
      }
      handler(context, kSafepointPc, kSafepointFunction);
      control_->safepoint_returns.fetch_add(1, std::memory_order_release);
      control_->Notify();
    }
    control_->exit_called.store(true, std::memory_order_release);
    control_->Notify();
    Exit(0);
  }

 private:
  FiberControl* control_;
};

struct ReplayFiberControl {
  std::atomic<bool> execute_returned{false};
};

class ReplayRuntimeThread final : public XThread {
 public:
  ReplayRuntimeThread(KernelState* kernel_state, ReplayFiberControl* control,
                      uint32_t resume_pc, uint32_t outer_return_address)
      : XThread(kernel_state, 64 * 1024, 0, resume_pc, 0,
                kCpu0CreationFlags | X_CREATE_SUSPENDED, true, false,
                kernel_state->GetSystemProcess()),
        control_(control),
        resume_pc_(resume_pc),
        outer_return_address_(outer_return_address) {}

  void Execute() override {
    const bool succeeded = kernel_state()->processor()->ExecuteRaw(
        thread_state(), resume_pc_, outer_return_address_);
    control_->execute_returned.store(true, std::memory_order_release);
    Exit(succeeded ? 0 : -1);
  }

 private:
  ReplayFiberControl* control_;
  uint32_t resume_pc_;
  uint32_t outer_return_address_;
};

struct CreatedReplayThread {
  CreatedReplayThread() = default;
  CreatedReplayThread(const CreatedReplayThread&) = delete;
  CreatedReplayThread& operator=(const CreatedReplayThread&) = delete;
  CreatedReplayThread(CreatedReplayThread&&) = default;
  CreatedReplayThread& operator=(CreatedReplayThread&&) = default;

  ~CreatedReplayThread() {
    if (!thread || XFAILED(status)) {
      return;
    }
    GuestScheduler* scheduler = thread->kernel_state()->guest_scheduler();
    if (scheduler) {
      scheduler->Shutdown();
    }
    thread->ReclaimExited();
    thread->ReleaseHandle();
    thread.reset();
  }

  object_ref<ReplayRuntimeThread> thread;
  X_STATUS status = X_STATUS_UNSUCCESSFUL;
};

CreatedReplayThread CreateReplayRuntimeThread(SchedulerEnvironment& environment,
                                              ReplayFiberControl& control,
                                              uint32_t resume_pc,
                                              uint32_t caller_continuation,
                                              uint32_t outer_return_address,
                                              uint32_t shared_address) {
  CreatedReplayThread result;
  if (!environment.ready()) {
    return result;
  }
  result.thread = object_ref<ReplayRuntimeThread>(
      new ReplayRuntimeThread(environment.emulator()->kernel_state(), &control,
                              resume_pc, outer_return_address));
  result.thread->set_name("Continuous replay runtime test");
  result.status = result.thread->Create();
  if (XFAILED(result.status)) {
    return result;
  }
  auto* context = result.thread->thread_state()->context();
  context->r[3] = 0;
  context->r[4] = shared_address;
  context->r[11] = outer_return_address;
  context->lr = caller_continuation;
  return result;
}

struct CreatedThread {
  CreatedThread() = default;
  CreatedThread(const CreatedThread&) = delete;
  CreatedThread& operator=(const CreatedThread&) = delete;
  CreatedThread(CreatedThread&&) = default;
  CreatedThread& operator=(CreatedThread&&) = default;

  ~CreatedThread() {
    if (!thread || XFAILED(status)) {
      return;
    }
    GuestScheduler* scheduler = thread->kernel_state()->guest_scheduler();
    if (scheduler) {
      scheduler->Shutdown();
    }
    thread->ReclaimExited();
    thread->ReleaseHandle();
    thread.reset();
  }

  object_ref<CheckpointRuntimeThread> thread;
  X_STATUS status = X_STATUS_UNSUCCESSFUL;
};

CreatedThread CreateRuntimeThread(SchedulerEnvironment& environment,
                                  FiberControl& control,
                                  uint32_t creation_flags) {
  CreatedThread result;
  if (!environment.ready()) {
    return result;
  }
  result.thread =
      object_ref<CheckpointRuntimeThread>(new CheckpointRuntimeThread(
          environment.emulator()->kernel_state(), &control, creation_flags));
  result.thread->set_name("Checkpoint runtime test");
  result.status = result.thread->Create();
  return result;
}

bool StopRuntimeThread(CreatedThread& created, FiberControl& control) {
  if (!created.thread || XFAILED(created.status)) {
    return false;
  }
  control.stop.store(true, std::memory_order_release);
  if (created.thread->thread_state()) {
    created.thread->thread_state()->context()->preempt_requested = 1;
  }
  return control.WaitForExit(2s);
}

struct PauseResult {
  Rejection rejection = Rejection::kInvalidTopology;
  GuestSchedulerCheckpointBarrierSnapshot snapshot;
};

PauseResult Pause(GuestScheduler& scheduler,
                  std::chrono::milliseconds timeout) {
  PauseResult result;
  result.rejection =
      scheduler.PauseForCheckpointBarrier(timeout, &result.snapshot);
  return result;
}

// A quiesce window whose roster moved under it is void rather than wrong, so
// the arm policy re-samples it instead of reporting a barrier state error.
Rejection PauseSettled(GuestScheduler& scheduler,
                       std::chrono::milliseconds timeout,
                       GuestSchedulerCheckpointBarrierSnapshot* out_snapshot) {
  constexpr int kAttempts = 8;
  Rejection rejection = Rejection::kTopologyChanged;
  for (int attempt = 0;
       attempt < kAttempts && rejection == Rejection::kTopologyChanged;
       ++attempt) {
    rejection = scheduler.PauseForCheckpointBarrier(timeout, out_snapshot);
  }
  return rejection;
}

bool WaitUntilSuspended(GuestScheduler& scheduler, XThread* thread,
                        std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(scheduler,
                                                                 thread)
            .suspended) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool WaitUntilRunning(GuestScheduler& scheduler, XThread* thread,
                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(scheduler,
                                                                 thread)
            .running) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool WaitUntilQueued(GuestScheduler& scheduler, XThread* thread,
                     std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(scheduler,
                                                                 thread)
            .queued) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool ContainsThread(const GuestSchedulerCheckpointBarrierSnapshot& snapshot,
                    uint32_t thread_id) {
  return std::any_of(snapshot.participants.begin(), snapshot.participants.end(),
                     [thread_id](const auto& participant) {
                       return participant.thread_id == thread_id;
                     });
}

const GuestSchedulerCheckpointParticipant* FindThread(
    const GuestSchedulerCheckpointBarrierSnapshot& snapshot,
    uint32_t thread_id) {
  auto it =
      std::find_if(snapshot.participants.begin(), snapshot.participants.end(),
                   [thread_id](const auto& participant) {
                     return participant.thread_id == thread_id;
                   });
  return it == snapshot.participants.end() ? nullptr : &*it;
}

bool RegistryContainsThread(
    const cpu::GuestExecutionCaptureThreadStateRegistrySnapshot& snapshot,
    uint32_t thread_id) {
  return std::any_of(snapshot.participants.begin(), snapshot.participants.end(),
                     [thread_id](const auto& participant) {
                       return participant.participant.guest_thread_id ==
                              thread_id;
                     });
}

bool ContainsRestorableJitSafepoint(
    const GuestSchedulerCheckpointBarrierSnapshot& snapshot) {
  return std::any_of(snapshot.participants.begin(), snapshot.participants.end(),
                     [](const auto& participant) {
                       return participant.guest_pc == kSafepointPc &&
                              participant.resume_kind ==
                                  ResumeKind::kJitSafepoint &&
                              participant.restorable;
                     });
}

TEST_CASE("Guest scheduler drains instruction coverage at actor boundaries",
          "[guest_scheduler_capture_observer][runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  InstructionCoverageControl control;
  auto thread = object_ref<InstructionCoverageRuntimeThread>(
      new InstructionCoverageRuntimeThread(
          environment.emulator()->kernel_state(), &control));
  thread->set_name("Scheduler instruction coverage test");
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  REQUIRE(control.WaitForCompletion(2s));
  scheduler.Shutdown();

  const auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == GuestSchedulerCaptureRecorderRejection::kNone);
  std::vector<uint64_t> yield_deltas;
  std::vector<uint64_t> switch_out_deltas;
  for (const GuestSchedulerCaptureEvent& event : snapshot.events) {
    if (event.guest_thread_id != thread->thread_id()) {
      continue;
    }
    if (event.kind == GuestSchedulerCaptureEventKind::kYield) {
      yield_deltas.push_back(event.guest_instruction_delta);
    } else if (event.kind == GuestSchedulerCaptureEventKind::kSwitchOut) {
      switch_out_deltas.push_back(event.guest_instruction_delta);
    } else {
      REQUIRE(event.guest_instruction_delta == 0);
    }
  }
  REQUIRE(yield_deltas == std::vector<uint64_t>{7, 11});
  REQUIRE(std::count(switch_out_deltas.begin(), switch_out_deltas.end(), 13) ==
          1);
  REQUIRE(std::accumulate(switch_out_deltas.begin(), switch_out_deltas.end(),
                          uint64_t{0}) == 13);

  std::atomic_ref<uint64_t*>(thread->thread_state()
                                 ->context()
                                 ->guest_execution_session_instruction_counter)
      .store(nullptr, std::memory_order_release);
  thread->ReclaimExited();
  thread->ReleaseHandle();
  thread.reset();
}

TEST_CASE("Guest scheduler partitions coverage across a deferred safepoint",
          "[guest_scheduler_capture_observer][runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());
  REQUIRE(cpu::backend::preempt_yield_handler);

  InstructionCoverageControl control;
  auto thread = object_ref<DeferredInstructionCoverageRuntimeThread>(
      new DeferredInstructionCoverageRuntimeThread(
          environment.emulator()->kernel_state(), &control));
  thread->set_name("Scheduler deferred coverage test");
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  REQUIRE(control.WaitForCompletion(2s));
  scheduler.Shutdown();

  const auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == GuestSchedulerCaptureRecorderRejection::kNone);
  std::vector<GuestSchedulerCaptureEvent> safepoints;
  uint64_t other_instruction_delta = 0;
  for (const GuestSchedulerCaptureEvent& event : snapshot.events) {
    if (event.guest_thread_id != thread->thread_id()) {
      continue;
    }
    if (event.kind == GuestSchedulerCaptureEventKind::kSafepoint) {
      safepoints.push_back(event);
    } else {
      other_instruction_delta += event.guest_instruction_delta;
    }
  }
  REQUIRE(safepoints.size() == 2);
  REQUIRE(safepoints[0].reason == GuestSchedulerCaptureReason::kDeferredLock);
  REQUIRE(safepoints[0].guest_instruction_delta == 5);
  REQUIRE(safepoints[0].value == 0);
  REQUIRE(safepoints[0].count == 0);
  REQUIRE(safepoints[1].reason == GuestSchedulerCaptureReason::kYielded);
  REQUIRE(safepoints[1].guest_instruction_delta == 7);
  REQUIRE(safepoints[1].value == 0);
  REQUIRE(safepoints[1].count == 1);
  REQUIRE(other_instruction_delta == 0);

  std::atomic_ref<uint64_t*>(thread->thread_state()
                                 ->context()
                                 ->guest_execution_session_instruction_counter)
      .store(nullptr, std::memory_order_release);
  thread->ReclaimExited();
  thread->ReleaseHandle();
  thread.reset();
}

TEST_CASE("Guest scheduler rejects foreign instruction counters",
          "[guest_scheduler_capture_observer][runtime]") {
  FiberControl control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  REQUIRE(control.WaitForStart(2s));
  uint64_t foreign_counter = 73;
  auto* context = created.thread->thread_state()->context();
  std::atomic_ref<uint64_t*>(
      context->guest_execution_session_instruction_counter)
      .store(&foreign_counter, std::memory_order_release);
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), true);
  REQUIRE(control.WaitForSafepointReturn(2s));

  REQUIRE(scheduler.capture_rejected());
  REQUIRE(foreign_counter == 73);
  std::atomic_ref<uint64_t*>(
      context->guest_execution_session_instruction_counter)
      .store(nullptr, std::memory_order_release);
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler publishes switch-out before checkpoint quiescence",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  FiberControl control;
  auto observer = std::make_shared<SwitchOutGateObserver>();
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(observer));

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  observer->SetTargetThread(created.thread->thread_id());
  REQUIRE(control.WaitForStart(2s));

  auto pause_future = std::async(std::launch::async,
                                 [&scheduler] { return Pause(scheduler, 2s); });
  const bool observer_entered = observer->WaitUntilEntered(2s);
  const bool pause_blocked =
      observer_entered &&
      pause_future.wait_for(20ms) == std::future_status::timeout;
  observer->Release();
  const bool pause_ready =
      pause_future.wait_for(3s) == std::future_status::ready;
  CHECK(observer_entered);
  CHECK(pause_blocked);
  REQUIRE(pause_ready);
  PauseResult pause = pause_future.get();
  REQUIRE(pause.rejection == Rejection::kNone);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              pause.snapshot.generation, nullptr) == Rejection::kNone);
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler capture inventories every cooperative block source",
          "[guest_scheduler_capture_wait][runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(4096);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  WaitInventoryControl control;
  auto thread =
      object_ref<WaitInventoryRuntimeThread>(new WaitInventoryRuntimeThread(
          environment.emulator()->kernel_state(), &control));
  thread->set_name("Scheduler wait inventory test");
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  REQUIRE(control.WaitForCompletion(5s));
  scheduler.Shutdown();

  const auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == GuestSchedulerCaptureRecorderRejection::kNone);
  std::array<size_t, 10> block_counts = {};
  for (const GuestSchedulerCaptureEvent& event : snapshot.events) {
    if (event.kind != GuestSchedulerCaptureEventKind::kBlock) {
      continue;
    }
    REQUIRE(event.value < block_counts.size());
    ++block_counts[event.value];
    const auto wait_kind =
        static_cast<GuestSchedulerCaptureWaitKind>(event.value);
    switch (wait_kind) {
      case GuestSchedulerCaptureWaitKind::kSingle:
        REQUIRE(event.wait.handle_count == 1);
        break;
      case GuestSchedulerCaptureWaitKind::kMultiAny:
      case GuestSchedulerCaptureWaitKind::kMultiAll:
        REQUIRE(event.wait.handle_count == 2);
        break;
      case GuestSchedulerCaptureWaitKind::kDelay:
        REQUIRE(event.wait.handle_count == 0);
        REQUIRE(event.wait.deadline_ms != 0);
        break;
      case GuestSchedulerCaptureWaitKind::kFence:
      case GuestSchedulerCaptureWaitKind::kIoOffload:
      case GuestSchedulerCaptureWaitKind::kSpinBackoff:
        REQUIRE(event.wait.handle_count == 0);
        REQUIRE(event.wait.deadline_ms == 0);
        break;
      case GuestSchedulerCaptureWaitKind::kIoCompletion:
        REQUIRE(event.wait.handle_count == 1);
        REQUIRE(event.wait.deadline_ms != 0);
        REQUIRE(event.wait.wait_epoch == 0);
        REQUIRE(event.wait.observed_wait_epoch == 0);
        break;
      case GuestSchedulerCaptureWaitKind::kSocketIo:
        REQUIRE(event.wait.handle_count == 1);
        REQUIRE(event.wait.wait_epoch == 0);
        REQUIRE(event.wait.observed_wait_epoch == 0);
        break;
      case GuestSchedulerCaptureWaitKind::kNone:
      default:
        FAIL("unexpected cooperative wait kind");
    }
  }

  REQUIRE(block_counts[static_cast<uint8_t>(
              GuestSchedulerCaptureWaitKind::kSingle)] >= 2);
  for (uint8_t kind =
           static_cast<uint8_t>(GuestSchedulerCaptureWaitKind::kMultiAny);
       kind <= static_cast<uint8_t>(GuestSchedulerCaptureWaitKind::kSocketIo);
       ++kind) {
    REQUIRE(block_counts[kind] >= 1);
  }
  REQUIRE(control.single_wait_timed_out);
  REQUIRE(control.multi_any_timed_out);
  REQUIRE(control.multi_all_timed_out);
  REQUIRE(control.signal_and_wait_timed_out);
  REQUIRE(control.delay_completed);
  REQUIRE(control.io_completion_timed_out);
  REQUIRE(control.socket_initialized);
  REQUIRE(control.socket_bound);
  REQUIRE(control.socket_received);

  thread->ReclaimExited();
  thread->ReleaseHandle();
  thread.reset();
}

TEST_CASE("Guest scheduler reready capture freezes decision-time provenance",
          "[guest_scheduler_capture_wait][runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(256);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  auto event =
      object_ref<XEvent>(new XEvent(environment.emulator()->kernel_state()));
  event->Initialize(false, false);
  RereadyWaitControl wait_control;
  auto thread =
      object_ref<RereadyWaitRuntimeThread>(new RereadyWaitRuntimeThread(
          environment.emulator()->kernel_state(), &wait_control, event.get()));
  thread->set_name("Scheduler reready decision test");

  RereadyRaceControl race;
  race.thread = thread.get();
  race.event = event.get();

  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  const auto blocked_deadline = std::chrono::steady_clock::now() + 2s;
  while (!GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                thread.get()) &&
         std::chrono::steady_clock::now() < blocked_deadline) {
    std::this_thread::yield();
  }
  REQUIRE(GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                thread.get()));
  std::thread signaler([&race]() {
    if (race.WaitForHook(2s)) {
      race.event->Set(0, false);
    }
  });
  const bool signaler_ready = race.WaitForSignaler(2s);
  const bool armed = GuestSchedulerCaptureWaitRuntimeTestAccess::
      ArmRereadyDecisionHookAndForce(scheduler, thread.get(),
                                     &RereadyRaceControl::Hook, &race);
  const bool completed = armed && wait_control.WaitForCompletion(2s);
  signaler.join();
  GuestSchedulerCaptureWaitRuntimeTestAccess::SetRereadyDecisionHook(
      scheduler, nullptr, nullptr);
  scheduler.Shutdown();

  REQUIRE(signaler_ready);
  REQUIRE(armed);
  REQUIRE(completed);
  REQUIRE(race.entered.load(std::memory_order_acquire));
  REQUIRE(race.signal_observed.load(std::memory_order_acquire));
  REQUIRE(event->cooperative_signal_epoch() !=
          race.decision_wait.observed_wait_epoch);
  const auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == GuestSchedulerCaptureRecorderRejection::kNone);
  const auto reready = std::find_if(
      snapshot.events.cbegin(), snapshot.events.cend(),
      [&thread](const GuestSchedulerCaptureEvent& event_record) {
        return event_record.kind == GuestSchedulerCaptureEventKind::kReready &&
               event_record.guest_thread_id == thread->thread_id();
      });
  REQUIRE(reready != snapshot.events.cend());
  REQUIRE(reready->reason == GuestSchedulerCaptureReason::kBackstop);
  REQUIRE(reready->wait == race.decision_wait);
  REQUIRE(reready->wait.wait_epoch == reready->wait.observed_wait_epoch);

  thread->ReclaimExited();
  thread->ReleaseHandle();
  thread.reset();
  event->ReleaseHandle();
  event.reset();
}

TEST_CASE("Guest scheduler orders blocked priority mutation before wake",
          "[guest_scheduler_capture][guest_scheduler_priority][runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(256);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  auto event =
      object_ref<XEvent>(new XEvent(environment.emulator()->kernel_state()));
  event->Initialize(false, false);
  RereadyWaitControl control;
  auto thread =
      object_ref<RereadyWaitRuntimeThread>(new RereadyWaitRuntimeThread(
          environment.emulator()->kernel_state(), &control, event.get()));
  thread->set_name("Scheduler blocked priority test");
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);

  const auto blocked_deadline = std::chrono::steady_clock::now() + 2s;
  while (!GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                thread.get()) &&
         std::chrono::steady_clock::now() < blocked_deadline) {
    std::this_thread::yield();
  }
  REQUIRE(GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                thread.get()));
  const auto inspection =
      GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(scheduler,
                                                               thread.get());
  const int32_t old_priority = thread->priority();
  const int32_t new_priority = old_priority == 31 ? 30 : old_priority + 1;
  thread->SetPriority(new_priority);
  REQUIRE(thread->priority() == new_priority);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::BlockedMaxPriority(
              scheduler, inspection.cpu) == new_priority);
  thread->SetPriority(old_priority);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::BlockedMaxPriority(
              scheduler, inspection.cpu) == old_priority);
  thread->SetPriority(new_priority);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::BlockedMaxPriority(
              scheduler, inspection.cpu) == new_priority);

  event->Set(0, false);
  REQUIRE(control.WaitForCompletion(2s));
  scheduler.Shutdown();

  const auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == GuestSchedulerCaptureRecorderRejection::kNone);
  const auto priority_change = std::find_if(
      snapshot.events.cbegin(), snapshot.events.cend(),
      [&thread](const GuestSchedulerCaptureEvent& captured) {
        return captured.kind ==
                   GuestSchedulerCaptureEventKind::kPriorityChange &&
               captured.guest_thread_id == thread->thread_id();
      });
  REQUIRE(priority_change != snapshot.events.cend());
  REQUIRE(priority_change->cpu == inspection.cpu);
  REQUIRE(priority_change->value == old_priority);
  REQUIRE(priority_change->priority == new_priority);
  const auto reready = std::find_if(
      snapshot.events.cbegin(), snapshot.events.cend(),
      [&thread](const GuestSchedulerCaptureEvent& captured) {
        return captured.kind == GuestSchedulerCaptureEventKind::kReready &&
               captured.guest_thread_id == thread->thread_id();
      });
  REQUIRE(reready != snapshot.events.cend());
  REQUIRE(priority_change->sequence < reready->sequence);

  thread->ReclaimExited();
  thread->ReleaseHandle();
  thread.reset();
  event->ReleaseHandle();
  event.reset();
}

TEST_CASE("Guest scheduler atomically publishes running and ready priorities",
          "[guest_scheduler_capture][guest_scheduler_priority][runtime]") {
  FiberControl running_control;
  FiberControl ready_control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(256);
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread running =
      CreateRuntimeThread(environment, running_control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(running.status));
  REQUIRE(running_control.WaitForStart(2s));
  const int32_t running_old_priority = running.thread->priority();
  REQUIRE(running_old_priority != 31);
  running.thread->SetPriority(31);

  CreatedThread ready = CreateRuntimeThread(
      environment, ready_control, kCpu0CreationFlags | X_CREATE_SUSPENDED);
  REQUIRE(XSUCCEEDED(ready.status));
  const int32_t ready_old_priority = ready.thread->priority();
  const int32_t preenqueue_priority = ready_old_priority >= 30
                                          ? ready_old_priority - 1
                                          : ready_old_priority + 1;
  ready.thread->SetPriority(preenqueue_priority);
  REQUIRE(XSUCCEEDED(ready.thread->Resume()));
  REQUIRE(WaitUntilQueued(scheduler, ready.thread.get(), 2s));
  const int32_t ready_new_priority = preenqueue_priority >= 30
                                         ? preenqueue_priority - 1
                                         : preenqueue_priority + 1;
  ready.thread->SetPriority(ready_new_priority);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::QueuedPriority(
              scheduler, ready.thread.get()) == ready_new_priority);
  ready.thread->SetPriority(ready_new_priority);

  REQUIRE(StopRuntimeThread(running, running_control));
  REQUIRE(ready_control.WaitForStart(2s));
  REQUIRE(StopRuntimeThread(ready, ready_control));
  scheduler.Shutdown();

  const auto snapshot = recorder->snapshot();
  REQUIRE(snapshot.rejection == GuestSchedulerCaptureRecorderRejection::kNone);
  const auto matching_priority_changes =
      [&snapshot](
          uint32_t thread_id) -> std::vector<GuestSchedulerCaptureEvent> {
    std::vector<GuestSchedulerCaptureEvent> matches;
    std::copy_if(snapshot.events.cbegin(), snapshot.events.cend(),
                 std::back_inserter(matches),
                 [thread_id](const GuestSchedulerCaptureEvent& captured) {
                   return captured.kind ==
                              GuestSchedulerCaptureEventKind::kPriorityChange &&
                          captured.guest_thread_id == thread_id;
                 });
    return matches;
  };
  const auto running_changes =
      matching_priority_changes(running.thread->thread_id());
  REQUIRE(running_changes.size() == 1);
  REQUIRE(running_changes[0].value == running_old_priority);
  REQUIRE(running_changes[0].priority == 31);
  REQUIRE(running_changes[0].cpu == 0);
  REQUIRE(running_changes[0].target_cpu == -1);
  const auto ready_changes =
      matching_priority_changes(ready.thread->thread_id());
  REQUIRE(ready_changes.size() == 2);
  REQUIRE(ready_changes[0].value == ready_old_priority);
  REQUIRE(ready_changes[0].priority == preenqueue_priority);
  REQUIRE(ready_changes[0].cpu == -1);
  REQUIRE(ready_changes[0].target_cpu == -1);
  REQUIRE(ready_changes[1].value == preenqueue_priority);
  REQUIRE(ready_changes[1].priority == ready_new_priority);
  REQUIRE(ready_changes[1].cpu == 0);
  REQUIRE(ready_changes[1].target_cpu == -1);
}

TEST_CASE("Guest scheduler orders base-only priority mutations",
          "[guest_scheduler_checkpoint][guest_scheduler_priority][runtime]") {
  FiberControl ready_control;
  FiberControl higher_priority_control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread ready =
      CreateRuntimeThread(environment, ready_control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(ready.status));
  REQUIRE(ready_control.WaitForStart(2s));

  CreatedThread higher_priority =
      CreateRuntimeThread(environment, higher_priority_control,
                          kCpu0CreationFlags | X_CREATE_SUSPENDED);
  REQUIRE(XSUCCEEDED(higher_priority.status));
  higher_priority.thread->SetPriority(31);
  REQUIRE(XSUCCEEDED(higher_priority.thread->Resume()));
  REQUIRE(higher_priority_control.WaitForStart(2s));
  REQUIRE(WaitUntilQueued(scheduler, ready.thread.get(), 2s));

  X_KTHREAD* guest_thread = ready.thread->guest_object<X_KTHREAD>();
  const int32_t old_base_priority = guest_thread->base_priority;
  const int32_t class_base_priority = guest_thread->base_priority_copy;
  const int32_t max_dynamic_priority = guest_thread->max_dynamic_priority;
  REQUIRE(old_base_priority < max_dynamic_priority);
  ready.thread->SetPriority(max_dynamic_priority);
  REQUIRE(ready.thread->priority() == max_dynamic_priority);
  REQUIRE(guest_thread->base_priority == old_base_priority);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::QueuedPriority(
              scheduler, ready.thread.get()) == max_dynamic_priority);

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const auto* before = FindThread(snapshot, ready.thread->thread_id());
  REQUIRE(before);
  REQUIRE(before->base_priority == max_dynamic_priority);
  REQUIRE(before->effective_priority == max_dynamic_priority);

  const int32_t new_base_priority = old_base_priority + 1;
  const int32_t new_base_increment = new_base_priority - class_base_priority;
  ready.thread->SetBasePriority(new_base_increment);
  REQUIRE(guest_thread->base_priority == new_base_priority);
  REQUIRE(ready.thread->priority() == max_dynamic_priority);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::QueuedPriority(
              scheduler, ready.thread.get()) == max_dynamic_priority);

  GuestSchedulerCheckpointBarrierSnapshot rejected_snapshot;
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(snapshot.generation,
                                                       &rejected_snapshot) ==
          Rejection::kTopologyChanged);
  REQUIRE(rejected_snapshot.rejection == Rejection::kTopologyChanged);
  REQUIRE(higher_priority_control.WaitForSafepointReturn(2s));

  GuestSchedulerCheckpointBarrierSnapshot updated_snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &updated_snapshot) == Rejection::kNone);
  const auto* after = FindThread(updated_snapshot, ready.thread->thread_id());
  REQUIRE(after);
  REQUIRE(after->base_priority == new_base_priority);
  REQUIRE(after->effective_priority == max_dynamic_priority);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              updated_snapshot.generation, nullptr) == Rejection::kNone);

  REQUIRE(StopRuntimeThread(higher_priority, higher_priority_control));
  REQUIRE(ready_control.WaitForSafepointReturn(2s));
  REQUIRE(StopRuntimeThread(ready, ready_control));
}

void ThrowBeforeCheckpointSnapshot(void*) { throw std::bad_alloc(); }

struct ReleaseRaceResult {
  bool setup = false;
  bool observer_attached = false;
  bool observer_armed = false;
  bool thread_created = false;
  bool thread_started = false;
  bool arrival_entered = false;
  bool release_call_completed = false;
  bool pause_completed = false;
  bool safepoint_returned = false;
  bool thread_stopped = false;
  bool switch_out_seen = false;
  bool redispatch_seen_after_switch_out = false;
  bool boundary_is_jit_safepoint = false;
  Rejection release_rejection = Rejection::kInvalidTopology;
  Rejection pause_rejection = Rejection::kInvalidTopology;
  GuestSchedulerCheckpointBarrierSnapshot release_snapshot;
  GuestSchedulerCheckpointBarrierSnapshot pause_snapshot;
  GuestSchedulerCheckpointRuntimeInspection held_after_release;
};

enum class ReleaseRaceKind { kTimeout, kCancel, kShutdown };

ReleaseRaceResult RunReleaseRace(ReleaseRaceKind kind) {
  BlockingGate arrival_gate;
  FiberControl control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  ReleaseRaceResult result;
  result.setup = environment.ready();
  if (!result.setup) {
    return result;
  }

  GuestScheduler& scheduler = *environment.scheduler();
  GuestSchedulerCheckpointRuntimeTestAccess::SetArrivalHook(
      scheduler, &BlockingGate::Hook, &arrival_gate);
  result.observer_attached = scheduler.AttachCaptureObserver(recorder);
  if (result.observer_attached) {
    result.observer_armed = recorder->Arm();
  }

  CreatedThread thread =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  result.thread_created = XSUCCEEDED(thread.status);
  if (!result.thread_created) {
    return result;
  }
  result.thread_started = control.WaitForStart(2s);
  if (!result.thread_started) {
    environment.scheduler()->Shutdown();
    return result;
  }

  const auto pause_timeout = kind == ReleaseRaceKind::kTimeout ? 50ms : 5s;
  auto pause_future = std::async(
      std::launch::async,
      [&scheduler, pause_timeout] { return Pause(scheduler, pause_timeout); });
  result.arrival_entered = arrival_gate.WaitUntilEntered(2s);

  std::future<void> shutdown_future;
  if (result.arrival_entered) {
    const auto barrier_snapshot =
        GuestSchedulerCheckpointRuntimeTestAccess::Snapshot(scheduler);
    if (kind == ReleaseRaceKind::kCancel) {
      result.release_rejection = scheduler.CancelCheckpointBarrier(
          barrier_snapshot.generation, &result.release_snapshot);
      result.release_call_completed = true;
    } else if (kind == ReleaseRaceKind::kShutdown) {
      shutdown_future = std::async(std::launch::async,
                                   [&scheduler]() { scheduler.Shutdown(); });
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (std::chrono::steady_clock::now() < deadline) {
        auto current =
            GuestSchedulerCheckpointRuntimeTestAccess::Snapshot(scheduler);
        if (!current.active && current.rejection == Rejection::kShutdown) {
          result.release_snapshot = std::move(current);
          result.release_rejection = Rejection::kShutdown;
          result.release_call_completed = true;
          break;
        }
        std::this_thread::yield();
      }
    } else {
      if (pause_future.wait_for(2s) == std::future_status::ready) {
        PauseResult pause_result = pause_future.get();
        result.pause_rejection = pause_result.rejection;
        result.pause_snapshot = std::move(pause_result.snapshot);
        result.pause_completed = true;
        result.release_snapshot = result.pause_snapshot;
        result.release_rejection = result.pause_rejection;
        result.release_call_completed = true;
      }
    }
    result.held_after_release =
        GuestSchedulerCheckpointRuntimeTestAccess::InspectHeld(
            scheduler, thread.thread.get());
  }

  arrival_gate.Release();
  if (shutdown_future.valid()) {
    shutdown_future.wait();
  }
  if (!result.pause_completed) {
    if (pause_future.wait_for(2s) != std::future_status::ready) {
      scheduler.Shutdown();
    }
    PauseResult pause_result = pause_future.get();
    result.pause_rejection = pause_result.rejection;
    result.pause_snapshot = std::move(pause_result.snapshot);
    result.pause_completed = true;
  }
  result.boundary_is_jit_safepoint =
      ContainsRestorableJitSafepoint(result.pause_snapshot);

  if (kind != ReleaseRaceKind::kShutdown) {
    result.safepoint_returned = control.WaitForSafepointReturn(2s);
    result.thread_stopped = StopRuntimeThread(thread, control);
  }

  const auto recorder_snapshot = recorder->snapshot();
  size_t switch_out_index = recorder_snapshot.events.size();
  for (size_t i = 0; i < recorder_snapshot.events.size(); ++i) {
    const auto& event = recorder_snapshot.events[i];
    if (event.guest_thread_id != thread.thread->thread_id()) {
      continue;
    }
    if (event.kind == Kind::kSwitchOut) {
      result.switch_out_seen = true;
      switch_out_index = std::min(switch_out_index, i);
    } else if (event.kind == Kind::kDispatch && i > switch_out_index) {
      result.redispatch_seen_after_switch_out = true;
    }
  }
  return result;
}

struct TopologyResult {
  bool setup = false;
  bool observer_attached = false;
  bool running_created = false;
  bool suspended_created = false;
  bool running_started = false;
  bool suspended_omitted = false;
  bool suspended_in_processor_roster = false;
  bool suspended_started_after_resume = false;
  bool running_stopped = false;
  bool suspended_stopped = false;
  Rejection pause_rejection = Rejection::kInvalidTopology;
  Rejection final_rejection = Rejection::kInvalidTopology;
  GuestSchedulerCheckpointBarrierSnapshot provisional;
  GuestSchedulerCheckpointBarrierSnapshot final_snapshot;
};

TopologyResult RunTopologyMutation() {
  FiberControl running_control;
  FiberControl suspended_control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  TopologyResult result;
  result.setup = environment.ready();
  if (!result.setup) {
    return result;
  }
  GuestScheduler& scheduler = *environment.scheduler();
  result.observer_attached = scheduler.AttachCaptureObserver(recorder);
  if (result.observer_attached) {
    recorder->Arm();
  }

  CreatedThread running =
      CreateRuntimeThread(environment, running_control, kCpu0CreationFlags);
  CreatedThread suspended = CreateRuntimeThread(
      environment, suspended_control, kCpu1CreationFlags | X_CREATE_SUSPENDED);
  result.running_created = XSUCCEEDED(running.status);
  result.suspended_created = XSUCCEEDED(suspended.status);
  if (!result.running_created || !result.suspended_created) {
    return result;
  }
  result.running_started = running_control.WaitForStart(2s);
  if (!result.running_started) {
    scheduler.Shutdown();
    return result;
  }

  result.pause_rejection = PauseSettled(scheduler, 2s, &result.provisional);
  const auto processor_roster = environment.emulator()
                                    ->processor()
                                    ->QueryGuestExecutionCaptureParticipants();
  result.suspended_omitted =
      !ContainsThread(result.provisional, suspended.thread->thread_id());
  result.suspended_in_processor_roster =
      RegistryContainsThread(processor_roster, suspended.thread->thread_id());

  suspended.thread->Resume();
  result.final_rejection = scheduler.FinalizeAndResumeCheckpointBarrier(
      result.provisional.generation, &result.final_snapshot);
  result.suspended_started_after_resume = suspended_control.WaitForStart(2s);
  result.running_stopped = StopRuntimeThread(running, running_control);
  result.suspended_stopped = StopRuntimeThread(suspended, suspended_control);
  return result;
}

}  // namespace

TEST_CASE("Guest scheduler checkpoint rejects a partially started dispatch set",
          "[guest_scheduler_checkpoint][runtime]") {
  BlockingGate startup_gate;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  GuestSchedulerCheckpointRuntimeTestAccess::SetStartupHook(
      scheduler, &BlockingGate::Hook, &startup_gate);

  auto start_future = std::async(std::launch::async,
                                 [&scheduler]() { scheduler.EnsureStarted(); });
  const bool hook_entered = startup_gate.WaitUntilEntered(2s);
  uint8_t partial_mask = 0;
  Rejection premature_rejection = Rejection::kInvalidTopology;
  if (hook_entered) {
    partial_mask =
        GuestSchedulerCheckpointRuntimeTestAccess::DispatchThreadMask(
            scheduler);
    GuestSchedulerCheckpointBarrierSnapshot ignored;
    premature_rejection = scheduler.PauseForCheckpointBarrier(10ms, &ignored);
  }
  startup_gate.Release();
  start_future.get();

  REQUIRE(hook_entered);
  REQUIRE(partial_mask == 0b000001);
  REQUIRE(premature_rejection == Rejection::kNotStarted);
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::DispatchReady(scheduler));
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::DispatchThreadMask(
              scheduler) == 0b111111);

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  REQUIRE(snapshot.dispatch_cpu_mask == 0b111111);
  REQUIRE(snapshot.participants.empty());
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              snapshot.generation, nullptr) == Rejection::kNone);
}

TEST_CASE(
    "Guest scheduler releases a barrier when snapshot construction throws",
    "[guest_scheduler_checkpoint][runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  scheduler.EnsureStarted();
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::DispatchReady(scheduler));

  GuestSchedulerCheckpointRuntimeTestAccess::SetSnapshotHook(
      scheduler, &ThrowBeforeCheckpointSnapshot, nullptr);
  GuestSchedulerCheckpointBarrierSnapshot failed_snapshot;
  REQUIRE(scheduler.PauseForCheckpointBarrier(2s, &failed_snapshot) ==
          Rejection::kInvalidTopology);
  REQUIRE(failed_snapshot.generation != 0);
  REQUIRE(failed_snapshot.rejection == Rejection::kInvalidTopology);
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::Snapshot(scheduler).active);

  GuestSchedulerCheckpointRuntimeTestAccess::SetSnapshotHook(scheduler, nullptr,
                                                             nullptr);
  GuestSchedulerCheckpointBarrierSnapshot retry_snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &retry_snapshot) == Rejection::kNone);
  REQUIRE(retry_snapshot.generation > failed_snapshot.generation);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              retry_snapshot.generation, nullptr) == Rejection::kNone);
}

TEST_CASE("Guest scheduler checkpoint timeout waits for fiber switch-out",
          "[guest_scheduler_checkpoint][runtime]") {
  const ReleaseRaceResult result = RunReleaseRace(ReleaseRaceKind::kTimeout);
  REQUIRE(result.setup);
  REQUIRE(result.observer_attached);
  REQUIRE(result.observer_armed);
  REQUIRE(result.thread_created);
  REQUIRE(result.thread_started);
  REQUIRE(result.arrival_entered);
  REQUIRE(result.release_call_completed);
  REQUIRE(result.pause_completed);
  REQUIRE(result.pause_rejection == Rejection::kTimedOut);
  REQUIRE(result.pause_snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(result.pause_snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
  REQUIRE(result.boundary_is_jit_safepoint);
  REQUIRE(result.held_after_release.found);
  REQUIRE(result.held_after_release.phase == HeldPhase::kReleasePending);
  REQUIRE(result.held_after_release.running);
  REQUIRE_FALSE(result.held_after_release.queued);
  REQUIRE(result.safepoint_returned);
  REQUIRE(result.thread_stopped);
  REQUIRE(result.switch_out_seen);
  REQUIRE(result.redispatch_seen_after_switch_out);
}

TEST_CASE("Guest scheduler checkpoint cancel waits for fiber switch-out",
          "[guest_scheduler_checkpoint][runtime]") {
  const ReleaseRaceResult result = RunReleaseRace(ReleaseRaceKind::kCancel);
  REQUIRE(result.setup);
  REQUIRE(result.arrival_entered);
  REQUIRE(result.release_call_completed);
  REQUIRE(result.release_rejection == Rejection::kCancelled);
  REQUIRE(result.pause_rejection == Rejection::kCancelled);
  REQUIRE(result.pause_snapshot.rejection == Rejection::kCancelled);
  REQUIRE(result.pause_snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(result.pause_snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
  REQUIRE(result.boundary_is_jit_safepoint);
  REQUIRE(result.held_after_release.found);
  REQUIRE(result.held_after_release.phase == HeldPhase::kReleasePending);
  REQUIRE(result.held_after_release.running);
  REQUIRE_FALSE(result.held_after_release.queued);
  REQUIRE(result.safepoint_returned);
  REQUIRE(result.switch_out_seen);
  REQUIRE(result.redispatch_seen_after_switch_out);
}

TEST_CASE("Guest scheduler checkpoint shutdown waits for fiber switch-out",
          "[guest_scheduler_checkpoint][runtime]") {
  const ReleaseRaceResult result = RunReleaseRace(ReleaseRaceKind::kShutdown);
  REQUIRE(result.setup);
  REQUIRE(result.arrival_entered);
  REQUIRE(result.release_call_completed);
  REQUIRE(result.release_rejection == Rejection::kShutdown);
  REQUIRE(result.pause_rejection == Rejection::kShutdown);
  REQUIRE(result.pause_snapshot.rejection == Rejection::kShutdown);
  REQUIRE(result.pause_snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(result.pause_snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
  REQUIRE(result.boundary_is_jit_safepoint);
  REQUIRE(result.held_after_release.found);
  REQUIRE(result.held_after_release.phase == HeldPhase::kReleasePending);
  REQUIRE(result.held_after_release.running);
  REQUIRE_FALSE(result.held_after_release.queued);
  REQUIRE(result.switch_out_seen);
  REQUIRE_FALSE(result.redispatch_seen_after_switch_out);
}

TEST_CASE("Guest scheduler checkpoint roster and release contract are explicit",
          "[guest_scheduler_checkpoint][runtime]") {
  const TopologyResult result = RunTopologyMutation();
  REQUIRE(result.setup);
  REQUIRE(result.observer_attached);
  REQUIRE(result.running_created);
  REQUIRE(result.suspended_created);
  REQUIRE(result.running_started);
  REQUIRE(result.pause_rejection == Rejection::kNone);
  REQUIRE(result.provisional.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(result.provisional.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
  REQUIRE(result.suspended_omitted);
  REQUIRE(result.suspended_in_processor_roster);
  REQUIRE(result.final_rejection == Rejection::kTopologyChanged);
  REQUIRE(result.final_snapshot.rejection == Rejection::kTopologyChanged);
  REQUIRE(result.final_snapshot.roster_scope == RosterScope::kSchedulerOwned);
  REQUIRE(result.final_snapshot.release_policy ==
          ReleasePolicy::kRunningSafepointsRequeueAtHead);
  REQUIRE(result.suspended_started_after_resume);
  REQUIRE(result.running_stopped);
  REQUIRE(result.suspended_stopped);
}

TEST_CASE("Guest scheduler checkpoint-only safepoint does not yield",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  FiberControl control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  REQUIRE(control.WaitForStart(2s));

  PauseResult pause = Pause(scheduler, 2s);
  REQUIRE(pause.rejection == Rejection::kNone);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              pause.snapshot.generation, nullptr) == Rejection::kNone);
  REQUIRE(control.WaitForSafepointReturn(2s));

  const auto snapshot = recorder->snapshot();
  REQUIRE(std::none_of(snapshot.events.begin(), snapshot.events.end(),
                       [](const GuestSchedulerCaptureEvent& event) {
                         return event.kind == Kind::kSafepoint;
                       }));
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::SchedulerSafepointPending(
          created.thread.get()));
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler scheduler-only safepoint yields once",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  FiberControl control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  REQUIRE(control.WaitForStart(2s));
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), true);
  REQUIRE(control.WaitForSafepointReturn(2s));

  const auto snapshot = recorder->snapshot();
  size_t safepoint_count = 0;
  for (const auto& event : snapshot.events) {
    if (event.kind != Kind::kSafepoint) {
      continue;
    }
    ++safepoint_count;
    REQUIRE(event.reason == GuestSchedulerCaptureReason::kYielded);
    REQUIRE(event.flags == kGuestSchedulerCaptureFlagSchedulerRequested);
  }
  REQUIRE(safepoint_count == 1);
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::SchedulerSafepointPending(
          created.thread.get()));
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler checkpoint co-delivers an earlier scheduler request",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  FiberControl control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  REQUIRE(control.WaitForStart(2s));
  created.thread->thread_state()->RequestGuestExecutionCaptureJitSafepoint();
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), false);

  PauseResult pause = Pause(scheduler, 2s);
  REQUIRE(pause.rejection == Rejection::kNone);
  const auto* participant =
      FindThread(pause.snapshot, created.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->guest_pc == kSafepointPc);
  REQUIRE(participant->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              pause.snapshot.generation, nullptr) == Rejection::kNone);
  REQUIRE(control.WaitForSafepointReturn(2s));

  const auto snapshot = recorder->snapshot();
  const uint16_t both = kGuestSchedulerCaptureFlagSchedulerRequested |
                        kGuestSchedulerCaptureFlagCaptureRequested;
  size_t safepoint_count = 0;
  for (const auto& event : snapshot.events) {
    if (event.kind != Kind::kSafepoint) {
      continue;
    }
    ++safepoint_count;
    REQUIRE(event.reason == GuestSchedulerCaptureReason::kYielded);
    REQUIRE(event.flags == both);
  }
  REQUIRE(safepoint_count == 1);
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::SchedulerSafepointPending(
          created.thread.get()));
  std::atomic_ref<uint8_t>(
      created.thread->thread_state()->context()->capture_rendezvous_requested)
      .store(0, std::memory_order_release);
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler checkpoint preserves a request raised while held",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  BlockingGate arrival_gate;
  FiberControl control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  REQUIRE(control.WaitForStart(2s));
  created.thread->thread_state()->RequestGuestExecutionCaptureJitSafepoint();
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), false);
  GuestSchedulerCheckpointRuntimeTestAccess::SetArrivalHook(
      scheduler, &BlockingGate::Hook, &arrival_gate);
  auto pause_future = std::async(std::launch::async,
                                 [&scheduler] { return Pause(scheduler, 2s); });
  const bool arrival_entered = arrival_gate.WaitUntilEntered(2s);
  if (!arrival_entered) {
    arrival_gate.Release();
  }
  REQUIRE(arrival_entered);
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), true);
  arrival_gate.Release();
  REQUIRE(pause_future.wait_for(2s) == std::future_status::ready);
  PauseResult pause = pause_future.get();
  GuestSchedulerCheckpointRuntimeTestAccess::SetArrivalHook(scheduler, nullptr,
                                                            nullptr);
  REQUIRE(pause.rejection == Rejection::kNone);
  const auto* participant =
      FindThread(pause.snapshot, created.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->guest_pc == kSafepointPc);
  REQUIRE(participant->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              pause.snapshot.generation, nullptr) == Rejection::kNone);
  REQUIRE(control.WaitForSafepointReturn(2s));
  REQUIRE_FALSE(control.WaitForSafepointReturns(2, 100ms));

  const auto snapshot = recorder->snapshot();
  size_t safepoint_count = 0;
  for (const auto& event : snapshot.events) {
    if (event.kind != Kind::kSafepoint) {
      continue;
    }
    ++safepoint_count;
    REQUIRE(event.reason == GuestSchedulerCaptureReason::kYielded);
    REQUIRE(event.flags & kGuestSchedulerCaptureFlagSchedulerRequested);
  }
  REQUIRE(safepoint_count == 1);
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::SchedulerSafepointPending(
          created.thread.get()));
  REQUIRE(std::atomic_ref<uint8_t>(
              created.thread->thread_state()->context()->preempt_requested)
              .load(std::memory_order_acquire) == 0);
  std::atomic_ref<uint8_t>(
      created.thread->thread_state()->context()->capture_rendezvous_requested)
      .store(0, std::memory_order_release);
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler dispatch consumes an ordinary off-CPU request",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  BlockingGate yield_gate;
  FiberControl control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(128);
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());

  CreatedThread created =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(created.status));
  REQUIRE(control.WaitForStart(2s));
  GuestSchedulerCheckpointRuntimeTestAccess::SetYieldHook(
      scheduler, &BlockingGate::Hook, &yield_gate);
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), true);
  const bool yield_entered = yield_gate.WaitUntilEntered(2s);
  if (!yield_entered) {
    yield_gate.Release();
  }
  REQUIRE(yield_entered);
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, created.thread.get(), true);
  GuestSchedulerCheckpointRuntimeTestAccess::SetYieldHook(scheduler, nullptr,
                                                          nullptr);
  yield_gate.Release();

  REQUIRE(control.WaitForSafepointReturn(2s));
  REQUIRE_FALSE(control.WaitForSafepointReturns(2, 100ms));
  const auto snapshot = recorder->snapshot();
  size_t safepoint_count = 0;
  for (const auto& event : snapshot.events) {
    if (event.kind != Kind::kSafepoint) {
      continue;
    }
    ++safepoint_count;
    REQUIRE(event.reason == GuestSchedulerCaptureReason::kYielded);
    REQUIRE(event.flags == kGuestSchedulerCaptureFlagSchedulerRequested);
  }
  REQUIRE(safepoint_count == 1);
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::SchedulerSafepointPending(
          created.thread.get()));
  REQUIRE(std::atomic_ref<uint8_t>(
              created.thread->thread_state()->context()->preempt_requested)
              .load(std::memory_order_acquire) == 0);
  REQUIRE(StopRuntimeThread(created, control));
}

TEST_CASE("Guest scheduler yields at the held real JIT safepoint",
          "[guest_scheduler_checkpoint][guest_scheduler_capture][runtime]") {
  BlockingGate arrival_gate;
  FiberControl blocker_control;
  ReplayFiberControl replay_control;
  auto recorder = std::make_shared<GuestSchedulerCaptureEventRecorder>(256);
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  REQUIRE(InstallReplayGuestCode(environment.emulator()));
  GuestScheduler& scheduler = *environment.scheduler();
  REQUIRE(scheduler.AttachCaptureObserver(recorder));
  REQUIRE(recorder->Arm());
  Memory* memory = environment.emulator()->memory();

  const uint32_t shared_address = memory->SystemHeapAlloc(4096);
  REQUIRE(shared_address != 0);
  std::memset(memory->TranslateVirtual(shared_address), 0, 4096);
  store_and_swap<uint32_t>(
      memory->TranslateVirtual(shared_address + kSharedTokenB), 1);

  CreatedReplayThread replay = CreateReplayRuntimeThread(
      environment, replay_control, kReplayResumeA, kReplayCallerA,
      kReplayOuterReturnA, shared_address);
  REQUIRE(XSUCCEEDED(replay.status));
  REQUIRE(XSUCCEEDED(replay.thread->Resume()));
  REQUIRE(WaitForGuestWord(memory, shared_address + kSharedReachedA, 1, 2s));
  REQUIRE(WaitUntilRunning(scheduler, replay.thread.get(), 2s));

  CreatedThread blocker =
      CreateRuntimeThread(environment, blocker_control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(blocker.status));
  REQUIRE_FALSE(blocker_control.WaitForStart(100ms));

  GuestSchedulerCheckpointRuntimeTestAccess::SetArrivalHook(
      scheduler, &BlockingGate::Hook, &arrival_gate);
  auto pause_future = std::async(std::launch::async,
                                 [&scheduler] { return Pause(scheduler, 2s); });
  const bool arrival_entered = arrival_gate.WaitUntilEntered(2s);
  if (!arrival_entered) {
    arrival_gate.Release();
  }
  REQUIRE(arrival_entered);
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, replay.thread.get(), true);
  store_and_swap<uint32_t>(
      memory->TranslateVirtual(shared_address + kSharedPoisonGate), 1);
  arrival_gate.Release();
  REQUIRE(pause_future.wait_for(2s) == std::future_status::ready);
  PauseResult pause = pause_future.get();
  GuestSchedulerCheckpointRuntimeTestAccess::SetArrivalHook(scheduler, nullptr,
                                                            nullptr);
  REQUIRE(pause.rejection == Rejection::kNone);
  const auto* participant =
      FindThread(pause.snapshot, replay.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->guest_pc == kReplayFinalA);
  REQUIRE(participant->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              pause.snapshot.generation, nullptr) == Rejection::kNone);

  REQUIRE(blocker_control.WaitForStart(2s));
  REQUIRE(load_and_swap<uint32_t>(
              memory->TranslateVirtual(shared_address + kSharedPoisonA)) == 0);
  REQUIRE_FALSE(
      GuestSchedulerCheckpointRuntimeTestAccess::SchedulerSafepointPending(
          replay.thread.get()));

  const auto snapshot = recorder->snapshot();
  size_t replay_safepoint_count = 0;
  for (const auto& event : snapshot.events) {
    if (event.kind != Kind::kSafepoint ||
        event.guest_thread_id != replay.thread->thread_id()) {
      continue;
    }
    ++replay_safepoint_count;
    REQUIRE(event.reason == GuestSchedulerCaptureReason::kYielded);
    REQUIRE(event.flags == (kGuestSchedulerCaptureFlagSchedulerRequested |
                            kGuestSchedulerCaptureFlagCaptureRequested));
  }
  REQUIRE(replay_safepoint_count == 1);

  REQUIRE(StopRuntimeThread(blocker, blocker_control));
  REQUIRE(xe::threading::Wait(replay.thread->wait_handle(), false, 2s) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(load_and_swap<uint32_t>(memory->TranslateVirtual(
              shared_address + kSharedPoisonA)) == 0xA1);
  REQUIRE(replay_control.execute_returned.load(std::memory_order_acquire));
  memory->SystemHeapFree(shared_address);
}

TEST_CASE("Guest scheduler checkpoint preserves an exact ready JIT route",
          "[guest_scheduler_checkpoint][runtime]") {
  FiberControl ready_control;
  FiberControl higher_priority_control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread ready =
      CreateRuntimeThread(environment, ready_control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(ready.status));
  REQUIRE(ready_control.WaitForStart(2s));

  CreatedThread higher_priority =
      CreateRuntimeThread(environment, higher_priority_control,
                          kCpu0CreationFlags | X_CREATE_SUSPENDED);
  REQUIRE(XSUCCEEDED(higher_priority.status));
  higher_priority.thread->SetPriority(31);
  REQUIRE(XSUCCEEDED(higher_priority.thread->Resume()));
  REQUIRE(higher_priority_control.WaitForStart(2s));

  const auto parked = GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(
      scheduler, ready.thread.get());
  REQUIRE(parked.queued);
  REQUIRE_FALSE(parked.running);
  REQUIRE(parked.checkpoint_jit_safepoint_pc == kSafepointPc);

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const auto* participant = FindThread(snapshot, ready.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->state ==
          GuestSchedulerCheckpointParticipantState::kReady);
  REQUIRE(participant->capture_instance_id ==
          ready.thread->thread_state()->guest_execution_capture_instance_id());
  REQUIRE(participant->effective_priority == ready.thread->priority());
  REQUIRE(participant->base_priority ==
          ready.thread->guest_object<X_KTHREAD>()->base_priority);
  REQUIRE(participant->suspension_count == 0);
  REQUIRE(participant->quantum_remaining_us == 0);
  REQUIRE(participant->ready_queue_level == ready.thread->priority());
  REQUIRE(participant->ready_queue_fifo_ordinal != UINT32_MAX);
  REQUIRE(participant->guest_pc == kSafepointPc);
  REQUIRE(participant->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(participant->restorable);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              snapshot.generation, nullptr) == Rejection::kNone);

  REQUIRE(StopRuntimeThread(higher_priority, higher_priority_control));
  REQUIRE(ready_control.WaitForSafepointReturn(2s));
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(
              scheduler, ready.thread.get())
              .checkpoint_jit_safepoint_pc == 0);
  REQUIRE(StopRuntimeThread(ready, ready_control));
}

TEST_CASE("Guest scheduler checkpoint preserves an exact suspended JIT route",
          "[guest_scheduler_checkpoint][runtime]") {
  FiberControl control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread suspended =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(suspended.status));
  REQUIRE(control.WaitForStart(2s));
  REQUIRE(XSUCCEEDED(suspended.thread->Suspend()));
  suspended.thread->thread_state()->context()->preempt_requested = 1;
  REQUIRE(WaitUntilSuspended(scheduler, suspended.thread.get(), 2s));

  const auto parked = GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(
      scheduler, suspended.thread.get());
  REQUIRE_FALSE(parked.queued);
  REQUIRE(parked.suspended);
  REQUIRE_FALSE(parked.running);
  REQUIRE(parked.checkpoint_jit_safepoint_pc == kSafepointPc);

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const auto* participant = FindThread(snapshot, suspended.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->state ==
          GuestSchedulerCheckpointParticipantState::kSuspended);
  REQUIRE(
      participant->capture_instance_id ==
      suspended.thread->thread_state()->guest_execution_capture_instance_id());
  REQUIRE(participant->effective_priority == suspended.thread->priority());
  REQUIRE(participant->base_priority ==
          suspended.thread->guest_object<X_KTHREAD>()->base_priority);
  REQUIRE(participant->suspension_count == suspended.thread->suspend_count());
  REQUIRE(participant->suspension_count != 0);
  REQUIRE(participant->quantum_remaining_us == 0);
  REQUIRE(participant->ready_queue_level == -1);
  REQUIRE(participant->ready_queue_fifo_ordinal == UINT32_MAX);
  REQUIRE(participant->guest_pc == kSafepointPc);
  REQUIRE(participant->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(participant->restorable);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              snapshot.generation, nullptr) == Rejection::kNone);

  REQUIRE(XSUCCEEDED(suspended.thread->Resume()));
  REQUIRE(control.WaitForSafepointReturn(2s));
  REQUIRE(GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(
              scheduler, suspended.thread.get())
              .checkpoint_jit_safepoint_pc == 0);
  REQUIRE(StopRuntimeThread(suspended, control));
}

TEST_CASE("Guest scheduler checkpoint authenticates a blocked wait topology",
          "[guest_scheduler_checkpoint][guest_scheduler_capture_wait]"
          "[runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto event =
      object_ref<XEvent>(new XEvent(environment.emulator()->kernel_state()));
  event->Initialize(false, false);
  RereadyWaitControl control;
  constexpr uint32_t kBlockedContinuationPc = 0x82004000;
  auto thread =
      object_ref<RereadyWaitRuntimeThread>(new RereadyWaitRuntimeThread(
          environment.emulator()->kernel_state(), &control, event.get(),
          kBlockedContinuationPc));
  thread->set_name("Scheduler blocked checkpoint topology test");
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);

  const auto blocked_deadline = std::chrono::steady_clock::now() + 2s;
  while (!GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                thread.get()) &&
         std::chrono::steady_clock::now() < blocked_deadline) {
    std::this_thread::yield();
  }
  REQUIRE(GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                thread.get()));

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const auto* participant = FindThread(snapshot, thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->state ==
          GuestSchedulerCheckpointParticipantState::kBlocked);
  REQUIRE(participant->cpu >= 0);
  REQUIRE(participant->effective_priority == thread->priority());
  REQUIRE(participant->base_priority ==
          thread->guest_object<X_KTHREAD>()->base_priority);
  REQUIRE(participant->suspension_count == 0);
  REQUIRE(participant->quantum_remaining_us == 0);
  REQUIRE(participant->ready_queue_level == -1);
  REQUIRE(participant->ready_queue_fifo_ordinal == UINT32_MAX);
  REQUIRE(participant->resume_kind == ResumeKind::kAfterBlockingExport);
  REQUIRE(participant->guest_pc == kBlockedContinuationPc);
  REQUIRE_FALSE(participant->restorable);
  REQUIRE(participant->blocked_wait_kind ==
          GuestSchedulerCaptureWaitKind::kSingle);
  REQUIRE(participant->blocked_wait.handle_count == 1);
  REQUIRE(participant->blocked_wait.handles[0] == event->handle());
  REQUIRE(participant->blocked_wait.flags ==
          (kGuestSchedulerCaptureWaitFlagGated |
           kGuestSchedulerCaptureWaitFlagInterruptible));
  REQUIRE(participant->blocked_wait.wait_epoch ==
          participant->blocked_wait.signal_epochs_before[0]);
  REQUIRE(participant->blocked_wait.observed_wait_epoch ==
          participant->blocked_wait.signal_epochs_observed[0]);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              snapshot.generation, nullptr) == Rejection::kNone);

  event->Set(0, false);
  REQUIRE(control.WaitForCompletion(2s));
  scheduler.Shutdown();
  thread->ReclaimExited();
  thread->ReleaseHandle();
  thread.reset();
  event->ReleaseHandle();
  event.reset();
}

TEST_CASE("Guest scheduler checkpoint names a re-readied waiter's wait shape",
          "[guest_scheduler_checkpoint][guest_scheduler_capture_wait]"
          "[runtime]") {
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();
  auto event =
      object_ref<XEvent>(new XEvent(environment.emulator()->kernel_state()));
  event->Initialize(false, false);
  RereadyWaitControl control;
  auto waiter =
      object_ref<RereadyWaitRuntimeThread>(new RereadyWaitRuntimeThread(
          environment.emulator()->kernel_state(), &control, event.get()));
  waiter->set_name("Scheduler re-readied waiter census test");
  REQUIRE(waiter->Create() == X_STATUS_SUCCESS);

  const auto blocked_deadline = std::chrono::steady_clock::now() + 2s;
  while (!GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                waiter.get()) &&
         std::chrono::steady_clock::now() < blocked_deadline) {
    std::this_thread::yield();
  }
  REQUIRE(GuestSchedulerCaptureWaitRuntimeTestAccess::IsBlocked(scheduler,
                                                                waiter.get()));

  // A higher-priority fiber holds the woken waiter on the ready queue, which is
  // the state whose wait source a checkpoint could not name before.
  FiberControl higher_priority_control;
  CreatedThread higher_priority =
      CreateRuntimeThread(environment, higher_priority_control,
                          kCpu0CreationFlags | X_CREATE_SUSPENDED);
  REQUIRE(XSUCCEEDED(higher_priority.status));
  higher_priority.thread->SetPriority(31);
  REQUIRE(XSUCCEEDED(higher_priority.thread->Resume()));
  REQUIRE(higher_priority_control.WaitForStart(2s));

  event->Set(0, false);
  // No timeslice here: only a safepoint returns this CPU to the re-ready poll.
  GuestSchedulerCheckpointRuntimeTestAccess::RequestSchedulerSafepoint(
      scheduler, higher_priority.thread.get(), true);
  REQUIRE(higher_priority_control.WaitForSafepointReturn(2s));
  REQUIRE(WaitUntilQueued(scheduler, waiter.get(), 2s));

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const auto* participant = FindThread(snapshot, waiter->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->state ==
          GuestSchedulerCheckpointParticipantState::kReady);
  REQUIRE(participant->resume_kind == ResumeKind::kNativeContinuation);
  REQUIRE(participant->guest_pc == 0);
  REQUIRE_FALSE(participant->restorable);
  REQUIRE(participant->blocked_wait_kind ==
          GuestSchedulerCaptureWaitKind::kSingle);
  REQUIRE(participant->blocked_wait.handle_count == 1);
  REQUIRE(participant->blocked_wait.flags ==
          (kGuestSchedulerCaptureWaitFlagGated |
           kGuestSchedulerCaptureWaitFlagInterruptible));
  // Scalars only: the handle and epoch arrays read wait_gate_objects, which is
  // valid only while the fiber is parked.
  REQUIRE(participant->blocked_wait.handles[0] == 0);
  REQUIRE(participant->blocked_wait.wait_epoch == 0);
  REQUIRE(participant->blocked_wait.observed_wait_epoch == 0);
  REQUIRE(participant->blocked_wait.deadline_ms == 0);
  REQUIRE(participant->blocked_wait.observed_uptime_ms == 0);
  REQUIRE(participant->blocked_wait.signal_epochs_before[0] == 0);
  REQUIRE(participant->blocked_wait.signal_epochs_observed[0] == 0);
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              snapshot.generation, nullptr) == Rejection::kNone);

  REQUIRE(StopRuntimeThread(higher_priority, higher_priority_control));
  REQUIRE(control.WaitForCompletion(2s));
  scheduler.Shutdown();
  waiter->ReclaimExited();
  waiter->ReleaseHandle();
  waiter.reset();
  event->ReleaseHandle();
  event.reset();
}

TEST_CASE("Guest scheduler discards a suspended exact JIT route",
          "[guest_scheduler_checkpoint][runtime][continuous_replay]") {
  FiberControl control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread suspended =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(suspended.status));
  REQUIRE(control.WaitForStart(2s));
  REQUIRE(XSUCCEEDED(suspended.thread->Suspend()));
  suspended.thread->thread_state()->context()->preempt_requested = 1;
  REQUIRE(WaitUntilSuspended(scheduler, suspended.thread.get(), 2s));

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const std::array<GuestSchedulerCheckpointJitRoute, 1> route = {{
      {suspended.thread.get(), kSafepointPc},
  }};
  GuestSchedulerCheckpointBarrierSnapshot stale_snapshot;
  stale_snapshot.generation = 0xFFFFFFFFFFFFFFFFull;
  stale_snapshot.rejection = Rejection::kNone;
  REQUIRE(scheduler.FinalizeAndDiscardCheckpointBarrier(
              snapshot.generation + 1, route, &stale_snapshot) ==
          Rejection::kStaleGeneration);
  REQUIRE(stale_snapshot.generation == snapshot.generation + 1);
  REQUIRE(stale_snapshot.rejection == Rejection::kStaleGeneration);
  REQUIRE_FALSE(stale_snapshot.active);

  GuestSchedulerCheckpointBarrierSnapshot discarded_snapshot;
  REQUIRE(scheduler.FinalizeAndDiscardCheckpointBarrier(
              snapshot.generation, route, &discarded_snapshot) ==
          Rejection::kNone);
  REQUIRE(discarded_snapshot.rejection == Rejection::kNone);

  GuestSchedulerCheckpointBarrierSnapshot duplicate_snapshot;
  duplicate_snapshot.generation = 0xFFFFFFFFFFFFFFFFull;
  duplicate_snapshot.rejection = Rejection::kInvalidTopology;
  REQUIRE(scheduler.FinalizeAndDiscardCheckpointBarrier(
              snapshot.generation, route, &duplicate_snapshot) ==
          Rejection::kNotActive);
  REQUIRE(duplicate_snapshot.generation == snapshot.generation);
  REQUIRE(duplicate_snapshot.rejection == Rejection::kNotActive);
  REQUIRE_FALSE(duplicate_snapshot.active);
  REQUIRE(xe::threading::Wait(suspended.thread->wait_handle(), false, 2s) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(control.safepoint_returns.load(std::memory_order_acquire) == 0);
}

TEST_CASE("Guest scheduler shutdown drains an accepted checkpoint discard",
          "[guest_scheduler_checkpoint][runtime][continuous_replay]") {
  BlockingGate finalized_gate;
  FiberControl control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread suspended =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(suspended.status));
  REQUIRE(control.WaitForStart(2s));
  REQUIRE(XSUCCEEDED(suspended.thread->Suspend()));
  suspended.thread->thread_state()->context()->preempt_requested = 1;
  REQUIRE(WaitUntilSuspended(scheduler, suspended.thread.get(), 2s));

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const std::array<GuestSchedulerCheckpointJitRoute, 1> route = {{
      {suspended.thread.get(), kSafepointPc},
  }};
  GuestSchedulerCheckpointRuntimeTestAccess::SetDiscardHook(
      scheduler, &BlockingGate::Hook, &finalized_gate);
  auto finalize_future = std::async(
      std::launch::async,
      [&scheduler, &route, generation = snapshot.generation] {
        return scheduler.FinalizeAndDiscardCheckpointBarrier(generation, route);
      });
  const bool finalize_held = finalized_gate.WaitUntilEntered(2s);
  if (!finalize_held) {
    finalized_gate.Release();
  }
  REQUIRE(finalize_held);

  auto shutdown_future =
      std::async(std::launch::async, [&scheduler]() { scheduler.Shutdown(); });
  const bool shutdown_completed_before_finalize_return =
      shutdown_future.wait_for(2s) == std::future_status::ready;
  const bool exit_signaled_before_finalize_return =
      xe::threading::Wait(suspended.thread->wait_handle(), false, 0ms) ==
      xe::threading::WaitResult::kSuccess;
  const X_KTHREAD* kthread = suspended.thread->guest_object<X_KTHREAD>();
  const bool full_exit_state_before_finalize_return =
      kthread->terminated &&
      kthread->thread_state == KTHREAD_STATE_TERMINATED &&
      kthread->header.signal_state == 1 && kthread->exit_status == 0;

  finalized_gate.Release();
  GuestSchedulerCheckpointRuntimeTestAccess::SetDiscardHook(scheduler, nullptr,
                                                            nullptr);
  REQUIRE(shutdown_future.wait_for(2s) == std::future_status::ready);
  shutdown_future.get();
  REQUIRE(finalize_future.wait_for(2s) == std::future_status::ready);
  REQUIRE(finalize_future.get() == Rejection::kNone);
  REQUIRE(shutdown_completed_before_finalize_return);
  REQUIRE(exit_signaled_before_finalize_return);
  REQUIRE(full_exit_state_before_finalize_return);
  REQUIRE(control.safepoint_returns.load(std::memory_order_acquire) == 0);
}

TEST_CASE("Guest scheduler checkpoint discard owns a racing terminate",
          "[guest_scheduler_checkpoint][runtime][continuous_replay]") {
  BlockingGate finalized_gate;
  FiberControl control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread suspended =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(suspended.status));
  REQUIRE(control.WaitForStart(2s));
  REQUIRE(XSUCCEEDED(suspended.thread->Suspend()));
  suspended.thread->thread_state()->context()->preempt_requested = 1;
  REQUIRE(WaitUntilSuspended(scheduler, suspended.thread.get(), 2s));

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const std::array<GuestSchedulerCheckpointJitRoute, 1> route = {{
      {suspended.thread.get(), kSafepointPc},
  }};
  GuestSchedulerCheckpointRuntimeTestAccess::SetDiscardHook(
      scheduler, &BlockingGate::Hook, &finalized_gate);
  auto finalize_future = std::async(
      std::launch::async,
      [&scheduler, &route, generation = snapshot.generation] {
        return scheduler.FinalizeAndDiscardCheckpointBarrier(generation, route);
      });
  const bool finalize_held = finalized_gate.WaitUntilEntered(2s);
  if (!finalize_held) {
    finalized_gate.Release();
  }
  REQUIRE(finalize_held);

  const X_STATUS terminate_status = suspended.thread->Terminate(0x1357);
  const bool exit_unsignaled =
      xe::threading::Wait(suspended.thread->wait_handle(), false, 0ms) ==
      xe::threading::WaitResult::kTimeout;
  const bool guest_state_untouched =
      suspended.thread->guest_object<X_KTHREAD>()->header.signal_state == 0;

  finalized_gate.Release();
  GuestSchedulerCheckpointRuntimeTestAccess::SetDiscardHook(scheduler, nullptr,
                                                            nullptr);
  REQUIRE(XSUCCEEDED(terminate_status));
  REQUIRE(exit_unsignaled);
  REQUIRE(guest_state_untouched);
  REQUIRE(finalize_future.wait_for(2s) == std::future_status::ready);
  REQUIRE(finalize_future.get() == Rejection::kNone);
  REQUIRE(xe::threading::Wait(suspended.thread->wait_handle(), false, 2s) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(suspended.thread->guest_object<X_KTHREAD>()->exit_status == 0);
  REQUIRE(control.safepoint_returns.load(std::memory_order_acquire) == 0);
}

TEST_CASE("Guest scheduler terminate owns a racing checkpoint discard",
          "[guest_scheduler_checkpoint][runtime][continuous_replay]") {
  BlockingGate terminate_gate;
  FiberControl control;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  GuestScheduler& scheduler = *environment.scheduler();

  CreatedThread suspended =
      CreateRuntimeThread(environment, control, kCpu0CreationFlags);
  REQUIRE(XSUCCEEDED(suspended.status));
  REQUIRE(control.WaitForStart(2s));
  REQUIRE(XSUCCEEDED(suspended.thread->Suspend()));
  suspended.thread->thread_state()->context()->preempt_requested = 1;
  REQUIRE(WaitUntilSuspended(scheduler, suspended.thread.get(), 2s));

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &snapshot) == Rejection::kNone);
  const std::array<GuestSchedulerCheckpointJitRoute, 1> route = {{
      {suspended.thread.get(), kSafepointPc},
  }};
  GuestSchedulerCheckpointRuntimeTestAccess::SetTerminateHook(
      scheduler, &BlockingGate::Hook, &terminate_gate);
  auto terminate_future = std::async(std::launch::async, [&suspended]() {
    return suspended.thread->Terminate(0x2468);
  });
  const bool terminate_held = terminate_gate.WaitUntilEntered(2s);
  if (!terminate_held) {
    terminate_gate.Release();
  }
  REQUIRE(terminate_held);
  const bool guest_state_untouched =
      suspended.thread->guest_object<X_KTHREAD>()->header.signal_state == 0;

  GuestSchedulerCheckpointBarrierSnapshot rejected_snapshot;
  const Rejection discard_rejection =
      scheduler.FinalizeAndDiscardCheckpointBarrier(snapshot.generation, route,
                                                    &rejected_snapshot);

  terminate_gate.Release();
  GuestSchedulerCheckpointRuntimeTestAccess::SetTerminateHook(scheduler,
                                                              nullptr, nullptr);
  REQUIRE(guest_state_untouched);
  REQUIRE(discard_rejection == Rejection::kTopologyChanged);
  REQUIRE(rejected_snapshot.generation == snapshot.generation);
  REQUIRE(rejected_snapshot.rejection == Rejection::kTopologyChanged);
  REQUIRE(terminate_future.wait_for(2s) == std::future_status::ready);
  REQUIRE(XSUCCEEDED(terminate_future.get()));
  REQUIRE(xe::threading::Wait(suspended.thread->wait_handle(), false, 2s) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(suspended.thread->guest_object<X_KTHREAD>()->exit_status == 0x2468);
  REQUIRE(control.safepoint_returns.load(std::memory_order_acquire) == 0);
}

TEST_CASE(
    "Guest scheduler discards an authenticated continuous replay boundary",
    "[guest_scheduler_checkpoint][runtime][continuous_replay]") {
  ReplayFiberControl control_a;
  ReplayFiberControl control_b;
  SchedulerEnvironment environment;
  REQUIRE(environment.ready());
  REQUIRE(InstallReplayGuestCode(environment.emulator()));
  GuestScheduler& scheduler = *environment.scheduler();
  Memory* memory = environment.emulator()->memory();

  const uint32_t shared_address = memory->SystemHeapAlloc(4096);
  REQUIRE(shared_address != 0);
  std::memset(memory->TranslateVirtual(shared_address), 0, 4096);

  CreatedReplayThread replay_a = CreateReplayRuntimeThread(
      environment, control_a, kReplayResumeA, kReplayCallerA,
      kReplayOuterReturnA, shared_address);
  CreatedReplayThread replay_b = CreateReplayRuntimeThread(
      environment, control_b, kReplayResumeB, kReplayCallerB,
      kReplayOuterReturnB, shared_address);
  REQUIRE(XSUCCEEDED(replay_a.status));
  REQUIRE(XSUCCEEDED(replay_b.status));
  REQUIRE(kReplayOuterReturnA != 0xBCBCBCBCu);
  REQUIRE(kReplayOuterReturnB != 0xBCBCBCBCu);

  REQUIRE(XSUCCEEDED(replay_a.thread->Resume()));
  REQUIRE(WaitForGuestWord(memory, shared_address + kSharedTokenA, 1, 2s));

  replay_b.thread->SetPriority(31);
  REQUIRE(XSUCCEEDED(replay_b.thread->Resume()));
  REQUIRE(WaitForGuestWord(memory, shared_address + kSharedTokenB, 1, 2s));
  REQUIRE(WaitForGuestWord(memory, shared_address + kSharedReachedB, 1, 2s));

  replay_b.thread->SetPriority(0);
  replay_a.thread->SetPriority(31);
  REQUIRE(WaitForGuestWord(memory, shared_address + kSharedReachedA, 1, 2s));

  const auto ready_b = GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(
      scheduler, replay_b.thread.get());
  REQUIRE(ready_b.queued);
  REQUIRE_FALSE(ready_b.running);
  REQUIRE(ready_b.checkpoint_jit_safepoint_pc == kReplayFinalB);

  GuestSchedulerCheckpointBarrierSnapshot incomplete_snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &incomplete_snapshot) ==
          Rejection::kNone);
  REQUIRE(incomplete_snapshot.quiesced);
  const std::array<GuestSchedulerCheckpointJitRoute, 2> duplicate_routes = {{
      {replay_a.thread.get(), kReplayFinalA},
      {replay_a.thread.get(), kReplayFinalA},
  }};
  GuestSchedulerCheckpointBarrierSnapshot rejected_snapshot;
  REQUIRE(scheduler.FinalizeAndDiscardCheckpointBarrier(
              incomplete_snapshot.generation, duplicate_routes,
              &rejected_snapshot) == Rejection::kInvalidTopology);
  REQUIRE(rejected_snapshot.rejection == Rejection::kInvalidTopology);
  REQUIRE(xe::threading::Wait(replay_a.thread->wait_handle(), false, 0ms) ==
          xe::threading::WaitResult::kTimeout);
  REQUIRE(xe::threading::Wait(replay_b.thread->wait_handle(), false, 0ms) ==
          xe::threading::WaitResult::kTimeout);

  REQUIRE(WaitUntilRunning(scheduler, replay_a.thread.get(), 2s));
  GuestSchedulerCheckpointBarrierSnapshot wrong_pc_snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &wrong_pc_snapshot) == Rejection::kNone);
  REQUIRE(wrong_pc_snapshot.quiesced);
  const std::array<GuestSchedulerCheckpointJitRoute, 2> wrong_pc_routes = {{
      {replay_a.thread.get(), kReplayFinalA + 4},
      {replay_b.thread.get(), kReplayFinalB},
  }};
  GuestSchedulerCheckpointBarrierSnapshot wrong_pc_rejected_snapshot;
  REQUIRE(scheduler.FinalizeAndDiscardCheckpointBarrier(
              wrong_pc_snapshot.generation, wrong_pc_routes,
              &wrong_pc_rejected_snapshot) == Rejection::kInvalidTopology);
  REQUIRE(wrong_pc_rejected_snapshot.rejection == Rejection::kInvalidTopology);
  REQUIRE(xe::threading::Wait(replay_a.thread->wait_handle(), false, 0ms) ==
          xe::threading::WaitResult::kTimeout);
  REQUIRE(xe::threading::Wait(replay_b.thread->wait_handle(), false, 0ms) ==
          xe::threading::WaitResult::kTimeout);

  REQUIRE(WaitUntilRunning(scheduler, replay_a.thread.get(), 2s));
  GuestSchedulerCheckpointBarrierSnapshot held_snapshot;
  REQUIRE(PauseSettled(scheduler, 2s, &held_snapshot) == Rejection::kNone);
  REQUIRE(held_snapshot.quiesced);
  REQUIRE(held_snapshot.participants.size() == 2);

  const auto* held_a = FindThread(held_snapshot, replay_a.thread->thread_id());
  const auto* held_b = FindThread(held_snapshot, replay_b.thread->thread_id());
  REQUIRE(held_a);
  REQUIRE(held_b);
  REQUIRE(held_a->state == GuestSchedulerCheckpointParticipantState::kRunning);
  REQUIRE(held_b->state == GuestSchedulerCheckpointParticipantState::kReady);
  REQUIRE(held_a->guest_pc == kReplayFinalA);
  REQUIRE(held_b->guest_pc == kReplayFinalB);
  REQUIRE(held_a->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(held_b->resume_kind == ResumeKind::kJitSafepoint);
  REQUIRE(held_a->restorable);
  REQUIRE(held_b->restorable);

  const auto* context_a = replay_a.thread->thread_state()->context();
  const auto* context_b = replay_b.thread->thread_state()->context();
  REQUIRE(context_a->r[3] == 1);
  REQUIRE(context_b->r[3] == 2);
  REQUIRE(context_a->r[11] == kReplayOuterReturnA);
  REQUIRE(context_b->r[11] == kReplayOuterReturnB);
  REQUIRE(context_a->lr == kReplayCallerA);
  REQUIRE(context_b->lr == kReplayCallerB);

  store_and_swap<uint32_t>(
      memory->TranslateVirtual(shared_address + kSharedPoisonGate), 1);
  const std::array<GuestSchedulerCheckpointJitRoute, 2> routes = {{
      {replay_a.thread.get(), kReplayFinalA},
      {replay_b.thread.get(), kReplayFinalB},
  }};
  GuestSchedulerCheckpointBarrierSnapshot discarded_snapshot;
  REQUIRE(scheduler.FinalizeAndDiscardCheckpointBarrier(
              held_snapshot.generation, routes, &discarded_snapshot) ==
          Rejection::kNone);
  REQUIRE(discarded_snapshot.quiesced);
  REQUIRE_FALSE(discarded_snapshot.active);
  REQUIRE(discarded_snapshot.rejection == Rejection::kNone);

  REQUIRE(xe::threading::Wait(replay_a.thread->wait_handle(), false, 2s) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE(xe::threading::Wait(replay_b.thread->wait_handle(), false, 2s) ==
          xe::threading::WaitResult::kSuccess);
  REQUIRE_FALSE(control_a.execute_returned.load(std::memory_order_acquire));
  REQUIRE_FALSE(control_b.execute_returned.load(std::memory_order_acquire));
  REQUIRE(load_and_swap<uint32_t>(
              memory->TranslateVirtual(shared_address + kSharedPoisonA)) == 0);
  REQUIRE(load_and_swap<uint32_t>(
              memory->TranslateVirtual(shared_address + kSharedPoisonB)) == 0);

  memory->SystemHeapFree(shared_address);
}

}  // namespace testing
}  // namespace kernel
}  // namespace xe

#endif
