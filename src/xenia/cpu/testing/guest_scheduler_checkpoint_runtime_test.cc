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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
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
constexpr uint32_t kCpu0CreationFlags = uint32_t{1} << 24;
constexpr uint32_t kCpu1CreationFlags = uint32_t{2} << 24;

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
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(
        lock, timeout, [this]() { return safepoint_returns.load() > 0; });
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
      handler(context, kSafepointPc);
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
                                  uint32_t creation_flags,
                                  int32_t priority = -1) {
  CreatedThread result;
  if (!environment.ready()) {
    return result;
  }
  result.thread =
      object_ref<CheckpointRuntimeThread>(new CheckpointRuntimeThread(
          environment.emulator()->kernel_state(), &control, creation_flags));
  result.thread->set_name("Checkpoint runtime test");
  if (priority >= 0) {
    result.thread->SetPriority(priority);
  }
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

  result.pause_rejection =
      scheduler.PauseForCheckpointBarrier(2s, &result.provisional);
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
  REQUIRE(scheduler.PauseForCheckpointBarrier(2s, &snapshot) ==
          Rejection::kNone);
  REQUIRE(snapshot.dispatch_cpu_mask == 0b111111);
  REQUIRE(snapshot.participants.empty());
  REQUIRE(scheduler.FinalizeAndResumeCheckpointBarrier(
              snapshot.generation, nullptr) == Rejection::kNone);
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

  CreatedThread higher_priority = CreateRuntimeThread(
      environment, higher_priority_control, kCpu0CreationFlags, 31);
  REQUIRE(XSUCCEEDED(higher_priority.status));
  REQUIRE(higher_priority_control.WaitForStart(2s));

  const auto parked = GuestSchedulerCheckpointRuntimeTestAccess::InspectThread(
      scheduler, ready.thread.get());
  REQUIRE(parked.queued);
  REQUIRE_FALSE(parked.running);
  REQUIRE(parked.checkpoint_jit_safepoint_pc == kSafepointPc);

  GuestSchedulerCheckpointBarrierSnapshot snapshot;
  REQUIRE(scheduler.PauseForCheckpointBarrier(2s, &snapshot) ==
          Rejection::kNone);
  const auto* participant = FindThread(snapshot, ready.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->state ==
          GuestSchedulerCheckpointParticipantState::kReady);
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
  REQUIRE(scheduler.PauseForCheckpointBarrier(2s, &snapshot) ==
          Rejection::kNone);
  const auto* participant = FindThread(snapshot, suspended.thread->thread_id());
  REQUIRE(participant);
  REQUIRE(participant->state ==
          GuestSchedulerCheckpointParticipantState::kSuspended);
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

}  // namespace testing
}  // namespace kernel
}  // namespace xe

#endif
