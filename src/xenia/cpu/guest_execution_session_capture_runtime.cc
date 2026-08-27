/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_capture_runtime.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/guest_scheduler.h"

namespace xe {
namespace cpu {
namespace {

using RuntimeState = GuestExecutionSessionCaptureRuntimeState;
using RuntimeRejection = GuestExecutionSessionCaptureRuntimeRejection;
using AssemblerAction = GuestExecutionSessionAssemblerAction;
using AssemblerState = GuestExecutionSessionAssemblerState;
using CheckpointParticipant = kernel::GuestSchedulerCheckpointParticipant;
using CheckpointRejection = kernel::GuestSchedulerCheckpointBarrierRejection;
using CheckpointResumeKind = kernel::GuestSchedulerCheckpointResumeKind;
using CheckpointSnapshot = kernel::GuestSchedulerCheckpointBarrierSnapshot;

bool Fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

bool IsPowerOfTwo(size_t value) { return value >= 2 && !(value & (value - 1)); }

bool IsTerminal(RuntimeState state) {
  return state == RuntimeState::kComplete || state == RuntimeState::kRejected ||
         state == RuntimeState::kShutdown;
}

const CheckpointParticipant* FindCheckpointParticipant(
    const CheckpointSnapshot& checkpoint, uint32_t guest_thread_id) {
  const auto it = std::find_if(
      checkpoint.participants.begin(), checkpoint.participants.end(),
      [guest_thread_id](const CheckpointParticipant& entry) {
        return entry.thread_id == guest_thread_id;
      });
  return it == checkpoint.participants.end() ? nullptr : &*it;
}

class DirectCheckpointController final
    : public GuestExecutionSessionCaptureRuntimeCheckpointController {
 public:
  explicit DirectCheckpointController(kernel::GuestScheduler& scheduler)
      : scheduler_(scheduler) {}

  CheckpointRejection Pause(std::chrono::milliseconds timeout,
                            CheckpointSnapshot* snapshot) override {
    return scheduler_.PauseForCheckpointBarrier(timeout, snapshot);
  }

  CheckpointRejection Finalize(uint64_t generation,
                               CheckpointSnapshot* snapshot) override {
    return scheduler_.FinalizeAndResumeCheckpointBarrier(generation, snapshot);
  }

  CheckpointRejection Cancel(uint64_t generation,
                             CheckpointSnapshot* snapshot) override {
    return scheduler_.CancelCheckpointBarrier(generation, snapshot);
  }

 private:
  kernel::GuestScheduler& scheduler_;
};

class DeferredPublisher final : public GuestExecutionSessionAssemblerPublisher {
 public:
  bool Publish(const GuestExecutionSessionBundle& bundle,
               std::string* error) noexcept override {
    if (bundle_) {
      return Fail(error, "capture runtime received a second staged bundle");
    }
    try {
      bundle_ = bundle;
    } catch (...) {
      return Fail(error, "capture runtime could not stage the session bundle");
    }
    return true;
  }

  std::optional<GuestExecutionSessionBundle> Take() {
    std::optional<GuestExecutionSessionBundle> result = std::move(bundle_);
    bundle_.reset();
    return result;
  }

  void Clear() { bundle_.reset(); }

 private:
  std::optional<GuestExecutionSessionBundle> bundle_;
};

enum class RuntimeEventKind : uint8_t {
  kStart,
  kStop,
  kLifecycle,
  kHostCallBegin,
  kHostCallEnd,
  kScheduler,
};

struct RuntimeEvent {
  RuntimeEventKind kind = RuntimeEventKind::kStart;
  GuestExecutionCaptureParticipantIdentity participant;
  GuestExecutionCaptureThreadStateLifecycleEvent lifecycle;
  GuestExecutionCaptureHostCallToken host_call_token;
  GuestExecutionCaptureHostCallOutcome host_call_outcome =
      GuestExecutionCaptureHostCallOutcome::kReturnedToHost;
  uint32_t function_address = 0;
  uint32_t function_end_address = 0;
  uint32_t return_address = 0;
  kernel::GuestSchedulerCaptureEvent scheduler;
};

// Dmitry Vyukov's bounded sequence-cell queue. There is one consumer and any
// number of callback producers. Producers perform no allocation or waiting.
class RuntimeEventQueue final {
 public:
  explicit RuntimeEventQueue(size_t capacity)
      : cells_(std::make_unique<Cell[]>(capacity)),
        mask_(capacity - 1),
        capacity_(capacity) {
    for (size_t index = 0; index < capacity; ++index) {
      cells_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  bool TryPush(const RuntimeEvent& event) noexcept {
    size_t position = enqueue_position_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[position & mask_];
      const size_t sequence = cell.sequence.load(std::memory_order_acquire);
      const intptr_t difference =
          static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
      if (!difference) {
        if (enqueue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          cell.event = event;
          cell.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;
      } else {
        position = enqueue_position_.load(std::memory_order_relaxed);
      }
    }
  }

  bool TryPop(RuntimeEvent* event) noexcept {
    Cell& cell = cells_[dequeue_position_ & mask_];
    const size_t sequence = cell.sequence.load(std::memory_order_acquire);
    const intptr_t difference = static_cast<intptr_t>(sequence) -
                                static_cast<intptr_t>(dequeue_position_ + 1);
    if (difference) {
      return false;
    }
    *event = cell.event;
    cell.sequence.store(dequeue_position_ + capacity_,
                        std::memory_order_release);
    ++dequeue_position_;
    return true;
  }

  bool MaybeNotEmpty() const noexcept {
    return enqueue_position_.load(std::memory_order_acquire) !=
           dequeue_position_;
  }

 private:
  struct Cell {
    std::atomic<size_t> sequence{0};
    RuntimeEvent event;
  };

  std::unique_ptr<Cell[]> cells_;
  const size_t mask_;
  const size_t capacity_;
  alignas(64) std::atomic<size_t> enqueue_position_{0};
  alignas(64) size_t dequeue_position_ = 0;
};

enum class AsyncFailure : uint8_t {
  kNone,
  kQueueOverflow,
  kSchedulerSequence,
  kHostCallRoster,
  kUnexpectedJitSafepoint,
  kSourceAfterSeal,
};

RuntimeRejection MapAsyncFailure(AsyncFailure failure) {
  switch (failure) {
    case AsyncFailure::kQueueOverflow:
      return RuntimeRejection::kQueueOverflow;
    case AsyncFailure::kSchedulerSequence:
      return RuntimeRejection::kSchedulerSequence;
    case AsyncFailure::kHostCallRoster:
    case AsyncFailure::kUnexpectedJitSafepoint:
    case AsyncFailure::kSourceAfterSeal:
      return RuntimeRejection::kSourceRejected;
    case AsyncFailure::kNone:
      break;
  }
  return RuntimeRejection::kSourceRejected;
}

const char* AsyncFailureMessage(AsyncFailure failure) {
  switch (failure) {
    case AsyncFailure::kQueueOverflow:
      return "capture runtime callback queue overflowed";
    case AsyncFailure::kSchedulerSequence:
      return "capture runtime scheduler event sequence is discontinuous";
    case AsyncFailure::kHostCallRoster:
      return "capture runtime host-call roster rejected an observation";
    case AsyncFailure::kUnexpectedJitSafepoint:
      return "capture runtime received an unsupported Processor JIT safepoint";
    case AsyncFailure::kSourceAfterSeal:
      return "capture runtime observed a source mutation while a checkpoint "
             "boundary was sealed";
    case AsyncFailure::kNone:
      break;
  }
  return "capture runtime source rejected the session";
}

}  // namespace

struct GuestExecutionSessionCaptureRuntime::Impl {
  Impl(Processor& processor, kernel::GuestScheduler& scheduler,
       const GuestExecutionSessionCaptureRuntimeConfig& config,
       const GuestExecutionSessionCaptureRuntimeDependencies& dependencies)
      : processor(processor),
        scheduler(scheduler),
        config(config),
        dependencies(dependencies),
        queue(config.event_queue_capacity) {
    if (dependencies.checkpoint_controller) {
      checkpoint_controller = dependencies.checkpoint_controller;
    } else {
      direct_checkpoint_controller =
          std::make_unique<DirectCheckpointController>(scheduler);
      checkpoint_controller = direct_checkpoint_controller.get();
    }
  }

  ~Impl() { StopWorker(); }

  bool InitializeAssembler(std::string* error) {
    GuestExecutionSessionAssemblerDependencies assembler_dependencies;
    assembler_dependencies.clock = dependencies.clock;
    assembler_dependencies.state_provider = dependencies.provider;
    assembler_dependencies.content_provider = dependencies.provider;
    assembler_dependencies.publisher = &deferred_publisher;
    assembler = GuestExecutionSessionAssembler::Create(
        config.assembler, assembler_dependencies, error);
    return assembler != nullptr;
  }

  void StartWorker() {
    worker = std::thread([this]() { WorkerMain(); });
  }

  void RequestShutdownFence() noexcept {
    // Publication and shutdown are totally ordered. A publisher that acquired
    // this gate first is allowed to finish before Shutdown establishes its
    // fence; once the fence is established, no publisher can enter.
    std::lock_guard<std::mutex> lock(publication_mutex);
    shutdown_requested.store(true, std::memory_order_release);
  }

  void StopWorker() noexcept {
    shutdown_pending.store(true, std::memory_order_release);
    RequestShutdownFence();
    wake_condition.notify_all();
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      worker.join();
    }
  }

  void LatchAsyncFailure(AsyncFailure failure) noexcept {
    AsyncFailure expected = AsyncFailure::kNone;
    async_failure.compare_exchange_strong(expected, failure,
                                          std::memory_order_release,
                                          std::memory_order_relaxed);
    wake_condition.notify_one();
  }

  bool Push(const RuntimeEvent& event) noexcept {
    if (!queue.TryPush(event)) {
      LatchAsyncFailure(AsyncFailure::kQueueOverflow);
      return false;
    }
    queued_event_count.fetch_add(1, std::memory_order_relaxed);
    wake_condition.notify_one();
    return true;
  }

  bool ForwardIfActive(const RuntimeEvent& event) noexcept {
    if (!session_active.load(std::memory_order_acquire)) {
      return true;
    }
    callback_count.fetch_add(1, std::memory_order_acq_rel);
    const bool accepted = ForwardInsideCallback(
        event, session_active.load(std::memory_order_acquire),
        capture_gate.load(std::memory_order_acquire));
    callback_count.fetch_sub(1, std::memory_order_acq_rel);
    return accepted;
  }

  bool ForwardInsideCallback(const RuntimeEvent& event, bool active,
                             bool accepting) noexcept {
    bool accepted = true;
    if (active && accepting) {
      accepted = Push(event);
    } else if (active) {
      LatchAsyncFailure(AsyncFailure::kSourceAfterSeal);
      accepted = false;
    }
    return accepted;
  }

  void WaitForCallbacks() const noexcept {
    while (callback_count.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  void SetState(RuntimeState new_state) {
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      state = new_state;
    }
    state_atomic.store(new_state, std::memory_order_release);
    status_condition.notify_all();
  }

  void SetAttachmentStatus(bool processor_value, bool scheduler_value) {
    std::lock_guard<std::mutex> lock(status_mutex);
    processor_attached = processor_value;
    scheduler_attached = scheduler_value;
  }

  bool ValidateCheckpoint(
      const CheckpointSnapshot& checkpoint,
      const GuestExecutionCaptureThreadStateRegistrySnapshot& registry,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) {
    if (!checkpoint.generation ||
        checkpoint.rejection != CheckpointRejection::kNone ||
        !checkpoint.active || !checkpoint.quiesced ||
        checkpoint.roster_scope !=
            kernel::GuestSchedulerCheckpointRosterScope::kSchedulerOwned ||
        checkpoint.release_policy !=
            kernel::GuestSchedulerCheckpointReleasePolicy::
                kRunningSafepointsRequeueAtHead) {
      return Fail(error,
                  "capture runtime checkpoint is not a held, complete "
                  "scheduler-owned roster");
    }
    if (!registry.all_ready() || registry.participants.empty()) {
      return Fail(error,
                  "capture runtime Processor participant roster is not ready");
    }
    if (registry.participants.size() != checkpoint.participants.size()) {
      return Fail(
          error,
          "capture runtime checkpoint omits a Processor participant; "
          "created-suspended and otherwise scheduler-unowned lifetimes are "
          "an explicit unsupported contract");
    }
    for (size_t index = 0; index < registry.participants.size(); ++index) {
      const auto& lifecycle = registry.participants[index];
      if (lifecycle.state !=
              GuestExecutionCaptureThreadStateLifecycleState::kReady ||
          !lifecycle.participant.capture_instance_id ||
          !lifecycle.participant.guest_thread_id) {
        return Fail(error,
                    "capture runtime participant identity is not publishable");
      }
      const CheckpointParticipant* checkpoint_participant =
          FindCheckpointParticipant(checkpoint,
                                    lifecycle.participant.guest_thread_id);
      if (!checkpoint_participant) {
        return Fail(error,
                    "capture runtime checkpoint and Processor rosters differ");
      }
      for (size_t prior = 0; prior < index; ++prior) {
        if (registry.participants[prior].participant.capture_instance_id ==
                lifecycle.participant.capture_instance_id ||
            registry.participants[prior].participant.guest_thread_id ==
                lifecycle.participant.guest_thread_id) {
          return Fail(error,
                      "capture runtime Processor roster contains a duplicate");
        }
      }
      std::string capability_error;
      if (!dependencies.provider->SupportsCheckpointParticipant(
              *checkpoint_participant, &capability_error)) {
        return Fail(error,
                    capability_error.empty()
                        ? "capture runtime provider cannot encode a durable "
                          "continuation for a checkpoint participant"
                        : std::move(capability_error));
      }
    }
    if (host_calls.rejection !=
        GuestExecutionCaptureHostCallRosterRejection::kNone) {
      return Fail(error, "capture runtime host-call roster is rejected");
    }
    for (const auto& call : host_calls.active_calls) {
      const CheckpointParticipant* checkpoint_participant =
          FindCheckpointParticipant(checkpoint,
                                    call.participant.guest_thread_id);
      if (!checkpoint_participant ||
          checkpoint_participant->resume_kind !=
              CheckpointResumeKind::kJitSafepoint ||
          !checkpoint_participant->restorable) {
        return Fail(error,
                    "capture runtime active outer call lacks an exact-PC JIT "
                    "safepoint continuation");
      }
    }
    return true;
  }

  bool ValidateFinalizedCheckpoint(const CheckpointSnapshot& provisional,
                                   const CheckpointSnapshot& final,
                                   std::string* error) {
    if (final.generation != provisional.generation ||
        final.rejection != CheckpointRejection::kNone || final.active ||
        !final.quiesced || final.roster_scope != provisional.roster_scope ||
        final.release_policy != provisional.release_policy ||
        final.participants != provisional.participants) {
      return Fail(error,
                  "capture runtime checkpoint changed before atomic release");
    }
    return true;
  }

  bool ValidateAssemblerHostDepths(
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) {
    const auto assembler_status = assembler->status();
    for (const auto& participant : assembler_status.participants) {
      uint32_t depth = 0;
      for (const auto& call : host_calls.active_calls) {
        if (call.participant == participant.identity) {
          depth = std::max(depth, call.participant_depth);
        }
      }
      if (depth != participant.host_call_depth) {
        return Fail(error,
                    "capture runtime host-call queue and roster depths differ");
      }
    }
    return true;
  }

  bool ArriveActiveParticipants(const CheckpointSnapshot& checkpoint,
                                std::string* error) {
    for (const auto& participant : assembler->status().participants) {
      if (!participant.host_call_depth || participant.arrived ||
          participant.held) {
        continue;
      }
      const CheckpointParticipant* checkpoint_participant =
          FindCheckpointParticipant(checkpoint,
                                    participant.identity.guest_thread_id);
      if (!checkpoint_participant ||
          checkpoint_participant->resume_kind !=
              CheckpointResumeKind::kJitSafepoint ||
          !checkpoint_participant->restorable) {
        return Fail(error,
                    "capture runtime cannot represent a non-safepoint active "
                    "outer call");
      }
      if (assembler->ArriveAtSafepoint(participant.identity) ==
          AssemblerAction::kReject) {
        return Fail(error, assembler->status().message);
      }
    }
    return true;
  }

  void EndProvider(bool accepted) noexcept {
    if (!provider_armed.load(std::memory_order_acquire)) {
      return;
    }
    dependencies.provider->EndCapture(accepted);
    provider_armed.store(false, std::memory_order_release);
  }

  bool CancelActiveBarrier(std::string* error = nullptr) noexcept {
    const uint64_t generation =
        checkpoint_generation.load(std::memory_order_acquire);
    if (!generation) {
      return true;
    }
    CheckpointRejection last_result = CheckpointRejection::kNone;
    for (size_t attempt = 0; attempt < config.checkpoint_release_attempts;
         ++attempt) {
      CheckpointSnapshot final;
      try {
        last_result = checkpoint_controller->Cancel(generation, &final);
      } catch (...) {
        last_result = CheckpointRejection::kInvalidTopology;
      }
      if (final.generation == generation && !final.active) {
        uint64_t expected = generation;
        checkpoint_generation.compare_exchange_strong(
            expected, 0, std::memory_order_release, std::memory_order_relaxed);
        return true;
      }
    }
    if (error) {
      *error = "capture runtime retained checkpoint generation " +
               std::to_string(generation) + " after " +
               std::to_string(config.checkpoint_release_attempts) +
               " failed cancellation attempts; last code " +
               std::to_string(static_cast<uint32_t>(last_result));
    }
    return false;
  }

  void Reject(RuntimeRejection rejection_value, std::string message) {
    capture_gate.store(false, std::memory_order_release);
    session_active.store(false, std::memory_order_release);
    WaitForCallbacks();
    std::string barrier_error;
    CancelActiveBarrier(&barrier_error);
    EndProvider(false);
    deferred_publisher.Clear();
    if (assembler) {
      assembler->Cancel(message);
    }
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      rejection = rejection_value;
      if (!barrier_error.empty()) {
        if (!message.empty()) {
          message += "; ";
        }
        message += barrier_error;
      }
      status_message = std::move(message);
    }
    SetState(RuntimeState::kRejected);
  }

  bool RejectAsyncFailureIfAny() {
    const AsyncFailure failure = async_failure.load(std::memory_order_acquire);
    if (failure == AsyncFailure::kNone) {
      return false;
    }
    Reject(MapAsyncFailure(failure), AsyncFailureMessage(failure));
    return true;
  }

  void HandleStart() {
    if (state_atomic.load(std::memory_order_acquire) != RuntimeState::kIdle) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime start was requested outside idle");
      return;
    }
    SetState(RuntimeState::kStarting);

    CheckpointSnapshot provisional;
    const CheckpointRejection pause_result =
        checkpoint_controller->Pause(config.checkpoint_timeout, &provisional);
    if (pause_result != CheckpointRejection::kNone) {
      if (provisional.generation && provisional.active) {
        checkpoint_generation.store(provisional.generation,
                                    std::memory_order_release);
      }
      Reject(RuntimeRejection::kCheckpointBarrier,
             "capture runtime start checkpoint barrier failed with code " +
                 std::to_string(static_cast<uint32_t>(pause_result)));
      return;
    }
    checkpoint_generation.store(provisional.generation,
                                std::memory_order_release);
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }

    // The provisional barrier is now the only legal topology. Keep the event
    // gate sealed while the roster, providers and assembler are initialized,
    // but reject any source transition that races that initialization.
    session_active.store(true, std::memory_order_release);
    const auto registry = processor.QueryGuestExecutionCaptureParticipants();
    const auto host_calls = host_call_roster.snapshot();
    std::string error;
    if (!ValidateCheckpoint(provisional, registry, host_calls, &error)) {
      Reject(RuntimeRejection::kCheckpointRoster, std::move(error));
      return;
    }
    if (!assembler->SeedParticipants(registry.participants, host_calls)) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime assembler rejected the initial roster");
      return;
    }
    if (!dependencies.provider->BeginCapture(provisional, registry.participants,
                                             host_calls, &error)) {
      Reject(RuntimeRejection::kProviderFailure,
             error.empty() ? "capture runtime provider failed to arm"
                           : std::move(error));
      return;
    }
    provider_armed.store(true, std::memory_order_release);
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!dependencies.event_bridge->BeginSession(
            *assembler, provisional, registry.participants, &error)) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             error.empty() ? "capture runtime event bridge failed to begin"
                           : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!assembler->Arm(&error) || !assembler->RequestStart(&error) ||
        !ArriveActiveParticipants(provisional, &error) ||
        assembler->status().state != AssemblerState::kRecording) {
      Reject(RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }

    capture_gate.store(true, std::memory_order_release);
    CheckpointSnapshot final;
    const CheckpointRejection finalize_result =
        checkpoint_controller->Finalize(provisional.generation, &final);
    const bool generation_released =
        final.generation == provisional.generation && !final.active;
    if (generation_released) {
      uint64_t expected = provisional.generation;
      checkpoint_generation.compare_exchange_strong(
          expected, 0, std::memory_order_release, std::memory_order_relaxed);
    }
    if (finalize_result != CheckpointRejection::kNone || !generation_released ||
        !ValidateFinalizedCheckpoint(provisional, final, &error)) {
      Reject(
          RuntimeRejection::kCheckpointBarrier,
          error.empty()
              ? "capture runtime start checkpoint release failed with code " +
                    std::to_string(static_cast<uint32_t>(finalize_result))
              : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }
    SetState(RuntimeState::kRecording);
  }

  AssemblerAction ProcessSourceEvent(const RuntimeEvent& event,
                                     std::string* error) {
    switch (event.kind) {
      case RuntimeEventKind::kLifecycle:
        return assembler->OnParticipantLifecycle(event.lifecycle);
      case RuntimeEventKind::kHostCallBegin:
        return assembler->OnOuterHostCallBegin(
            event.participant, event.function_address,
            event.function_end_address, event.return_address);
      case RuntimeEventKind::kHostCallEnd:
        return assembler->OnOuterHostCallEnd(event.participant,
                                             event.host_call_outcome);
      case RuntimeEventKind::kScheduler: {
        const AssemblerAction action =
            dependencies.event_bridge->OnSchedulerEvent(*assembler,
                                                        event.scheduler, error);
        scheduler_event_count.fetch_add(1, std::memory_order_relaxed);
        return action;
      }
      case RuntimeEventKind::kStart:
      case RuntimeEventKind::kStop:
        break;
    }
    return AssemblerAction::kContinue;
  }

  bool DrainSourceEventsDuringStop(std::string* error) {
    RuntimeEvent event;
    while (queue.TryPop(&event)) {
      processed_event_count.fetch_add(1, std::memory_order_relaxed);
      if (event.kind == RuntimeEventKind::kStart ||
          event.kind == RuntimeEventKind::kStop) {
        continue;
      }
      if (ProcessSourceEvent(event, error) == AssemblerAction::kReject) {
        if (error->empty()) {
          *error = assembler->status().message;
        }
        return false;
      }
    }
    return true;
  }

  void HandleStop(bool manual_request) {
    if (state_atomic.load(std::memory_order_acquire) !=
        RuntimeState::kRecording) {
      return;
    }
    SetState(RuntimeState::kStopping);

    CheckpointSnapshot provisional;
    const CheckpointRejection pause_result =
        checkpoint_controller->Pause(config.checkpoint_timeout, &provisional);
    if (pause_result != CheckpointRejection::kNone) {
      if (provisional.generation && provisional.active) {
        checkpoint_generation.store(provisional.generation,
                                    std::memory_order_release);
      }
      Reject(RuntimeRejection::kCheckpointBarrier,
             "capture runtime stop checkpoint barrier failed with code " +
                 std::to_string(static_cast<uint32_t>(pause_result)));
      return;
    }
    checkpoint_generation.store(provisional.generation,
                                std::memory_order_release);
    capture_gate.store(false, std::memory_order_release);
    WaitForCallbacks();

    std::string error;
    if (RejectAsyncFailureIfAny()) {
      return;
    }
    if (!DrainSourceEventsDuringStop(&error)) {
      Reject(RuntimeRejection::kAssemblerFailure, std::move(error));
      return;
    }
    // A manual stop becomes an assembler boundary only after the checkpoint
    // has closed the source gate and every callback admitted before that close
    // has drained. This keeps a host call that completed before the gate close
    // in the recording prefix instead of holding its begin and then treating
    // its end as unbalanced. A drained event may itself have triggered an
    // automatic reel boundary, in which case that earlier boundary wins.
    AssemblerState assembler_state = assembler->status().state;
    if (manual_request && assembler_state == AssemblerState::kRecording) {
      if (assembler->RequestStop() == AssemblerAction::kReject) {
        Reject(RuntimeRejection::kAssemblerFailure,
               assembler->status().message);
        return;
      }
      assembler_state = assembler->status().state;
    }
    if (assembler_state == AssemblerState::kRejected) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
      return;
    }
    if (assembler_state != AssemblerState::kStopRequested &&
        assembler_state != AssemblerState::kStopRendezvous &&
        assembler_state != AssemblerState::kPublishing) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime stop did not establish a boundary");
      return;
    }
    // These are the last source snapshots inside the capture. Processor closes
    // session_active under its lifecycle-registry lock; that release store is
    // the topology cutoff. Host callbacks classify at entry and count their
    // entire roster mutation, so either source is wholly before the cutoff and
    // captured or rejected, or wholly after it and outside the session.
    const auto host_calls = host_call_roster.snapshot();
    const auto registry =
        processor.QueryGuestExecutionCaptureParticipantsAtCutoff(
            session_active);
    WaitForCallbacks();
    if (RejectAsyncFailureIfAny()) {
      return;
    }
    if (!ValidateCheckpoint(provisional, registry, host_calls, &error) ||
        !ValidateAssemblerHostDepths(host_calls, &error)) {
      Reject(RuntimeRejection::kCheckpointRoster, std::move(error));
      return;
    }
    if (!ArriveActiveParticipants(provisional, &error)) {
      Reject(RuntimeRejection::kAssemblerFailure, std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!dependencies.provider->SealCapture(provisional, &error)) {
      Reject(RuntimeRejection::kProviderFailure,
             error.empty() ? "capture runtime provider failed to seal"
                           : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!dependencies.event_bridge->SealSession(*assembler, provisional,
                                                &error)) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             error.empty() ? "capture runtime event bridge failed to seal"
                           : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (assembler->status().state != AssemblerState::kPublishing) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime assembler did not reach publication rendezvous");
      return;
    }

    SetState(RuntimeState::kPublishing);
    if (!assembler->Publish(&error)) {
      Reject(RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    std::optional<GuestExecutionSessionBundle> staged =
        deferred_publisher.Take();
    if (!staged) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime assembler produced no staged bundle");
      return;
    }

    CheckpointSnapshot final;
    const CheckpointRejection finalize_result =
        checkpoint_controller->Finalize(provisional.generation, &final);
    const bool generation_released =
        final.generation == provisional.generation && !final.active;
    if (generation_released) {
      uint64_t expected = provisional.generation;
      checkpoint_generation.compare_exchange_strong(
          expected, 0, std::memory_order_release, std::memory_order_relaxed);
    }
    if (finalize_result != CheckpointRejection::kNone || !generation_released ||
        !ValidateFinalizedCheckpoint(provisional, final, &error)) {
      Reject(RuntimeRejection::kCheckpointBarrier,
             error.empty()
                 ? "capture runtime stop checkpoint release failed with code " +
                       std::to_string(static_cast<uint32_t>(finalize_result))
                 : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    EndProvider(true);
    if (!dependencies.event_bridge->FinalizeBundle(
            &*staged, scheduler_event_count.load(std::memory_order_relaxed),
            &error)) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             error.empty()
                 ? "capture runtime event bridge did not close the bundle"
                 : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!ValidateGuestExecutionSessionBundle(*staged, &error,
                                             config.assembler.bundle_limits)) {
      Reject(RuntimeRejection::kBundleValidation,
             error.empty() ? "capture runtime staged bundle is invalid"
                           : std::move(error));
      return;
    }
    {
      std::lock_guard<std::mutex> publication_lock(publication_mutex);
      if (shutdown_requested.load(std::memory_order_acquire)) {
        return;
      }
      if (!dependencies.publisher->Publish(*staged, &error)) {
        Reject(RuntimeRejection::kPublicationFailure,
               error.empty() ? "capture runtime canonical publication failed"
                             : std::move(error));
        return;
      }
      {
        std::lock_guard<std::mutex> lock(status_mutex);
        canonical_output_published = true;
        status_message.clear();
      }
      SetState(RuntimeState::kComplete);
    }
  }

  void ProcessEvent(const RuntimeEvent& event) {
    const RuntimeState current = state_atomic.load(std::memory_order_acquire);
    if (event.kind == RuntimeEventKind::kStart) {
      HandleStart();
      return;
    }
    if (event.kind == RuntimeEventKind::kStop) {
      HandleStop(true);
      return;
    }
    if (current != RuntimeState::kRecording) {
      return;
    }
    std::string error;
    if (ProcessSourceEvent(event, &error) == AssemblerAction::kReject) {
      Reject(event.kind == RuntimeEventKind::kScheduler
                 ? RuntimeRejection::kEventBridgeFailure
                 : RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    const AssemblerState assembler_state = assembler->status().state;
    if (assembler_state == AssemblerState::kRejected) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
    } else if (assembler_state == AssemblerState::kStopRequested) {
      HandleStop(false);
    }
  }

  void PollAssembler() {
    if (state_atomic.load(std::memory_order_acquire) !=
        RuntimeState::kRecording) {
      return;
    }
    const AssemblerAction action = assembler->Poll();
    if (action == AssemblerAction::kReject) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
      return;
    }
    if (assembler->status().state == AssemblerState::kStopRequested) {
      HandleStop(false);
    }
  }

  void WorkerMain() noexcept {
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      worker_running = true;
    }
    status_condition.notify_all();
    try {
      while (!shutdown_requested.load(std::memory_order_acquire)) {
        if (RejectAsyncFailureIfAny()) {
          break;
        }
        RuntimeEvent event;
        bool processed = false;
        while (queue.TryPop(&event)) {
          processed = true;
          processed_event_count.fetch_add(1, std::memory_order_relaxed);
          ProcessEvent(event);
          if (shutdown_requested.load(std::memory_order_acquire) ||
              IsTerminal(state_atomic.load(std::memory_order_acquire))) {
            break;
          }
        }
        if (shutdown_requested.load(std::memory_order_acquire)) {
          break;
        }
        if (IsTerminal(state_atomic.load(std::memory_order_acquire))) {
          break;
        }
        PollAssembler();
        if (IsTerminal(state_atomic.load(std::memory_order_acquire))) {
          break;
        }
        if (!processed) {
          std::unique_lock<std::mutex> lock(wake_mutex);
          wake_condition.wait_for(lock, config.control_poll_interval, [this]() {
            return shutdown_requested.load(std::memory_order_acquire) ||
                   queue.MaybeNotEmpty() ||
                   async_failure.load(std::memory_order_acquire) !=
                       AsyncFailure::kNone;
          });
        }
      }
    } catch (const std::bad_alloc&) {
      Reject(RuntimeRejection::kProviderFailure,
             "capture runtime control worker allocation failed");
    } catch (...) {
      Reject(RuntimeRejection::kSourceRejected,
             "capture runtime control worker raised an exception");
    }
    if (shutdown_requested.load(std::memory_order_acquire) &&
        !IsTerminal(state_atomic.load(std::memory_order_acquire))) {
      capture_gate.store(false, std::memory_order_release);
      session_active.store(false, std::memory_order_release);
      WaitForCallbacks();
      std::string barrier_error;
      CancelActiveBarrier(&barrier_error);
      EndProvider(false);
      deferred_publisher.Clear();
      if (assembler) {
        assembler->Cancel("capture runtime shut down");
      }
      {
        std::lock_guard<std::mutex> lock(status_mutex);
        rejection = RuntimeRejection::kCancelled;
        status_message = "capture runtime shut down";
        if (!barrier_error.empty()) {
          status_message += "; " + barrier_error;
        }
      }
      SetState(RuntimeState::kShutdown);
    }
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      worker_running = false;
    }
    status_condition.notify_all();
  }

  Processor& processor;
  kernel::GuestScheduler& scheduler;
  const GuestExecutionSessionCaptureRuntimeConfig config;
  const GuestExecutionSessionCaptureRuntimeDependencies dependencies;
  RuntimeEventQueue queue;
  GuestExecutionCaptureHostCallRoster host_call_roster;
  DeferredPublisher deferred_publisher;
  std::unique_ptr<DirectCheckpointController> direct_checkpoint_controller;
  GuestExecutionSessionCaptureRuntimeCheckpointController*
      checkpoint_controller = nullptr;
  std::unique_ptr<GuestExecutionSessionAssembler> assembler;

  std::thread worker;
  mutable std::mutex wake_mutex;
  std::condition_variable wake_condition;
  std::mutex publication_mutex;
  std::atomic<bool> shutdown_requested{false};
  std::atomic<bool> shutdown_pending{false};
  std::atomic<bool> session_active{false};
  std::atomic<bool> capture_gate{false};
  mutable std::atomic<uint64_t> callback_count{0};
  std::atomic<AsyncFailure> async_failure{AsyncFailure::kNone};
  std::atomic<uint64_t> last_scheduler_sequence{0};
  std::atomic<uint64_t> queued_event_count{0};
  std::atomic<uint64_t> processed_event_count{0};
  std::atomic<RuntimeState> state_atomic{RuntimeState::kIdle};
  std::atomic<bool> start_requested{false};
  std::atomic<bool> stop_requested{false};

  mutable std::mutex status_mutex;
  mutable std::condition_variable status_condition;
  RuntimeState state = RuntimeState::kIdle;
  RuntimeRejection rejection = RuntimeRejection::kNone;
  std::string status_message;
  std::atomic<uint64_t> checkpoint_generation{0};
  std::atomic<uint64_t> scheduler_event_count{0};
  bool processor_attached = false;
  bool scheduler_attached = false;
  std::atomic<bool> provider_armed{false};
  bool canonical_output_published = false;
  bool worker_running = false;
  std::weak_ptr<GuestExecutionSessionCaptureRuntime> owner;
};

std::shared_ptr<GuestExecutionSessionCaptureRuntime>
GuestExecutionSessionCaptureRuntime::CreateAndAttach(
    Processor& processor, kernel::GuestScheduler& scheduler,
    const GuestExecutionSessionCaptureRuntimeConfig& config,
    const GuestExecutionSessionCaptureRuntimeDependencies& dependencies,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!dependencies.clock || !dependencies.provider ||
      !dependencies.event_bridge || !dependencies.publisher) {
    Fail(error, "capture runtime dependencies are missing");
    return nullptr;
  }
  if (!IsPowerOfTwo(config.event_queue_capacity) ||
      config.checkpoint_timeout.count() <= 0 ||
      config.control_poll_interval.count() <= 0 ||
      !config.checkpoint_release_attempts ||
      config.checkpoint_release_attempts > 64) {
    Fail(error, "capture runtime queue or timing configuration is invalid");
    return nullptr;
  }

  std::unique_ptr<Impl> impl;
  try {
    impl = std::make_unique<Impl>(processor, scheduler, config, dependencies);
  } catch (...) {
    Fail(error, "capture runtime could not allocate its bounded queue");
    return nullptr;
  }
  if (!impl->InitializeAssembler(error)) {
    return nullptr;
  }
  auto runtime = std::shared_ptr<GuestExecutionSessionCaptureRuntime>(
      new GuestExecutionSessionCaptureRuntime(std::move(impl)));
  runtime->impl_->owner = runtime;
  runtime->impl_->StartWorker();
  if (!runtime->Attach(error)) {
    runtime->Shutdown();
    return nullptr;
  }
  return runtime;
}

GuestExecutionSessionCaptureRuntime::GuestExecutionSessionCaptureRuntime(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestExecutionSessionCaptureRuntime::~GuestExecutionSessionCaptureRuntime() {
  impl_->StopWorker();
}

bool GuestExecutionSessionCaptureRuntime::Attach(std::string* error) {
  const auto owner = impl_->owner.lock();
  if (!owner) {
    return Fail(error, "capture runtime lost ownership before attachment");
  }
  const auto scheduler_observer =
      std::static_pointer_cast<kernel::GuestSchedulerCaptureObserver>(owner);
  const auto processor_observer =
      std::static_pointer_cast<GuestExecutionCaptureHostCallObserver>(owner);
  if (!impl_->scheduler.AttachCaptureObserverTransactionally(
          scheduler_observer, [this, &processor_observer]() {
            return impl_->processor.AttachGuestExecutionCaptureHostCallObserver(
                processor_observer);
          })) {
    return Fail(error, "capture runtime observer transaction was rejected");
  }
  impl_->SetAttachmentStatus(true, true);
  return true;
}

bool GuestExecutionSessionCaptureRuntime::RequestStart() noexcept {
  if (impl_->shutdown_requested.load(std::memory_order_acquire) ||
      IsTerminal(impl_->state_atomic.load(std::memory_order_acquire))) {
    return false;
  }
  bool expected = false;
  if (!impl_->start_requested.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return false;
  }
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kStart;
  if (!impl_->Push(event)) {
    return false;
  }
  return true;
}

bool GuestExecutionSessionCaptureRuntime::RequestStop() noexcept {
  if (!impl_->start_requested.load(std::memory_order_acquire) ||
      impl_->shutdown_requested.load(std::memory_order_acquire) ||
      IsTerminal(impl_->state_atomic.load(std::memory_order_acquire))) {
    return false;
  }
  bool expected = false;
  if (!impl_->stop_requested.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return false;
  }
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kStop;
  if (!impl_->Push(event)) {
    return false;
  }
  return true;
}

void GuestExecutionSessionCaptureRuntime::Shutdown() noexcept {
  impl_->StopWorker();
  const auto owner = impl_->owner.lock();
  if (!owner) {
    return;
  }
  bool scheduler_attached = false;
  bool processor_attached = false;
  {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    scheduler_attached = impl_->scheduler_attached;
    processor_attached = impl_->processor_attached;
  }
  if (scheduler_attached) {
    const auto observer =
        std::static_pointer_cast<kernel::GuestSchedulerCaptureObserver>(owner);
    if (impl_->scheduler.DetachCaptureObserver(observer)) {
      scheduler_attached = false;
    }
  }
  if (processor_attached) {
    const auto observer =
        std::static_pointer_cast<GuestExecutionCaptureHostCallObserver>(owner);
    if (impl_->processor.DetachGuestExecutionCaptureHostCallObserver(
            observer)) {
      processor_attached = false;
    }
  }
  impl_->SetAttachmentStatus(processor_attached, scheduler_attached);
}

bool GuestExecutionSessionCaptureRuntime::WaitForTerminal(
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->status_mutex);
  return impl_->status_condition.wait_for(
      lock, timeout, [this]() { return IsTerminal(impl_->state); });
}

GuestExecutionSessionCaptureRuntimeStatus
GuestExecutionSessionCaptureRuntime::status() const {
  std::lock_guard<std::mutex> lock(impl_->status_mutex);
  GuestExecutionSessionCaptureRuntimeStatus result;
  result.state = impl_->state;
  result.rejection = impl_->rejection;
  result.checkpoint_generation =
      impl_->checkpoint_generation.load(std::memory_order_relaxed);
  result.queued_event_count =
      impl_->queued_event_count.load(std::memory_order_relaxed);
  result.processed_event_count =
      impl_->processed_event_count.load(std::memory_order_relaxed);
  result.scheduler_event_count =
      impl_->scheduler_event_count.load(std::memory_order_relaxed);
  result.last_scheduler_sequence =
      impl_->last_scheduler_sequence.load(std::memory_order_relaxed);
  result.processor_attached = impl_->processor_attached;
  result.scheduler_attached = impl_->scheduler_attached;
  result.provider_armed = impl_->provider_armed.load(std::memory_order_relaxed);
  result.canonical_output_published = impl_->canonical_output_published;
  result.worker_running = impl_->worker_running;
  result.shutdown_pending =
      impl_->shutdown_pending.load(std::memory_order_relaxed);
  result.message = impl_->status_message;
  return result;
}

GuestExecutionCaptureThreadStateLifecycleDisposition
GuestExecutionSessionCaptureRuntime::OnThreadStateSeed(
    std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
        events) noexcept {
  if (events.size() >
      impl_->config.assembler.bundle_limits.session.maximum_participants) {
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
  }
  for (const auto& event : events) {
    if (event.state != GuestExecutionCaptureThreadStateLifecycleState::kReady ||
        !event.participant.capture_instance_id ||
        !event.participant.guest_thread_id) {
      return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
    }
  }
  return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
}

GuestExecutionCaptureThreadStateLifecycleDisposition
GuestExecutionSessionCaptureRuntime::OnThreadStateLifecycle(
    GuestExecutionCaptureThreadStateLifecycleEvent event) noexcept {
  RuntimeEvent runtime_event;
  runtime_event.kind = RuntimeEventKind::kLifecycle;
  runtime_event.lifecycle = event;
  return impl_->ForwardIfActive(runtime_event)
             ? GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept
             : GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
}

GuestExecutionCaptureJitSafepointDisposition
GuestExecutionSessionCaptureRuntime::OnJitSafepoint(const ThreadState&,
                                                    uint32_t) noexcept {
  if (impl_->session_active.load(std::memory_order_acquire)) {
    impl_->LatchAsyncFailure(AsyncFailure::kUnexpectedJitSafepoint);
    return GuestExecutionCaptureJitSafepointDisposition::kReject;
  }
  return GuestExecutionCaptureJitSafepointDisposition::kAccept;
}

GuestExecutionCaptureHostCallToken
GuestExecutionSessionCaptureRuntime::OnHostGuestCallBegin(
    const ThreadState& thread_state, const GuestFunction& function,
    uint32_t return_address) noexcept {
  // The callback count covers the roster mutation as well as classification,
  // making the stop cutoff indivisible with respect to final roster snapshots.
  impl_->callback_count.fetch_add(1, std::memory_order_acq_rel);
  const bool active = impl_->session_active.load(std::memory_order_acquire);
  const bool accepting = impl_->capture_gate.load(std::memory_order_acquire);
  const GuestExecutionCaptureHostCallToken token =
      impl_->host_call_roster.OnHostGuestCallBegin(thread_state, function,
                                                   return_address);
  if (!token) {
    impl_->LatchAsyncFailure(AsyncFailure::kHostCallRoster);
    impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
    return {};
  }
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kHostCallBegin;
  event.participant = {thread_state.guest_execution_capture_instance_id(),
                       thread_state.thread_id()};
  event.host_call_token = token;
  event.function_address = function.address();
  event.function_end_address = function.end_address();
  event.return_address = return_address;
  impl_->ForwardInsideCallback(event, active, accepting);
  impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
  return token;
}

bool GuestExecutionSessionCaptureRuntime::OnHostGuestCallEnd(
    GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
    const GuestFunction& function,
    GuestExecutionCaptureHostCallOutcome outcome) noexcept {
  impl_->callback_count.fetch_add(1, std::memory_order_acq_rel);
  const bool active = impl_->session_active.load(std::memory_order_acquire);
  const bool accepting = impl_->capture_gate.load(std::memory_order_acquire);
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kHostCallEnd;
  event.participant = {thread_state.guest_execution_capture_instance_id(),
                       thread_state.thread_id()};
  event.host_call_token = token;
  event.host_call_outcome = outcome;
  event.function_address = function.address();
  event.function_end_address = function.end_address();
  const bool roster_accepted = impl_->host_call_roster.OnHostGuestCallEnd(
      token, thread_state, function, outcome);
  if (!roster_accepted) {
    impl_->LatchAsyncFailure(AsyncFailure::kHostCallRoster);
    impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }
  const bool forwarded = impl_->ForwardInsideCallback(event, active, accepting);
  impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
  return forwarded;
}

bool GuestExecutionSessionCaptureRuntime::OnSchedulerEvent(
    const kernel::GuestSchedulerCaptureEvent& event) noexcept {
  // GuestScheduler serializes observer delivery under its mutation lock, so a
  // single load/store validates continuity without a compare-exchange loop on
  // the permanent pre-arm path.
  const uint64_t previous =
      impl_->last_scheduler_sequence.load(std::memory_order_relaxed);
  if (!event.sequence || previous == std::numeric_limits<uint64_t>::max() ||
      event.sequence != previous + 1) {
    impl_->LatchAsyncFailure(AsyncFailure::kSchedulerSequence);
    return false;
  }
  impl_->last_scheduler_sequence.store(event.sequence,
                                       std::memory_order_release);
  RuntimeEvent runtime_event;
  runtime_event.kind = RuntimeEventKind::kScheduler;
  runtime_event.scheduler = event;
  return impl_->ForwardIfActive(runtime_event);
}

bool GuestExecutionSessionCaptureRuntime::CanDetach() const noexcept {
  return !impl_->session_active.load(std::memory_order_acquire) &&
         impl_->host_call_roster.CanDetach();
}

}  // namespace cpu
}  // namespace xe

#endif
