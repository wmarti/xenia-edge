/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_replay_tape.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {
namespace {

using namespace std::chrono_literals;

constexpr uint64_t kCoverageInstructionDelta = 96;

std::vector<uint8_t> Bytes(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i * 13);
  }
  return bytes;
}

GuestExecutionSessionSha256 Identity(uint8_t seed) {
  GuestExecutionSessionSha256 digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(seed + i);
  }
  return digest;
}

GuestExecutionSessionSha256 AddBlob(GuestExecutionSessionBundle* bundle,
                                    std::vector<uint8_t> bytes) {
  GuestExecutionSessionContentBlob blob;
  blob.sha256 = GuestExecutionSessionCodec::HashBytes(bytes);
  blob.bytes = std::move(bytes);
  const GuestExecutionSessionSha256 digest = blob.sha256;
  bundle->content_blobs.push_back(std::move(blob));
  return digest;
}

GuestExecutionSessionEvent Event(
    uint64_t sequence, uint32_t owner, GuestExecutionSessionEventKind kind,
    GuestExecutionSessionEventDisposition disposition) {
  GuestExecutionSessionEvent event;
  event.global_sequence = sequence;
  event.thread_ordinal = owner;
  event.kind = kind;
  event.disposition = disposition;
  return event;
}

GuestExecutionSessionChunkReference Reference(
    GuestExecutionSessionChunkKind kind, uint32_t ordinal,
    uint64_t first_sequence, uint64_t last_sequence, uint32_t count,
    const std::vector<uint8_t>& bytes) {
  GuestExecutionSessionChunkReference reference;
  reference.kind = kind;
  reference.ordinal = ordinal;
  reference.first_event_sequence = first_sequence;
  reference.last_event_sequence = last_sequence;
  reference.record_count = count;
  reference.encoded_size = bytes.size();
  reference.encoded_sha256 = GuestExecutionSessionCodec::HashBytes(bytes);
  return reference;
}

bool PayloadEquals(std::span<const uint8_t> payload,
                   const std::vector<uint8_t>& expected) {
  return payload.size() == expected.size() &&
         std::equal(payload.begin(), payload.end(), expected.begin());
}

// The coverage variant swaps only the kind of event five, so every sequence,
// owner and count the other cases assert stays identical.
GuestExecutionSessionBundle MakeTapeBundle(bool instruction_coverage = false) {
  GuestExecutionSessionBundle bundle;
  constexpr uint64_t kEpoch = 0x1122334455667788ull;
  const auto initial_0 = AddBlob(&bundle, Bytes(64, 1));
  const auto initial_1 = AddBlob(&bundle, Bytes(64, 2));
  const auto final_0 = AddBlob(&bundle, Bytes(64, 3));
  const auto final_1 = AddBlob(&bundle, Bytes(64, 4));
  const auto corpus_0 = AddBlob(&bundle, Bytes(128, 5));
  const auto corpus_1 = AddBlob(&bundle, Bytes(128, 6));
  const auto segment_0 = AddBlob(&bundle, Bytes(96, 7));
  const auto segment_1 = AddBlob(&bundle, Bytes(96, 8));
  const auto external_value = AddBlob(&bundle, Bytes(8, 9));

  GuestExecutionSessionCheckpointChunk initial;
  initial.session_epoch = kEpoch;
  initial.ordinal = 0;
  initial.checkpoint.global_sequence = 0;
  initial.checkpoint.thread_states = {{0, 64, initial_0}, {1, 64, initial_1}};

  GuestExecutionSessionEventChunk events;
  events.session_epoch = kEpoch;
  events.ordinal = 1;
  events.events.push_back(
      Event(1, 0, GuestExecutionSessionEventKind::kSegmentBegin,
            GuestExecutionSessionEventDisposition::kValidateDeterministic));
  GuestExecutionSessionEvent external =
      Event(2, 0, GuestExecutionSessionEventKind::kKernelExport,
            GuestExecutionSessionEventDisposition::kReplayCaptured);
  external.guest_address = 0x82000080;
  external.payload_kind =
      GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
  external.payload_size = 8;
  external.payload_sha256 = external_value;
  events.events.push_back(external);
  events.events.push_back(
      Event(3, 0, GuestExecutionSessionEventKind::kSegmentEnd,
            GuestExecutionSessionEventDisposition::kValidateDeterministic));
  events.events.push_back(
      Event(4, 1, GuestExecutionSessionEventKind::kSegmentBegin,
            GuestExecutionSessionEventDisposition::kValidateDeterministic));
  if (instruction_coverage) {
    GuestExecutionSessionEvent coverage =
        Event(5, 1, GuestExecutionSessionEventKind::kInstructionCoverage,
              GuestExecutionSessionEventDisposition::kValidateDeterministic);
    coverage.guest_instruction_delta = kCoverageInstructionDelta;
    events.events.push_back(coverage);
  } else {
    events.events.push_back(
        Event(5, 1, GuestExecutionSessionEventKind::kSynchronization,
              GuestExecutionSessionEventDisposition::kValidateDeterministic));
  }
  events.events.push_back(
      Event(6, 1, GuestExecutionSessionEventKind::kSegmentEnd,
            GuestExecutionSessionEventDisposition::kValidateDeterministic));
  events.events.push_back(
      Event(7, kGuestExecutionSessionNoThread,
            GuestExecutionSessionEventKind::kBoundaryRequest,
            GuestExecutionSessionEventDisposition::kReplayCaptured));
  events.events.push_back(
      Event(8, kGuestExecutionSessionNoThread,
            GuestExecutionSessionEventKind::kBoundaryHeld,
            GuestExecutionSessionEventDisposition::kReplayCaptured));

  GuestExecutionSessionCheckpointChunk final;
  final.session_epoch = kEpoch;
  final.ordinal = 2;
  final.checkpoint.global_sequence = 8;
  final.checkpoint.thread_states = {{0, 64, final_0}, {1, 64, final_1}};

  bundle.chunks.resize(3);
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      events, &bundle.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final, &bundle.chunks[2], &error));

  GuestExecutionSessionManifest& manifest = bundle.manifest;
  manifest.session_epoch = kEpoch;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 8;
  manifest.capture_start_tick = 100;
  manifest.stop_request_tick = 700;
  manifest.capture_end_tick = 800;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Identity(0x10);
  manifest.replay_config_sha256 = Identity(0x20);
  manifest.title_identity_sha256 = Identity(0x30);
  manifest.module_identity_sha256 = Identity(0x40);
  manifest.accepted_segment_count = 2;
  manifest.accepted_event_count = 8;
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = 7;
  manifest.stop_request_accepted_segment_count = 2;
  manifest.stop_request_guest_instruction_count =
      instruction_coverage ? kCoverageInstructionDelta : 0;
  manifest.maximum_stop_tail_event_count = 4;
  manifest.maximum_stop_tail_guest_instruction_count = 4;
  manifest.maximum_stop_tail_ticks = 200;

  GuestExecutionSessionParticipant participant_0;
  participant_0.ordinal = 0;
  participant_0.guest_thread_id = 11;
  participant_0.capture_instance_id = 0x100;
  participant_0.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  participant_0.first_event_sequence = 1;
  participant_0.last_event_sequence = 3;
  participant_0.held_after_event_sequence = 7;
  participant_0.initial_state_size = 64;
  participant_0.initial_state_sha256 = initial_0;
  manifest.participants.push_back(participant_0);
  GuestExecutionSessionParticipant participant_1 = participant_0;
  participant_1.ordinal = 1;
  participant_1.guest_thread_id = 12;
  participant_1.capture_instance_id = 0x101;
  participant_1.first_event_sequence = 4;
  participant_1.last_event_sequence = 6;
  participant_1.initial_state_sha256 = initial_1;
  manifest.participants.push_back(participant_1);

  manifest.segments.push_back(
      {0, 0, 1, 3, 0x82000000, 0x8200003C, corpus_0, segment_0});
  manifest.segments.push_back(
      {1, 1, 4, 6, 0x83000000, 0x8300003C, corpus_1, segment_1});
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                bundle.chunks[0]));
  manifest.chunks.push_back(Reference(GuestExecutionSessionChunkKind::kEvents,
                                      1, 1, 8, 8, bundle.chunks[1]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 2, 8, 8, 1,
                bundle.chunks[2]));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  return bundle;
}

std::unique_ptr<GuestExecutionReplayTape> CreateTape(
    const GuestExecutionSessionBundle& bundle) {
  std::string error;
  auto tape = GuestExecutionReplayTape::Create(bundle, &error);
  REQUIRE(tape);
  REQUIRE(error.empty());
  REQUIRE(tape->Start(&error));
  REQUIRE(error.empty());
  return tape;
}

GuestExecutionReplayTurn Acquire(GuestExecutionReplayTape& tape,
                                 uint32_t owner) {
  GuestExecutionReplayTurn turn;
  std::string error;
  REQUIRE(tape.Acquire(owner, 1s, &turn, &error) ==
          GuestExecutionReplayAcquireResult::kAcquired);
  REQUIRE(error.empty());
  REQUIRE(turn);
  return turn;
}

void CommitExpected(GuestExecutionReplayTape& tape,
                    const GuestExecutionReplayTurn& turn) {
  std::string error;
  if (turn.event.disposition ==
      GuestExecutionSessionEventDisposition::kReplayCaptured) {
    REQUIRE(tape.CommitCaptured(turn, &error));
  } else {
    REQUIRE(tape.CommitDeterministic(turn, turn.event, &error));
  }
  REQUIRE(error.empty());
}

}  // namespace

TEST_CASE("Guest execution replay tape orders persistent participants",
          "[guest-execution-replay]") {
  GuestExecutionSessionBundle bundle = MakeTapeBundle();
  auto tape = CreateTape(bundle);

  auto participant_1 = std::async(std::launch::async, [&]() {
    GuestExecutionReplayTurn turn;
    std::string error;
    const auto result = tape->Acquire(1, 1s, &turn, &error);
    return std::make_pair(result, std::move(turn));
  });
  REQUIRE(participant_1.wait_for(20ms) == std::future_status::timeout);

  GuestExecutionReplayTurn turn = Acquire(*tape, 0);
  REQUIRE(turn.event.global_sequence == 1);
  CommitExpected(*tape, turn);
  turn = Acquire(*tape, 0);
  REQUIRE(turn.event.global_sequence == 2);
  REQUIRE(PayloadEquals(turn.payload, Bytes(8, 9)));
  CommitExpected(*tape, turn);
  turn = Acquire(*tape, 0);
  REQUIRE(turn.event.global_sequence == 3);
  CommitExpected(*tape, turn);

  REQUIRE(participant_1.wait_for(1s) == std::future_status::ready);
  auto [result, participant_1_turn] = participant_1.get();
  REQUIRE(result == GuestExecutionReplayAcquireResult::kAcquired);
  REQUIRE(participant_1_turn.event.global_sequence == 4);
  CommitExpected(*tape, participant_1_turn);
  turn = Acquire(*tape, 1);
  REQUIRE(turn.event.global_sequence == 5);
  CommitExpected(*tape, turn);
  turn = Acquire(*tape, 1);
  REQUIRE(turn.event.global_sequence == 6);
  CommitExpected(*tape, turn);
  turn = Acquire(*tape, kGuestExecutionSessionNoThread);
  REQUIRE(turn.event.global_sequence == 7);
  CommitExpected(*tape, turn);
  turn = Acquire(*tape, kGuestExecutionSessionNoThread);
  REQUIRE(turn.event.global_sequence == 8);
  CommitExpected(*tape, turn);

  const GuestExecutionReplayTapeStatus status = tape->status();
  REQUIRE(status.state == GuestExecutionReplayTapeState::kComplete);
  REQUIRE(status.consumed_event_count == 8);
  REQUIRE(status.total_event_count == 8);
  REQUIRE_FALSE(status.has_active_lease);

  GuestExecutionReplayTurn complete_turn;
  std::string error;
  REQUIRE(tape->Acquire(0, 1s, &complete_turn, &error) ==
          GuestExecutionReplayAcquireResult::kComplete);
  REQUIRE_FALSE(complete_turn);
}

TEST_CASE("Guest execution replay tape fails closed on divergence",
          "[guest-execution-replay]") {
  SECTION("deterministic mismatch") {
    auto tape = CreateTape(MakeTapeBundle());
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    GuestExecutionSessionEvent observed = turn.event;
    observed.guest_address = 4;
    std::string error;
    REQUIRE_FALSE(tape->CommitDeterministic(turn, observed, &error));
    REQUIRE(tape->status().state == GuestExecutionReplayTapeState::kRejected);
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kDeterministicMismatch);
  }

  SECTION("wrong disposition") {
    auto tape = CreateTape(MakeTapeBundle());
    const GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    std::string error;
    REQUIRE_FALSE(tape->CommitCaptured(turn, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kDispositionMismatch);
  }

  SECTION("altered lease") {
    auto tape = CreateTape(MakeTapeBundle());
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    ++turn.event.global_sequence;
    std::string error;
    REQUIRE_FALSE(tape->CommitDeterministic(turn, turn.event, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kLeaseMismatch);
  }

  SECTION("out-of-order owner timeout") {
    auto tape = CreateTape(MakeTapeBundle());
    GuestExecutionReplayTurn turn;
    std::string error;
    REQUIRE(tape->Acquire(1, 10ms, &turn, &error) ==
            GuestExecutionReplayAcquireResult::kRejected);
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kTimeout);
  }
}

TEST_CASE("Guest execution replay tape cancellation wakes waiters",
          "[guest-execution-replay]") {
  auto tape = CreateTape(MakeTapeBundle());
  auto waiter = std::async(std::launch::async, [&]() {
    GuestExecutionReplayTurn turn;
    std::string error;
    return tape->Acquire(1, 2s, &turn, &error);
  });
  REQUIRE(waiter.wait_for(20ms) == std::future_status::timeout);
  tape->Cancel("test cancellation");
  REQUIRE(waiter.wait_for(1s) == std::future_status::ready);
  REQUIRE(waiter.get() == GuestExecutionReplayAcquireResult::kRejected);
  REQUIRE(tape->status().rejection ==
          GuestExecutionReplayTapeRejection::kCancelled);
  REQUIRE(tape->status().message == "test cancellation");
}

TEST_CASE("Guest execution replay tape borrows payloads from its own storage",
          "[guest-execution-replay]") {
  const std::vector<uint8_t> expected = Bytes(8, 9);

  SECTION("the borrowed span aliases the tape for the whole lease") {
    auto tape = CreateTape(MakeTapeBundle());
    CommitExpected(*tape, Acquire(*tape, 0));
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    REQUIRE(turn.event.global_sequence == 2);
    REQUIRE(turn.payload.size() == expected.size());
    REQUIRE(PayloadEquals(turn.payload, expected));
    const uint8_t* borrowed = turn.payload.data();
    REQUIRE(borrowed != nullptr);
    REQUIRE(borrowed != expected.data());
    REQUIRE(tape->status().has_active_lease);
    REQUIRE(turn.payload.data() == borrowed);
    REQUIRE(PayloadEquals(turn.payload, expected));
    CommitExpected(*tape, turn);
    turn = Acquire(*tape, 0);
    REQUIRE(turn.event.global_sequence == 3);
    REQUIRE(turn.payload.empty());
  }

  SECTION("releasing a turn drops the borrowed view and the lease") {
    auto tape = CreateTape(MakeTapeBundle());
    CommitExpected(*tape, Acquire(*tape, 0));
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    REQUIRE_FALSE(turn.payload.empty());
    turn.Reset();
    REQUIRE_FALSE(turn);
    REQUIRE(turn.payload.empty());
    std::string error;
    REQUIRE_FALSE(tape->CommitCaptured(turn, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kLeaseMismatch);
  }

  SECTION("a committed turn is stale") {
    auto tape = CreateTape(MakeTapeBundle());
    const GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    CommitExpected(*tape, turn);
    std::string error;
    REQUIRE_FALSE(tape->CommitDeterministic(turn, turn.event, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kLeaseMismatch);
  }
}

TEST_CASE("Guest execution replay tape validates leases by identity",
          "[guest-execution-replay]") {
  SECTION("lease identifier mismatch") {
    auto tape = CreateTape(MakeTapeBundle());
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    ++turn.lease_id;
    std::string error;
    REQUIRE_FALSE(tape->CommitDeterministic(turn, turn.event, &error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kLeaseMismatch);
  }

  SECTION("lease identifiers are monotonic and never reused") {
    auto tape = CreateTape(MakeTapeBundle());
    const GuestExecutionReplayTurn first = Acquire(*tape, 0);
    CommitExpected(*tape, first);
    GuestExecutionReplayTurn second = Acquire(*tape, 0);
    REQUIRE(second.lease_id > first.lease_id);
    second.lease_id = first.lease_id;
    std::string error;
    REQUIRE_FALSE(tape->CommitCaptured(second, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kLeaseMismatch);
  }

  SECTION("a substituted payload view of equal bytes is still rejected") {
    auto tape = CreateTape(MakeTapeBundle());
    CommitExpected(*tape, Acquire(*tape, 0));
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    const std::vector<uint8_t> forged = Bytes(8, 9);
    REQUIRE(PayloadEquals(turn.payload, forged));
    turn.payload = std::span<const uint8_t>(forged);
    std::string error;
    REQUIRE_FALSE(tape->CommitCaptured(turn, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kLeaseMismatch);
  }
}

TEST_CASE("Guest execution replay tape wakes the owning waiter",
          "[guest-execution-replay]") {
  auto tape = CreateTape(MakeTapeBundle());
  auto participant_1 = std::async(std::launch::async, [&]() {
    GuestExecutionReplayTurn turn;
    std::string error;
    const auto result = tape->Acquire(1, 5s, &turn, &error);
    return std::make_pair(result, std::move(turn));
  });
  auto coordinator = std::async(std::launch::async, [&]() {
    GuestExecutionReplayTurn turn;
    std::string error;
    return tape->Acquire(kGuestExecutionSessionNoThread, 5s, &turn, &error);
  });
  REQUIRE(participant_1.wait_for(20ms) == std::future_status::timeout);
  REQUIRE(coordinator.wait_for(20ms) == std::future_status::timeout);

  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    REQUIRE(turn.event.global_sequence == sequence);
    CommitExpected(*tape, turn);
  }

  REQUIRE(participant_1.wait_for(2s) == std::future_status::ready);
  auto [result, owner_turn] = participant_1.get();
  REQUIRE(result == GuestExecutionReplayAcquireResult::kAcquired);
  REQUIRE(owner_turn.event.global_sequence == 4);
  REQUIRE(coordinator.wait_for(50ms) == std::future_status::timeout);

  tape->Cancel("test broadcast");
  REQUIRE(coordinator.wait_for(2s) == std::future_status::ready);
  REQUIRE(coordinator.get() == GuestExecutionReplayAcquireResult::kRejected);
  REQUIRE(tape->status().rejection ==
          GuestExecutionReplayTapeRejection::kCancelled);
}

TEST_CASE("Guest execution replay tape types ordering-only commits",
          "[guest-execution-replay]") {
  SECTION("instruction coverage advances the cursor alone") {
    auto tape = CreateTape(MakeTapeBundle(true));
    for (int index = 0; index < 3; ++index) {
      CommitExpected(*tape, Acquire(*tape, 0));
    }
    CommitExpected(*tape, Acquire(*tape, 1));
    GuestExecutionReplayTurn turn = Acquire(*tape, 1);
    REQUIRE(turn.event.global_sequence == 5);
    REQUIRE(turn.event.kind ==
            GuestExecutionSessionEventKind::kInstructionCoverage);
    REQUIRE(turn.payload.empty());
    std::string error;
    REQUIRE(tape->CommitOrderingOnly(turn, &error));
    REQUIRE(error.empty());
    const GuestExecutionReplayTapeStatus status = tape->status();
    REQUIRE(status.state == GuestExecutionReplayTapeState::kRunning);
    REQUIRE(status.rejection == GuestExecutionReplayTapeRejection::kNone);
    REQUIRE(status.consumed_event_count == 5);
    REQUIRE(status.next_event_sequence == 6);
    REQUIRE_FALSE(status.has_active_lease);
  }

  SECTION("a deterministic control event is not admitted") {
    auto tape = CreateTape(MakeTapeBundle(true));
    const GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    REQUIRE(turn.event.kind == GuestExecutionSessionEventKind::kSegmentBegin);
    std::string error;
    REQUIRE_FALSE(tape->CommitOrderingOnly(turn, &error));
    REQUIRE(error ==
            "ordering-only commit is admitted only for instruction coverage");
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kOrderingOnlyMismatch);
    REQUIRE(tape->status().consumed_event_count == 0);
  }

  SECTION("a captured event carrying a payload is not admitted") {
    auto tape = CreateTape(MakeTapeBundle(true));
    CommitExpected(*tape, Acquire(*tape, 0));
    const GuestExecutionReplayTurn turn = Acquire(*tape, 0);
    REQUIRE(turn.event.kind == GuestExecutionSessionEventKind::kKernelExport);
    REQUIRE_FALSE(turn.payload.empty());
    std::string error;
    REQUIRE_FALSE(tape->CommitOrderingOnly(turn, &error));
    REQUIRE(tape->status().rejection ==
            GuestExecutionReplayTapeRejection::kOrderingOnlyMismatch);
    REQUIRE(tape->status().consumed_event_count == 1);
  }
}

TEST_CASE("Guest execution replay tape validates the complete bundle",
          "[guest-execution-replay]") {
  GuestExecutionSessionBundle bundle = MakeTapeBundle();
  bundle.content_blobs.pop_back();
  std::string error;
  REQUIRE_FALSE(GuestExecutionReplayTape::Create(bundle, &error));
  REQUIRE_FALSE(error.empty());
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
