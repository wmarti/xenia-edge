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
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_continuous_event.h"
#include "xenia/cpu/guest_execution_session_capture_runtime.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/pm4_marker_sink.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/memory.h"

DECLARE_path(guest_execution_capture_output);

namespace xe {
namespace kernel {

class GuestExecutionSessionCaptureRuntimeTestAccess final {
 public:
  static void SetCaptureRejected(GuestScheduler& scheduler) {
    std::lock_guard<std::mutex> lock(scheduler.lock_);
    scheduler.capture_rejected_ = true;
  }
};

}  // namespace kernel

namespace cpu {

class GuestExecutionSessionCaptureRuntimeTestAccess final {
 public:
  static void SetRequestStartPrequeueHook(
      GuestExecutionSessionCaptureRuntime& runtime, void (*hook)(void*),
      void* context) {
    runtime.SetRequestStartPrequeueTestHook(hook, context);
  }
};

class GuestExecutionSessionTitleCaptureRuntimeTestAccess final {
 public:
  static std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime> Create(
      Memory& memory, Processor& processor,
      const GuestExecutionSessionTitleCaptureConfig& config,
      const GuestExecutionSessionSha256& capture_build_sha256,
      const GuestExecutionSessionSha256& replay_config_sha256,
      std::string* error) {
    return GuestExecutionSessionTitleCaptureRuntime::
        CreateAndAttachProviderWithProvenance(memory, processor, config, true,
                                              capture_build_sha256,
                                              replay_config_sha256, error);
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

class TitleCaptureTestGraphicsSystem final : public gpu::GraphicsSystem {
 public:
  explicit TitleCaptureTestGraphicsSystem(Memory& memory) { memory_ = &memory; }

  std::string name() const override { return "title-capture-test"; }

 private:
  std::unique_ptr<gpu::CommandProcessor> CreateCommandProcessor() override {
    return nullptr;
  }
};

class TitleCaptureTestCommandProcessor final : public gpu::CommandProcessor {
 public:
  explicit TitleCaptureTestCommandProcessor(gpu::GraphicsSystem& graphics)
      : gpu::CommandProcessor(&graphics, nullptr) {}

  void TracePlaybackWroteMemory(uint32_t, uint32_t) override {}
  void RestoreEdramSnapshot(const void*) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
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

  void Advance(uint64_t delta) {
    now.fetch_add(delta, std::memory_order_relaxed);
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

  bool CollectInstructionCoverageDeltas(
      std::vector<GuestExecutionSessionInstructionCoverageDelta>* output,
      std::string*) noexcept override {
    std::lock_guard<std::mutex> lock(coverage_mutex);
    output->clear();
    output->swap(pending_coverage);
    ++coverage_collection_count;
    return true;
  }

  bool SealCapture(const CheckpointSnapshot&,
                   const GuestExecutionCaptureHostCallRosterSnapshot&,
                   std::string*) noexcept override {
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

  void QueueInstructionCoverage(
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint64_t guest_instruction_delta) {
    std::lock_guard<std::mutex> lock(coverage_mutex);
    pending_coverage.push_back({participant, guest_instruction_delta});
  }

  std::atomic<uint32_t> begin_count{0};
  std::atomic<uint32_t> seal_count{0};
  std::atomic<uint32_t> end_count{0};
  std::atomic<uint32_t> coverage_collection_count{0};
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
  std::mutex coverage_mutex;
  std::vector<GuestExecutionSessionInstructionCoverageDelta> pending_coverage;
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
    const uint64_t instruction_delta =
        event.guest_instruction_delta ? event.guest_instruction_delta : 10;
    if (!RequireContinue(
            assembler.OnInstructionCoverage(participant, instruction_delta),
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
      auto decode_checkpoint_state =
          [&](size_t index, GuestExecutionSessionCheckpointChunk* chunk,
              ppc::GuestPPCThreadCheckpoint* state) {
            if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
                    bundle->chunks[index], chunk, error) ||
                chunk->checkpoint.thread_states.size() != 1) {
              return false;
            }
            const GuestExecutionSessionThreadStateReference& reference =
                chunk->checkpoint.thread_states.front();
            const auto blob = std::find_if(
                bundle->content_blobs.cbegin(), bundle->content_blobs.cend(),
                [&reference](
                    const GuestExecutionSessionContentBlob& candidate) {
                  return candidate.sha256 == reference.sha256;
                });
            return blob != bundle->content_blobs.cend() &&
                   ppc::GuestPPCThreadCheckpointCodec::Decode(blob->bytes,
                                                              state, error);
          };
      GuestExecutionSessionCheckpointChunk initial_checkpoint;
      GuestExecutionSessionCheckpointChunk final_checkpoint;
      ppc::GuestPPCThreadCheckpoint initial_thread_state;
      ppc::GuestPPCThreadCheckpoint final_thread_state;
      if (!decode_checkpoint_state(0, &initial_checkpoint,
                                   &initial_thread_state) ||
          !decode_checkpoint_state(final_index, &final_checkpoint,
                                   &final_thread_state)) {
        return false;
      }
      const GuestExecutionSessionThreadStateReference& final_state =
          final_checkpoint.checkpoint.thread_states.front();

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
      boundary.checkpoint.binding = BindingFor(final_thread_state);

      std::vector<uint8_t> overlay_bytes;
      if (!GuestExecutionContinuousEventCodec::Encode(continuous_events,
                                                      &overlay_bytes, error)) {
        return false;
      }
      const uint32_t overlay_ordinal = static_cast<uint32_t>(final_index);
      const uint32_t start_topology_ordinal = overlay_ordinal + 1;
      const uint32_t final_topology_ordinal = overlay_ordinal + 2;
      final_checkpoint.ordinal = overlay_ordinal + 3;
      auto make_topology =
          [&](const ppc::GuestPPCThreadCheckpoint& state,
              GuestExecutionSessionSchedulerTopologyBoundary boundary_kind,
              uint32_t ordinal, uint64_t global_sequence) {
            GuestExecutionSessionSchedulerTopologyChunk topology;
            topology.session_epoch = bundle->manifest.session_epoch;
            topology.ordinal = ordinal;
            topology.boundary = boundary_kind;
            topology.global_sequence = global_sequence;
            GuestExecutionSessionSchedulerTopologyParticipant durable;
            durable.ordinal = participant.ordinal;
            durable.guest_thread_id = participant.guest_thread_id;
            durable.capture_instance_id = participant.capture_instance_id;
            durable.state =
                GuestExecutionSessionSchedulerParticipantState::kRunning;
            durable.cpu = 0;
            durable.effective_priority = 0;
            durable.base_priority = 0;
            durable.suspension_count = 0;
            durable.quantum_remaining_us = 0;
            durable.resume_kind =
                GuestExecutionSessionSchedulerResumeKind::kJitSafepoint;
            durable.guest_pc = state.resume_pc;
            durable.restorable = true;
            topology.participants.push_back(durable);
            return topology;
          };
      const auto start_topology =
          make_topology(initial_thread_state,
                        GuestExecutionSessionSchedulerTopologyBoundary::kStart,
                        start_topology_ordinal, 0);
      const auto final_topology = make_topology(
          final_thread_state,
          GuestExecutionSessionSchedulerTopologyBoundary::kFinal,
          final_topology_ordinal, bundle->manifest.last_event_sequence);
      std::vector<uint8_t> start_topology_bytes;
      std::vector<uint8_t> final_topology_bytes;
      std::vector<uint8_t> final_bytes;
      if (!GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
              start_topology, &start_topology_bytes, error) ||
          !GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
              final_topology, &final_topology_bytes, error) ||
          !GuestExecutionSessionCodec::EncodeCheckpointChunk(
              final_checkpoint, &final_bytes, error)) {
        return false;
      }
      bundle->chunks.insert(bundle->chunks.begin() + final_index,
                            std::move(overlay_bytes));
      bundle->chunks.insert(bundle->chunks.end() - 1,
                            std::move(start_topology_bytes));
      bundle->chunks.insert(bundle->chunks.end() - 1,
                            std::move(final_topology_bytes));
      bundle->chunks.back() = std::move(final_bytes);
      bundle->manifest.chunks.insert(
          bundle->manifest.chunks.begin() + final_index,
          ReferenceFor(GuestExecutionSessionChunkKind::kContinuousEvents,
                       overlay_ordinal,
                       continuous_events.front().global_sequence,
                       continuous_events.back().global_sequence,
                       static_cast<uint32_t>(continuous_events.size()),
                       bundle->chunks[final_index]));
      bundle->manifest.chunks.insert(
          bundle->manifest.chunks.end() - 1,
          ReferenceFor(GuestExecutionSessionChunkKind::kSchedulerTopology,
                       start_topology.ordinal, start_topology.global_sequence,
                       start_topology.global_sequence, 1,
                       bundle->chunks[start_topology_ordinal]));
      bundle->manifest.chunks.insert(
          bundle->manifest.chunks.end() - 1,
          ReferenceFor(GuestExecutionSessionChunkKind::kSchedulerTopology,
                       final_topology.ordinal, final_topology.global_sequence,
                       final_topology.global_sequence, 1,
                       bundle->chunks[final_topology_ordinal]));
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
  bool Publish(const GuestExecutionSessionBundle& bundle,
               std::string*) noexcept override {
    stop_reason.store(bundle.manifest.stop_reason, std::memory_order_release);
    stop_request_guest_instruction_count.store(
        bundle.manifest.stop_request_guest_instruction_count,
        std::memory_order_release);
    stop_request_matching_guest_marker_count.store(
        bundle.manifest.stop_request_matching_guest_marker_count,
        std::memory_order_release);
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
  std::atomic<GuestExecutionSessionStopReason> stop_reason{
      GuestExecutionSessionStopReason::kManualRequest};
  std::atomic<uint64_t> stop_request_guest_instruction_count{0};
  std::atomic<uint64_t> stop_request_matching_guest_marker_count{0};

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
    participant.capture_instance_id =
        thread_state.guest_execution_capture_instance_id();
    participant.guest_pc = kResumePc;
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
      if (block_pause_number.load(std::memory_order_acquire) == current_pause) {
        pause_entered = true;
        condition.notify_all();
        condition.wait(lock, [this]() { return release_pause; });
      }
      held_snapshot = provisional;
      held_snapshot.participants.front().guest_pc =
          current_pause == 1 ? start_guest_pc : stop_guest_pc;
    }
    if (pause_result != CheckpointRejection::kNone) {
      *snapshot = {};
      snapshot->rejection = pause_result;
      return pause_result;
    }
    *snapshot = held_snapshot;
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
    *snapshot = held_snapshot;
    snapshot->generation = generation;
    snapshot->active = finalize_keeps_active;
    snapshot->rejection = finalize_result;
    return finalize_result;
  }

  CheckpointRejection Cancel(uint64_t generation,
                             CheckpointSnapshot* snapshot) override {
    const uint32_t current_cancel = ++cancel_count;
    *snapshot = held_snapshot;
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
  uint32_t start_guest_pc = kResumePc;
  uint32_t stop_guest_pc = kResumePc;
  std::atomic<uint32_t> block_pause_number{0};
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
  CheckpointSnapshot held_snapshot;
};

enum class ExternalSinkOperation : uint8_t {
  kHold,
  kResumeAfterStart,
  kSealAndDetach,
  kAbortAndDetach,
};

struct ExternalSinkObservation {
  ExternalSinkOperation operation = ExternalSinkOperation::kHold;
  uint32_t checkpoint_pause_count = 0;
  uint32_t checkpoint_finalize_count = 0;
  uint32_t provider_begin_count = 0;
  uint32_t provider_seal_count = 0;
};

class FakeExternalSink final
    : public GuestExecutionSessionCaptureRuntimeExternalSink {
 public:
  bool Hold(gpu::Pm4MarkerHoldToken* token,
            std::string* error) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordOperationLocked(ExternalSinkOperation::kHold);
    ++hold_count_;
    if (!attached_) {
      return Fail(error, "test PM4 external sink held after detach");
    }
    if (fail_hold_call && hold_count_ == fail_hold_call) {
      return Fail(error, "test PM4 external sink hold failed");
    }
    if (held_) {
      *token = hold_token_;
      return true;
    }
    hold_token_.sink_generation = generation_;
    hold_token_.hold_epoch = ++hold_epoch_;
    hold_token_.last_ordinal = last_ordinal_;
    held_ = true;
    *token = hold_token_;
    return true;
  }

  bool AcknowledgeArmAndResumeAfterStart(const gpu::Pm4MarkerHoldToken& token,
                                         std::string* error) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordOperationLocked(ExternalSinkOperation::kResumeAfterStart);
    if (!held_ || token != hold_token_) {
      return Fail(error, "test PM4 external sink resumed while unheld");
    }
    if (fail_resume) {
      return Fail(error, "test PM4 external sink resume failed");
    }
    ++generation_;
    held_ = false;
    hold_token_ = {};
    return true;
  }

  bool IsSourceHealthy(std::string* error) const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_) {
      return true;
    }
    if (!healthy.load()) {
      return Fail(error, "test PM4 external source was lost");
    }
    return true;
  }

  bool SealAndDetach(const gpu::Pm4MarkerHoldToken& token,
                     std::string* error) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordOperationLocked(ExternalSinkOperation::kSealAndDetach);
    if (produce_swap_during_seal) {
      ++last_ordinal_;
      healthy.store(false);
    }
    if (!attached_ || !held_ || token != hold_token_ || !healthy.load()) {
      return Fail(error, "test PM4 external sink terminal seal failed");
    }
    held_ = false;
    attached_ = false;
    hold_token_ = {};
    return true;
  }

  bool AbortAndDetach(std::string* error) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_) {
      return true;
    }
    RecordOperationLocked(ExternalSinkOperation::kAbortAndDetach);
    if (fail_abort_detach) {
      return Fail(error, "test PM4 external sink abort detach failed");
    }
    held_ = false;
    attached_ = false;
    hold_token_ = {};
    return true;
  }

  void ShutdownSource() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (attached_) {
      healthy.store(false);
    }
  }

  void ProduceSwap() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_) {
      return;
    }
    ++last_ordinal_;
    if (held_) {
      healthy.store(false);
    }
  }

  std::vector<ExternalSinkOperation> operations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return operations_;
  }

  std::vector<ExternalSinkObservation> observations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return observations_;
  }

  bool held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_;
  }

  bool attached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return attached_;
  }

  uint32_t fail_hold_call = 0;
  bool fail_resume = false;
  bool fail_abort_detach = false;
  bool produce_swap_during_seal = false;
  std::atomic<bool> healthy{true};
  const FakeCheckpointController* checkpoint = nullptr;
  const FakeProvider* provider = nullptr;

 private:
  void RecordOperationLocked(ExternalSinkOperation operation) {
    operations_.push_back(operation);
    ExternalSinkObservation observation;
    observation.operation = operation;
    if (checkpoint) {
      observation.checkpoint_pause_count = checkpoint->pause_count.load();
      observation.checkpoint_finalize_count = checkpoint->finalize_count.load();
    }
    if (provider) {
      observation.provider_begin_count = provider->begin_count.load();
      observation.provider_seal_count = provider->seal_count.load();
    }
    observations_.push_back(observation);
  }

  mutable std::mutex mutex_;
  std::vector<ExternalSinkOperation> operations_;
  std::vector<ExternalSinkObservation> observations_;
  uint32_t hold_count_ = 0;
  uint64_t generation_ = 1;
  uint64_t hold_epoch_ = 0;
  uint64_t last_ordinal_ = 0;
  gpu::Pm4MarkerHoldToken hold_token_;
  bool held_ = false;
  bool attached_ = true;
};

struct RuntimeHarness {
  RuntimeHarness(RuntimeEnvironment& environment, ThreadState& thread,
                 size_t queue_capacity,
                 GuestExecutionReelCoverageMode coverage_mode =
                     GuestExecutionReelCoverageMode::kContinuousInstructions,
                 GuestExecutionSessionBoundaryKind boundary_kind =
                     GuestExecutionSessionBoundaryKind::kManual,
                 uint64_t boundary_value = 0, bool enable_external_sink = false,
                 uint32_t external_sink_ordinal = 0,
                 uint32_t fail_external_hold_call = 0,
                 bool fail_external_resume = false)
      : checkpoint(thread), config(MakeConfig(queue_capacity)) {
    config.assembler.coverage_mode = coverage_mode;
    config.assembler.boundary.kind = boundary_kind;
    if (boundary_kind == GuestExecutionSessionBoundaryKind::kGuestMarkerCount) {
      config.assembler.boundary.marker_source =
          GuestExecutionSessionMarkerSource::kPm4Swap;
      config.assembler.boundary.marker_identity = gpu::kPm4SwapMarkerOpcode;
      config.assembler.boundary.value = boundary_value;
    } else if (boundary_kind ==
               GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds) {
      config.assembler.boundary.value = boundary_value;
      config.assembler.limits.maximum_duration_ticks = boundary_value * 2;
    } else if (boundary_kind ==
               GuestExecutionSessionBoundaryKind::kGuestInstructionCount) {
      config.assembler.boundary.value = boundary_value;
    }
    if (enable_external_sink) {
      config.assembler.pm4_marker_sink_ordinal = external_sink_ordinal;
      external_sink.fail_hold_call = fail_external_hold_call;
      external_sink.fail_resume = fail_external_resume;
      external_sink.checkpoint = &checkpoint;
      external_sink.provider = &provider;
    }
    dependencies.clock = &clock;
    dependencies.provider = &provider;
    dependencies.event_bridge = &event_bridge;
    dependencies.publisher = &publisher;
    dependencies.checkpoint_controller = &checkpoint;
    dependencies.pm4_external_sink =
        enable_external_sink ? &external_sink : nullptr;
    runtime = GuestExecutionSessionCaptureRuntime::CreateAndAttach(
        *environment.processor, *environment.scheduler, config, dependencies,
        &error);
  }

  FakeClock clock;
  FakeProvider provider;
  CanonicalEventBridge event_bridge;
  CountingPublisher publisher;
  FakeCheckpointController checkpoint;
  FakeExternalSink external_sink;
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
  ImmediateGuestFunction function(0x82002000, 0x82002100);
  REQUIRE(function.Call(thread.get(), 0x82003000));
  auto pending = std::make_unique<ThreadState>(environment.processor.get(), 2);
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kIdle);
  REQUIRE(status.last_scheduler_sequence == 1);
  REQUIRE(status.queued_event_count == 0);
  REQUIRE(status.processed_event_count == 0);
  REQUIRE(status.scheduler_event_count == 0);
  REQUIRE_FALSE(status.external_sink_registered);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE_FALSE(status.external_sink_control_failed);
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

TEST_CASE("session capture runtime rejects exact instruction boundaries",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 16,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kGuestInstructionCount, 10);
  REQUIRE_FALSE(harness.runtime);
  REQUIRE(harness.error ==
          "capture runtime does not support exact instruction-count "
          "boundaries");
  thread.reset();
}

TEST_CASE("session capture runtime admits coverage before duration stop",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  constexpr uint64_t kDuration = 100000;
  RuntimeHarness harness(
      environment, *thread, 16,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds,
      kDuration);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  harness.checkpoint.block_pause_number = 2;
  harness.clock.Advance(kDuration);
  const bool pause_entered = harness.checkpoint.WaitForPause();
  bool event_accepted = false;
  if (pause_entered) {
    auto event = SchedulerEvent(
        harness.runtime->status().last_scheduler_sequence + 1, *thread);
    event.guest_instruction_delta = 23;
    event_accepted = harness.runtime->OnSchedulerEvent(event);
  }
  harness.checkpoint.ReleasePause();
  CHECK(pause_entered);
  REQUIRE(event_accepted);
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.stop_request_guest_instruction_count.load(
              std::memory_order_acquire) == 23);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE(
    "session capture runtime preserves an event boundary found while "
    "quiescing for duration",
    "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 16,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kGuestMarkerCount, 1);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.block_pause_number = 2;
  harness.clock.Advance(harness.config.assembler.limits.maximum_duration_ticks);
  const bool pause_entered = harness.checkpoint.WaitForPause();
  bool marker_accepted = false;
  if (pause_entered) {
    marker_accepted = harness.runtime->OnGuestMarker(
        GuestExecutionSessionMarkerSource::kPm4Swap, gpu::kPm4SwapMarkerOpcode);
  }
  harness.checkpoint.ReleasePause();
  CHECK(pause_entered);
  REQUIRE(marker_accepted);
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.stop_reason.load(std::memory_order_acquire) ==
          GuestExecutionSessionStopReason::kRequestedBoundary);
  REQUIRE(harness.publisher.stop_request_matching_guest_marker_count.load(
              std::memory_order_acquire) == 1);
  REQUIRE(harness.publisher.stop_request_guest_instruction_count.load(
              std::memory_order_acquire) == 10);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime preserves scheduler-ordered coverage",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  auto event = SchedulerEvent(
      harness.runtime->status().last_scheduler_sequence + 1, *thread);
  event.guest_instruction_delta = 7;
  REQUIRE(harness.runtime->OnSchedulerEvent(event));
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.runtime->status().state == RuntimeState::kComplete);
  REQUIRE(harness.publisher.stop_request_guest_instruction_count.load(
              std::memory_order_acquire) == 7);
  REQUIRE(harness.provider.coverage_collection_count.load(
              std::memory_order_acquire) != 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects unordered checkpoint residuals",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 16);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  harness.provider.QueueInstructionCoverage(
      {thread->guest_execution_capture_instance_id(), thread->thread_id()}, 7);
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kSourceRejected);
  REQUIRE_FALSE(status.canonical_output_published);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a stopped scheduler source",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 16);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  kernel::GuestExecutionSessionCaptureRuntimeTestAccess::SetCaptureRejected(
      *environment.scheduler);
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kEventBridgeFailure);
  REQUIRE_FALSE(status.canonical_output_published);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime orders coverage before host return",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 16);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  ImmediateGuestFunction function(0x82002000, 0x82002100);
  const GuestExecutionCaptureHostCallToken token =
      harness.runtime->OnHostGuestCallBegin(*thread, function, 0x82003000);
  REQUIRE(token);
  auto* context = thread->context();
  std::atomic_ref<uint64_t>(context->guest_execution_session_instruction_count)
      .store(9, std::memory_order_relaxed);
  std::atomic_ref<uint64_t*>(
      context->guest_execution_session_instruction_counter)
      .store(&context->guest_execution_session_instruction_count,
             std::memory_order_release);
  REQUIRE(harness.runtime->OnHostGuestCallEnd(
      token, *thread, function,
      GuestExecutionCaptureHostCallOutcome::kReturnedToHost));
  std::atomic_ref<uint64_t*>(
      context->guest_execution_session_instruction_counter)
      .store(nullptr, std::memory_order_release);
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.runtime->status().state == RuntimeState::kComplete);
  REQUIRE(harness.publisher.stop_request_guest_instruction_count.load(
              std::memory_order_acquire) == 19);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects foreign instruction counters",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 16);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  ImmediateGuestFunction function(0x82002000, 0x82002100);
  const GuestExecutionCaptureHostCallToken token =
      harness.runtime->OnHostGuestCallBegin(*thread, function, 0x82003000);
  REQUIRE(token);
  uint64_t foreign_counter = 83;
  auto* context = thread->context();
  std::atomic_ref<uint64_t*>(
      context->guest_execution_session_instruction_counter)
      .store(&foreign_counter, std::memory_order_release);
  REQUIRE_FALSE(harness.runtime->OnHostGuestCallEnd(
      token, *thread, function,
      GuestExecutionCaptureHostCallOutcome::kReturnedToHost));
  std::atomic_ref<uint64_t*>(
      context->guest_execution_session_instruction_counter)
      .store(nullptr, std::memory_order_release);
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.runtime->status().state == RuntimeState::kRejected);
  REQUIRE(foreign_counter == 83);

  harness.runtime->Shutdown();
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

TEST_CASE("session capture runtime admits a scheduler-unowned participant",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto scheduler_owned = environment.MakeThread(1);
  auto scheduler_unowned = environment.MakeThread(2);
  RuntimeHarness harness(environment, *scheduler_owned, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRecording);
  REQUIRE(status.rejection == RuntimeRejection::kNone);
  REQUIRE(harness.provider.begin_count.load() == 1);

  harness.runtime->Shutdown();
  scheduler_unowned.reset();
  scheduler_owned.reset();
}

TEST_CASE("session capture runtime rejects a partial preemption episode",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  auto& participant = harness.checkpoint.provisional.participants.front();
  SECTION("IRQL deferral") { participant.preempt_defers_irql = 1; }
  SECTION("critical-region deferral") { participant.preempt_defers_lock = 1; }
  SECTION("declined terminal count") {
    participant.capture_declined_safepoints = 1;
  }

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  INFO(status.message);
  INFO(static_cast<uint32_t>(status.rejection));
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kCheckpointRoster);
  REQUIRE(status.message.find("in-flight scheduler preemption episode") !=
          std::string::npos);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.provider.begin_count.load() == 0);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a partial final preemption episode",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.provisional.participants.front().preempt_defers_irql = 1;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  INFO(status.message);
  INFO(static_cast<uint32_t>(status.rejection));
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kCheckpointRoster);
  REQUIRE(status.message.find("in-flight scheduler preemption episode") !=
          std::string::npos);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.provider.begin_count.load() == 1);
  REQUIRE(harness.provider.seal_count.load() == 0);
  REQUIRE(harness.publisher.calls.load() == 0);

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

TEST_CASE("session capture runtime lets the assembler own the PM4 stop",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kGuestMarkerCount, 2);
  REQUIRE(harness.runtime);

  const auto before = harness.runtime->status();
  REQUIRE(harness.runtime->OnGuestMarker(
      GuestExecutionSessionMarkerSource::kPm4Swap, gpu::kPm4SwapMarkerOpcode));
  REQUIRE(harness.runtime->status().queued_event_count ==
          before.queued_event_count);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->OnGuestMarker(
      GuestExecutionSessionMarkerSource::kPm4Swap, gpu::kPm4SwapMarkerOpcode));
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(harness.runtime->OnGuestMarker(
      GuestExecutionSessionMarkerSource::kPm4Swap, gpu::kPm4SwapMarkerOpcode));

  const bool reached_terminal = harness.runtime->WaitForTerminal(2s);
  const auto status = harness.runtime->status();
  INFO("state=" << static_cast<uint32_t>(status.state)
                << " queued=" << status.queued_event_count
                << " processed=" << status.processed_event_count
                << " message=" << status.message);
  REQUIRE(reached_terminal);
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 1);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rendezvouses its PM4 external sink",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true);
  REQUIRE(harness.runtime);
  REQUIRE(harness.runtime->status().external_sink_registered);
  REQUIRE_FALSE(harness.runtime->status().external_sink_held);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart});
  REQUIRE_FALSE(harness.external_sink.held());
  REQUIRE_FALSE(harness.runtime->status().external_sink_held);
  REQUIRE(harness.runtime->status().external_sink_attested_generation == 1);
  REQUIRE(harness.runtime->status().external_sink_hold_epoch == 1);
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kComplete);
  REQUIRE(status.canonical_output_published);
  REQUIRE(status.external_sink_registered);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE_FALSE(status.external_sink_control_failed);
  REQUIRE_FALSE(harness.external_sink.held());
  REQUIRE_FALSE(harness.external_sink.attached());
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart,
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kSealAndDetach});
  const auto observations = harness.external_sink.observations();
  REQUIRE(observations.size() == 4);
  REQUIRE(observations[0].checkpoint_pause_count == 1);
  REQUIRE(observations[0].checkpoint_finalize_count == 0);
  REQUIRE(observations[0].provider_begin_count == 0);
  REQUIRE(observations[0].provider_seal_count == 0);
  REQUIRE(observations[1].checkpoint_pause_count == 1);
  REQUIRE(observations[1].checkpoint_finalize_count == 0);
  REQUIRE(observations[1].provider_begin_count == 1);
  REQUIRE(observations[1].provider_seal_count == 0);
  REQUIRE(observations[2].checkpoint_pause_count == 2);
  REQUIRE(observations[2].checkpoint_finalize_count == 1);
  REQUIRE(observations[2].provider_seal_count == 0);
  REQUIRE(observations[3].checkpoint_pause_count == 2);
  REQUIRE(observations[3].checkpoint_finalize_count == 1);
  REQUIRE(observations[3].provider_seal_count == 1);
  REQUIRE(harness.publisher.calls.load() == 1);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture resumes PM4 before releasing the start checkpoint",
          "[guest-execution-session-capture-runtime][concurrency]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true);
  REQUIRE(harness.runtime);

  harness.checkpoint.BlockNextFinalize();
  REQUIRE(harness.runtime->RequestStart());
  const bool finalize_entered = harness.checkpoint.WaitForFinalize();
  if (!finalize_entered) {
    harness.checkpoint.ReleaseFinalize();
  }
  REQUIRE(finalize_entered);
  REQUIRE_FALSE(harness.external_sink.held());
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart});

  harness.external_sink.ProduceSwap();
  REQUIRE(harness.external_sink.healthy.load());
  harness.checkpoint.ReleaseFinalize();
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  REQUIRE(harness.runtime->status().state == RuntimeState::kComplete);
  REQUIRE(harness.runtime->status().canonical_output_published);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime seals PM4 before publication and shutdown",
          "[guest-execution-session-capture-runtime][concurrency]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.publisher.Block();
  REQUIRE(harness.runtime->RequestStop());
  const bool publish_entered = harness.publisher.WaitForPublish();
  if (!publish_entered) {
    harness.publisher.Release();
  }
  REQUIRE(publish_entered);

  const auto publishing = harness.runtime->status();
  REQUIRE(publishing.state == RuntimeState::kPublishing);
  REQUIRE(publishing.external_sink_terminally_detached);
  REQUIRE_FALSE(publishing.external_sink_held);
  REQUIRE_FALSE(harness.external_sink.attached());
  REQUIRE(harness.checkpoint.finalize_count.load() == 2);
  harness.external_sink.ProduceSwap();
  harness.external_sink.ShutdownSource();
  harness.publisher.Release();

  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto complete = harness.runtime->status();
  REQUIRE(complete.state == RuntimeState::kComplete);
  REQUIRE(complete.canonical_output_published);
  REQUIRE_FALSE(complete.external_sink_control_failed);
  REQUIRE(harness.publisher.calls.load() == 1);

  // Once the terminal ticket has detached the source, later title work and
  // command-processor shutdown cannot retroactively invalidate the bundle.
  harness.external_sink.ProduceSwap();
  harness.external_sink.ShutdownSource();
  REQUIRE(harness.runtime->status().state == RuntimeState::kComplete);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a swap before terminal PM4 seal",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.external_sink.produce_swap_during_seal = true;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kExternalSinkControl);
  REQUIRE(status.external_sink_control_failed);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.checkpoint.finalize_count.load() == 1);
  REQUIRE(harness.publisher.calls.load() == 0);
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart,
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kSealAndDetach,
              ExternalSinkOperation::kAbortAndDetach});

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects PM4 source loss after resume",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  harness.external_sink.healthy.store(false);
  REQUIRE(harness.runtime->WaitForTerminal(2s));

  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kExternalSinkControl);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE(status.external_sink_control_failed);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(status.message.find("test PM4 external source was lost") !=
          std::string::npos);
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart,
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kAbortAndDetach});
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a PM4 external sink hold failure",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true, 0, 1);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kExternalSinkControl);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE(status.external_sink_control_failed);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(status.message.find("test PM4 external sink hold failed") !=
          std::string::npos);
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold, ExternalSinkOperation::kHold,
              ExternalSinkOperation::kAbortAndDetach});
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects a PM4 external sink resume failure",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true, 0, 0, true);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kExternalSinkControl);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE(status.external_sink_control_failed);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(status.message.find("test PM4 external sink resume failed") !=
          std::string::npos);
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart,
              ExternalSinkOperation::kAbortAndDetach});
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture retains its checkpoint when PM4 detach fails",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true, 0, 0, true);
  REQUIRE(harness.runtime);
  harness.external_sink.fail_abort_detach = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kExternalSinkControl);
  REQUIRE(status.external_sink_held);
  REQUIRE_FALSE(status.external_sink_terminally_detached);
  REQUIRE(status.external_sink_control_failed);
  REQUIRE(status.checkpoint_generation == 1);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(status.message.find("test PM4 external sink abort detach failed") !=
          std::string::npos);
  REQUIRE(status.message.find("retained its checkpoint") != std::string::npos);
  REQUIRE(harness.checkpoint.finalize_count.load() == 0);
  REQUIRE(harness.checkpoint.cancel_count.load() == 0);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  harness.external_sink.fail_abort_detach = false;
  std::string cleanup_error;
  REQUIRE(harness.external_sink.AbortAndDetach(&cleanup_error));
  REQUIRE(cleanup_error.empty());
  thread.reset();
}

TEST_CASE("session capture runtime rejects a stop PM4 sink hold failure",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true, 0, 2);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kExternalSinkControl);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE(status.external_sink_control_failed);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kResumeAfterStart,
              ExternalSinkOperation::kHold, ExternalSinkOperation::kHold,
              ExternalSinkOperation::kAbortAndDetach});
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime verifies its PM4 sink ordinal",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true, 1);
  REQUIRE_FALSE(harness.runtime);
  REQUIRE(harness.error ==
          "capture runtime PM4 external sink ordinal differs from the "
          "assembler configuration");
  thread.reset();
}

TEST_CASE("session capture runtime detaches its PM4 sink during idle shutdown",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(
      environment, *thread, 8,
      GuestExecutionReelCoverageMode::kContinuousInstructions,
      GuestExecutionSessionBoundaryKind::kManual, 0, true);
  REQUIRE(harness.runtime);

  harness.runtime->Shutdown();
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kShutdown);
  REQUIRE_FALSE(status.external_sink_held);
  REQUIRE(status.external_sink_terminally_detached);
  REQUIRE_FALSE(status.external_sink_control_failed);
  REQUIRE_FALSE(harness.external_sink.held());
  REQUIRE_FALSE(harness.external_sink.attached());
  REQUIRE(harness.external_sink.operations() ==
          std::vector<ExternalSinkOperation>{
              ExternalSinkOperation::kHold,
              ExternalSinkOperation::kAbortAndDetach});
  REQUIRE(harness.publisher.calls.load() == 0);
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

TEST_CASE("session capture runtime binds initial state to scheduler safepoint",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.start_guest_pc = kResumePc + 4;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kBundleValidation);
  REQUIRE(status.message.find("initial PPC continuation differs") !=
          std::string::npos);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime binds final state to scheduler safepoint",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  harness.checkpoint.stop_guest_pc = kResumePc + 4;
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kBundleValidation);
  REQUIRE(status.message.find("final PPC continuation differs") !=
          std::string::npos);
  REQUIRE_FALSE(status.canonical_output_published);
  REQUIRE(harness.publisher.calls.load() == 0);

  harness.runtime->Shutdown();
  thread.reset();
}

TEST_CASE("session capture runtime rejects an unmodeled scheduler resume route",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  auto thread = environment.MakeThread(1);
  RuntimeHarness harness(environment, *thread, 8);
  REQUIRE(harness.runtime);
  harness.checkpoint.provisional.participants.front().resume_kind =
      kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation;
  harness.checkpoint.provisional.participants.front().restorable = true;

  REQUIRE(harness.runtime->RequestStart());
  REQUIRE(WaitForState(*harness.runtime, RuntimeState::kRecording));
  REQUIRE(RecordCanonicalDispatch(*harness.runtime, *thread));
  REQUIRE(harness.runtime->RequestStop());
  REQUIRE(harness.runtime->WaitForTerminal(2s));
  const auto status = harness.runtime->status();
  REQUIRE(status.state == RuntimeState::kRejected);
  REQUIRE(status.rejection == RuntimeRejection::kBundleValidation);
  REQUIRE(status.message.find("initial PPC continuation differs") !=
          std::string::npos);
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

TEST_CASE("title session capture request remains inert without output",
          "[guest-execution-session-capture-runtime]") {
  struct OutputRestore {
    std::filesystem::path value = cvars::guest_execution_capture_output;
    ~OutputRestore() { cvars::guest_execution_capture_output = value; }
  } restore;

  cvars::guest_execution_capture_output.clear();
  REQUIRE_FALSE(GuestExecutionSessionTitleCaptureRuntime::IsRequested());
  cvars::guest_execution_capture_output = "unused-session-capture";
  REQUIRE(GuestExecutionSessionTitleCaptureRuntime::IsRequested());
}

TEST_CASE("title session capture owner detaches before its dependencies",
          "[guest-execution-session-capture-runtime]") {
  RuntimeEnvironment environment;
  GuestExecutionSessionTitleCaptureConfig config;
  config.output_directory = "unused-title-session-capture";
  config.warmup_milliseconds = 1;
  config.maximum_bundle_bytes = 1u << 20;
  std::string error;

  auto provider_only =
      GuestExecutionSessionTitleCaptureRuntimeTestAccess::Create(
          *environment.memory, *environment.processor, config, Digest(0x51),
          Digest(0x71), &error);
  REQUIRE(provider_only);
  REQUIRE(environment.processor->guest_invocation_capture_sink());
  provider_only.reset();
  REQUIRE_FALSE(environment.processor->guest_invocation_capture_sink());

  auto capture = GuestExecutionSessionTitleCaptureRuntimeTestAccess::Create(
      *environment.memory, *environment.processor, config, Digest(0x52),
      Digest(0x72), &error);
  REQUIRE(capture);
  TitleCaptureTestGraphicsSystem graphics(*environment.memory);
  TitleCaptureTestCommandProcessor command_processor(graphics);
  REQUIRE(capture->AttachRuntime(*environment.scheduler, command_processor,
                                 "title=4D5307E6", "module=halo3", &error));
  REQUIRE(capture->runtime_attached());
  REQUIRE(command_processor.pm4_marker_dispatcher_status().sink_attached);
  REQUIRE(environment.processor->guest_invocation_capture_sink());

  capture->Shutdown();
  capture->Shutdown();
  REQUIRE_FALSE(capture->runtime_attached());
  const gpu::Pm4MarkerDispatcherStatus source =
      command_processor.pm4_marker_dispatcher_status();
  REQUIRE_FALSE(source.sink_attached);
  REQUIRE_FALSE(source.sink_held);
  REQUIRE_FALSE(source.shut_down);
  REQUIRE_FALSE(environment.processor->guest_invocation_capture_sink());

  auto relaunched = GuestExecutionSessionTitleCaptureRuntimeTestAccess::Create(
      *environment.memory, *environment.processor, config, Digest(0x53),
      Digest(0x73), &error);
  REQUIRE(relaunched);
  REQUIRE(relaunched->AttachRuntime(*environment.scheduler, command_processor,
                                    "title=4D5307E6", "module=halo3-relaunch",
                                    &error));
  REQUIRE(command_processor.pm4_marker_dispatcher_status().sink_attached);
  relaunched.reset();
  REQUIRE_FALSE(command_processor.pm4_marker_dispatcher_status().sink_attached);
  REQUIRE_FALSE(environment.processor->guest_invocation_capture_sink());

  command_processor.Shutdown();
  REQUIRE(command_processor.pm4_marker_dispatcher_status().shut_down);
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
