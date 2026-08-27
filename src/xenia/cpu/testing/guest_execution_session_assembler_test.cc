/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_assembler.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_execution_continuous_event.h"
#include "xenia/cpu/guest_execution_session_capture_event_bridge.h"

namespace xe {
namespace cpu {
namespace test {

namespace {

using Action = GuestExecutionSessionAssemblerAction;
using Rejection = GuestExecutionSessionAssemblerRejection;
using State = GuestExecutionSessionAssemblerState;

constexpr GuestExecutionCaptureParticipantIdentity kA = {0x100, 7};
constexpr GuestExecutionCaptureParticipantIdentity kB = {0x101, 8};
constexpr GuestExecutionCaptureParticipantIdentity kC = {0x102, 9};
constexpr uint64_t kEpoch = 0x123456789ABCDEF0ull;
constexpr uint64_t kStartTick = 100;

class ScopedTestDirectory {
 public:
  ScopedTestDirectory() {
    std::error_code filesystem_error;
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error) {
      throw std::runtime_error("temporary directory is unavailable");
    }
    const uint64_t nonce = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint32_t attempt = 0; attempt < 100; ++attempt) {
      path_ = temporary_root /
              ("xenia-session-assembler-test-" + std::to_string(nonce) + "-" +
               std::to_string(attempt));
      filesystem_error.clear();
      if (std::filesystem::create_directory(path_, filesystem_error)) {
        return;
      }
      if (filesystem_error) {
        throw std::runtime_error("temporary directory could not be created");
      }
    }
    throw std::runtime_error("unique temporary directory could not be created");
  }

  ~ScopedTestDirectory() {
    std::error_code filesystem_error;
    std::filesystem::remove_all(path_, filesystem_error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::vector<uint8_t> Bytes(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i * 17);
  }
  return bytes;
}

GuestExecutionSessionSha256 Digest(uint8_t seed) {
  GuestExecutionSessionSha256 digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(seed + i);
  }
  return digest;
}

class FakeClock final : public ppc::GuestInvocationRecorderClock {
 public:
  uint64_t NowTicks() const override { return now; }

  uint64_t now = kStartTick;
};

class FakeStateProvider final
    : public GuestExecutionSessionAssemblerStateProvider {
 public:
  bool EncodeParticipantState(
      const GuestExecutionCaptureParticipantIdentity& participant,
      std::vector<uint8_t>* output, std::string* error) noexcept override {
    ++calls;
    if (fail) {
      *error = "state provider failed";
      return false;
    }
    if (reenter_target) {
      reenter_target->Poll();
    }
    if (encode_thread_checkpoint) {
      const auto participant_it =
          std::find(checkpoint_participants.cbegin(),
                    checkpoint_participants.cend(), participant);
      if (participant_it == checkpoint_participants.cend()) {
        *error = "thread checkpoint participant differs";
        return false;
      }
      ppc::GuestPPCThreadCheckpoint checkpoint;
      checkpoint.participant_ordinal = static_cast<uint32_t>(
          participant_it - checkpoint_participants.cbegin());
      checkpoint.guest_thread_id = participant.guest_thread_id;
      checkpoint.resume_pc = 0x82000040;
      checkpoint.owning_function_address = 0x82000000;
      checkpoint.owning_function_end_address = 0x820000FC;
      checkpoint.outer_guest_return_address = 0x82001000;
      return ppc::GuestPPCThreadCheckpointCodec::Encode(checkpoint, output,
                                                        error);
    }
    output->assign(
        state_size,
        static_cast<uint8_t>(participant.capture_instance_id + generation));
    return true;
  }

  size_t state_size = 64;
  uint8_t generation = 0;
  bool fail = false;
  bool encode_thread_checkpoint = false;
  std::vector<GuestExecutionCaptureParticipantIdentity> checkpoint_participants;
  uint32_t calls = 0;
  GuestExecutionSessionAssembler* reenter_target = nullptr;
};

class FakeContentProvider final
    : public GuestExecutionSessionAssemblerContentProvider {
 public:
  FakeContentProvider() {
    std::vector<uint8_t> code_page = Bytes(JitCorpus::kPageSize, 3);
    entries.push_back(
        {GuestExecutionSessionContentKind::kGuestCode, 0x82000000, code_page});
    entries.push_back(
        {GuestExecutionSessionContentKind::kGuestPage, 0x1000, Bytes(4096, 2)});
    ExecutionJitCorpusBuilder builder(JitCorpus::kConfigGuestScheduler);
    ExecutionJitCorpus::FunctionRecord function = {0x82000000, 0x820000FC, 64,
                                                   0};
    std::string error;
    if (!builder.AddCodePage(0x82000000, code_page.data(), code_page.size(),
                             &error) ||
        !builder.AddFunction(function, &error) ||
        !builder.Encode(&corpus, &error)) {
      throw std::runtime_error("failed to build test execution corpus: " +
                               error);
    }
  }

  bool CollectCheckpointContent(
      bool initial_checkpoint,
      std::vector<GuestExecutionSessionAssemblerContent>* output,
      std::string* error) noexcept override {
    ++calls;
    if (fail) {
      *error = "content provider failed";
      return false;
    }
    if (reenter_target && !initial_checkpoint) {
      reenter_target->Poll();
    }
    *output = entries;
    return true;
  }

  bool CollectSessionCodeCorpus(std::vector<uint8_t>* output,
                                std::string* error) noexcept override {
    ++corpus_calls;
    if (corpus_fail) {
      *error = "corpus provider failed";
      return false;
    }
    *output = corpus;
    return true;
  }

  std::vector<GuestExecutionSessionAssemblerContent> entries;
  std::vector<uint8_t> corpus;
  bool fail = false;
  bool corpus_fail = false;
  uint32_t calls = 0;
  uint32_t corpus_calls = 0;
  GuestExecutionSessionAssembler* reenter_target = nullptr;
};

class FakePublisher final : public GuestExecutionSessionAssemblerPublisher {
 public:
  bool Publish(const GuestExecutionSessionBundle& bundle,
               std::string* error) noexcept override {
    ++calls;
    if (cancel_target) {
      cancel_target->Cancel("cancelled during publication");
    }
    if (fail) {
      *error = "publisher failed";
      return false;
    }
    bundles.push_back(bundle);
    return true;
  }

  bool fail = false;
  uint32_t calls = 0;
  std::vector<GuestExecutionSessionBundle> bundles;
  GuestExecutionSessionAssembler* cancel_target = nullptr;
};

GuestExecutionSessionAssemblerConfig MakeConfig(
    GuestExecutionSessionBoundaryKind kind =
        GuestExecutionSessionBoundaryKind::kManual,
    uint64_t value = 0) {
  GuestExecutionSessionAssemblerConfig config;
  config.session_epoch = kEpoch;
  config.boundary.kind = kind;
  config.boundary.value = value;
  config.limits.maximum_segment_count = 8;
  config.limits.maximum_event_count = 64;
  config.limits.maximum_guest_instruction_count = 1024;
  config.limits.maximum_guest_marker_count = 16;
  config.limits.maximum_duration_ticks = 1000;
  config.maximum_stop_tail_event_count = 16;
  config.maximum_stop_tail_guest_instruction_count = 64;
  config.maximum_stop_tail_ticks = 300;
  config.maximum_start_rendezvous_ticks = 200;
  config.capture_tick_frequency = 1000000000;
  config.maximum_events_per_chunk = 8;
  config.capture_build_sha256 = Digest(1);
  config.replay_config_sha256 = Digest(2);
  config.title_identity_sha256 = Digest(3);
  config.module_identity_sha256 = Digest(4);
  return config;
}

GuestExecutionSessionAssemblerConfig MakeContinuousConfig(
    GuestExecutionSessionBoundaryKind kind =
        GuestExecutionSessionBoundaryKind::kManual,
    uint64_t value = 0) {
  GuestExecutionSessionAssemblerConfig config = MakeConfig(kind, value);
  config.coverage_mode =
      GuestExecutionReelCoverageMode::kContinuousInstructions;
  return config;
}

GuestExecutionSessionAssemblerSegmentEnd MakeSegmentEnd(uint64_t ordinal,
                                                        uint64_t start_tick,
                                                        uint64_t end_tick,
                                                        uint8_t seed = 0x10) {
  GuestExecutionSessionAssemblerSegmentEnd end;
  end.status.state = GuestInvocationCaptureState::kPublished;
  end.status.recorder_state = ppc::GuestInvocationRecorderState::kComplete;
  end.status.segment_ordinal = ordinal;
  end.status.accepted_segment_count = 1;
  end.status.capture_start_tick = start_tick;
  end.status.capture_end_tick = end_tick;
  end.segment = Bytes(96, static_cast<uint8_t>(seed + 1));
  return end;
}

struct SeedSpec {
  GuestExecutionCaptureParticipantIdentity identity;
  uint32_t depth = 0;
};

// Builds the observer seed transaction and the roster snapshot taken with it.
void MakeSeeds(
    const std::vector<SeedSpec>& specs,
    std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>* events,
    GuestExecutionCaptureHostCallRosterSnapshot* roster) {
  events->clear();
  *roster = {};
  uint64_t token = 1;
  for (const SeedSpec& spec : specs) {
    GuestExecutionCaptureThreadStateLifecycleEvent event;
    event.participant = spec.identity;
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kReady;
    events->push_back(event);
    for (uint32_t depth = 1; depth <= spec.depth; ++depth) {
      GuestExecutionCaptureActiveHostCall call;
      call.token = {token++};
      call.participant = spec.identity;
      call.function_address = 0x82000000;
      call.function_end_address = 0x820000FC;
      call.participant_depth = depth;
      roster->active_calls.push_back(call);
    }
  }
}

struct Harness {
  explicit Harness(GuestExecutionSessionAssemblerConfig config = MakeConfig())
      : config(config) {}

  bool Create(GuestExecutionSessionAssemblerPublisher* target = nullptr) {
    GuestExecutionSessionAssemblerDependencies dependencies;
    dependencies.clock = &clock;
    dependencies.state_provider = &states;
    dependencies.content_provider = &content;
    dependencies.publisher = target ? target : &publisher;
    assembler =
        GuestExecutionSessionAssembler::Create(config, dependencies, &error);
    return assembler != nullptr;
  }

  bool SeedOnly(const std::vector<SeedSpec>& specs) {
    std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> events;
    GuestExecutionCaptureHostCallRosterSnapshot roster;
    MakeSeeds(specs, &events, &roster);
    return assembler->SeedParticipants(events, roster);
  }

  void Seed(const std::vector<SeedSpec>& specs,
            GuestExecutionSessionAssemblerPublisher* target = nullptr) {
    REQUIRE(Create(target));
    REQUIRE(SeedOnly(specs));
  }

  // Seeds every participant outside guest code so recording starts at once.
  void StartOutside(
      std::vector<GuestExecutionCaptureParticipantIdentity> identities,
      GuestExecutionSessionAssemblerPublisher* target = nullptr) {
    std::vector<SeedSpec> specs;
    for (const GuestExecutionCaptureParticipantIdentity& identity :
         identities) {
      specs.push_back({identity, 0});
    }
    Seed(specs, target);
    REQUIRE(assembler->Arm(&error));
    REQUIRE(assembler->RequestStart(&error));
    REQUIRE(state() == State::kRecording);
  }

  Action Enter(const GuestExecutionCaptureParticipantIdentity& identity) {
    return assembler->OnOuterHostCallBegin(identity, 0x82000000, 0x820000FC,
                                           0x82001000);
  }

  Action Leave(const GuestExecutionCaptureParticipantIdentity& identity) {
    return assembler->OnOuterHostCallEnd(
        identity, GuestExecutionCaptureHostCallOutcome::kReturnedToHost);
  }

  // Records one root invocation segment whose recorder ticks track the clock.
  void RecordSegment(const GuestExecutionCaptureParticipantIdentity& identity,
                     uint64_t instructions = 10, uint8_t seed = 0x10) {
    const uint64_t start_tick = clock.now;
    REQUIRE(assembler->OnSegmentBegin(identity, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++clock.now;
    REQUIRE(assembler->OnInstructionCoverage(identity, instructions) ==
            Action::kContinue);
    ++clock.now;
    REQUIRE(assembler->OnSegmentEnd(
                identity, MakeSegmentEnd(status().next_segment_ordinal,
                                         start_tick, clock.now, seed)) ==
            Action::kContinue);
  }

  // One complete outer dispatch owning exactly one segment.
  void Dispatch(const GuestExecutionCaptureParticipantIdentity& identity,
                uint64_t instructions = 10, uint8_t seed = 0x10) {
    REQUIRE(Enter(identity) == Action::kContinue);
    ++clock.now;
    RecordSegment(identity, instructions, seed);
    REQUIRE(Leave(identity) == Action::kContinue);
  }

  GuestExecutionSessionAssemblerStatus status() const {
    return assembler->status();
  }
  State state() const { return status().state; }
  Rejection rejection() const { return status().rejection; }

  const GuestExecutionSessionBundle& PublishedBundle() {
    REQUIRE(assembler->Publish(&error));
    REQUIRE(error.empty());
    REQUIRE(state() == State::kComplete);
    REQUIRE(status().published);
    REQUIRE(publisher.bundles.size() == 1);
    std::string validation_error;
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(
        publisher.bundles[0].manifest, publisher.bundles[0].chunks,
        &validation_error));
    return publisher.bundles[0];
  }

  void RequireRejected(Rejection expected) {
    REQUIRE(state() == State::kRejected);
    REQUIRE(rejection() == expected);
    REQUIRE_FALSE(status().published);
    REQUIRE(publisher.calls == 0);
  }

  GuestExecutionSessionAssemblerConfig config;
  FakeClock clock;
  FakeStateProvider states;
  FakeContentProvider content;
  FakePublisher publisher;
  std::unique_ptr<GuestExecutionSessionAssembler> assembler;
  std::string error;
};

GuestExecutionSessionAssemblerExternalEvent ScalarEvent(
    GuestExecutionSessionEventKind kind, uint64_t value) {
  GuestExecutionSessionAssemblerExternalEvent event;
  event.kind = kind;
  if (kind == GuestExecutionSessionEventKind::kKernelExport ||
      kind == GuestExecutionSessionEventKind::kExternOrBuiltin) {
    event.guest_address = 0x82000080;
  }
  event.payload_kind =
      GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
  for (size_t i = 0; i < 8; ++i) {
    event.payload.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
  return event;
}

GuestExecutionSessionAssemblerMutation GuestMutation(uint64_t address,
                                                     size_t size,
                                                     uint8_t seed) {
  GuestExecutionSessionAssemblerMutation mutation;
  mutation.source = GuestExecutionSessionMutationSource::kActiveGuestThread;
  mutation.guest_address = address;
  mutation.byte_count = size;
  mutation.bytes = Bytes(size, seed);
  return mutation;
}

GuestExecutionSessionAssemblerMutation SinkMutation(
    GuestExecutionSessionMutationSource source, uint32_t sink, uint64_t address,
    size_t size, uint8_t seed) {
  GuestExecutionSessionAssemblerMutation mutation =
      GuestMutation(address, size, seed);
  mutation.source = source;
  mutation.external_sink_ordinal = sink;
  return mutation;
}

void RequireCreateFails(GuestExecutionSessionAssemblerConfig config,
                        const std::string& diagnostic) {
  Harness harness(config);
  REQUIRE_FALSE(harness.Create());
  REQUIRE(harness.error.find(diagnostic) != std::string::npos);
}

kernel::GuestSchedulerCheckpointBarrierSnapshot BridgeCheckpoint(
    uint64_t generation,
    std::initializer_list<GuestExecutionCaptureParticipantIdentity> identities =
        {kA}) {
  kernel::GuestSchedulerCheckpointBarrierSnapshot checkpoint;
  checkpoint.generation = generation;
  checkpoint.active = true;
  checkpoint.quiesced = true;
  checkpoint.dispatch_cpu_mask = 1;
  checkpoint.quiesced_cpu_mask = 1;
  checkpoint.roster_scope =
      kernel::GuestSchedulerCheckpointRosterScope::kSchedulerOwned;
  checkpoint.release_policy = kernel::GuestSchedulerCheckpointReleasePolicy::
      kRunningSafepointsRequeueAtHead;
  for (const GuestExecutionCaptureParticipantIdentity& identity : identities) {
    kernel::GuestSchedulerCheckpointParticipant participant;
    participant.thread_id = identity.guest_thread_id;
    participant.guest_pc = 0x82000040;
    participant.cpu = 0;
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kReady;
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint;
    participant.restorable = true;
    checkpoint.participants.push_back(participant);
  }
  return checkpoint;
}

kernel::GuestSchedulerCaptureEvent BridgeSchedulerEvent(
    uint64_t sequence, kernel::GuestSchedulerCaptureEventKind kind,
    GuestExecutionCaptureParticipantIdentity identity = kA) {
  kernel::GuestSchedulerCaptureEvent event;
  event.sequence = sequence;
  event.capture_instance_id = identity.capture_instance_id;
  event.guest_thread_id = identity.guest_thread_id;
  event.kind = kind;
  event.cpu = 0;
  event.priority = 8;
  if (kind == kernel::GuestSchedulerCaptureEventKind::kEnqueueReady ||
      kind == kernel::GuestSchedulerCaptureEventKind::kReready) {
    event.target_cpu = 0;
  } else if (kind == kernel::GuestSchedulerCaptureEventKind::kMigrate) {
    event.target_cpu = 1;
  }
  if (kind == kernel::GuestSchedulerCaptureEventKind::kYield) {
    event.flags = kernel::kGuestSchedulerCaptureFlagQuantumEnd;
  } else if (kind == kernel::GuestSchedulerCaptureEventKind::kSafepoint) {
    event.guest_pc = 0x82000040;
  } else if (kind == kernel::GuestSchedulerCaptureEventKind::kBlock ||
             kind == kernel::GuestSchedulerCaptureEventKind::kReready) {
    event.value = 1;
    event.wait.handle_count = 1;
    event.wait.handles[0] = 0x20;
    event.wait.wait_epoch = 7;
    event.wait.observed_wait_epoch = 7;
    event.wait.signal_epochs_before[0] = 7;
    event.wait.signal_epochs_observed[0] = 7;
    event.wait.flags = kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
    if (kind == kernel::GuestSchedulerCaptureEventKind::kBlock) {
      event.flags = kernel::kGuestSchedulerCaptureFlagGated |
                    kernel::kGuestSchedulerCaptureFlagInterruptible;
      event.wait.flags |= kernel::kGuestSchedulerCaptureWaitFlagGated;
    }
  }
  return event;
}

kernel::GuestSchedulerCaptureEvent BridgeBlockEvent(
    uint64_t sequence, kernel::GuestSchedulerCaptureWaitKind wait_kind,
    bool external_has_deadline = true) {
  kernel::GuestSchedulerCaptureEvent event = BridgeSchedulerEvent(
      sequence, kernel::GuestSchedulerCaptureEventKind::kBlock);
  event.value = static_cast<uint8_t>(wait_kind);
  event.flags = 0;
  event.wait = {};
  event.wait.observed_uptime_ms = 50;
  bool interruptible = true;
  switch (wait_kind) {
    case kernel::GuestSchedulerCaptureWaitKind::kSingle:
      event.wait.handle_count = 1;
      event.wait.handles[0] = 0x20;
      event.flags = kernel::kGuestSchedulerCaptureFlagGated;
      event.wait.flags = kernel::kGuestSchedulerCaptureWaitFlagGated;
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kMultiAny:
    case kernel::GuestSchedulerCaptureWaitKind::kMultiAll:
      event.wait.handle_count = 2;
      event.wait.handles[0] = 0x20;
      event.wait.handles[1] = 0x24;
      event.flags = kernel::kGuestSchedulerCaptureFlagGated;
      event.wait.flags = kernel::kGuestSchedulerCaptureWaitFlagGated;
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kDelay:
      event.wait.deadline_ms = 100;
      event.flags = kernel::kGuestSchedulerCaptureFlagGated |
                    kernel::kGuestSchedulerCaptureFlagHasDeadline;
      event.wait.flags = kernel::kGuestSchedulerCaptureWaitFlagGated;
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kFence:
    case kernel::GuestSchedulerCaptureWaitKind::kIoOffload:
      interruptible = false;
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff:
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kIoCompletion:
      event.wait.handle_count = 1;
      event.wait.handles[0] = 0x30;
      if (external_has_deadline) {
        event.wait.deadline_ms = 100;
        event.flags = kernel::kGuestSchedulerCaptureFlagHasDeadline;
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kSocketIo:
      event.wait.handle_count = 1;
      event.wait.handles[0] = 0x34;
      if (external_has_deadline) {
        event.wait.deadline_ms = 100;
        event.flags = kernel::kGuestSchedulerCaptureFlagHasDeadline;
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kNone:
    default:
      break;
  }
  if (interruptible) {
    event.flags |= kernel::kGuestSchedulerCaptureFlagInterruptible;
    event.wait.flags |= kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
  }
  return event;
}

std::vector<GuestExecutionContinuousEvent> DecodeBridgeOverlay(
    const GuestExecutionSessionBundle& bundle) {
  std::vector<GuestExecutionContinuousEvent> result;
  for (size_t index = 0; index < bundle.manifest.chunks.size(); ++index) {
    if (bundle.manifest.chunks[index].kind !=
        GuestExecutionSessionChunkKind::kContinuousEvents) {
      continue;
    }
    std::vector<GuestExecutionContinuousEvent> chunk;
    std::string error;
    REQUIRE(GuestExecutionContinuousEventCodec::Decode(bundle.chunks[index],
                                                       &chunk, &error));
    result.insert(result.end(), chunk.begin(), chunk.end());
  }
  return result;
}

}  // namespace

TEST_CASE("session assembler validates configuration and dependencies",
          "[guest-execution-session-assembler]") {
  {
    Harness harness;
    GuestExecutionSessionAssemblerDependencies dependencies;
    dependencies.clock = &harness.clock;
    dependencies.state_provider = &harness.states;
    dependencies.content_provider = &harness.content;
    std::string error;
    REQUIRE_FALSE(GuestExecutionSessionAssembler::Create(harness.config,
                                                         dependencies, &error));
    REQUIRE(error.find("dependencies") != std::string::npos);
  }
  {
    Harness harness;
    REQUIRE(harness.Create());
    REQUIRE(harness.state() == State::kIdle);
  }

  GuestExecutionSessionAssemblerConfig config = MakeConfig();
  config.session_epoch = 0;
  RequireCreateFails(config, "epoch");

  config = MakeConfig();
  config.capture_tick_frequency = 0;
  RequireCreateFails(config, "frequency");

  config = MakeConfig();
  config.maximum_stop_tail_ticks = 0;
  RequireCreateFails(config, "rendezvous maxima");

  config = MakeConfig();
  config.maximum_start_rendezvous_ticks = 0;
  RequireCreateFails(config, "rendezvous maxima");

  config = MakeConfig();
  config.title_identity_sha256 = {};
  RequireCreateFails(config, "SHA-256");

  config = MakeConfig();
  config.maximum_events_per_chunk = 0;
  RequireCreateFails(config, "events per chunk");

  config = MakeConfig();
  config.maximum_events_per_chunk =
      config.bundle_limits.session.maximum_events_per_chunk + 1;
  RequireCreateFails(config, "events per chunk");

  config = MakeConfig();
  config.bundle_limits.session.maximum_chunk_bytes = 1024;
  RequireCreateFails(config, "events per chunk");

  config = MakeConfig();
  config.boundary.value = 1;
  RequireCreateFails(config, "manual");

  config = MakeConfig(GuestExecutionSessionBoundaryKind::kGuestMarkerCount, 2);
  RequireCreateFails(config, "source");

  config = MakeConfig(
      GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 0);
  RequireCreateFails(config, "duration");

  // At 1 GHz UINT64_MAX ns is exactly UINT64_MAX ticks, which the reel bounds.
  config =
      MakeConfig(GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds,
                 UINT64_MAX);
  RequireCreateFails(config, "hard bound");

  config =
      MakeConfig(GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds,
                 UINT64_MAX);
  config.capture_tick_frequency = 3000000000;
  RequireCreateFails(config, "duration");

  config = MakeConfig(
      GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 1001);
  RequireCreateFails(config, "hard bound");

  config = MakeConfig();
  config.boundary.kind = static_cast<GuestExecutionSessionBoundaryKind>(99);
  RequireCreateFails(config, "kind");

  config = MakeContinuousConfig();
  config.boundary.kind = GuestExecutionSessionBoundaryKind::kSegmentCount;
  config.boundary.value = 1;
  RequireCreateFails(config, "segment-count");

  config = MakeConfig();
  config.coverage_mode = static_cast<GuestExecutionReelCoverageMode>(99);
  RequireCreateFails(config, "coverage mode");
}

TEST_CASE("session assembler seeds a fixed roster and freezes it at arming",
          "[guest-execution-session-assembler]") {
  SECTION("invalid seed transactions are rejected without changing state") {
    Harness harness;
    REQUIRE(harness.Create());
    REQUIRE_FALSE(harness.SeedOnly({}));
    const std::vector<SeedSpec> duplicate_thread = {
        {kA, 0}, {{0x103, kA.guest_thread_id}, 0}};
    REQUIRE_FALSE(harness.SeedOnly(duplicate_thread));
    const std::vector<SeedSpec> duplicate_instance = {
        {kA, 0}, {{kA.capture_instance_id, 11}, 0}};
    REQUIRE_FALSE(harness.SeedOnly(duplicate_instance));
    const std::vector<SeedSpec> zero_instance = {{kA, 0}, {{0, 11}, 0}};
    REQUIRE_FALSE(harness.SeedOnly(zero_instance));

    std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> events;
    GuestExecutionCaptureHostCallRosterSnapshot roster;
    MakeSeeds({{kA, 0}}, &events, &roster);
    events[0].state = GuestExecutionCaptureThreadStateLifecycleState::kPending;
    REQUIRE_FALSE(harness.assembler->SeedParticipants(events, roster));

    MakeSeeds({{kA, 0}, {kB, 1}}, &events, &roster);
    roster.rejection =
        GuestExecutionCaptureHostCallRosterRejection::kCounterOverflow;
    REQUIRE_FALSE(harness.assembler->SeedParticipants(events, roster));

    // A roster call owned by a thread outside the seed is a lost participant.
    MakeSeeds({{kA, 0}, {kB, 1}}, &events, &roster);
    roster.active_calls[0].participant = kC;
    REQUIRE_FALSE(harness.assembler->SeedParticipants(events, roster));

    MakeSeeds({{kA, 0}, {kB, 2}}, &events, &roster);
    roster.active_calls.erase(roster.active_calls.begin());
    REQUIRE_FALSE(harness.assembler->SeedParticipants(events, roster));
    REQUIRE(harness.state() == State::kIdle);
    REQUIRE(harness.status().participants.empty());

    const std::vector<SeedSpec> valid = {{kA, 0}, {kB, 2}};
    REQUIRE(harness.SeedOnly(valid));
    REQUIRE(harness.status().participants.size() == 2);
    REQUIRE(harness.status().participants[1].host_call_depth == 2);
    const std::vector<SeedSpec> again = {{kA, 0}};
    REQUIRE_FALSE(harness.SeedOnly(again));
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("idle lifecycle events edit the roster and arming freezes it") {
    Harness harness;
    harness.Seed({{kC, 0}});
    GuestExecutionCaptureThreadStateLifecycleEvent event;
    event.participant = kA;
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kPending;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kContinue);
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kReady;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kContinue);
    event.participant = kB;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kContinue);
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kDestroying;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kContinue);
    REQUIRE(harness.status().participants.size() == 2);

    uint32_t sink = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &sink));
    REQUIRE(sink == 0);
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.state() == State::kArming);
    // Ordinals follow capture instance ids, not seed or lifecycle order.
    REQUIRE(harness.status().participants[0].identity == kA);
    REQUIRE(harness.status().participants[0].ordinal == 0);
    REQUIRE(harness.status().participants[1].identity == kC);
    REQUIRE(harness.status().participants[1].ordinal == 1);

    REQUIRE_FALSE(harness.assembler->RegisterExternalSink("dma", &sink));
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("duplicate registration rejects while idle") {
    Harness harness;
    harness.Seed({{kA, 0}});
    GuestExecutionCaptureThreadStateLifecycleEvent event;
    event.participant = kA;
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kReady;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kLifecycleChanged);
  }

  SECTION("lifecycle changes after arming reject the session") {
    Harness harness;
    harness.Seed({{kA, 0}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    GuestExecutionCaptureThreadStateLifecycleEvent event;
    event.participant = kB;
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kReady;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kLifecycleChanged);
  }

  SECTION("lifecycle changes during recording reject the session") {
    Harness harness;
    harness.StartOutside({kA});
    GuestExecutionCaptureThreadStateLifecycleEvent event;
    event.participant = kA;
    event.state = GuestExecutionCaptureThreadStateLifecycleState::kDestroying;
    REQUIRE(harness.assembler->OnParticipantLifecycle(event) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kLifecycleChanged);
  }

  SECTION("arming requires a participant") {
    Harness harness;
    REQUIRE(harness.Create());
    REQUIRE_FALSE(harness.assembler->Arm(&harness.error));
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("arming requires a registered PM4 marker sink when configured") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.pm4_marker_sink_ordinal = 0;
    Harness harness(config);
    harness.Seed({{kA, 0}});
    REQUIRE_FALSE(harness.assembler->Arm(&harness.error));
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("unknown and mismatched identities reject") {
    Harness harness;
    harness.StartOutside({kA});
    const GuestExecutionCaptureParticipantIdentity mismatched = {
        kA.capture_instance_id, 99};
    REQUIRE(harness.Enter(mismatched) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }
}

TEST_CASE("session assembler records one manual window with nested calls",
          "[guest-execution-session-assembler]") {
  Harness harness;
  harness.StartOutside({kA});
  REQUIRE(harness.status().encoded_chunk_count == 0);
  REQUIRE(harness.status().capture_start_tick == kStartTick);
  // Only participant state is captured at start; pages wait for the stop.
  REQUIRE(harness.states.calls == 1);
  REQUIRE(harness.content.calls == 0);

  REQUIRE(harness.Enter(kA) == Action::kContinue);
  REQUIRE(harness.status().last_event_sequence == 1);
  // Nested host-to-guest dispatches are depth only, never session events.
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  REQUIRE(harness.status().participants[0].host_call_depth == 3);
  REQUIRE(harness.Leave(kA) == Action::kContinue);
  REQUIRE(harness.Leave(kA) == Action::kContinue);
  REQUIRE(harness.status().last_event_sequence == 1);

  ++harness.clock.now;
  REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
          Action::kContinue);
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  REQUIRE(harness.assembler->OnExternalEvent(
              kA, ScalarEvent(GuestExecutionSessionEventKind::kKernelExport,
                              0x1122334455667788ull)) == Action::kContinue);
  REQUIRE(harness.assembler->OnGuestMarker(
              std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
              0x99) == Action::kContinue);
  REQUIRE(harness.assembler->OnMemoryMutation(
              kA, GuestMutation(0x2000, 4, 0x30)) == Action::kContinue);
  harness.clock.now += 5;
  REQUIRE(harness.assembler->OnSegmentEnd(
              kA, MakeSegmentEnd(0, kStartTick + 1, harness.clock.now)) ==
          Action::kContinue);
  REQUIRE(harness.status().last_event_sequence == 7);
  REQUIRE(harness.status().accepted_segment_count == 1);
  REQUIRE(harness.status().next_segment_ordinal == 1);

  harness.clock.now += 10;
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.state() == State::kStopRequested);
  REQUIRE(harness.status().stop_request_event_sequence == 8);
  REQUIRE(harness.status().stop_request_tick == harness.clock.now);
  REQUIRE_FALSE(harness.status().participants[0].held);
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 2) == Action::kContinue);
  harness.states.generation = 1;
  harness.clock.now += 20;
  REQUIRE(harness.Leave(kA) == Action::kHold);
  REQUIRE(harness.state() == State::kPublishing);
  REQUIRE(harness.status().participants[0].held);
  REQUIRE(harness.status().participants[0].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn);
  REQUIRE(harness.status().capture_end_tick == harness.clock.now);
  // Nothing is encoded and no provider runs on the arriving thread.
  REQUIRE(harness.status().staged_event_count == 11);
  REQUIRE(harness.status().encoded_chunk_count == 0);
  REQUIRE(harness.states.calls == 1);
  REQUIRE(harness.content.calls == 0);
  REQUIRE(harness.publisher.calls == 0);
  // Held participants may not resume before publication completes.
  REQUIRE(harness.Enter(kA) == Action::kHold);
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.assembler->Poll() == Action::kHold);

  const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
  REQUIRE(harness.states.calls == 2);
  REQUIRE(harness.content.calls == 2);
  REQUIRE(harness.content.corpus_calls == 1);
  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  REQUIRE(manifest.session_epoch == kEpoch);
  REQUIRE(manifest.boundary.kind == GuestExecutionSessionBoundaryKind::kManual);
  REQUIRE(manifest.stop_reason ==
          GuestExecutionSessionStopReason::kManualRequest);
  REQUIRE(manifest.first_event_sequence == 1);
  REQUIRE(manifest.last_event_sequence == 11);
  REQUIRE(manifest.stop_request_event_sequence == 8);
  REQUIRE(manifest.accepted_event_count == 11);
  REQUIRE(manifest.accepted_segment_count == 1);
  REQUIRE(manifest.stop_request_accepted_segment_count == 1);
  REQUIRE(manifest.stop_request_guest_instruction_count == 10);
  REQUIRE(manifest.stop_request_matching_guest_marker_count == 0);
  REQUIRE(manifest.capture_start_tick == kStartTick);
  REQUIRE(manifest.stop_request_tick == kStartTick + 16);
  REQUIRE(manifest.capture_end_tick == kStartTick + 36);
  REQUIRE(manifest.participants.size() == 1);
  const GuestExecutionSessionParticipant& participant =
      manifest.participants[0];
  REQUIRE(participant.guest_thread_id == kA.guest_thread_id);
  REQUIRE(participant.capture_instance_id == kA.capture_instance_id);
  REQUIRE(participant.initial_outer_call_state ==
          GuestExecutionSessionInitialOuterCallState::kOutside);
  REQUIRE(participant.boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn);
  REQUIRE(participant.first_event_sequence == 1);
  REQUIRE(participant.last_event_sequence == 10);
  REQUIRE(participant.held_after_event_sequence == 10);
  REQUIRE(participant.initial_state_size == 64);
  REQUIRE(manifest.segments.size() == 1);
  REQUIRE(manifest.segments[0].thread_ordinal == 0);
  REQUIRE(manifest.segments[0].first_event_sequence == 2);
  REQUIRE(manifest.segments[0].last_event_sequence == 7);
  REQUIRE(manifest.segments[0].entry_guest_address == 0x82000000);
  REQUIRE(manifest.segments[0].end_guest_address == 0x820000FC);
  REQUIRE(manifest.segments[0].code_corpus_sha256 ==
          GuestExecutionSessionCodec::HashBytes(harness.content.corpus));
  REQUIRE(manifest.chunks.size() == 4);
  REQUIRE(manifest.chunks[0].kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
  REQUIRE(manifest.chunks[1].kind == GuestExecutionSessionChunkKind::kEvents);
  REQUIRE(manifest.chunks[1].record_count == 8);
  REQUIRE(manifest.chunks[2].kind == GuestExecutionSessionChunkKind::kEvents);
  REQUIRE(manifest.chunks[2].record_count == 3);
  REQUIRE(manifest.chunks[3].kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
  REQUIRE(manifest.chunks[3].first_event_sequence == 11);
  // Initial and final state, page, code, scalar, mutation, corpus, segment.
  REQUIRE(bundle.content_blobs.size() == 8);

  GuestExecutionSessionCheckpointChunk initial;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(bundle.chunks[0],
                                                            &initial));
  REQUIRE(initial.checkpoint.thread_states[0].sha256 ==
          participant.initial_state_sha256);
  REQUIRE(initial.checkpoint.content.size() == 2);
  REQUIRE(initial.checkpoint.content[0].guest_address == 0x1000);

  GuestExecutionSessionEventChunk events;
  REQUIRE(
      GuestExecutionSessionCodec::DecodeEventChunk(bundle.chunks[1], &events));
  REQUIRE(events.events[0].kind ==
          GuestExecutionSessionEventKind::kOuterHostCallBegin);
  REQUIRE(events.events[1].kind ==
          GuestExecutionSessionEventKind::kSegmentBegin);
  REQUIRE(events.events[2].guest_instruction_delta == 10);
  REQUIRE(events.events[3].payload_size == 8);
  REQUIRE(events.events[4].thread_ordinal == kGuestExecutionSessionNoThread);
  REQUIRE(events.events[5].kind ==
          GuestExecutionSessionEventKind::kMemoryMutation);
  REQUIRE(events.events[5].payload_size == 4);
  REQUIRE(events.events[6].kind == GuestExecutionSessionEventKind::kSegmentEnd);
  REQUIRE(events.events[7].kind ==
          GuestExecutionSessionEventKind::kBoundaryRequest);

  // A completed session ignores later work and never publishes twice.
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
  REQUIRE(harness.publisher.calls == 1);
}

TEST_CASE("session assembler publishes continuous zero-segment coverage",
          "[guest-execution-session-assembler]") {
  Harness harness(MakeContinuousConfig());
  harness.StartOutside({kA});
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  ++harness.clock.now;
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
  REQUIRE(harness.state() == State::kPublishing);

  const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  REQUIRE(manifest.segments.empty());
  REQUIRE(manifest.accepted_segment_count == 0);
  REQUIRE(manifest.stop_request_accepted_segment_count == 0);
  REQUIRE(manifest.stop_request_guest_instruction_count == 10);
  REQUIRE(manifest.accepted_event_count == 5);
  REQUIRE(manifest.participants.size() == 1);
  REQUIRE(manifest.participants[0].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint);
  REQUIRE(manifest.chunks.front().kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
  REQUIRE(manifest.chunks.front().first_event_sequence == 0);
  REQUIRE(manifest.chunks[1].kind ==
          GuestExecutionSessionChunkKind::kCodeCorpus);
  REQUIRE(manifest.chunks.back().kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
  REQUIRE(manifest.chunks.back().first_event_sequence ==
          manifest.last_event_sequence);
  REQUIRE(harness.content.corpus_calls == 1);
  GuestExecutionSessionCodeCorpusChunk corpus_reference;
  REQUIRE(GuestExecutionSessionCodec::DecodeCodeCorpusChunk(bundle.chunks[1],
                                                            &corpus_reference));
  REQUIRE(corpus_reference.code_corpus_sha256 ==
          GuestExecutionSessionCodec::HashBytes(harness.content.corpus));

  std::string validation_error;
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &validation_error));
  REQUIRE(validation_error.empty());
  std::vector<uint8_t> encoded_manifest;
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      manifest, &encoded_manifest, &validation_error));
  GuestExecutionSessionManifest decoded_manifest;
  REQUIRE(GuestExecutionSessionCodec::DecodeManifest(
      encoded_manifest, &decoded_manifest, &validation_error));
  REQUIRE(decoded_manifest == manifest);

  for (size_t i = 0; i < manifest.chunks.size(); ++i) {
    if (manifest.chunks[i].kind != GuestExecutionSessionChunkKind::kEvents) {
      continue;
    }
    GuestExecutionSessionEventChunk events;
    REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(bundle.chunks[i],
                                                         &events));
    for (const GuestExecutionSessionEvent& event : events.events) {
      REQUIRE(event.kind != GuestExecutionSessionEventKind::kSegmentBegin);
      REQUIRE(event.kind != GuestExecutionSessionEventKind::kSegmentEnd);
    }
  }
}

TEST_CASE("continuous session accepts a segment-free outer return",
          "[guest-execution-session-assembler]") {
  Harness harness(MakeContinuousConfig());
  harness.StartOutside({kA});
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.Leave(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.state() == State::kPublishing);
  REQUIRE(harness.PublishedBundle().manifest.segments.empty());
  REQUIRE(harness.status().coverage_mode ==
          GuestExecutionReelCoverageMode::kContinuousInstructions);
}

TEST_CASE("continuous session accepts nested segment-free host calls",
          "[guest-execution-session-assembler]") {
  Harness harness(MakeContinuousConfig());
  harness.StartOutside({kA});
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  REQUIRE(harness.Leave(kA) == Action::kContinue);
  REQUIRE(harness.Leave(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.PublishedBundle().manifest.segments.empty());
}

TEST_CASE("continuous session holds a segment-free stop-tail return",
          "[guest-execution-session-assembler]") {
  Harness harness(MakeContinuousConfig());
  harness.StartOutside({kA});
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  ++harness.clock.now;
  REQUIRE(harness.Leave(kA) == Action::kHold);
  const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
  REQUIRE(bundle.manifest.participants[0].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn);
  REQUIRE(bundle.manifest.segments.empty());
}

TEST_CASE("continuous session stops on an exact instruction boundary",
          "[guest-execution-session-assembler]") {
  Harness harness(MakeContinuousConfig(
      GuestExecutionSessionBoundaryKind::kGuestInstructionCount, 10));
  harness.StartOutside({kA});
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  REQUIRE(harness.state() == State::kStopRequested);
  ++harness.clock.now;
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
  const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
  REQUIRE(bundle.manifest.stop_reason ==
          GuestExecutionSessionStopReason::kRequestedBoundary);
  REQUIRE(bundle.manifest.segments.empty());
}

TEST_CASE("continuous session assembler rejects missing participant progress",
          "[guest-execution-session-assembler]") {
  SECTION("only coordinator boundary events") {
    Harness harness(MakeContinuousConfig());
    harness.StartOutside({kA});
    REQUIRE(harness.assembler->RequestStop() == Action::kReject);
    harness.RequireRejected(Rejection::kReelRejected);
  }

  SECTION("participant control events are not instruction progress") {
    Harness harness(MakeContinuousConfig());
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.Leave(kA) == Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kReject);
    harness.RequireRejected(Rejection::kReelRejected);
  }

  SECTION("legacy segment begin is illegal") {
    Harness harness(MakeContinuousConfig());
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("legacy segment end is illegal") {
    Harness harness(MakeContinuousConfig());
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentEnd(kA, MakeSegmentEnd(0, 100, 101)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("pre-arm legacy segment begin is illegal") {
    Harness harness(MakeContinuousConfig());
    harness.Seed({{kA, 0}});
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("pre-arm legacy segment end is illegal") {
    Harness harness(MakeContinuousConfig());
    harness.Seed({{kA, 0}});
    REQUIRE(harness.assembler->OnSegmentEnd(kA, MakeSegmentEnd(0, 100, 101)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("stop-tail legacy segment begin is illegal") {
    Harness harness(MakeContinuousConfig());
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("stop-tail legacy segment end is illegal") {
    Harness harness(MakeContinuousConfig());
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->OnSegmentEnd(kA, MakeSegmentEnd(0, 100, 101)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }
}

TEST_CASE("continuous session publication requires exact code closure",
          "[guest-execution-session-assembler]") {
  auto reach_publication = [](Harness* harness) {
    harness->StartOutside({kA});
    REQUIRE(harness->Enter(kA) == Action::kContinue);
    REQUIRE(harness->assembler->OnInstructionCoverage(kA, 10) ==
            Action::kContinue);
    REQUIRE(harness->assembler->RequestStop() == Action::kHold);
    REQUIRE(harness->assembler->ArriveAtSafepoint(kA) == Action::kHold);
    REQUIRE(harness->state() == State::kPublishing);
  };

  SECTION("initial checkpoint without guest code rejects") {
    Harness harness(MakeContinuousConfig());
    harness.content.entries.erase(
        std::remove_if(
            harness.content.entries.begin(), harness.content.entries.end(),
            [](const GuestExecutionSessionAssemblerContent& entry) {
              return entry.kind == GuestExecutionSessionContentKind::kGuestCode;
            }),
        harness.content.entries.end());
    reach_publication(&harness);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kEncodingFailure);
  }

  SECTION("corpus provider failure rejects") {
    Harness harness(MakeContinuousConfig());
    harness.content.corpus_fail = true;
    reach_publication(&harness);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kContentFailure);
  }

  SECTION("checkpoint code differing from the corpus rejects") {
    Harness harness(MakeContinuousConfig());
    for (GuestExecutionSessionAssemblerContent& entry :
         harness.content.entries) {
      if (entry.kind == GuestExecutionSessionContentKind::kGuestCode) {
        entry.bytes[0] ^= 0x80;
      }
    }
    reach_publication(&harness);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kEncodingFailure);
  }

  SECTION("malformed corpus bytes reject before publication") {
    Harness harness(MakeContinuousConfig());
    harness.content.corpus = Bytes(128, 0x10);
    reach_publication(&harness);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kContentFailure);
  }
}
TEST_CASE("session assembler holds three participants through mixed arrivals",
          "[guest-execution-session-assembler]") {
  Harness harness;
  harness.Seed({{kA, 0}, {kB, 1}, {kC, 2}});
  uint32_t gpu = 0;
  REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
  REQUIRE(harness.assembler->Arm(&harness.error));
  REQUIRE(harness.assembler->RequestStart(&harness.error));
  REQUIRE(harness.state() == State::kStartRendezvous);
  REQUIRE(harness.status().participants[0].arrived);
  REQUIRE_FALSE(harness.status().participants[1].arrived);

  // Outside participants are parked before their next outer dispatch.
  REQUIRE(harness.Enter(kA) == Action::kHold);
  REQUIRE(harness.status().participants[0].host_call_depth == 0);
  REQUIRE(harness.assembler->OnInstructionCoverage(kB, 3) == Action::kContinue);
  REQUIRE(harness.assembler->ArriveAtSafepoint(kB) == Action::kHold);
  REQUIRE(harness.Leave(kC) == Action::kContinue);
  REQUIRE(harness.Leave(kC) == Action::kHold);
  REQUIRE(harness.state() == State::kStartRendezvous);
  REQUIRE(harness.assembler->Poll() == Action::kHold);
  REQUIRE(harness.states.calls == 0);
  REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kContinue);
  REQUIRE(harness.state() == State::kRecording);
  REQUIRE(harness.states.calls == 3);
  REQUIRE_FALSE(harness.status().external_sinks[0].held);

  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  harness.RecordSegment(kA);
  REQUIRE(harness.Enter(kC) == Action::kContinue);
  harness.RecordSegment(kC, 4, 0x20);
  REQUIRE(harness.Leave(kB) == Action::kContinue);
  REQUIRE(harness.assembler->OnMemoryMutation(
              std::nullopt,
              SinkMutation(GuestExecutionSessionMutationSource::kGpu, gpu,
                           0x3000, 16, 0x40)) == Action::kContinue);
  REQUIRE(harness.status().last_event_sequence == 10);

  harness.clock.now += 10;
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.state() == State::kStopRequested);
  REQUIRE(harness.status().participants[1].held);
  REQUIRE(harness.status().participants[1].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside);
  REQUIRE(harness.Enter(kB) == Action::kHold);
  REQUIRE(harness.assembler->OnInstructionCoverage(kC, 1) == Action::kContinue);
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
  REQUIRE(harness.state() == State::kStopRequested);
  REQUIRE(harness.Leave(kC) == Action::kHold);
  REQUIRE(harness.state() == State::kStopRendezvous);
  REQUIRE(harness.assembler->OnGuestMarker(
              std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
              0x77) == Action::kContinue);
  harness.states.generation = 1;
  REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kHold);
  REQUIRE(harness.state() == State::kPublishing);

  const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  REQUIRE(manifest.participants.size() == 3);
  REQUIRE(manifest.participants[0].initial_outer_call_state ==
          GuestExecutionSessionInitialOuterCallState::kOutside);
  REQUIRE(manifest.participants[0].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint);
  REQUIRE(manifest.participants[0].held_after_event_sequence == 13);
  REQUIRE(manifest.participants[1].initial_outer_call_state ==
          GuestExecutionSessionInitialOuterCallState::kActive);
  REQUIRE(manifest.participants[1].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside);
  REQUIRE(manifest.participants[1].first_event_sequence == 9);
  REQUIRE(manifest.participants[1].last_event_sequence == 9);
  REQUIRE(manifest.participants[1].held_after_event_sequence == 11);
  REQUIRE(manifest.participants[2].initial_outer_call_state ==
          GuestExecutionSessionInitialOuterCallState::kOutside);
  REQUIRE(manifest.participants[2].boundary_arrival_kind ==
          GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn);
  REQUIRE(manifest.participants[2].held_after_event_sequence == 14);
  REQUIRE(manifest.last_event_sequence == 16);
  REQUIRE(manifest.stop_request_event_sequence == 11);
  // Coverage reported before recording began is not session work.
  REQUIRE(manifest.stop_request_guest_instruction_count == 14);
  REQUIRE(manifest.segments.size() == 2);
  REQUIRE(manifest.segments[0].thread_ordinal == 0);
  REQUIRE(manifest.segments[1].thread_ordinal == 2);
  REQUIRE(manifest.segments[1].first_event_sequence == 6);
}

TEST_CASE("session assembler stops exactly at event-driven boundaries",
          "[guest-execution-session-assembler]") {
  SECTION("instruction boundary stops on the exact delta") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kGuestInstructionCount, 20));
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA, 15);
    REQUIRE(harness.state() == State::kRecording);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 5) ==
            Action::kContinue);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.status().stop_request_event_sequence == 6);
    REQUIRE(harness.status().stop_reason ==
            GuestExecutionReelStopReason::kGuestInstructionCount);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_reason ==
            GuestExecutionSessionStopReason::kRequestedBoundary);
    REQUIRE(manifest.stop_request_guest_instruction_count == 20);
  }

  SECTION("instruction boundary overshoot rejects instead of truncating") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kGuestInstructionCount, 20));
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA, 15);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 6) == Action::kReject);
    harness.RequireRejected(Rejection::kHardLimit);
  }

  SECTION("guest-marker boundary counts only matching markers") {
    GuestExecutionSessionAssemblerConfig config =
        MakeConfig(GuestExecutionSessionBoundaryKind::kGuestMarkerCount, 2);
    config.boundary.marker_source = GuestExecutionSessionMarkerSource::kKernel;
    config.boundary.marker_identity = 0x55;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->OnGuestMarker(
                kA, GuestExecutionSessionMarkerSource::kKernel, 0x55) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnGuestMarker(
                kA, GuestExecutionSessionMarkerSource::kKernel, 0x56) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnGuestMarker(
                std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
                0x55) == Action::kContinue);
    REQUIRE(harness.state() == State::kRecording);
    REQUIRE(harness.assembler->OnGuestMarker(
                kA, GuestExecutionSessionMarkerSource::kKernel, 0x55) ==
            Action::kContinue);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_request_matching_guest_marker_count == 2);
    REQUIRE(manifest.stop_request_event_sequence == 9);
  }

  SECTION("PM4 swap marker boundary stops on the matching no-thread marker") {
    GuestExecutionSessionAssemblerConfig config =
        MakeConfig(GuestExecutionSessionBoundaryKind::kGuestMarkerCount, 2);
    config.boundary.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
    config.boundary.marker_identity = 0x99;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->OnGuestMarker(
                std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
                0x99) == Action::kContinue);
    REQUIRE(harness.assembler->OnGuestMarker(
                std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
                0x98) == Action::kContinue);
    REQUIRE(harness.state() == State::kRecording);
    REQUIRE(harness.assembler->OnGuestMarker(
                std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
                0x99) == Action::kContinue);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.status().stop_request_event_sequence == 8);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_request_matching_guest_marker_count == 2);
    REQUIRE(manifest.stop_reason ==
            GuestExecutionSessionStopReason::kRequestedBoundary);
  }

  SECTION("segment boundary stops after the final root of several") {
    Harness harness(
        MakeConfig(GuestExecutionSessionBoundaryKind::kSegmentCount, 2));
    harness.StartOutside({kA});
    harness.Dispatch(kA, 10, 0x10);
    REQUIRE(harness.state() == State::kRecording);
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA, 12, 0x20);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.status().stop_reason ==
            GuestExecutionReelStopReason::kSegmentCount);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
    REQUIRE(bundle.manifest.segments.size() == 2);
    REQUIRE(bundle.manifest.segments[1].ordinal == 1);
    REQUIRE(bundle.manifest.segments[1].first_event_sequence == 7);
    REQUIRE(bundle.manifest.stop_request_accepted_segment_count == 2);
    REQUIRE(bundle.manifest.stop_request_event_sequence == 10);
  }

  SECTION("an outer dispatch owns exactly one segment") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("an outer dispatch returning without its segment rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
    REQUIRE(harness.Leave(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("a segment inside a dispatch that predates the start rejects") {
    Harness harness;
    harness.Seed({{kA, 1}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kContinue);
    REQUIRE(harness.state() == State::kRecording);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("segments never overlap and only end where they began") {
    Harness harness;
    harness.StartOutside({kA, kB});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.Enter(kB) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kB, 0x83000000, 0x830000FC) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("out-of-order segment ordinals reject") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(1, kStartTick, harness.clock.now)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kRejectedSegment);
  }

  SECTION("a segment ending in the stop tail is retained") {
    Harness harness;
    harness.StartOutside({kA});
    harness.Dispatch(kA);
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    const uint64_t start_tick = harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 3) ==
            Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.status().stop_request_event_sequence == 9);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(1, start_tick, harness.clock.now, 0x20)) ==
            Action::kContinue);
    REQUIRE(harness.status().accepted_segment_count == 2);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.segments.size() == 2);
    REQUIRE(manifest.segments[1].first_event_sequence == 7);
    REQUIRE(manifest.segments[1].last_event_sequence == 10);
    REQUIRE(manifest.stop_request_accepted_segment_count == 1);
    REQUIRE(manifest.participants[0].held_after_event_sequence == 11);
  }

  SECTION("a segment with a diverging corpus digest rejects at publication") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    GuestExecutionSessionAssemblerSegmentEnd end =
        MakeSegmentEnd(0, kStartTick, harness.clock.now);
    end.code_corpus_sha256 = Digest(0x77);
    REQUIRE(harness.assembler->OnSegmentEnd(kA, end) == Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kRejectedSegment);
  }

  SECTION("a segment naming the session corpus digest publishes") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    GuestExecutionSessionAssemblerSegmentEnd end =
        MakeSegmentEnd(0, kStartTick, harness.clock.now);
    end.code_corpus_sha256 =
        GuestExecutionSessionCodec::HashBytes(harness.content.corpus);
    REQUIRE(harness.assembler->OnSegmentEnd(kA, end) == Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    harness.PublishedBundle();
  }
}

TEST_CASE("session assembler orders segments by the session clock",
          "[guest-execution-session-assembler]") {
  SECTION("a later event from another participant does not reject the end") {
    Harness harness;
    harness.StartOutside({kA, kB});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.Enter(kB) == Action::kContinue);
    ++harness.clock.now;
    const uint64_t start_tick = harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
            Action::kContinue);
    // The recorder completes here; the session observes its end only later.
    const uint64_t recorder_end_tick = harness.clock.now;
    harness.clock.now += 3;
    REQUIRE(harness.assembler->OnInstructionCoverage(kB, 1) ==
            Action::kContinue);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(0, start_tick, recorder_end_tick)) ==
            Action::kContinue);
    REQUIRE(harness.state() == State::kRecording);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
    REQUIRE(harness.assembler->ArriveAtSafepoint(kB) == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.segments[0].first_event_sequence == 3);
    REQUIRE(manifest.segments[0].last_event_sequence == 6);
  }

  SECTION("recorder ticks from the future or before the session reject") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(0, kStartTick, harness.clock.now + 1)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("a recorder that started before recording rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(0, kStartTick - 1, harness.clock.now)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }
}

TEST_CASE("session assembler stops at the exact duration tick",
          "[guest-execution-session-assembler]") {
  SECTION("polling one tick early continues and the target tick stops") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 500));
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    harness.clock.now = kStartTick + 499;
    REQUIRE(harness.assembler->Poll() == Action::kContinue);
    REQUIRE(harness.state() == State::kRecording);
    harness.clock.now = kStartTick + 500;
    REQUIRE(harness.assembler->Poll() == Action::kHold);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.status().stop_request_tick == kStartTick + 500);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_reason ==
            GuestExecutionSessionStopReason::kRequestedBoundary);
    REQUIRE(manifest.boundary.value == 500);
  }

  SECTION("an event at the target tick lands after the request") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 500));
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    harness.clock.now = kStartTick + 500;
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 3) ==
            Action::kContinue);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.status().stop_request_event_sequence == 5);
    REQUIRE(harness.status().last_event_sequence == 6);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_request_guest_instruction_count == 10);
    REQUIRE(manifest.participants[0].held_after_event_sequence == 7);
  }

  SECTION("an outer return at the target tick becomes the arrival") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 500));
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    harness.clock.now = kStartTick + 500;
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_request_event_sequence == 5);
    REQUIRE(manifest.participants[0].boundary_arrival_kind ==
            GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn);
    REQUIRE(manifest.participants[0].held_after_event_sequence == 6);
  }

  SECTION("an outer dispatch at the target tick is held outside") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 500));
    harness.StartOutside({kA});
    harness.Dispatch(kA);
    harness.clock.now = kStartTick + 500;
    REQUIRE(harness.Enter(kA) == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    REQUIRE(harness.status().participants[0].host_call_depth == 0);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.participants[0].boundary_arrival_kind ==
            GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside);
    REQUIRE(manifest.participants[0].last_event_sequence == 5);
  }

  SECTION("a duration stop holds outside participants and waits for others") {
    Harness harness(MakeConfig(
        GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds, 500));
    harness.StartOutside({kA, kB});
    REQUIRE(harness.Enter(kB) == Action::kContinue);
    harness.Dispatch(kA);
    harness.clock.now = kStartTick + 500;
    REQUIRE(harness.assembler->Poll() == Action::kHold);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.status().participants[0].held);
    REQUIRE_FALSE(harness.status().participants[1].held);
    REQUIRE(harness.assembler->ArriveAtSafepoint(kB) == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    const GuestExecutionSessionManifest& manifest =
        harness.PublishedBundle().manifest;
    REQUIRE(manifest.stop_request_event_sequence == 7);
    REQUIRE(manifest.participants[0].boundary_arrival_kind ==
            GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside);
    REQUIRE(manifest.participants[1].boundary_arrival_kind ==
            GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint);
    REQUIRE(manifest.participants[1].held_after_event_sequence == 8);
  }
}

TEST_CASE("session assembler rejects timeouts, limits and overflow",
          "[guest-execution-session-assembler]") {
  SECTION("start rendezvous times out") {
    Harness harness;
    harness.Seed({{kA, 1}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    harness.clock.now = kStartTick + 200;
    REQUIRE(harness.assembler->Poll() == Action::kHold);
    harness.clock.now = kStartTick + 201;
    REQUIRE(harness.assembler->Poll() == Action::kReject);
    harness.RequireRejected(Rejection::kStartRendezvousTimeout);
    REQUIRE(harness.states.calls == 0);
  }

  SECTION("stop tail event maximum rejects rather than truncates") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.maximum_stop_tail_event_count = 2;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kStopTailLimit);
  }

  SECTION("aggregate event maximum bounds the stop tail") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.limits.maximum_event_count = 8;
    config.bundle_limits.session.maximum_total_events = 8;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kHardLimit);
    REQUIRE(harness.publisher.calls == 0);
  }

  SECTION("aggregate event maximum bounds the requested boundary") {
    GuestExecutionSessionAssemblerConfig config =
        MakeConfig(GuestExecutionSessionBoundaryKind::kGuestMarkerCount, 1);
    config.boundary.marker_source = GuestExecutionSessionMarkerSource::kKernel;
    config.boundary.marker_identity = 0x55;
    config.limits.maximum_event_count = 5;
    config.bundle_limits.session.maximum_total_events = 5;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->OnGuestMarker(
                kA, GuestExecutionSessionMarkerSource::kKernel, 0x55) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kHardLimit);
    REQUIRE(harness.publisher.calls == 0);
  }

  SECTION("aggregate checkpoint state maximum rejects the seed roster") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.bundle_limits.session.maximum_total_checkpoint_thread_states = 1;
    Harness harness(config);
    REQUIRE(harness.Create());
    REQUIRE_FALSE(harness.SeedOnly({{kA, 0}}));
    harness.RequireRejected(Rejection::kHardLimit);
    REQUIRE(harness.publisher.calls == 0);
    REQUIRE(harness.states.calls == 0);
  }

  SECTION("stop tail instruction maximum rejects") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.maximum_stop_tail_guest_instruction_count = 4;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 4) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kStopTailLimit);
  }

  SECTION("stop tail tick maximum rejects on poll and on arrival") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    const uint64_t request_tick = harness.status().stop_request_tick;
    harness.clock.now = request_tick + 300;
    REQUIRE(harness.assembler->Poll() == Action::kHold);
    harness.clock.now = request_tick + 301;
    REQUIRE(harness.Leave(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kStopTailLimit);
  }

  SECTION("stop tail tick maximum rejects while waiting for a sink") {
    Harness harness;
    harness.Seed({{kA, 0}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kContinue);
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE(harness.state() == State::kStopRendezvous);
    const uint64_t request_tick = harness.status().stop_request_tick;
    harness.clock.now = request_tick + 300;
    REQUIRE(harness.assembler->Poll() == Action::kHold);
    harness.clock.now = request_tick + 301;
    REQUIRE(harness.assembler->Poll() == Action::kReject);
    harness.RequireRejected(Rejection::kStopTailLimit);
  }

  SECTION("event hard maximum is a rejection, never a truncated session") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.limits.maximum_event_count = 5;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kHardLimit);
    REQUIRE(harness.status().stop_reason ==
            GuestExecutionReelStopReason::kMaximumEventCount);
  }

  SECTION("segment hard maximum is a rejection, never a truncated session") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.limits.maximum_segment_count = 1;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(0, kStartTick, harness.clock.now)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kHardLimit);
    REQUIRE(harness.status().stop_reason ==
            GuestExecutionReelStopReason::kMaximumSegmentCount);
  }

  SECTION("event payload byte limit rejects") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.bundle_limits.session.maximum_event_payload_bytes = 8;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    GuestExecutionSessionAssemblerExternalEvent event;
    event.kind = GuestExecutionSessionEventKind::kExternOrBuiltin;
    event.guest_address = 0x82000080;
    event.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
    event.payload = Bytes(16, 1);
    REQUIRE(harness.assembler->OnExternalEvent(kA, event) == Action::kReject);
    harness.RequireRejected(Rejection::kStorageLimit);
  }

  SECTION("content blob count limit rejects the checkpoints at publication") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.bundle_limits.maximum_content_blobs = 2;
    Harness harness(config);
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kStorageLimit);
  }

  SECTION("overlapping checkpoint content rejects at publication") {
    Harness harness;
    harness.content.entries.push_back(
        {GuestExecutionSessionContentKind::kGuestPage, 0x1000, Bytes(4096, 9)});
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kContentFailure);
  }

  SECTION(
      "a participant state that changes size rejects the final checkpoint") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    harness.states.state_size = 65;
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kCheckpointFailure);
  }

  SECTION("a failing corpus provider rejects at publication") {
    Harness harness;
    harness.content.corpus_fail = true;
    harness.StartOutside({kA});
    harness.Dispatch(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kContentFailure);
  }

  SECTION("clock regression rejects") {
    Harness harness;
    harness.StartOutside({kA});
    harness.clock.now = kStartTick - 1;
    REQUIRE(harness.Enter(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }
}

TEST_CASE("session assembler rejects unsupported, unmodeled and abnormal work",
          "[guest-execution-session-assembler]") {
  SECTION("an unsupported dependency rejects with its diagnostic event") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnUnsupportedDependency(
                kA, ppc::kGuestInvocationDependencyMmio) == Action::kReject);
    harness.RequireRejected(Rejection::kUnsupportedDependency);
    REQUIRE(harness.status().last_event_sequence == 2);
  }

  SECTION("unknown dependency flags are an invalid call") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.assembler->OnUnsupportedDependency(kA, 1u << 31) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("a segment with rejected dependencies rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    GuestExecutionSessionAssemblerSegmentEnd end =
        MakeSegmentEnd(0, kStartTick, harness.clock.now);
    end.status.state = GuestInvocationCaptureState::kRejected;
    end.status.rejected_dependency_flags =
        ppc::kGuestInvocationDependencyKernelExport;
    REQUIRE(harness.assembler->OnSegmentEnd(kA, end) == Action::kReject);
    harness.RequireRejected(Rejection::kUnsupportedDependency);
  }

  SECTION("a non-published segment rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    ++harness.clock.now;
    GuestExecutionSessionAssemblerSegmentEnd end =
        MakeSegmentEnd(0, kStartTick, harness.clock.now);
    end.status.state = GuestInvocationCaptureState::kStopped;
    REQUIRE(harness.assembler->OnSegmentEnd(kA, end) == Action::kReject);
    harness.RequireRejected(Rejection::kRejectedSegment);
  }

  SECTION("a segment end without a begin rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentEnd(
                kA, MakeSegmentEnd(0, kStartTick, kStartTick)) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("an unmodeled guest-thread mutation is recorded and rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    GuestExecutionSessionAssemblerMutation mutation;
    mutation.source = GuestExecutionSessionMutationSource::kOtherGuestThread;
    mutation.disposition =
        GuestExecutionSessionEventDisposition::kRejectSession;
    mutation.guest_address = 0x2000;
    mutation.byte_count = 4;
    REQUIRE(harness.assembler->OnMemoryMutation(kA, mutation) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kUnmodeledMutation);
    REQUIRE(harness.status().last_event_sequence == 2);
  }

  SECTION("an unmodeled asynchronous mutation is recorded and rejects") {
    Harness harness;
    harness.Seed({{kA, 0}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kContinue);
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    GuestExecutionSessionAssemblerMutation mutation;
    mutation.source = GuestExecutionSessionMutationSource::kGpu;
    mutation.disposition =
        GuestExecutionSessionEventDisposition::kRejectSession;
    mutation.external_sink_ordinal = gpu;
    mutation.guest_address = 0x3000;
    mutation.byte_count = 16;
    REQUIRE(harness.assembler->OnMemoryMutation(std::nullopt, mutation) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kUnmodeledMutation);
    REQUIRE(harness.status().last_event_sequence == 2);
  }

  SECTION("an asynchronous mutation during an open segment rejects") {
    Harness harness;
    harness.Seed({{kA, 0}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kContinue);
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    REQUIRE(harness.assembler->OnMemoryMutation(
                std::nullopt,
                SinkMutation(GuestExecutionSessionMutationSource::kGpu, gpu,
                             0x3000, 4, 1)) == Action::kReject);
    harness.RequireRejected(Rejection::kUnmodeledMutation);
  }

  SECTION("no-thread mutations and interrupts cannot be deterministic") {
    Harness harness;
    harness.Seed({{kA, 0}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kContinue);
    GuestExecutionSessionAssemblerMutation mutation = SinkMutation(
        GuestExecutionSessionMutationSource::kDma, gpu, 0x3000, 4, 1);
    mutation.disposition =
        GuestExecutionSessionEventDisposition::kValidateDeterministic;
    REQUIRE(harness.assembler->OnMemoryMutation(std::nullopt, mutation) ==
            Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);

    Harness interrupts;
    interrupts.StartOutside({kA});
    GuestExecutionSessionAssemblerExternalEvent event;
    event.kind = GuestExecutionSessionEventKind::kInterrupt;
    event.disposition =
        GuestExecutionSessionEventDisposition::kValidateDeterministic;
    REQUIRE(interrupts.assembler->OnExternalEvent(std::nullopt, event) ==
            Action::kReject);
    interrupts.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("an asynchronous mutation without a registered sink rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.assembler->OnMemoryMutation(
                std::nullopt,
                SinkMutation(GuestExecutionSessionMutationSource::kDma, 0,
                             0x2000, 4, 1)) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("a held sink that keeps mutating at the stop rejects") {
    Harness harness;
    harness.Seed({{kA, 0}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kContinue);
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE(harness.state() == State::kStopRendezvous);
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    REQUIRE(harness.assembler->OnMemoryMutation(
                std::nullopt,
                SinkMutation(GuestExecutionSessionMutationSource::kGpu, gpu,
                             0x3000, 4, 1)) == Action::kReject);
    harness.RequireRejected(Rejection::kExternalSinkNotHeld);
  }

  SECTION("a held sink that keeps mutating at the start rejects") {
    Harness harness;
    harness.Seed({{kA, 1}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kHold);
    REQUIRE(harness.state() == State::kStartRendezvous);
    REQUIRE(harness.assembler->OnMemoryMutation(
                std::nullopt,
                SinkMutation(GuestExecutionSessionMutationSource::kGpu, gpu,
                             0x3000, 4, 1)) == Action::kReject);
    harness.RequireRejected(Rejection::kExternalSinkNotHeld);
  }

  SECTION("a held PM4 sink may not emit swap markers") {
    GuestExecutionSessionAssemblerConfig config = MakeConfig();
    config.pm4_marker_sink_ordinal = 0;
    Harness harness(config);
    harness.Seed({{kA, 1}});
    uint32_t gpu = 0;
    REQUIRE(harness.assembler->RegisterExternalSink("gpu", &gpu));
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->OnExternalSinkHeld(gpu) == Action::kHold);
    REQUIRE(harness.assembler->OnGuestMarker(
                std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
                0x99) == Action::kReject);
    harness.RequireRejected(Rejection::kExternalSinkNotHeld);
  }

  SECTION("an external event that rejects the session is fail closed") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    GuestExecutionSessionAssemblerExternalEvent event;
    event.kind = GuestExecutionSessionEventKind::kClockOrTimebase;
    event.disposition = GuestExecutionSessionEventDisposition::kRejectSession;
    REQUIRE(harness.assembler->OnExternalEvent(kA, event) == Action::kReject);
    harness.RequireRejected(Rejection::kUnsupportedDependency);
  }

  SECTION("malformed external events are invalid calls") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    GuestExecutionSessionAssemblerExternalEvent event;
    event.kind = GuestExecutionSessionEventKind::kSegmentBegin;
    REQUIRE(harness.assembler->OnExternalEvent(kA, event) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("an abnormal host call outcome rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(
        harness.assembler->OnOuterHostCallEnd(
            kA, GuestExecutionCaptureHostCallOutcome::kAbortedByHostUnwind) ==
        Action::kReject);
    harness.RequireRejected(Rejection::kAbnormalHostCall);
  }

  SECTION("a dispatch that fails to enter rejects inside a session only") {
    Harness before;
    before.Seed({{kA, 1}});
    REQUIRE(before.assembler->Arm(&before.error));
    REQUIRE(before.assembler->OnOuterHostCallEnd(
                kA, GuestExecutionCaptureHostCallOutcome::kFailedToEnter) ==
            Action::kContinue);
    REQUIRE(before.status().participants[0].host_call_depth == 0);

    Harness inside;
    inside.StartOutside({kA});
    REQUIRE(inside.Enter(kA) == Action::kContinue);
    REQUIRE(inside.assembler->OnOuterHostCallEnd(
                kA, GuestExecutionCaptureHostCallOutcome::kFailedToEnter) ==
            Action::kReject);
    inside.RequireRejected(Rejection::kAbnormalHostCall);
  }

  SECTION("an unbalanced host call end rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Leave(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("coverage outside guest code rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }
}

TEST_CASE("session assembler rejects participants that resume while parked",
          "[guest-execution-session-assembler]") {
  SECTION("the last safepoint arrival completes the start") {
    Harness harness;
    harness.Seed({{kA, 1}, {kB, 0}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kContinue);
    REQUIRE(harness.state() == State::kRecording);
  }

  SECTION("a parked participant that keeps executing rejects the start") {
    Harness harness;
    harness.Seed({{kA, 1}, {kB, 1}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kParticipantNotHeld);
  }

  SECTION("an outside participant reaching a safepoint at start rejects") {
    Harness harness;
    harness.Seed({{kA, 0}, {kB, 1}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE(harness.assembler->RequestStart(&harness.error));
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kParticipantNotHeld);
  }

  SECTION("a safepoint-held participant that resumes after the stop rejects") {
    Harness harness;
    harness.StartOutside({kA, kB});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.Enter(kB) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    harness.RequireRejected(Rejection::kParticipantNotHeld);
  }

  SECTION("a participant may not park inside an open segment") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("an outer return inside an open segment rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    REQUIRE(harness.assembler->OnSegmentBegin(kA, 0x82000000, 0x820000FC) ==
            Action::kContinue);
    REQUIRE(harness.Leave(kA) == Action::kReject);
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION(
      "a held outside participant is held again on dispatch, not rejected") {
    Harness harness;
    harness.StartOutside({kA, kB});
    REQUIRE(harness.Enter(kA) == Action::kContinue);
    ++harness.clock.now;
    harness.RecordSegment(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.status().participants[1].held);
    REQUIRE(harness.Enter(kB) == Action::kHold);
    REQUIRE(harness.state() == State::kStopRequested);
    REQUIRE(harness.Leave(kA) == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    harness.PublishedBundle();
  }
}

TEST_CASE("session assembler detects dependency callback reentry",
          "[guest-execution-session-assembler]") {
  SECTION("state provider reentry at the initial checkpoint") {
    Harness harness;
    harness.Seed({{kA, 0}});
    harness.states.reenter_target = harness.assembler.get();
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE_FALSE(harness.assembler->RequestStart(&harness.error));
    harness.RequireRejected(Rejection::kCallbackReentry);
  }

  SECTION("content provider reentry at publication") {
    Harness harness;
    harness.StartOutside({kA});
    harness.content.reenter_target = harness.assembler.get();
    harness.Dispatch(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kCallbackReentry);
  }

  SECTION("a publisher that cancels the session still reports its output") {
    Harness harness;
    harness.StartOutside({kA});
    harness.publisher.cancel_target = harness.assembler.get();
    harness.Dispatch(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    REQUIRE(harness.state() == State::kRejected);
    REQUIRE(harness.rejection() == Rejection::kCancelled);
    REQUIRE(harness.status().published);
    REQUIRE(harness.publisher.bundles.size() == 1);
  }

  SECTION("provider failures reject without publication") {
    Harness harness;
    harness.states.fail = true;
    harness.Seed({{kA, 0}});
    REQUIRE(harness.assembler->Arm(&harness.error));
    REQUIRE_FALSE(harness.assembler->RequestStart(&harness.error));
    harness.RequireRejected(Rejection::kCheckpointFailure);
    REQUIRE(harness.error.find("state provider") != std::string::npos);
  }
}

TEST_CASE("session assembler rolls event chunks over at the configured size",
          "[guest-execution-session-assembler]") {
  GuestExecutionSessionAssemblerConfig config = MakeConfig();
  config.maximum_events_per_chunk = 3;
  Harness harness(config);
  harness.StartOutside({kA});
  REQUIRE(harness.Enter(kA) == Action::kContinue);
  ++harness.clock.now;
  harness.RecordSegment(kA);
  for (uint64_t i = 0; i < 6; ++i) {
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) ==
            Action::kContinue);
  }
  REQUIRE(harness.status().last_event_sequence == 10);
  // Nothing is encoded on input threads.
  REQUIRE(harness.status().staged_event_count == 10);
  REQUIRE(harness.status().encoded_chunk_count == 0);
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.Leave(kA) == Action::kHold);

  const GuestExecutionSessionBundle& bundle = harness.PublishedBundle();
  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  REQUIRE(manifest.last_event_sequence == 13);
  REQUIRE(manifest.chunks.size() == 7);
  uint64_t expected_sequence = 1;
  for (size_t i = 1; i + 1 < manifest.chunks.size(); ++i) {
    const GuestExecutionSessionChunkReference& chunk = manifest.chunks[i];
    REQUIRE(chunk.kind == GuestExecutionSessionChunkKind::kEvents);
    REQUIRE(chunk.ordinal == i);
    REQUIRE(chunk.record_count <= 3);
    REQUIRE(chunk.first_event_sequence == expected_sequence);
    expected_sequence = chunk.last_event_sequence + 1;
  }
  REQUIRE(expected_sequence == 14);
  REQUIRE(manifest.chunks.back().kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
  REQUIRE(manifest.chunks.back().first_event_sequence == 13);
  REQUIRE(harness.status().encoded_chunk_count == 7);
}

TEST_CASE("session assembler fails closed on publication, cancel and teardown",
          "[guest-execution-session-assembler]") {
  SECTION("publisher failure rejects and marks nothing published") {
    Harness harness;
    harness.publisher.fail = true;
    harness.StartOutside({kA});
    harness.Dispatch(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    REQUIRE(harness.error == "publisher failed");
    REQUIRE(harness.state() == State::kRejected);
    REQUIRE(harness.rejection() == Rejection::kPublicationFailure);
    REQUIRE_FALSE(harness.status().published);
    REQUIRE(harness.publisher.calls == 1);
    REQUIRE(harness.publisher.bundles.empty());
  }

  SECTION("publication before the session is held rejects") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    harness.RequireRejected(Rejection::kInvalidCall);
  }

  SECTION("a session without a segment cannot complete") {
    Harness harness;
    harness.StartOutside({kA});
    REQUIRE(harness.assembler->OnGuestMarker(
                std::nullopt, GuestExecutionSessionMarkerSource::kPm4Swap,
                0x99) == Action::kContinue);
    REQUIRE(harness.assembler->RequestStop() == Action::kReject);
    harness.RequireRejected(Rejection::kReelRejected);
  }

  SECTION("cancellation rejects, drops staged work and refuses later work") {
    Harness harness;
    harness.StartOutside({kA});
    harness.Dispatch(kA);
    REQUIRE(harness.status().staged_event_count == 5);
    REQUIRE(harness.status().content_blob_count == 2);
    REQUIRE(harness.status().content_blob_bytes == 64 + 96);
    harness.assembler->Cancel("operator cancelled");
    harness.RequireRejected(Rejection::kCancelled);
    REQUIRE(harness.status().message == "operator cancelled");
    REQUIRE(harness.status().staged_event_count == 0);
    REQUIRE(harness.status().staged_chunk_bytes == 0);
    REQUIRE(harness.status().content_blob_bytes == 0);
    REQUIRE(harness.status().content_blob_count == 2);
    REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kReject);
    REQUIRE(harness.assembler->Poll() == Action::kReject);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    REQUIRE(harness.publisher.calls == 0);
    harness.assembler->Cancel("again");
    REQUIRE(harness.status().message == "operator cancelled");
  }
}

TEST_CASE("session assembler publishes through the directory publisher",
          "[guest-execution-session-assembler]") {
  ScopedTestDirectory directory;
  const std::filesystem::path output = directory.path() / "session";
  std::error_code filesystem_error;

  SECTION("a complete session round trips through the bundle reader") {
    GuestExecutionSessionDirectoryPublisher publisher(output);
    Harness harness;
    harness.StartOutside({kA}, &publisher);
    harness.Dispatch(kA);
    harness.states.generation = 1;
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.assembler->Publish(&harness.error));
    REQUIRE(harness.error.empty());
    REQUIRE(harness.status().published);
    REQUIRE(harness.state() == State::kComplete);

    GuestExecutionSessionBundle bundle;
    std::string read_error;
    REQUIRE(ReadGuestExecutionSessionBundle(output, &bundle, &read_error));
    REQUIRE(bundle.manifest.session_epoch == kEpoch);
    REQUIRE(bundle.manifest.participants.size() == 1);
    REQUIRE(bundle.manifest.segments.size() == 1);
    REQUIRE(bundle.content_blobs.size() == 6);
  }

  SECTION("an existing output directory fails publication cleanly") {
    REQUIRE(std::filesystem::create_directory(output, filesystem_error));
    GuestExecutionSessionDirectoryPublisher publisher(output);
    Harness harness;
    harness.StartOutside({kA}, &publisher);
    harness.Dispatch(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE_FALSE(harness.assembler->Publish(&harness.error));
    REQUIRE(harness.state() == State::kRejected);
    REQUIRE(harness.rejection() == Rejection::kPublicationFailure);
    REQUIRE_FALSE(harness.assembler->status().published);
    REQUIRE_FALSE(std::filesystem::exists(directory.path() / "session.part",
                                          filesystem_error));
    REQUIRE(std::filesystem::is_empty(output, filesystem_error));
  }

  SECTION("teardown after the hold publishes nothing") {
    GuestExecutionSessionDirectoryPublisher publisher(output);
    Harness harness;
    harness.StartOutside({kA}, &publisher);
    harness.Dispatch(kA);
    REQUIRE(harness.assembler->RequestStop() == Action::kHold);
    REQUIRE(harness.state() == State::kPublishing);
    REQUIRE(harness.status().staged_event_count == 7);
    REQUIRE(harness.status().content_blob_bytes > 0);
    harness.assembler.reset();
    REQUIRE_FALSE(std::filesystem::exists(output, filesystem_error));
    REQUIRE_FALSE(std::filesystem::exists(directory.path() / "session.part",
                                          filesystem_error));
  }
}

TEST_CASE("scheduler event bridge closes the canonical continuous tape",
          "[guest-execution-session-capture-event-bridge]") {
  Harness harness(MakeContinuousConfig());
  REQUIRE(harness.Create());
  harness.states.encode_thread_checkpoint = true;
  harness.states.checkpoint_participants = {kA, kB};
  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> seeds;
  GuestExecutionCaptureHostCallRosterSnapshot roster;
  MakeSeeds({{kB, 1}, {kA, 1}}, &seeds, &roster);
  REQUIRE(harness.assembler->SeedParticipants(seeds, roster));

  GuestExecutionSessionCaptureSchedulerEventBridge bridge;
  REQUIRE(bridge.BeginSession(*harness.assembler, BridgeCheckpoint(1, {kA, kB}),
                              seeds, &harness.error));
  REQUIRE(harness.assembler->Arm(&harness.error));
  REQUIRE(harness.assembler->RequestStart(&harness.error));
  REQUIRE(harness.state() == State::kStartRendezvous);
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
  REQUIRE(harness.assembler->ArriveAtSafepoint(kB) == Action::kContinue);
  REQUIRE(harness.state() == State::kRecording);
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 10) ==
          Action::kContinue);
  REQUIRE(harness.assembler->OnInstructionCoverage(kB, 11) ==
          Action::kContinue);

  kernel::GuestSchedulerCaptureEvent dispatch = BridgeSchedulerEvent(
      40, kernel::GuestSchedulerCaptureEventKind::kDispatch);
  dispatch.flags = kernel::kGuestSchedulerCaptureFlagFreshQuantum;
  REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, dispatch,
                                  &harness.error) == Action::kContinue);
  kernel::GuestSchedulerCaptureEvent yield = BridgeSchedulerEvent(
      41, kernel::GuestSchedulerCaptureEventKind::kYield, kB);
  yield.guest_instruction_delta = 5;
  REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, yield, &harness.error) ==
          Action::kContinue);
  kernel::GuestSchedulerCaptureEvent block = BridgeSchedulerEvent(
      42, kernel::GuestSchedulerCaptureEventKind::kBlock, kB);
  block.guest_instruction_delta = 6;
  REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, block, &harness.error) ==
          Action::kContinue);
  kernel::GuestSchedulerCaptureEvent reready = BridgeSchedulerEvent(
      43, kernel::GuestSchedulerCaptureEventKind::kReready, kB);
  reready.reason = kernel::GuestSchedulerCaptureReason::kSignalEpoch;
  reready.wait.flags = kernel::kGuestSchedulerCaptureWaitFlagGated |
                       kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
  reready.wait.observed_wait_epoch = 8;
  reready.wait.signal_epochs_observed[0] = 8;
  REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, reready,
                                  &harness.error) == Action::kContinue);
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  REQUIRE(harness.state() == State::kStopRequested);
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kHold);
  REQUIRE(harness.assembler->ArriveAtSafepoint(kB) == Action::kHold);
  REQUIRE(harness.state() == State::kPublishing);
  REQUIRE(bridge.SealSession(*harness.assembler, BridgeCheckpoint(2, {kA, kB}),
                             &harness.error));
  const bool published = harness.assembler->Publish(&harness.error);
  INFO(harness.error);
  REQUIRE(published);
  REQUIRE(harness.publisher.bundles.size() == 1);

  GuestExecutionSessionBundle bundle = harness.publisher.bundles.front();
  REQUIRE(bundle.manifest.stop_request_guest_instruction_count == 32);
  const bool finalized = bridge.FinalizeBundle(&bundle, 4, &harness.error);
  INFO(harness.error);
  REQUIRE(finalized);
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &harness.error));
  const std::vector<GuestExecutionContinuousEvent> overlay =
      DecodeBridgeOverlay(bundle);
  REQUIRE(overlay.size() == bundle.manifest.accepted_event_count);
  std::vector<GuestExecutionContinuousEvent> scheduler_overlay;
  for (const GuestExecutionContinuousEvent& event : overlay) {
    if (event.kind == GuestExecutionSessionEventKind::kThreadDispatch ||
        event.kind == GuestExecutionSessionEventKind::kSynchronization) {
      scheduler_overlay.push_back(event);
    }
  }
  REQUIRE(scheduler_overlay.size() == 4);
  REQUIRE(scheduler_overlay[0].actor ==
          GuestExecutionContinuousEventIdentity{});
  REQUIRE(scheduler_overlay[0].subject ==
          GuestExecutionContinuousEventIdentity{0, kA.guest_thread_id});
  REQUIRE(scheduler_overlay[1].actor ==
          GuestExecutionContinuousEventIdentity{1, kB.guest_thread_id});
  REQUIRE(scheduler_overlay[1].subject ==
          GuestExecutionContinuousEventIdentity{1, kB.guest_thread_id});
  REQUIRE(scheduler_overlay[2].actor ==
          GuestExecutionContinuousEventIdentity{1, kB.guest_thread_id});
  REQUIRE(scheduler_overlay[2].subject ==
          GuestExecutionContinuousEventIdentity{1, kB.guest_thread_id});
  REQUIRE(scheduler_overlay[3].actor ==
          GuestExecutionContinuousEventIdentity{});
  REQUIRE(scheduler_overlay[3].subject ==
          GuestExecutionContinuousEventIdentity{1, kB.guest_thread_id});

  std::vector<GuestExecutionContinuousEvent> checkpoint_overlay;
  for (const GuestExecutionContinuousEvent& event : overlay) {
    if (event.checkpoint.kind ==
        GuestExecutionContinuousCheckpointReferenceKind::kThreadState) {
      checkpoint_overlay.push_back(event);
    }
  }
  REQUIRE(checkpoint_overlay.size() == 2);
  for (uint32_t ordinal = 0; ordinal < checkpoint_overlay.size(); ++ordinal) {
    const GuestExecutionContinuousEvent& checkpoint =
        checkpoint_overlay[ordinal];
    const GuestExecutionSessionParticipant& participant =
        bundle.manifest.participants[ordinal];
    REQUIRE(checkpoint.kind ==
            GuestExecutionSessionEventKind::kJitSafepointArrival);
    REQUIRE(checkpoint.subject == GuestExecutionContinuousEventIdentity{
                                      ordinal, participant.guest_thread_id});
    REQUIRE(checkpoint.checkpoint.checkpoint_global_sequence ==
            bundle.manifest.last_event_sequence);
    REQUIRE(checkpoint.checkpoint.checkpoint_global_sequence >
            checkpoint.global_sequence);
    REQUIRE(checkpoint.checkpoint.binding.participant_ordinal == ordinal);
    REQUIRE(checkpoint.checkpoint.binding.guest_thread_id ==
            participant.guest_thread_id);
  }
  REQUIRE(overlay.back().kind == GuestExecutionSessionEventKind::kBoundaryHeld);
  REQUIRE(overlay.back().checkpoint.kind ==
          GuestExecutionContinuousCheckpointReferenceKind::kNone);

  std::vector<uint64_t> decoded_scheduler_sequences;
  for (size_t chunk_index = 0; chunk_index < bundle.manifest.chunks.size();
       ++chunk_index) {
    if (bundle.manifest.chunks[chunk_index].kind !=
        GuestExecutionSessionChunkKind::kEvents) {
      continue;
    }
    GuestExecutionSessionEventChunk chunk;
    REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(
        bundle.chunks[chunk_index], &chunk, &harness.error));
    for (const GuestExecutionSessionEvent& event : chunk.events) {
      if (event.kind != GuestExecutionSessionEventKind::kThreadDispatch &&
          event.kind != GuestExecutionSessionEventKind::kSynchronization) {
        continue;
      }
      const auto blob = std::find_if(
          bundle.content_blobs.cbegin(), bundle.content_blobs.cend(),
          [&event](const GuestExecutionSessionContentBlob& candidate) {
            return candidate.sha256 == event.payload_sha256;
          });
      REQUIRE(blob != bundle.content_blobs.cend());
      kernel::GuestSchedulerCaptureEvent decoded;
      REQUIRE(GuestExecutionSessionCaptureSchedulerEventBridge::
                  DecodeSchedulerEventPayload(blob->bytes, &decoded,
                                              &harness.error));
      decoded_scheduler_sequences.push_back(decoded.sequence);
      if (decoded.sequence == 43) {
        REQUIRE(decoded.wait == reready.wait);
      }
    }
  }
  REQUIRE(decoded_scheduler_sequences == std::vector<uint64_t>{40, 41, 42, 43});
}

TEST_CASE("scheduler event bridge accepts only complete modeled source tapes",
          "[guest-execution-session-capture-event-bridge]") {
  Harness harness(MakeContinuousConfig());
  REQUIRE(harness.Create());
  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> seeds;
  GuestExecutionCaptureHostCallRosterSnapshot roster;
  MakeSeeds({{kA, 1}}, &seeds, &roster);
  REQUIRE(harness.assembler->SeedParticipants(seeds, roster));
  GuestExecutionSessionCaptureSchedulerEventBridge bridge;
  REQUIRE(bridge.BeginSession(*harness.assembler, BridgeCheckpoint(1), seeds,
                              &harness.error));
  REQUIRE(harness.assembler->Arm(&harness.error));
  REQUIRE(harness.assembler->RequestStart(&harness.error));
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kContinue);

  SECTION("every modeled scheduler transition is accepted") {
    const std::vector<kernel::GuestSchedulerCaptureEventKind> kinds = {
        kernel::GuestSchedulerCaptureEventKind::kEnqueueReady,
        kernel::GuestSchedulerCaptureEventKind::kDequeueReady,
        kernel::GuestSchedulerCaptureEventKind::kDispatch,
        kernel::GuestSchedulerCaptureEventKind::kSwitchOut,
        kernel::GuestSchedulerCaptureEventKind::kYield,
        kernel::GuestSchedulerCaptureEventKind::kPreemptRequest,
        kernel::GuestSchedulerCaptureEventKind::kSafepoint,
        kernel::GuestSchedulerCaptureEventKind::kBlock,
        kernel::GuestSchedulerCaptureEventKind::kReready,
        kernel::GuestSchedulerCaptureEventKind::kParkSuspended,
        kernel::GuestSchedulerCaptureEventKind::kResume,
        kernel::GuestSchedulerCaptureEventKind::kPriorityChange,
        kernel::GuestSchedulerCaptureEventKind::kMigrate,
    };
    uint64_t sequence = 100;
    for (kernel::GuestSchedulerCaptureEventKind kind : kinds) {
      kernel::GuestSchedulerCaptureEvent event =
          BridgeSchedulerEvent(sequence++, kind);
      switch (kind) {
        case kernel::GuestSchedulerCaptureEventKind::kPreemptRequest:
          event.reason = kernel::GuestSchedulerCaptureReason::kPriority;
          break;
        case kernel::GuestSchedulerCaptureEventKind::kSafepoint:
          event.reason = kernel::GuestSchedulerCaptureReason::kForcedIrql;
          event.flags = kernel::kGuestSchedulerCaptureFlagSchedulerRequested;
          event.value = 2;
          event.count = kernel::kGuestSchedulerCaptureForcedIrqlMinimumDeclines;
          break;
        case kernel::GuestSchedulerCaptureEventKind::kBlock:
          event.flags = kernel::kGuestSchedulerCaptureFlagGated |
                        kernel::kGuestSchedulerCaptureFlagInterruptible;
          break;
        case kernel::GuestSchedulerCaptureEventKind::kReready:
          event.reason = kernel::GuestSchedulerCaptureReason::kPolled;
          break;
        default:
          break;
      }
      REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, event,
                                      &harness.error) == Action::kContinue);
    }
  }

  SECTION("an unsupported participant transition rejects permanently") {
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    9, kernel::GuestSchedulerCaptureEventKind::kExit),
                &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("unsupported or malformed") !=
            std::string::npos);
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    10, kernel::GuestSchedulerCaptureEventKind::kDispatch),
                &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("not recording") != std::string::npos);
  }

  SECTION("terminate transitions reject") {
    kernel::GuestSchedulerCaptureEvent event = BridgeSchedulerEvent(
        9, kernel::GuestSchedulerCaptureEventKind::kTerminate);
    event.reason = kernel::GuestSchedulerCaptureReason::kPreemptRequested;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, event,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("unsupported or malformed") !=
            std::string::npos);
  }

  SECTION("forget transitions reject") {
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    9, kernel::GuestSchedulerCaptureEventKind::kForget),
                &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("unsupported or malformed") !=
            std::string::npos);
  }

  SECTION("the global shutdown transition rejects without a participant") {
    kernel::GuestSchedulerCaptureEvent shutdown;
    shutdown.sequence = 9;
    shutdown.kind = kernel::GuestSchedulerCaptureEventKind::kShutdown;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, shutdown,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("unsupported or malformed") !=
            std::string::npos);
  }

  SECTION("version 1 scheduler payloads are explicitly non-replayable") {
    std::vector<uint8_t> payload(
        GuestExecutionSessionCaptureSchedulerEventBridge::
            kSchedulerPayloadV1Size,
        0);
    const std::string magic = "XEGSCE1";
    std::copy(magic.cbegin(), magic.cend(), payload.begin());
    payload[8] = 1;
    kernel::GuestSchedulerCaptureEvent decoded;
    REQUIRE_FALSE(
        GuestExecutionSessionCaptureSchedulerEventBridge::
            DecodeSchedulerEventPayload(payload, &decoded, &harness.error));
    REQUIRE(harness.error.find("version 1 is not deterministic-replayable") !=
            std::string::npos);
  }

  SECTION("safepoint provenance fails closed") {
    kernel::GuestSchedulerCaptureEvent safepoint = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kSafepoint);
    safepoint.reason = kernel::GuestSchedulerCaptureReason::kYielded;
    safepoint.flags = kernel::kGuestSchedulerCaptureFlagSchedulerRequested;
    safepoint.guest_pc = 0;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, safepoint,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("safepoint provenance") != std::string::npos);
  }

  SECTION("nonterminal instruction coverage fails closed") {
    kernel::GuestSchedulerCaptureEvent dispatch = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kDispatch);
    dispatch.guest_instruction_delta = 1;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, dispatch,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("unsupported or malformed") !=
            std::string::npos);
  }

  SECTION("capture-only safepoint provenance fails closed") {
    kernel::GuestSchedulerCaptureEvent safepoint = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kSafepoint);
    safepoint.reason = kernel::GuestSchedulerCaptureReason::kYielded;
    safepoint.flags = kernel::kGuestSchedulerCaptureFlagCaptureRequested;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, safepoint,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("safepoint provenance") != std::string::npos);
  }

  SECTION("forced safepoint without declined visits fails closed") {
    kernel::GuestSchedulerCaptureEvent safepoint = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kSafepoint);
    safepoint.reason = kernel::GuestSchedulerCaptureReason::kForcedIrql;
    safepoint.flags = kernel::kGuestSchedulerCaptureFlagSchedulerRequested;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, safepoint,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("safepoint provenance") != std::string::npos);
  }

  SECTION("forced safepoint below the force threshold fails closed") {
    kernel::GuestSchedulerCaptureEvent safepoint = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kSafepoint);
    safepoint.reason = kernel::GuestSchedulerCaptureReason::kForcedIrql;
    safepoint.flags = kernel::kGuestSchedulerCaptureFlagSchedulerRequested;
    safepoint.value = 2;
    safepoint.count =
        kernel::kGuestSchedulerCaptureForcedIrqlMinimumDeclines - 1;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, safepoint,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("safepoint provenance") != std::string::npos);
  }

  SECTION("IRQL safepoint below dispatch level fails closed") {
    kernel::GuestSchedulerCaptureEvent safepoint = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kSafepoint);
    safepoint.reason = kernel::GuestSchedulerCaptureReason::kDeferredIrql;
    safepoint.flags = kernel::kGuestSchedulerCaptureFlagSchedulerRequested;
    safepoint.value = 1;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, safepoint,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("safepoint provenance") != std::string::npos);
  }

  SECTION("yielded safepoint at dispatch level fails closed") {
    kernel::GuestSchedulerCaptureEvent safepoint = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kSafepoint);
    safepoint.reason = kernel::GuestSchedulerCaptureReason::kYielded;
    safepoint.flags = kernel::kGuestSchedulerCaptureFlagSchedulerRequested;
    safepoint.value = 2;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, safepoint,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("safepoint provenance") != std::string::npos);
  }

  SECTION("pre-enqueue priority mutation has no CPU owner") {
    kernel::GuestSchedulerCaptureEvent priority_change = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kPriorityChange);
    priority_change.cpu = -1;
    priority_change.value = 7;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, priority_change,
                                    &harness.error) == Action::kContinue);
  }

  SECTION("priority mutation cannot name a target CPU") {
    kernel::GuestSchedulerCaptureEvent priority_change = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kPriorityChange);
    priority_change.target_cpu = 1;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, priority_change,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("priority-change CPU") != std::string::npos);
  }

  SECTION("wait provenance fails closed") {
    kernel::GuestSchedulerCaptureEvent block = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kBlock);
    block.wait.observed_wait_epoch++;
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, block,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("wait epochs") != std::string::npos);
  }

  SECTION("untyped production waits fail closed") {
    kernel::GuestSchedulerCaptureEvent block = BridgeSchedulerEvent(
        10, kernel::GuestSchedulerCaptureEventKind::kBlock);
    block.value = 0;
    block.flags = 0;
    block.wait = {};
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, block,
                                    &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("wait kind") != std::string::npos);
  }

  SECTION("unknown participants fail closed") {
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    10, kernel::GuestSchedulerCaptureEventKind::kDispatch, kB),
                &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("unknown participant") != std::string::npos);
  }

  SECTION("a source sequence gap rejects permanently") {
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    10, kernel::GuestSchedulerCaptureEventKind::kDispatch),
                &harness.error) == Action::kContinue);
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    12, kernel::GuestSchedulerCaptureEventKind::kDispatch),
                &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("source sequence has a gap") !=
            std::string::npos);
    REQUIRE(bridge.OnSchedulerEvent(
                *harness.assembler,
                BridgeSchedulerEvent(
                    11, kernel::GuestSchedulerCaptureEventKind::kDispatch),
                &harness.error) == Action::kReject);
    REQUIRE(harness.error.find("not recording") != std::string::npos);
  }
}

TEST_CASE("scheduler event bridge authenticates every cooperative wait kind",
          "[guest-execution-session-capture-event-bridge]") {
  Harness harness(MakeContinuousConfig());
  REQUIRE(harness.Create());
  harness.states.encode_thread_checkpoint = true;
  harness.states.checkpoint_participants = {kA};
  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> seeds;
  GuestExecutionCaptureHostCallRosterSnapshot roster;
  MakeSeeds({{kA, 1}}, &seeds, &roster);
  REQUIRE(harness.assembler->SeedParticipants(seeds, roster));

  GuestExecutionSessionCaptureSchedulerEventBridge bridge;
  REQUIRE(bridge.BeginSession(*harness.assembler, BridgeCheckpoint(1), seeds,
                              &harness.error));
  REQUIRE(harness.assembler->Arm(&harness.error));
  REQUIRE(harness.assembler->RequestStart(&harness.error));
  REQUIRE(harness.assembler->ArriveAtSafepoint(kA) == Action::kContinue);
  REQUIRE(harness.assembler->OnInstructionCoverage(kA, 1) == Action::kContinue);

  std::vector<kernel::GuestSchedulerCaptureEvent> expected;
  uint64_t sequence = 100;
  for (uint8_t value =
           static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSingle);
       value <=
       static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSocketIo);
       ++value) {
    expected.push_back(BridgeBlockEvent(
        sequence++, static_cast<kernel::GuestSchedulerCaptureWaitKind>(value)));
  }
  expected.push_back(BridgeBlockEvent(
      sequence++, kernel::GuestSchedulerCaptureWaitKind::kIoCompletion, false));
  expected.push_back(BridgeBlockEvent(
      sequence++, kernel::GuestSchedulerCaptureWaitKind::kSocketIo, false));
  for (const auto& event : expected) {
    REQUIRE(bridge.OnSchedulerEvent(*harness.assembler, event,
                                    &harness.error) == Action::kContinue);
  }
  REQUIRE(harness.assembler->RequestStop() == Action::kHold);
  const Action stop_arrival = harness.assembler->ArriveAtSafepoint(kA);
  INFO(harness.status().message);
  INFO(static_cast<uint32_t>(harness.rejection()));
  REQUIRE(stop_arrival == Action::kHold);
  REQUIRE(bridge.SealSession(*harness.assembler, BridgeCheckpoint(2),
                             &harness.error));
  REQUIRE(harness.assembler->Publish(&harness.error));
  GuestExecutionSessionBundle bundle = harness.publisher.bundles.front();
  REQUIRE(bridge.FinalizeBundle(&bundle, expected.size(), &harness.error));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &harness.error));

  std::array<std::vector<uint8_t>, 10> encoded_by_wait_kind;
  std::array<bool, 10> decoded_wait_kind = {};
  std::array<bool, 2> decoded_io_completion_deadline = {};
  std::array<bool, 2> decoded_socket_deadline = {};
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    kernel::GuestSchedulerCaptureEvent decoded;
    std::string decode_error;
    if (!GuestExecutionSessionCaptureSchedulerEventBridge::
            DecodeSchedulerEventPayload(blob.bytes, &decoded, &decode_error)) {
      continue;
    }
    REQUIRE(decoded.kind == kernel::GuestSchedulerCaptureEventKind::kBlock);
    REQUIRE(decoded.value < encoded_by_wait_kind.size());
    if (!decoded_wait_kind[decoded.value]) {
      encoded_by_wait_kind[decoded.value] = blob.bytes;
    }
    decoded_wait_kind[decoded.value] = true;
    const auto expected_event = std::find_if(
        expected.cbegin(), expected.cend(), [&decoded](const auto& candidate) {
          return candidate.sequence == decoded.sequence;
        });
    REQUIRE(expected_event != expected.cend());
    REQUIRE(decoded == *expected_event);
    if (decoded.value ==
        static_cast<uint8_t>(
            kernel::GuestSchedulerCaptureWaitKind::kIoCompletion)) {
      decoded_io_completion_deadline
          [(decoded.flags & kernel::kGuestSchedulerCaptureFlagHasDeadline) !=
           0] = true;
    }
    if (decoded.value ==
        static_cast<uint8_t>(
            kernel::GuestSchedulerCaptureWaitKind::kSocketIo)) {
      decoded_socket_deadline[(decoded.flags &
                               kernel::kGuestSchedulerCaptureFlagHasDeadline) !=
                              0] = true;
    }
  }
  for (uint8_t value =
           static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSingle);
       value <=
       static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSocketIo);
       ++value) {
    REQUIRE(decoded_wait_kind[value]);
  }
  REQUIRE(decoded_io_completion_deadline[0]);
  REQUIRE(decoded_io_completion_deadline[1]);
  REQUIRE(decoded_socket_deadline[0]);
  REQUIRE(decoded_socket_deadline[1]);

  auto require_malformed =
      [&](kernel::GuestSchedulerCaptureWaitKind wait_kind,
          const std::function<void(std::vector<uint8_t>*)>& mutate) {
        std::vector<uint8_t> payload =
            encoded_by_wait_kind[static_cast<uint8_t>(wait_kind)];
        mutate(&payload);
        kernel::GuestSchedulerCaptureEvent ignored;
        REQUIRE_FALSE(
            GuestExecutionSessionCaptureSchedulerEventBridge::
                DecodeSchedulerEventPayload(payload, &ignored, &harness.error));
      };
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kSingle,
                    [](auto* payload) { (*payload)[60] = 0; });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kMultiAny,
                    [](auto* payload) { (*payload)[60] = 0; });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kMultiAll,
                    [](auto* payload) { (*payload)[60] = 0; });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kDelay, [](auto* payload) {
        std::fill(payload->begin() + 64, payload->begin() + 72, 0);
      });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kFence,
                    [](auto* payload) { (*payload)[64] = 1; });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kIoOffload,
                    [](auto* payload) { (*payload)[64] = 1; });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff,
                    [](auto* payload) { (*payload)[64] = 1; });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kIoCompletion,
                    [](auto* payload) { (*payload)[60] = 0; });
  require_malformed(kernel::GuestSchedulerCaptureWaitKind::kSocketIo,
                    [](auto* payload) { (*payload)[60] = 0; });

  const auto clear_event_flag = [](std::vector<uint8_t>* payload,
                                   uint16_t flag) {
    (*payload)[40] &= static_cast<uint8_t>(~flag);
    (*payload)[41] &= static_cast<uint8_t>(~(flag >> 8));
  };
  const auto set_event_flag = [](std::vector<uint8_t>* payload, uint16_t flag) {
    (*payload)[40] |= static_cast<uint8_t>(flag);
    (*payload)[41] |= static_cast<uint8_t>(flag >> 8);
  };
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kSingle, [&](auto* payload) {
        clear_event_flag(payload,
                         kernel::kGuestSchedulerCaptureFlagInterruptible);
        (*payload)[61] &= static_cast<uint8_t>(
            ~kernel::kGuestSchedulerCaptureWaitFlagInterruptible);
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kDelay, [&](auto* payload) {
        clear_event_flag(payload, kernel::kGuestSchedulerCaptureFlagGated);
        (*payload)[61] &=
            static_cast<uint8_t>(~kernel::kGuestSchedulerCaptureWaitFlagGated);
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kDelay, [&](auto* payload) {
        set_event_flag(payload, kernel::kGuestSchedulerCaptureFlagAlertable);
        (*payload)[61] |= kernel::kGuestSchedulerCaptureWaitFlagAlertable;
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kFence, [&](auto* payload) {
        set_event_flag(payload, kernel::kGuestSchedulerCaptureFlagAlertable);
        (*payload)[61] |= kernel::kGuestSchedulerCaptureWaitFlagAlertable;
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kIoOffload, [&](auto* payload) {
        set_event_flag(payload,
                       kernel::kGuestSchedulerCaptureFlagInterruptible);
        (*payload)[61] |= kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff, [&](auto* payload) {
        set_event_flag(payload, kernel::kGuestSchedulerCaptureFlagAlertable);
        (*payload)[61] |= kernel::kGuestSchedulerCaptureWaitFlagAlertable;
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kIoCompletion, [&](auto* payload) {
        set_event_flag(payload, kernel::kGuestSchedulerCaptureFlagAlertable);
        (*payload)[61] |= kernel::kGuestSchedulerCaptureWaitFlagAlertable;
      });
  require_malformed(
      kernel::GuestSchedulerCaptureWaitKind::kSocketIo, [&](auto* payload) {
        clear_event_flag(payload,
                         kernel::kGuestSchedulerCaptureFlagInterruptible);
        (*payload)[61] &= static_cast<uint8_t>(
            ~kernel::kGuestSchedulerCaptureWaitFlagInterruptible);
      });

  std::vector<uint8_t> high_bit_kind =
      encoded_by_wait_kind[static_cast<uint8_t>(
          kernel::GuestSchedulerCaptureWaitKind::kSingle)];
  high_bit_kind[12] = 0x01;
  high_bit_kind[13] = 0x01;
  high_bit_kind[14] = 0;
  high_bit_kind[15] = 0;
  kernel::GuestSchedulerCaptureEvent ignored;
  REQUIRE_FALSE(
      GuestExecutionSessionCaptureSchedulerEventBridge::
          DecodeSchedulerEventPayload(high_bit_kind, &ignored, &harness.error));
  REQUIRE(harness.error.find("kind is out of range") != std::string::npos);
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

#endif
