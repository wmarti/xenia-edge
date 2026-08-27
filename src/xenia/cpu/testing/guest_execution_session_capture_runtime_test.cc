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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_continuous_event.h"
#include "xenia/cpu/guest_execution_session_capture_runtime.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class GuestExecutionSessionCaptureRuntimeTestAccess final {
 public:
  static void SetRequestStartPrequeueHook(
      GuestExecutionSessionCaptureRuntime& runtime, void (*hook)(void*),
      void* context) {
    runtime.SetRequestStartPrequeueTestHook(hook, context);
  }
};

namespace testing {
namespace {

using namespace std::chrono_literals;

using RuntimeRejection = GuestExecutionSessionCaptureRuntimeRejection;
using RuntimeState = GuestExecutionSessionCaptureRuntimeState;
using CheckpointParticipant = kernel::GuestSchedulerCheckpointParticipant;
using CheckpointRejection = kernel::GuestSchedulerCheckpointBarrierRejection;
using CheckpointSnapshot = kernel::GuestSchedulerCheckpointBarrierSnapshot;

constexpr uint32_t kCodePageAddress = 0x82000000;
constexpr uint32_t kFunctionEndAddress = 0x820000FC;
constexpr uint32_t kResumePc = 0x82000040;

bool Fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

class StubBackend final : public backend::Backend {
 public:
  void* AllocThreadData() override { return this; }
  void FreeThreadData(void*) override {}
  void CommitExecutableRange(uint32_t, uint32_t) override {}
  std::unique_ptr<backend::Assembler> CreateAssembler() override {
    return nullptr;
  }
  std::unique_ptr<GuestFunction> CreateGuestFunction(Module*,
                                                     uint32_t) override {
    return nullptr;
  }
  uint64_t CalculateNextHostInstruction(ThreadDebugInfo*,
                                        uint64_t current_pc) override {
    return current_pc;
  }
};

class ImmediateGuestFunction final : public GuestFunction {
 public:
  ImmediateGuestFunction(uint32_t address, uint32_t end_address)
      : GuestFunction(nullptr, address) {
    set_end_address(end_address);
  }

  uint8_t* machine_code() const override { return nullptr; }
  size_t machine_code_length() const override { return 0; }

 protected:
  bool CallImpl(ThreadState*, uint32_t) override { return true; }
};

class BlockingGuestFunction final : public GuestFunction {
 public:
  BlockingGuestFunction(uint32_t address, uint32_t end_address)
      : GuestFunction(nullptr, address) {
    set_end_address(end_address);
  }

  uint8_t* machine_code() const override { return nullptr; }
  size_t machine_code_length() const override { return 0; }

  bool WaitForEntry() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 2s, [this]() { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 protected:
  bool CallImpl(ThreadState*, uint32_t) override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return released_; });
    return true;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class BlockingHook final {
 public:
  static void Hook(void* context) {
    static_cast<BlockingHook*>(context)->Enter();
  }

  bool WaitForEntry() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 2s, [this]() { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
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

class RuntimeEnvironment final {
 public:
  RuntimeEnvironment() {
    memory = std::make_unique<Memory>();
    if (!memory->Initialize()) {
      throw std::runtime_error("runtime test memory initialization failed");
    }
    processor = std::make_unique<Processor>(memory.get(), nullptr);
    if (!processor->Setup(std::make_unique<StubBackend>())) {
      throw std::runtime_error("runtime test Processor setup failed");
    }
    scheduler = std::make_unique<kernel::GuestScheduler>(nullptr);
  }

  std::unique_ptr<ThreadState> MakeThread(uint32_t thread_id) {
    auto thread = std::make_unique<ThreadState>(processor.get(), thread_id);
    if (thread->PublishGuestExecutionCaptureReady() !=
        GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept) {
      throw std::runtime_error("runtime test ThreadState publication failed");
    }
    return thread;
  }

  std::unique_ptr<Memory> memory;
  std::unique_ptr<Processor> processor;
  std::unique_ptr<kernel::GuestScheduler> scheduler;
};

GuestExecutionSessionSha256 Digest(uint8_t seed) {
  GuestExecutionSessionSha256 digest = {};
  for (size_t index = 0; index < digest.size(); ++index) {
    digest[index] = static_cast<uint8_t>(seed + index);
  }
  return digest;
}

GuestExecutionSessionCaptureRuntimeConfig MakeConfig(size_t queue_capacity) {
  GuestExecutionSessionCaptureRuntimeConfig config;
  config.event_queue_capacity = queue_capacity;
  config.checkpoint_timeout = 100ms;
  config.control_poll_interval = 1ms;
  config.assembler.session_epoch = 1;
  config.assembler.coverage_mode =
      GuestExecutionReelCoverageMode::kContinuousInstructions;
  config.assembler.boundary.kind = GuestExecutionSessionBoundaryKind::kManual;
  config.assembler.limits.maximum_segment_count = 8;
  config.assembler.limits.maximum_event_count = 64;
  config.assembler.limits.maximum_guest_instruction_count = 1024;
  config.assembler.limits.maximum_guest_marker_count = 16;
  config.assembler.limits.maximum_duration_ticks = 1000;
  config.assembler.maximum_stop_tail_event_count = 16;
  config.assembler.maximum_stop_tail_guest_instruction_count = 64;
  config.assembler.maximum_stop_tail_ticks = 300;
  config.assembler.maximum_start_rendezvous_ticks = 200;
  config.assembler.capture_tick_frequency = 1000000000;
  config.assembler.maximum_events_per_chunk = 8;
  config.assembler.capture_build_sha256 = Digest(1);
  config.assembler.replay_config_sha256 = Digest(2);
  config.assembler.title_identity_sha256 = Digest(3);
  config.assembler.module_identity_sha256 = Digest(4);
  return config;
}

class FakeClock final : public ppc::GuestInvocationRecorderClock {
 public:
  uint64_t NowTicks() const override {
    return now.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  mutable std::atomic<uint64_t> now{1000};
};

class FakeProvider final : public GuestExecutionSessionCaptureRuntimeProvider {
 public:
  FakeProvider() : code_page_(JitCorpus::kPageSize, 0x5A) {
    ExecutionJitCorpusBuilder builder(JitCorpus::kConfigGuestScheduler);
    const ExecutionJitCorpus::FunctionRecord function = {
        kCodePageAddress, kFunctionEndAddress, 64, 0};
    std::string error;
    if (!builder.AddCodePage(kCodePageAddress, code_page_.data(),
                             code_page_.size(), &error) ||
        !builder.AddFunction(function, &error) ||
        !builder.Encode(&code_corpus_, &error)) {
      throw std::runtime_error("runtime test corpus construction failed: " +
                               error);
    }
  }

  bool SupportsCheckpointParticipant(const CheckpointParticipant& participant,
                                     std::string*) noexcept override {
    return participant.restorable;
  }

  bool BeginCapture(
      const CheckpointSnapshot&,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>,
      const GuestExecutionCaptureHostCallRosterSnapshot&,
      std::string*) noexcept override {
    ++begin_count;
    return true;
  }

  bool SealCapture(const CheckpointSnapshot&, std::string*) noexcept override {
    ++seal_count;
    std::unique_lock<std::mutex> lock(seal_mutex);
    seal_entered = true;
    seal_condition.notify_all();
    seal_condition.wait(lock, [this]() { return !block_seal; });
    return true;
  }

  void EndCapture(bool accepted) noexcept override {
    ++end_count;
    ended_accepted.store(accepted, std::memory_order_release);
  }

  bool EncodeParticipantState(
      const GuestExecutionCaptureParticipantIdentity& participant,
      std::vector<uint8_t>* output, std::string* error) noexcept override {
    {
      std::unique_lock<std::mutex> lock(state_mutex);
      state_encode_entered = true;
      state_condition.notify_all();
      state_condition.wait(lock, [this]() { return !block_state_encode; });
    }
    if (emit_invalid_state) {
      output->assign(64, static_cast<uint8_t>(participant.capture_instance_id));
      return true;
    }
    ppc::GuestPPCThreadCheckpoint checkpoint;
    checkpoint.participant_ordinal = 0;
    checkpoint.guest_thread_id = participant.guest_thread_id;
    checkpoint.resume_kind = ppc::GuestPPCThreadResumeKind::kGuestBlockHead;
    checkpoint.resume_pc = kResumePc;
    checkpoint.owning_function_address = kCodePageAddress;
    checkpoint.owning_function_end_address = kFunctionEndAddress;
    checkpoint.outer_guest_return_address = kCodePageAddress + 0x100;
    checkpoint.registers.gpr.front() =
        state_generation.fetch_add(1, std::memory_order_relaxed);
    return ppc::GuestPPCThreadCheckpointCodec::Encode(checkpoint, output,
                                                      error);
  }

  bool CollectCheckpointContent(
      bool initial_checkpoint,
      std::vector<GuestExecutionSessionAssemblerContent>* output,
      std::string*) noexcept override {
    output->clear();
    if (initial_checkpoint) {
      output->push_back({GuestExecutionSessionContentKind::kGuestCode,
                         kCodePageAddress, code_page_});
    }
    return true;
  }

  bool CollectSessionCodeCorpus(std::vector<uint8_t>* output,
                                std::string*) noexcept override {
    *output = code_corpus_;
    return true;
  }

  void BlockStateEncoding() {
    std::lock_guard<std::mutex> lock(state_mutex);
    block_state_encode = true;
    state_encode_entered = false;
  }

  bool WaitForStateEncoding() {
    std::unique_lock<std::mutex> lock(state_mutex);
    return state_condition.wait_for(lock, 2s,
                                    [this]() { return state_encode_entered; });
  }

  void ReleaseStateEncoding() {
    std::lock_guard<std::mutex> lock(state_mutex);
    block_state_encode = false;
    state_condition.notify_all();
  }

  void BlockSeal() {
    std::lock_guard<std::mutex> lock(seal_mutex);
    block_seal = true;
    seal_entered = false;
  }

  bool WaitForSeal() {
    std::unique_lock<std::mutex> lock(seal_mutex);
    return seal_condition.wait_for(lock, 2s, [this]() { return seal_entered; });
  }

  void ReleaseSeal() {
    std::lock_guard<std::mutex> lock(seal_mutex);
    block_seal = false;
    seal_condition.notify_all();
  }

  std::atomic<uint32_t> begin_count{0};
  std::atomic<uint32_t> seal_count{0};
  std::atomic<uint32_t> end_count{0};
  std::atomic<bool> ended_accepted{false};
  bool emit_invalid_state = false;

 private:
  std::mutex state_mutex;
  std::condition_variable state_condition;
  bool block_state_encode = false;
  bool state_encode_entered = false;
  std::mutex seal_mutex;
  std::condition_variable seal_condition;
  bool block_seal = false;
  bool seal_entered = false;
  std::atomic<uint64_t> state_generation{1};
  std::vector<uint8_t> code_page_;
  std::vector<uint8_t> code_corpus_;
};

class CanonicalEventBridge final
    : public GuestExecutionSessionCaptureRuntimeEventBridge {
 public:
  bool BeginSession(
      GuestExecutionSessionAssembler&, const CheckpointSnapshot&,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>,
      std::string*) noexcept override {
    return true;
  }

  GuestExecutionSessionAssemblerAction OnSchedulerEvent(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCaptureEvent& event,
      std::string* error) noexcept override {
    using Action = GuestExecutionSessionAssemblerAction;
    const GuestExecutionCaptureParticipantIdentity participant = {
        event.capture_instance_id, event.guest_thread_id};
    const bool coverage_only =
        record_next_event_as_segment_only_.exchange(false);
    if (!coverage_only &&
        !RequireContinue(assembler.OnOuterHostCallBegin(participant, 0x82000000,
                                                        0x820000FC, 0x82001000),
                         "outer call begin", error)) {
      return Action::kReject;
    }
    if (!RequireContinue(assembler.OnInstructionCoverage(participant, 10),
                         "instruction coverage", error)) {
      return Action::kReject;
    }
    if (!coverage_only &&
        !RequireContinue(
            assembler.OnOuterHostCallEnd(
                participant,
                GuestExecutionCaptureHostCallOutcome::kReturnedToHost),
            "outer call end", error)) {
      return Action::kReject;
    }
    observed_scheduler_event_count_.fetch_add(1, std::memory_order_relaxed);
    return Action::kContinue;
  }

  bool SealSession(GuestExecutionSessionAssembler&, const CheckpointSnapshot&,
                   std::string*) noexcept override {
    return true;
  }

  bool FinalizeBundle(GuestExecutionSessionBundle* bundle,
                      uint64_t scheduler_event_count,
                      std::string* error) noexcept override {
    if (omit_overlay) {
      return true;
    }
    try {
      if (!bundle ||
          scheduler_event_count !=
              observed_scheduler_event_count_.load(std::memory_order_relaxed) ||
          bundle->manifest.participants.size() != 1 ||
          bundle->chunks.size() < 4) {
        return Fail(error, "test bridge cannot close the scheduler tape");
      }
      const size_t final_index = bundle->chunks.size() - 1;
      GuestExecutionSessionCheckpointChunk final_checkpoint;
      if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
              bundle->chunks[final_index], &final_checkpoint, error) ||
          final_checkpoint.checkpoint.thread_states.size() != 1) {
        return false;
      }
      const GuestExecutionSessionThreadStateReference& final_state =
          final_checkpoint.checkpoint.thread_states.front();
      const auto state_blob = std::find_if(
          bundle->content_blobs.cbegin(), bundle->content_blobs.cend(),
          [&final_state](const GuestExecutionSessionContentBlob& blob) {
            return blob.sha256 == final_state.sha256;
          });
      if (state_blob == bundle->content_blobs.cend()) {
        return Fail(error, "test bridge final state blob is missing");
      }
      ppc::GuestPPCThreadCheckpoint checkpoint;
      if (!ppc::GuestPPCThreadCheckpointCodec::Decode(state_blob->bytes,
                                                      &checkpoint, error)) {
        return false;
      }

      std::vector<GuestExecutionSessionEvent> canonical_events;
      for (size_t index = 0; index < final_index; ++index) {
        if (bundle->manifest.chunks[index].kind !=
            GuestExecutionSessionChunkKind::kEvents) {
          continue;
        }
        GuestExecutionSessionEventChunk chunk;
        if (!GuestExecutionSessionCodec::DecodeEventChunk(bundle->chunks[index],
                                                          &chunk, error)) {
          return false;
        }
        canonical_events.insert(canonical_events.end(), chunk.events.begin(),
                                chunk.events.end());
      }
      if (canonical_events.empty()) {
        return Fail(error, "test bridge canonical tape is empty");
      }
      std::vector<GuestExecutionContinuousEvent> continuous_events;
      continuous_events.reserve(canonical_events.size());
      for (const GuestExecutionSessionEvent& canonical : canonical_events) {
        GuestExecutionContinuousEvent continuous;
        continuous.global_sequence = canonical.global_sequence;
        continuous.kind = canonical.kind;
        if (canonical.thread_ordinal != kGuestExecutionSessionNoThread) {
          continuous.actor = {
              canonical.thread_ordinal,
              bundle->manifest.participants[canonical.thread_ordinal]
                  .guest_thread_id};
        }
        continuous_events.push_back(continuous);
      }
      GuestExecutionContinuousEvent& boundary = continuous_events.back();
      const GuestExecutionSessionParticipant& participant =
          bundle->manifest.participants.front();
      boundary.subject = {participant.ordinal, participant.guest_thread_id};
      boundary.checkpoint.kind =
          GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
      boundary.checkpoint.checkpoint_global_sequence =
          final_checkpoint.checkpoint.global_sequence;
      boundary.checkpoint.state_size = final_state.byte_size;
      boundary.checkpoint.state_sha256 = final_state.sha256;
      boundary.checkpoint.binding = BindingFor(checkpoint);

      std::vector<uint8_t> overlay_bytes;
      if (!GuestExecutionContinuousEventCodec::Encode(continuous_events,
                                                      &overlay_bytes, error)) {
        return false;
      }
      const uint32_t overlay_ordinal = static_cast<uint32_t>(final_index);
      final_checkpoint.ordinal = overlay_ordinal + 1;
      std::vector<uint8_t> final_bytes;
      if (!GuestExecutionSessionCodec::EncodeCheckpointChunk(
              final_checkpoint, &final_bytes, error)) {
        return false;
      }
      bundle->chunks.insert(bundle->chunks.begin() + final_index,
                            std::move(overlay_bytes));
      bundle->chunks.back() = std::move(final_bytes);
      bundle->manifest.chunks.insert(
          bundle->manifest.chunks.begin() + final_index,
          ReferenceFor(GuestExecutionSessionChunkKind::kContinuousEvents,
                       overlay_ordinal,
                       continuous_events.front().global_sequence,
                       continuous_events.back().global_sequence,
                       static_cast<uint32_t>(continuous_events.size()),
                       bundle->chunks[final_index]));
      bundle->manifest.chunks.back() = ReferenceFor(
          GuestExecutionSessionChunkKind::kCheckpoint, final_checkpoint.ordinal,
          final_checkpoint.checkpoint.global_sequence,
          final_checkpoint.checkpoint.global_sequence, 1,
          bundle->chunks.back());
      return true;
    } catch (...) {
      return Fail(error, "test bridge could not allocate the event overlay");
    }
  }

  void RecordNextEventAsSegmentOnly() {
    record_next_event_as_segment_only_.store(true);
  }

  bool omit_overlay = false;

 private:
  static GuestExecutionSessionChunkReference ReferenceFor(
      GuestExecutionSessionChunkKind kind, uint32_t ordinal,
      uint64_t first_sequence, uint64_t last_sequence, uint32_t record_count,
      const std::vector<uint8_t>& encoded) {
    GuestExecutionSessionChunkReference reference;
    reference.kind = kind;
    reference.ordinal = ordinal;
    reference.first_event_sequence = first_sequence;
    reference.last_event_sequence = last_sequence;
    reference.record_count = record_count;
    reference.encoded_size = encoded.size();
    reference.encoded_sha256 = GuestExecutionSessionCodec::HashBytes(encoded);
    return reference;
  }

  static ppc::GuestPPCThreadCheckpointBinding BindingFor(
      const ppc::GuestPPCThreadCheckpoint& checkpoint) {
    ppc::GuestPPCThreadCheckpointBinding binding;
    binding.participant_ordinal = checkpoint.participant_ordinal;
    binding.guest_thread_id = checkpoint.guest_thread_id;
    binding.resume_kind = checkpoint.resume_kind;
    binding.resume_pc = checkpoint.resume_pc;
    binding.owning_function_address = checkpoint.owning_function_address;
    binding.owning_function_end_address =
        checkpoint.owning_function_end_address;
    binding.outer_guest_return_address = checkpoint.outer_guest_return_address;
    binding.pending_external_event_sequence =
        checkpoint.pending_external_event_sequence;
    binding.pending_export_guest_address =
        checkpoint.pending_export_guest_address;
    return binding;
  }

  static bool RequireContinue(GuestExecutionSessionAssemblerAction action,
                              const char* operation,
                              std::string* error) noexcept {
    if (action == GuestExecutionSessionAssemblerAction::kContinue) {
      return true;
    }
    *error = std::string("test bridge failed to record canonical ") + operation;
    return false;
  }

  std::atomic<bool> record_next_event_as_segment_only_{false};
  std::atomic<uint64_t> observed_scheduler_event_count_{0};
};

class CountingPublisher final : public GuestExecutionSessionAssemblerPublisher {
 public:
  bool Publish(const GuestExecutionSessionBundle&,
               std::string*) noexcept override {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    condition.notify_all();
    GuestExecutionSessionCaptureRuntime* runtime = reentrant_runtime;
    if (runtime && !reentered.exchange(true, std::memory_order_acq_rel)) {
      lock.unlock();
      runtime->Shutdown();
      lock.lock();
    }
    condition.wait(lock, [this]() { return !blocked; });
    ++calls;
    return true;
  }

  void Block() {
    std::lock_guard<std::mutex> lock(mutex);
    blocked = true;
    entered = false;
  }

  bool WaitForPublish() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, 2s, [this]() { return entered; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    blocked = false;
    condition.notify_all();
  }

  void ReenterShutdownOnPublish(GuestExecutionSessionCaptureRuntime* runtime) {
    reentrant_runtime = runtime;
  }

  std::atomic<uint32_t> calls{0};
  std::atomic<bool> reentered{false};

 private:
  std::mutex mutex;
  std::condition_variable condition;
  bool blocked = false;
  bool entered = false;
  GuestExecutionSessionCaptureRuntime* reentrant_runtime = nullptr;
};

class FakeCheckpointController final
    : public GuestExecutionSessionCaptureRuntimeCheckpointController {
 public:
  explicit FakeCheckpointController(const ThreadState& thread_state) {
    provisional.generation = 1;
    provisional.active = true;
    provisional.quiesced = true;
    provisional.dispatch_cpu_mask = 1;
    provisional.quiesced_cpu_mask = 1;
    provisional.roster_scope =
        kernel::GuestSchedulerCheckpointRosterScope::kSchedulerOwned;
    provisional.release_policy = kernel::GuestSchedulerCheckpointReleasePolicy::
        kRunningSafepointsRequeueAtHead;
    CheckpointParticipant participant;
    participant.thread_id = thread_state.thread_id();
    participant.guest_pc = 0x82001000;
    participant.cpu = 0;
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kRunning;
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint;
    participant.restorable = true;
    provisional.participants.push_back(participant);
  }

  CheckpointRejection Pause(std::chrono::milliseconds,
                            CheckpointSnapshot* snapshot) override {
    const uint32_t current_pause = ++pause_count;
    {
      std::unique_lock<std::mutex> lock(mutex);
      if (block_pause_number == current_pause) {
        pause_entered = true;
        condition.notify_all();
        condition.wait(lock, [this]() { return release_pause; });
      }
    }
    if (pause_result != CheckpointRejection::kNone) {
      *snapshot = {};
      snapshot->rejection = pause_result;
      return pause_result;
    }
    *snapshot = provisional;
    return CheckpointRejection::kNone;
  }

  CheckpointRejection Finalize(uint64_t generation,
                               CheckpointSnapshot* snapshot) override {
    ++finalize_count;
    {
      std::unique_lock<std::mutex> lock(mutex);
      finalize_entered = true;
      condition.notify_all();
      condition.wait(lock, [this]() { return !block_finalize || release; });
    }
    *snapshot = provisional;
    snapshot->generation = generation;
    snapshot->active = finalize_keeps_active;
    snapshot->rejection = finalize_result;
    return finalize_result;
  }

  CheckpointRejection Cancel(uint64_t generation,
                             CheckpointSnapshot* snapshot) override {
    const uint32_t current_cancel = ++cancel_count;
    *snapshot = provisional;
    snapshot->generation = generation;
    if (cancel_always_keeps_active || current_cancel <= cancel_failures) {
      snapshot->active = true;
      snapshot->rejection = CheckpointRejection::kInvalidTopology;
      return CheckpointRejection::kInvalidTopology;
    }
    snapshot->active = false;
    snapshot->rejection = CheckpointRejection::kCancelled;
    return CheckpointRejection::kCancelled;
  }

  bool WaitForFinalize() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, 2s, [this]() { return finalize_entered; });
  }

  void BlockNextFinalize() {
    std::lock_guard<std::mutex> lock(mutex);
    block_finalize = true;
    finalize_entered = false;
    release = false;
  }

  void ReleaseFinalize() {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    condition.notify_all();
  }

  bool WaitForPause() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, 2s, [this]() { return pause_entered; });
  }

  void ReleasePause() {
    std::lock_guard<std::mutex> lock(mutex);
    release_pause = true;
    condition.notify_all();
  }

  CheckpointSnapshot provisional;
  CheckpointRejection pause_result = CheckpointRejection::kNone;
  CheckpointRejection finalize_result = CheckpointRejection::kNone;
  bool block_finalize = false;
  bool finalize_keeps_active = false;
  bool cancel_always_keeps_active = false;
  uint32_t block_pause_number = 0;
  uint32_t cancel_failures = 0;
  std::atomic<uint32_t> pause_count{0};
  std::atomic<uint32_t> finalize_count{0};
  std::atomic<uint32_t> cancel_count{0};

 private:
  std::mutex mutex;
  std::condition_variable condition;
  bool finalize_entered = false;
  bool release = false;
  bool pause_entered = false;
  bool release_pause = false;
};

struct RuntimeHarness {
  RuntimeHarness(RuntimeEnvironment& environment, ThreadState& thread,
                 size_t queue_capacity,
                 GuestExecutionReelCoverageMode coverage_mode =
                     GuestExecutionReelCoverageMode::kContinuousInstructions)
      : checkpoint(thread), config(MakeConfig(queue_capacity)) {
    config.assembler.coverage_mode = coverage_mode;
    dependencies.clock = &clock;
    dependencies.provider = &provider;
    dependencies.event_bridge = &event_bridge;
    dependencies.publisher = &publisher;
    dependencies.checkpoint_controller = &checkpoint;
    runtime = GuestExecutionSessionCaptureRuntime::CreateAndAttach(
        *environment.processor, *environment.scheduler, config, dependencies,
        &error);
  }

  FakeClock clock;
  FakeProvider provider;
  CanonicalEventBridge event_bridge;
  CountingPublisher publisher;
  FakeCheckpointController checkpoint;
  GuestExecutionSessionCaptureRuntimeConfig config;
  GuestExecutionSessionCaptureRuntimeDependencies dependencies;
  std::shared_ptr<GuestExecutionSessionCaptureRuntime> runtime;
  std::string error;
};

kernel::GuestSchedulerCaptureEvent SchedulerEvent(uint64_t sequence) {
  kernel::GuestSchedulerCaptureEvent event;
  event.sequence = sequence;
  event.kind = kernel::GuestSchedulerCaptureEventKind::kDispatch;
  event.capture_instance_id = 1;
  event.guest_thread_id = 1;
  event.cpu = 0;
  return event;
}

kernel::GuestSchedulerCaptureEvent SchedulerEvent(uint64_t sequence,
                                                  const ThreadState& thread) {
  auto event = SchedulerEvent(sequence);
  event.capture_instance_id = thread.guest_execution_capture_instance_id();
  event.guest_thread_id = thread.thread_id();
  return event;
}

bool WaitForState(const GuestExecutionSessionCaptureRuntime& runtime,
                  RuntimeState state) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime.status().state == state) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool WaitForShutdownPending(
    const GuestExecutionSessionCaptureRuntime& runtime) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime.status().shutdown_pending) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool RecordCanonicalDispatch(GuestExecutionSessionCaptureRuntime& runtime,
                             const ThreadState& thread) {
  const auto before = runtime.status();
  if (!runtime.OnSchedulerEvent(
          SchedulerEvent(before.last_scheduler_sequence + 1, thread))) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status = runtime.status();
    if (status.processed_event_count > before.processed_event_count) {
      return status.state == RuntimeState::kRecording;
    }
    std::this_thread::yield();
  }
  return false;
}

}  // namespace

TEST_CASE("session capture runtime attachment is a two-source transaction",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  auto processor_occupant =
      std::make_shared<GuestExecutionCaptureHostCallRoster>();
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      processor_occupant));

  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE_FALSE(harness.runtime);
  REQUIRE(harness.error == "capture runtime observer transaction was rejected");

  auto scheduler_replacement =
      std::make_shared<kernel::GuestSchedulerCaptureEventRecorder>(8);
  REQUIRE(environment.scheduler->AttachCaptureObserver(scheduler_replacement));
  REQUIRE(environment.scheduler->DetachCaptureObserver(scheduler_replacement));
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      processor_occupant));
  thread.reset();
}

TEST_CASE("session capture runtime pre-arm path only observes sources",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->OnSchedulerEvent(SchedulerEvent(1)));
  auto pending = std::make_unique<ThreadState>(environment.processor.get(), 2);
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kIdle);
  REQUIRE(status.last_scheduler_sequence == 1);
  REQUIRE(status.queued_event_count == 0);
  REQUIRE(status.processed_event_count == 0);
  REQUIRE(status.scheduler_event_count == 0);
  REQUIRE(harness.provider.begin_count.load() == 0);
  REQUIRE(harness.provider.seal_count.load() == 0);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  pending.reset();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a one-cell event queue",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 1);
  REQUIRE_FALSE(harness.runtime);
  REQUIRE(harness.error ==
          "capture runtime event queue capacity must be a power of two and at "
          "least two");
  thread.reset();
}

TEST_CASE("session capture runtime rejects invocation-segment mode",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8,
                         GuestExecutionReelCoverageMode::kInvocationSegments);
  REQUIRE_FALSE(harness.runtime);
  REQUIRE(harness.error ==
          "capture runtime requires continuous instruction coverage mode");
  thread.reset();
}

TEST_CASE("session capture runtime linearizes concurrent start and stop",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  BlockingHook prequeue_gate;
  GuestExecutionSessionCaptureRuntimeTestAccess::SetRequestStartPrequeueHook(
      *harness.runtime, &BlockingHook::Hook, &prequeue_gate);

  bool start_result = false;
  bool stop_result = false;
  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool stop_attempting = false;
  bool stop_returned = false;
  std::thread start([&]() { start_result = harness.runtime->RequestStart(); });
  const bool start_hook_entered = prequeue_gate.WaitForEntry();
  std::thread stop;
  if (start_hook_entered) {
    stop = std::thread([&]() {
      {
        std::lock_guard<std::mutex> lock(stop_mutex);
        stop_attempting = true;
      }
      stop_condition.notify_all();
      stop_result = harness.runtime->RequestStop();
      {
        std::lock_guard<std::mutex> lock(stop_mutex);
        stop_returned = true;
      }
      stop_condition.notify_all();
    });
  }
  bool stop_attempt_seen = false;
  bool stop_returned_before_start_enqueued = false;
  if (stop.joinable()) {
    std::unique_lock<std::mutex> lock(stop_mutex);
    stop_attempt_seen =
        stop_condition.wait_for(lock, 2s, [&]() { return stop_attempting; });
    if (stop_attempt_seen) {
      stop_returned_before_start_enqueued =
          stop_condition.wait_for(lock, 100ms, [&]() { return stop_returned; });
    }
  }
  prequeue_gate.Release();
  start.join();
  if (stop.joinable()) {
    stop.join();
  }
  GuestExecutionSessionCaptureRuntimeTestAccess::SetRequestStartPrequeueHook(
      *harness.runtime, nullptr, nullptr);

  REQUIRE(start_hook_entered);
  REQUIRE(stop_attempt_seen);
  REQUIRE_FALSE(stop_returned_before_start_enqueued);
  REQUIRE(start_result);
  REQUIRE(stop_result);
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.runtime->status().state == RuntimeState::kRejected);
  REQUIRE(harness.runtime->status().queued_event_count == 2);
  REQUIRE(harness.runtime->status().processed_event_count == 2);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime retries a retained checkpoint generation",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.finalize_result = CheckpointRejection::kInvalidTopology;
  harness.checkpoint.finalize_keeps_active = true;
  harness.checkpoint.cancel_failures = 1;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kCheckpointBarrier);
  REQUIRE(status.checkpoint_generation == 0);
  REQUIRE(harness.checkpoint.cancel_count.load() == 2);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime exposes an unreleased checkpoint generation",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.finalize_result = CheckpointRejection::kInvalidTopology;
  harness.checkpoint.finalize_keeps_active = true;
  harness.checkpoint.cancel_always_keeps_active = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.checkpoint_generation == 1);
  REQUIRE(harness.checkpoint.cancel_count.load() ==
          harness.config.checkpoint_release_attempts);
  REQUIRE(status.message.find("retained checkpoint generation 1") !=
          std::string::npos);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime timeout never publishes",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.pause_result = CheckpointRejection::kTimedOut;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kCheckpointBarrier);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);
  REQUIRE(harness.provider.begin_count.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime queue overflow fails closed",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 2);
  REQUIRE(harness.runtime);
  harness.checkpoint.block_finalize = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.checkpoint.WaitForFinalize());
  REQUIRE(harness.runtime->OnSchedulerEvent(SchedulerEvent(1)));
  REQUIRE(harness.runtime->OnSchedulerEvent(SchedulerEvent(2)));
  REQUIRE_FALSE(harness.runtime->OnSchedulerEvent(SchedulerEvent(3)));
  harness.checkpoint.ReleaseFinalize();

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kQueueOverflow);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime preserves exact-full wrap control order",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 2);
  REQUIRE(harness.runtime);
  harness.checkpoint.block_finalize = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.checkpoint.WaitForFinalize());
  bool source_result = false;
  bool stop_result = false;
  std::thread source([&]() {
    source_result =
        harness.runtime->OnSchedulerEvent(SchedulerEvent(1, *thread));
  });
  source.join();
  std::thread control([&]() { stop_result = harness.runtime->RequestStop(); });
  control.join();
  harness.checkpoint.ReleaseFinalize();

  REQUIRE(source_result);
  REQUIRE(stop_result);
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.queued_event_count == 3);
  REQUIRE(status.processed_event_count == 3);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 1);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a no-op overlay finalizer",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.event_bridge.omit_overlay = true;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kBundleValidation);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a non-checkpoint state blob",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.provider.emit_invalid_state = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kEventBridgeFailure);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime shutdown fences a blocked start release",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.block_finalize = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.checkpoint.WaitForFinalize());
  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown([&]() {
    harness.runtime->Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  REQUIRE(WaitForShutdownPending(*harness.runtime));
  REQUIRE_FALSE(shutdown_returned.load(std::memory_order_acquire));
  harness.checkpoint.ReleaseFinalize();
  shutdown.join();

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kShutdown);
  REQUIRE(status.checkpoint_generation == 0);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);
  REQUIRE(harness.provider.end_count.load() == 1);
  REQUIRE_FALSE(harness.provider.ended_accepted.load());
  thread.reset();
}

TEST_CASE("session capture runtime serializes concurrent shutdown callers",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.block_pause_number = 1;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.checkpoint.WaitForPause());
  std::atomic<uint32_t> returned{0};
  std::thread first([&]() {
    harness.runtime->Shutdown();
    returned.fetch_add(1, std::memory_order_release);
  });
  std::thread second([&]() {
    harness.runtime->Shutdown();
    returned.fetch_add(1, std::memory_order_release);
  });
  REQUIRE(WaitForShutdownPending(*harness.runtime));
  REQUIRE(returned.load(std::memory_order_acquire) == 0);
  harness.checkpoint.ReleasePause();
  first.join();
  second.join();

  const auto status = harness.runtime->status();
  REQUIRE(returned.load(std::memory_order_acquire) == 2);
  REQUIRE(status.state == RuntimeState::kShutdown);
  REQUIRE_FALSE(status.worker_running);
  REQUIRE_FALSE(status.processor_attached);
  REQUIRE_FALSE(status.scheduler_attached);
  thread.reset();
}

TEST_CASE("session capture runtime shutdown fences a blocked seal",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.provider.BlockSeal();
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.provider.WaitForSeal());
  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown([&]() {
    harness.runtime->Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  REQUIRE(WaitForShutdownPending(*harness.runtime));
  REQUIRE_FALSE(shutdown_returned.load(std::memory_order_acquire));
  harness.provider.ReleaseSeal();
  shutdown.join();

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kShutdown);
  REQUIRE(status.checkpoint_generation == 0);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);
  REQUIRE(harness.provider.end_count.load() == 1);
  REQUIRE_FALSE(harness.provider.ended_accepted.load());
  thread.reset();
}

TEST_CASE("session capture runtime shutdown fences a blocked stop release",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.BlockNextFinalize();
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.checkpoint.WaitForFinalize());
  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown([&]() {
    harness.runtime->Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  REQUIRE(WaitForShutdownPending(*harness.runtime));
  REQUIRE_FALSE(shutdown_returned.load(std::memory_order_acquire));
  harness.checkpoint.ReleaseFinalize();
  shutdown.join();

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kShutdown);
  REQUIRE(status.checkpoint_generation == 0);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);
  REQUIRE(harness.provider.end_count.load() == 1);
  REQUIRE_FALSE(harness.provider.ended_accepted.load());
  thread.reset();
}

TEST_CASE("session capture runtime shutdown serializes a blocked publisher",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.publisher.Block();
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.publisher.WaitForPublish());
  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown([&]() {
    harness.runtime->Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  REQUIRE(WaitForShutdownPending(*harness.runtime));
  REQUIRE_FALSE(shutdown_returned.load(std::memory_order_acquire));
  harness.publisher.Release();
  shutdown.join();

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.checkpoint_generation == 0);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 1);
  REQUIRE_FALSE(status.worker_running);
  thread.reset();
}

TEST_CASE("session capture runtime publisher may reenter shutdown",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.publisher.ReenterShutdownOnPublish(harness.runtime.get());
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.publisher.reentered.load(std::memory_order_acquire));
  REQUIRE(harness.publisher.calls.load() == 1);
  REQUIRE(harness.runtime->status().state == RuntimeState::kComplete);
  REQUIRE(harness.runtime->status().canonical_output_published);

  harness.runtime->Shutdown();
  const auto status = harness.runtime->status();
  REQUIRE_FALSE(status.worker_running);
  REQUIRE_FALSE(status.processor_attached);
  REQUIRE_FALSE(status.scheduler_attached);
  thread.reset();
}

TEST_CASE("session capture runtime rejects lifecycle mutation before cutoff",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.block_pause_number = 2;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.checkpoint.WaitForPause());
  auto pending = std::make_unique<ThreadState>(environment.processor.get(), 2);
  harness.checkpoint.ReleasePause();

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  pending.reset();
  thread.reset();
}

TEST_CASE("session capture runtime includes host call before cutoff",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.block_pause_number = 2;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.checkpoint.WaitForPause());
  BlockingGuestFunction function(0x82002000, 0x82002100);
  bool call_result = false;
  std::thread source(
      [&]() { call_result = function.Call(thread.get(), 0x82003000); });
  const bool entered = function.WaitForEntry();
  bool segment_queued = false;
  if (entered) {
    harness.event_bridge.RecordNextEventAsSegmentOnly();
    auto event = SchedulerEvent(
        harness.runtime->status().last_scheduler_sequence + 1, *thread);
    segment_queued = harness.runtime->OnSchedulerEvent(event);
  }
  function.Release();
  source.join();
  REQUIRE(entered);
  REQUIRE(segment_queued);
  REQUIRE(call_result);
  harness.checkpoint.ReleasePause();

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 1);
  REQUIRE(status.queued_event_count >= 4);
  REQUIRE(status.processed_event_count == status.queued_event_count);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE(
    "session capture runtime includes zero-segment host call before cutoff",
    "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.block_pause_number = 2;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.checkpoint.WaitForPause());
  ImmediateGuestFunction function(0x82002000, 0x82002100);
  REQUIRE(function.Call(thread.get(), 0x82003000));
  harness.checkpoint.ReleasePause();

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 1);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime excludes lifecycle mutation after cutoff",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.provider.BlockSeal();
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.provider.WaitForSeal());
  auto pending = std::make_unique<ThreadState>(environment.processor.get(), 2);
  ImmediateGuestFunction function(0x82002000, 0x82002100);
  REQUIRE(function.Call(thread.get(), 0x82003000));
  harness.provider.ReleaseSeal();

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 1);

  harness.runtime->Shutdown();
  pending.reset();
  thread.reset();
}

TEST_CASE("session capture runtime lifecycle callback never enters assembler",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.provider.BlockStateEncoding();

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.provider.WaitForStateEncoding());
  std::unique_ptr<ThreadState> pending;
  std::mutex callback_mutex;
  std::condition_variable callback_condition;
  bool callback_returned = false;
  std::thread source([&]() {
    pending = std::make_unique<ThreadState>(environment.processor.get(), 2);
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      callback_returned = true;
    }
    callback_condition.notify_all();
  });
  bool returned_without_worker = false;
  {
    std::unique_lock<std::mutex> lock(callback_mutex);
    returned_without_worker = callback_condition.wait_for(
        lock, 250ms, [&]() { return callback_returned; });
  }
  harness.provider.ReleaseStateEncoding();
  source.join();
  REQUIRE(returned_without_worker);

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.runtime->status().state == RuntimeState::kRejected);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  pending.reset();
  thread.reset();
}

TEST_CASE("session capture runtime shutdown detaches before dispatch",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  REQUIRE(harness.runtime->status().processor_attached);
  REQUIRE(harness.runtime->status().scheduler_attached);

  harness.runtime->Shutdown();
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kShutdown);
  REQUIRE_FALSE(status.worker_running);
  REQUIRE_FALSE(status.processor_attached);
  REQUIRE_FALSE(status.scheduler_attached);

  auto scheduler_replacement =
      std::make_shared<kernel::GuestSchedulerCaptureEventRecorder>(8);
  auto processor_replacement =
      std::make_shared<GuestExecutionCaptureHostCallRoster>();
  REQUIRE(environment.scheduler->AttachCaptureObserver(scheduler_replacement));
  REQUIRE(environment.processor->AttachGuestExecutionCaptureHostCallObserver(
      processor_replacement));
  REQUIRE(environment.scheduler->DetachCaptureObserver(scheduler_replacement));
  REQUIRE(environment.processor->DetachGuestExecutionCaptureHostCallObserver(
      processor_replacement));

  thread.reset();
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
