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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

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
    output->assign(
        state_size,
        static_cast<uint8_t>(participant.capture_instance_id + generation));
    return true;
  }

  size_t state_size = 64;
  uint8_t generation = 0;
  bool fail = false;
  uint32_t calls = 0;
  GuestExecutionSessionAssembler* reenter_target = nullptr;
};

class FakeContentProvider final
    : public GuestExecutionSessionAssemblerContentProvider {
 public:
  FakeContentProvider() {
    entries.push_back({GuestExecutionSessionContentKind::kGuestCode, 0x82000000,
                       Bytes(0x100, 3)});
    entries.push_back(
        {GuestExecutionSessionContentKind::kGuestPage, 0x1000, Bytes(4096, 2)});
    corpus = Bytes(128, 0x10);
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

}  // namespace test
}  // namespace cpu
}  // namespace xe

#endif
