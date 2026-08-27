/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {

namespace {

GuestExecutionSessionSha256 Digest(uint8_t seed) {
  GuestExecutionSessionSha256 digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(seed + i);
  }
  return digest;
}

GuestExecutionSessionEvent PlainEvent(uint64_t sequence,
                                      GuestExecutionSessionEventKind kind,
                                      uint32_t thread_ordinal = 0) {
  GuestExecutionSessionEvent event;
  event.global_sequence = sequence;
  event.thread_ordinal = thread_ordinal;
  event.kind = kind;
  return event;
}

GuestExecutionSessionEventChunk MakeEventChunk() {
  GuestExecutionSessionEventChunk chunk;
  chunk.session_epoch = 0x123456789ABCDEF0ull;
  chunk.ordinal = 1;
  GuestExecutionSessionEvent begin =
      PlainEvent(1, GuestExecutionSessionEventKind::kSegmentBegin);
  begin.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  chunk.events.push_back(begin);

  GuestExecutionSessionEvent external =
      PlainEvent(2, GuestExecutionSessionEventKind::kKernelExport);
  external.guest_address = 0x82000080;
  external.payload_kind =
      GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
  external.payload_size = 8;
  external.payload_sha256 = Digest(0x20);
  chunk.events.push_back(external);

  GuestExecutionSessionEvent mutation =
      PlainEvent(3, GuestExecutionSessionEventKind::kMemoryMutation,
                 kGuestExecutionSessionNoThread);
  mutation.mutation_source = GuestExecutionSessionMutationSource::kGpu;
  mutation.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
  mutation.guest_address = 0x2000;
  mutation.byte_count = 4;
  mutation.payload_size = 4;
  mutation.payload_sha256 = Digest(0x30);
  chunk.events.push_back(mutation);

  GuestExecutionSessionEvent coverage =
      PlainEvent(4, GuestExecutionSessionEventKind::kInstructionCoverage);
  coverage.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  coverage.guest_instruction_delta = 10;
  chunk.events.push_back(coverage);

  GuestExecutionSessionEvent marker =
      PlainEvent(5, GuestExecutionSessionEventKind::kGuestMarker,
                 kGuestExecutionSessionNoThread);
  marker.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
  marker.marker_identity = 0x99;
  chunk.events.push_back(marker);

  GuestExecutionSessionEvent end =
      PlainEvent(6, GuestExecutionSessionEventKind::kSegmentEnd);
  end.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  chunk.events.push_back(end);

  chunk.events.push_back(
      PlainEvent(7, GuestExecutionSessionEventKind::kBoundaryRequest,
                 kGuestExecutionSessionNoThread));
  chunk.events.push_back(
      PlainEvent(8, GuestExecutionSessionEventKind::kBoundaryHeld,
                 kGuestExecutionSessionNoThread));
  return chunk;
}

GuestExecutionSessionCheckpointChunk MakeCheckpointChunk(uint32_t ordinal,
                                                         uint64_t sequence,
                                                         uint8_t seed) {
  GuestExecutionSessionCheckpointChunk chunk;
  chunk.session_epoch = 0x123456789ABCDEF0ull;
  chunk.ordinal = ordinal;
  chunk.checkpoint.global_sequence = sequence;
  chunk.checkpoint.thread_states.push_back({0, 2644, Digest(seed)});
  chunk.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestPage, 0x1000, 4096,
       Digest(static_cast<uint8_t>(seed + 1))});
  chunk.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestCode, 0x4000, 0x100,
       Digest(static_cast<uint8_t>(seed + 2))});
  return chunk;
}

GuestExecutionSessionChunkReference ReferenceFor(
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

struct SessionFixture {
  GuestExecutionSessionManifest manifest;
  GuestExecutionSessionEventChunk events;
  GuestExecutionSessionCheckpointChunk initial_checkpoint;
  GuestExecutionSessionCheckpointChunk final_checkpoint;
  GuestExecutionSessionCodeCorpusChunk code_corpus;
  std::vector<std::vector<uint8_t>> chunks;
  std::vector<uint8_t> encoded_manifest;
};

SessionFixture MakeSessionFixture() {
  SessionFixture fixture;
  fixture.events = MakeEventChunk();
  fixture.initial_checkpoint = MakeCheckpointChunk(0, 0, 0x40);
  fixture.final_checkpoint = MakeCheckpointChunk(2, 8, 0x50);

  std::string error;
  fixture.chunks.resize(3);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture.initial_checkpoint, &fixture.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &fixture.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture.final_checkpoint, &fixture.chunks[2], &error));

  GuestExecutionSessionManifest& manifest = fixture.manifest;
  manifest.session_epoch = fixture.events.session_epoch;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 8;
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 900;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Digest(1);
  manifest.replay_config_sha256 = Digest(2);
  manifest.title_identity_sha256 = Digest(3);
  manifest.module_identity_sha256 = Digest(4);
  manifest.accepted_segment_count = 1;
  manifest.accepted_event_count = 8;
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = 7;
  manifest.stop_request_tick = 500;
  manifest.stop_request_accepted_segment_count = 1;
  manifest.stop_request_guest_instruction_count = 10;
  manifest.maximum_stop_tail_event_count = 16;
  manifest.maximum_stop_tail_guest_instruction_count = 64;
  manifest.maximum_stop_tail_ticks = 1000;
  GuestExecutionSessionParticipant participant;
  participant.ordinal = 0;
  participant.guest_thread_id = 7;
  participant.capture_instance_id = 0x100;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  participant.first_event_sequence = 1;
  participant.last_event_sequence = 6;
  participant.held_after_event_sequence = 7;
  participant.initial_state_size = 2644;
  participant.initial_state_sha256 =
      fixture.initial_checkpoint.checkpoint.thread_states[0].sha256;
  manifest.participants.push_back(participant);
  manifest.segments.push_back(
      {0, 0, 1, 6, 0x82000000, 0x820000FC, Digest(5), Digest(6)});
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]));
  manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 8, 8, fixture.chunks[1]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2, 8, 8, 1,
                   fixture.chunks[2]));
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      manifest, &fixture.encoded_manifest, &error));
  return fixture;
}

void ReplaceEventChunk(SessionFixture* fixture) {
  const auto reference = std::find_if(
      fixture->manifest.chunks.begin(), fixture->manifest.chunks.end(),
      [](const GuestExecutionSessionChunkReference& chunk) {
        return chunk.kind == GuestExecutionSessionChunkKind::kEvents;
      });
  REQUIRE(reference != fixture->manifest.chunks.end());
  const size_t index =
      static_cast<size_t>(reference - fixture->manifest.chunks.begin());
  fixture->events.ordinal = static_cast<uint32_t>(index);
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture->events, &fixture->chunks[index], &error));
  fixture->manifest.chunks[index] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, fixture->events.ordinal,
      fixture->events.events.front().global_sequence,
      fixture->events.events.back().global_sequence,
      static_cast<uint32_t>(fixture->events.events.size()),
      fixture->chunks[index]);
}

SessionFixture MakeContinuousSessionFixture() {
  SessionFixture fixture = MakeSessionFixture();
  fixture.events.events[0].kind =
      GuestExecutionSessionEventKind::kOuterHostCallBegin;
  fixture.events.events[5].kind =
      GuestExecutionSessionEventKind::kOuterHostCallEnd;
  fixture.manifest.accepted_segment_count = 0;
  fixture.manifest.stop_request_accepted_segment_count = 0;
  fixture.manifest.segments.clear();
  fixture.code_corpus.session_epoch = fixture.manifest.session_epoch;
  fixture.code_corpus.ordinal = 1;
  fixture.code_corpus.code_corpus_sha256 = Digest(0x70);
  fixture.events.ordinal = 2;
  fixture.final_checkpoint.ordinal = 3;
  std::string error;
  std::vector<std::vector<uint8_t>> chunks(4);
  chunks[0] = std::move(fixture.chunks[0]);
  REQUIRE(GuestExecutionSessionCodec::EncodeCodeCorpusChunk(
      fixture.code_corpus, &chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                       &chunks[2], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture.final_checkpoint, &chunks[3], &error));
  fixture.chunks = std::move(chunks);
  fixture.manifest.chunks.clear();
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]));
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCodeCorpus, 1, 0, 0, 1,
                   fixture.chunks[1]));
  fixture.manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 2, 1, 8, 8, fixture.chunks[2]));
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 3, 8, 8, 1,
                   fixture.chunks[3]));
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(fixture.manifest,
                                                      fixture.chunks, &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      fixture.manifest, &fixture.encoded_manifest, &error));
  return fixture;
}
void ExtendHeldBoundary(SessionFixture* fixture,
                        GuestExecutionSessionEvent tail_event) {
  REQUIRE(fixture->events.events.back().kind ==
          GuestExecutionSessionEventKind::kBoundaryHeld);
  const uint64_t old_last = fixture->manifest.last_event_sequence;
  tail_event.global_sequence = old_last;
  fixture->events.events.back() = tail_event;
  fixture->events.events.push_back(
      PlainEvent(old_last + 1, GuestExecutionSessionEventKind::kBoundaryHeld,
                 kGuestExecutionSessionNoThread));
  ++fixture->manifest.last_event_sequence;
  ++fixture->manifest.accepted_event_count;
  ReplaceEventChunk(fixture);

  fixture->final_checkpoint.checkpoint.global_sequence = old_last + 1;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture->final_checkpoint, &fixture->chunks[2], &error));
  fixture->manifest.chunks[2] =
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint,
                   fixture->final_checkpoint.ordinal, old_last + 1,
                   old_last + 1, 1, fixture->chunks[2]);
}

void MakeEventCanonicalWithoutPayload(GuestExecutionSessionEvent* event) {
  event->thread_ordinal = 0;
  event->disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  event->mutation_source = GuestExecutionSessionMutationSource::kNone;
  event->guest_address = 0;
  event->byte_count = 0;
  event->payload_kind = GuestExecutionSessionPayloadKind::kNone;
  event->payload_size = 0;
  event->payload_sha256 = {};
  event->marker_source = GuestExecutionSessionMarkerSource::kNone;
  event->marker_identity = 0;
  event->guest_instruction_delta = 0;
}

SessionFixture MakeGuestMarkerBoundaryFixture() {
  SessionFixture fixture = MakeSessionFixture();
  GuestExecutionSessionEvent& end = fixture.events.events[4];
  MakeEventCanonicalWithoutPayload(&end);
  end.kind = GuestExecutionSessionEventKind::kSegmentEnd;
  GuestExecutionSessionEvent& marker = fixture.events.events[5];
  MakeEventCanonicalWithoutPayload(&marker);
  marker.kind = GuestExecutionSessionEventKind::kGuestMarker;
  marker.thread_ordinal = kGuestExecutionSessionNoThread;
  marker.disposition = GuestExecutionSessionEventDisposition::kReplayCaptured;
  marker.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
  marker.marker_identity = 0x99;
  ReplaceEventChunk(&fixture);
  fixture.manifest.boundary.kind =
      GuestExecutionSessionBoundaryKind::kGuestMarkerCount;
  fixture.manifest.boundary.value = 1;
  fixture.manifest.boundary.marker_source =
      GuestExecutionSessionMarkerSource::kPm4Swap;
  fixture.manifest.boundary.marker_identity = 0x99;
  fixture.manifest.stop_reason =
      GuestExecutionSessionStopReason::kRequestedBoundary;
  fixture.manifest.stop_request_matching_guest_marker_count = 1;
  fixture.manifest.segments[0].last_event_sequence = 5;
  fixture.manifest.participants[0].last_event_sequence = 5;
  return fixture;
}

SessionFixture MakeInstructionBoundaryFixture() {
  SessionFixture fixture = MakeSessionFixture();
  GuestExecutionSessionEvent& end = fixture.events.events[4];
  MakeEventCanonicalWithoutPayload(&end);
  end.kind = GuestExecutionSessionEventKind::kSegmentEnd;
  GuestExecutionSessionEvent& coverage = fixture.events.events[5];
  MakeEventCanonicalWithoutPayload(&coverage);
  coverage.kind = GuestExecutionSessionEventKind::kInstructionCoverage;
  coverage.guest_instruction_delta = 5;
  ReplaceEventChunk(&fixture);
  fixture.manifest.boundary.kind =
      GuestExecutionSessionBoundaryKind::kGuestInstructionCount;
  fixture.manifest.boundary.value = 15;
  fixture.manifest.stop_reason =
      GuestExecutionSessionStopReason::kRequestedBoundary;
  fixture.manifest.stop_request_guest_instruction_count = 15;
  fixture.manifest.segments[0].last_event_sequence = 5;
  return fixture;
}

SessionFixture MakeSegmentBoundaryFixture() {
  SessionFixture fixture = MakeSessionFixture();
  fixture.manifest.boundary.kind =
      GuestExecutionSessionBoundaryKind::kSegmentCount;
  fixture.manifest.boundary.value = 1;
  fixture.manifest.stop_reason =
      GuestExecutionSessionStopReason::kRequestedBoundary;
  fixture.manifest.stop_request_tick = 800;
  return fixture;
}

SessionFixture MakeDurationBoundaryFixture() {
  SessionFixture fixture = MakeSessionFixture();
  fixture.manifest.boundary.kind =
      GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds;
  fixture.manifest.boundary.value = 333333334;
  fixture.manifest.capture_tick_frequency = 3;
  fixture.manifest.stop_reason =
      GuestExecutionSessionStopReason::kRequestedBoundary;
  fixture.manifest.stop_request_tick = 102;
  return fixture;
}

SessionFixture MakeAsynchronousRendezvousFixture() {
  SessionFixture fixture;
  constexpr uint64_t kEpoch = 0x123456789ABCDEF0ull;
  fixture.events.session_epoch = kEpoch;
  fixture.events.ordinal = 1;

  auto add_participant_control = [&](uint64_t sequence, uint32_t participant,
                                     GuestExecutionSessionEventKind kind) {
    GuestExecutionSessionEvent event = PlainEvent(sequence, kind, participant);
    MakeEventCanonicalWithoutPayload(&event);
    event.thread_ordinal = participant;
    fixture.events.events.push_back(event);
  };
  add_participant_control(1, 1, GuestExecutionSessionEventKind::kSegmentBegin);
  add_participant_control(2, 2,
                          GuestExecutionSessionEventKind::kOuterHostCallBegin);
  add_participant_control(3, 2, GuestExecutionSessionEventKind::kSegmentBegin);

  GuestExecutionSessionEvent coverage =
      PlainEvent(4, GuestExecutionSessionEventKind::kInstructionCoverage, 1);
  MakeEventCanonicalWithoutPayload(&coverage);
  coverage.thread_ordinal = 1;
  coverage.kind = GuestExecutionSessionEventKind::kInstructionCoverage;
  coverage.guest_instruction_delta = 7;
  fixture.events.events.push_back(coverage);
  coverage.global_sequence = 5;
  coverage.thread_ordinal = 2;
  coverage.guest_instruction_delta = 3;
  fixture.events.events.push_back(coverage);

  fixture.events.events.push_back(
      PlainEvent(6, GuestExecutionSessionEventKind::kBoundaryRequest,
                 kGuestExecutionSessionNoThread));

  coverage.global_sequence = 7;
  coverage.thread_ordinal = 1;
  coverage.guest_instruction_delta = 2;
  fixture.events.events.push_back(coverage);
  GuestExecutionSessionEvent marker =
      PlainEvent(8, GuestExecutionSessionEventKind::kGuestMarker,
                 kGuestExecutionSessionNoThread);
  marker.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
  marker.marker_identity = 0x55;
  fixture.events.events.push_back(marker);
  add_participant_control(9, 2, GuestExecutionSessionEventKind::kSegmentEnd);
  add_participant_control(10, 2,
                          GuestExecutionSessionEventKind::kOuterHostCallEnd);
  add_participant_control(11, 1, GuestExecutionSessionEventKind::kSegmentEnd);
  add_participant_control(12, 1,
                          GuestExecutionSessionEventKind::kJitSafepointArrival);
  fixture.events.events.push_back(
      PlainEvent(13, GuestExecutionSessionEventKind::kBoundaryHeld,
                 kGuestExecutionSessionNoThread));

  fixture.initial_checkpoint.session_epoch = kEpoch;
  fixture.initial_checkpoint.ordinal = 0;
  fixture.initial_checkpoint.checkpoint.global_sequence = 0;
  fixture.final_checkpoint.session_epoch = kEpoch;
  fixture.final_checkpoint.ordinal = 2;
  fixture.final_checkpoint.checkpoint.global_sequence = 13;
  for (uint32_t i = 0; i < 3; ++i) {
    fixture.initial_checkpoint.checkpoint.thread_states.push_back(
        {i, 2644, Digest(static_cast<uint8_t>(0x40 + i * 4))});
    fixture.final_checkpoint.checkpoint.thread_states.push_back(
        {i, 2644, Digest(static_cast<uint8_t>(0x60 + i * 4))});
  }
  fixture.initial_checkpoint.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestPage, 0x1000, 4096,
       Digest(0x80)});
  fixture.final_checkpoint.checkpoint.content =
      fixture.initial_checkpoint.checkpoint.content;

  std::string error;
  fixture.chunks.resize(3);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture.initial_checkpoint, &fixture.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &fixture.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture.final_checkpoint, &fixture.chunks[2], &error));

  GuestExecutionSessionManifest& manifest = fixture.manifest;
  manifest.session_epoch = kEpoch;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 13;
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 800;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Digest(1);
  manifest.replay_config_sha256 = Digest(2);
  manifest.title_identity_sha256 = Digest(3);
  manifest.module_identity_sha256 = Digest(4);
  manifest.accepted_segment_count = 2;
  manifest.accepted_event_count = 13;
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = 6;
  manifest.stop_request_tick = 500;
  manifest.stop_request_guest_instruction_count = 10;
  manifest.maximum_stop_tail_event_count = 6;
  manifest.maximum_stop_tail_guest_instruction_count = 2;
  manifest.maximum_stop_tail_ticks = 300;

  GuestExecutionSessionParticipant participant;
  participant.ordinal = 0;
  participant.guest_thread_id = 7;
  participant.capture_instance_id = 0x100;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  participant.held_after_event_sequence = 6;
  participant.initial_state_size = 2644;
  participant.initial_state_sha256 =
      fixture.initial_checkpoint.checkpoint.thread_states[0].sha256;
  manifest.participants.push_back(participant);

  participant.ordinal = 1;
  participant.guest_thread_id = 8;
  participant.capture_instance_id = 0x101;
  participant.initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kActive;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint;
  participant.first_event_sequence = 1;
  participant.last_event_sequence = 12;
  participant.held_after_event_sequence = 12;
  participant.initial_state_sha256 =
      fixture.initial_checkpoint.checkpoint.thread_states[1].sha256;
  manifest.participants.push_back(participant);

  participant.ordinal = 2;
  participant.guest_thread_id = 9;
  participant.capture_instance_id = 0x102;
  participant.initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kOutside;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn;
  participant.first_event_sequence = 2;
  participant.last_event_sequence = 10;
  participant.held_after_event_sequence = 10;
  participant.initial_state_sha256 =
      fixture.initial_checkpoint.checkpoint.thread_states[2].sha256;
  manifest.participants.push_back(participant);

  manifest.segments.push_back(
      {0, 1, 1, 11, 0x82000000, 0x820000FC, Digest(5), Digest(6)});
  manifest.segments.push_back(
      {1, 2, 3, 9, 0x83000000, 0x830000FC, Digest(7), Digest(8)});
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kEvents, 1, 1, 13, 13,
                   fixture.chunks[1]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2, 13, 13, 1,
                   fixture.chunks[2]));
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      manifest, &fixture.encoded_manifest, &error));
  return fixture;
}

void CheckBoundaryFixtureValid(const SessionFixture& fixture) {
  std::string error;
  std::vector<uint8_t> encoded_manifest;
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      fixture.manifest, &encoded_manifest, &error));
  GuestExecutionSessionManifest decoded_manifest;
  REQUIRE(GuestExecutionSessionCodec::DecodeManifest(
      encoded_manifest, &decoded_manifest, &error));
  CHECK(decoded_manifest == fixture.manifest);
  CHECK(GuestExecutionSessionCodec::ValidateSession(decoded_manifest,
                                                    fixture.chunks, &error));
}

void WriteU32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  REQUIRE(offset + 4 <= bytes->size());
  for (size_t i = 0; i < 4; ++i) {
    (*bytes)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void WriteU64(std::vector<uint8_t>* bytes, size_t offset, uint64_t value) {
  REQUIRE(offset + 8 <= bytes->size());
  for (size_t i = 0; i < 8; ++i) {
    (*bytes)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void RewritePayloadHash(std::vector<uint8_t>* bytes) {
  REQUIRE(bytes->size() >= GuestExecutionSessionCodec::kEnvelopeHeaderSize +
                               GuestExecutionSessionCodec::kEnvelopeFooterSize);
  const size_t payload_offset = GuestExecutionSessionCodec::kEnvelopeHeaderSize;
  const size_t payload_size = bytes->size() -
                              GuestExecutionSessionCodec::kEnvelopeHeaderSize -
                              GuestExecutionSessionCodec::kEnvelopeFooterSize;
  const GuestExecutionSessionSha256 digest =
      GuestExecutionSessionCodec::HashBytes(bytes->data() + payload_offset,
                                            payload_size);
  std::copy(digest.cbegin(), digest.cend(), bytes->begin() + 64);
  const size_t footer_offset =
      bytes->size() - GuestExecutionSessionCodec::kEnvelopeFooterSize;
  std::copy(digest.cbegin(), digest.cend(),
            bytes->begin() + footer_offset + 64);
}

}  // namespace

TEST_CASE("Guest execution session metadata round trips and binds all chunks",
          "[cpu]") {
  const SessionFixture fixture = MakeSessionFixture();
  std::string error;

  GuestExecutionSessionManifest decoded_manifest;
  REQUIRE(GuestExecutionSessionCodec::DecodeManifest(
      fixture.encoded_manifest, &decoded_manifest, &error));
  CHECK(decoded_manifest == fixture.manifest);

  GuestExecutionSessionCheckpointChunk decoded_initial;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      fixture.chunks[0], &decoded_initial, &error));
  CHECK(decoded_initial == fixture.initial_checkpoint);

  GuestExecutionSessionEventChunk decoded_events;
  REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(
      fixture.chunks[1], &decoded_events, &error));
  CHECK(decoded_events == fixture.events);

  GuestExecutionSessionCheckpointChunk decoded_final;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      fixture.chunks[2], &decoded_final, &error));
  CHECK(decoded_final == fixture.final_checkpoint);
  CHECK(GuestExecutionSessionCodec::ValidateSession(decoded_manifest,
                                                    fixture.chunks, &error));
}

TEST_CASE("Continuous session shape is inferred from canonical contents",
          "[cpu]") {
  std::string error;

  SECTION("valid zero-segment coverage round trips") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    REQUIRE(fixture.manifest.segments.empty());
    REQUIRE(fixture.manifest.chunks[1].kind ==
            GuestExecutionSessionChunkKind::kCodeCorpus);
    GuestExecutionSessionManifest decoded;
    REQUIRE(GuestExecutionSessionCodec::DecodeManifest(fixture.encoded_manifest,
                                                       &decoded, &error));
    REQUIRE(decoded == fixture.manifest);
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(decoded, fixture.chunks,
                                                        &error));
    GuestExecutionSessionCodeCorpusChunk decoded_corpus;
    REQUIRE(GuestExecutionSessionCodec::DecodeCodeCorpusChunk(
        fixture.chunks[1], &decoded_corpus, &error));
    REQUIRE(decoded_corpus == fixture.code_corpus);
  }

  SECTION("continuous version 2 without a corpus chunk fails closed") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    fixture.chunks.erase(fixture.chunks.begin() + 1);
    fixture.manifest.chunks.erase(fixture.manifest.chunks.begin() + 1);
    fixture.events.ordinal = 1;
    fixture.final_checkpoint.ordinal = 2;
    REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
        fixture.events, &fixture.chunks[1], &error));
    REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
        fixture.final_checkpoint, &fixture.chunks[2], &error));
    fixture.manifest.chunks[1] = ReferenceFor(
        GuestExecutionSessionChunkKind::kEvents, 1, 1, 8, 8, fixture.chunks[1]);
    fixture.manifest.chunks[2] =
        ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2, 8, 8, 1,
                     fixture.chunks[2]);
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("coverage without an outer guest call rejects") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    MakeEventCanonicalWithoutPayload(&fixture.events.events[0]);
    fixture.events.events[0].kind =
        GuestExecutionSessionEventKind::kSynchronization;
    MakeEventCanonicalWithoutPayload(&fixture.events.events[5]);
    fixture.events.events[5].kind =
        GuestExecutionSessionEventKind::kSynchronization;
    ReplaceEventChunk(&fixture);
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("coverage before its outer guest call rejects") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    MakeEventCanonicalWithoutPayload(&fixture.events.events[0]);
    fixture.events.events[0].kind =
        GuestExecutionSessionEventKind::kInstructionCoverage;
    fixture.events.events[0].guest_instruction_delta = 10;
    MakeEventCanonicalWithoutPayload(&fixture.events.events[3]);
    fixture.events.events[3].kind =
        GuestExecutionSessionEventKind::kOuterHostCallBegin;
    ReplaceEventChunk(&fixture);
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("coverage after its outer guest call rejects") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    MakeEventCanonicalWithoutPayload(&fixture.events.events[3]);
    fixture.events.events[3].kind =
        GuestExecutionSessionEventKind::kOuterHostCallEnd;
    MakeEventCanonicalWithoutPayload(&fixture.events.events[5]);
    fixture.events.events[5].kind =
        GuestExecutionSessionEventKind::kInstructionCoverage;
    fixture.events.events[5].guest_instruction_delta = 10;
    ReplaceEventChunk(&fixture);
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("coverage in a seeded active outer guest call is valid") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    fixture.manifest.participants[0].initial_outer_call_state =
        GuestExecutionSessionInitialOuterCallState::kActive;
    MakeEventCanonicalWithoutPayload(&fixture.events.events[0]);
    fixture.events.events[0].kind =
        GuestExecutionSessionEventKind::kSynchronization;
    ReplaceEventChunk(&fixture);
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("participant events without timed instruction progress reject") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    GuestExecutionSessionEvent& coverage = fixture.events.events[3];
    coverage.kind = GuestExecutionSessionEventKind::kSynchronization;
    coverage.guest_instruction_delta = 0;
    fixture.manifest.stop_request_guest_instruction_count = 0;
    ReplaceEventChunk(&fixture);
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("segment control without a segment reference rejects") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    fixture.events.events[0].kind =
        GuestExecutionSessionEventKind::kSegmentBegin;
    ReplaceEventChunk(&fixture);
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("segment-count boundary is illegal without segments") {
    SessionFixture fixture = MakeContinuousSessionFixture();
    fixture.manifest.boundary.kind =
        GuestExecutionSessionBoundaryKind::kSegmentCount;
    fixture.manifest.boundary.value = 1;
    fixture.manifest.stop_reason =
        GuestExecutionSessionStopReason::kRequestedBoundary;
    REQUIRE_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("legacy segmented fixture remains valid") {
    SessionFixture fixture = MakeSessionFixture();
    REQUIRE_FALSE(fixture.manifest.segments.empty());
    REQUIRE(std::none_of(
        fixture.manifest.chunks.cbegin(), fixture.manifest.chunks.cend(),
        [](const GuestExecutionSessionChunkReference& chunk) {
          return chunk.kind == GuestExecutionSessionChunkKind::kCodeCorpus;
        }));
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }
}
TEST_CASE("Guest execution boundary proofs round trip and validate", "[cpu]") {
  SECTION("manual stop between events") {
    CheckBoundaryFixtureValid(MakeSessionFixture());
  }
  SECTION("exact segment stop event") {
    CheckBoundaryFixtureValid(MakeSegmentBoundaryFixture());
  }
  SECTION("matching guest marker identity and count") {
    CheckBoundaryFixtureValid(MakeGuestMarkerBoundaryFixture());
  }
  SECTION("explicit guest instruction deltas") {
    CheckBoundaryFixtureValid(MakeInstructionBoundaryFixture());
  }
  SECTION("checked rational duration rounds up") {
    CheckBoundaryFixtureValid(MakeDurationBoundaryFixture());
  }
  SECTION("staggered asynchronous participant rendezvous") {
    CheckBoundaryFixtureValid(MakeAsynchronousRendezvousFixture());
  }
}

TEST_CASE("Guest execution session codecs reject every truncated prefix",
          "[cpu]") {
  const SessionFixture fixture = MakeSessionFixture();
  std::string error;
  GuestExecutionSessionManifest manifest;
  for (size_t cut = 0; cut < fixture.encoded_manifest.size(); ++cut) {
    INFO("manifest cut = " << cut);
    CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
        fixture.encoded_manifest.data(), cut, &manifest, &error));
    CHECK(manifest == GuestExecutionSessionManifest{});
  }

  for (size_t chunk_index = 0; chunk_index < fixture.chunks.size();
       ++chunk_index) {
    const std::vector<uint8_t>& encoded = fixture.chunks[chunk_index];
    for (size_t cut = 0; cut < encoded.size(); ++cut) {
      INFO("chunk = " << chunk_index << ", cut = " << cut);
      if (chunk_index == 1) {
        GuestExecutionSessionEventChunk chunk;
        CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
            encoded.data(), cut, &chunk, &error));
        CHECK(chunk == GuestExecutionSessionEventChunk{});
      } else {
        GuestExecutionSessionCheckpointChunk chunk;
        CHECK_FALSE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
            encoded.data(), cut, &chunk, &error));
        CHECK(chunk == GuestExecutionSessionCheckpointChunk{});
      }
    }
  }
}

TEST_CASE("Guest execution envelopes reject corruption and missing closure",
          "[cpu]") {
  const SessionFixture fixture = MakeSessionFixture();
  std::string error;
  GuestExecutionSessionEventChunk event_output;

  std::vector<uint8_t> corrupt = fixture.chunks[1];
  corrupt[0] ^= 1;
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      corrupt, &event_output, &error));

  corrupt = fixture.chunks[1];
  corrupt[GuestExecutionSessionCodec::kEnvelopeHeaderSize + 10] ^= 1;
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      corrupt, &event_output, &error));

  corrupt = fixture.chunks[1];
  corrupt[corrupt.size() - GuestExecutionSessionCodec::kEnvelopeFooterSize] ^=
      1;
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      corrupt, &event_output, &error));

  corrupt = fixture.chunks[1];
  corrupt.resize(corrupt.size() -
                 GuestExecutionSessionCodec::kEnvelopeFooterSize);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      corrupt, &event_output, &error));

  corrupt = fixture.chunks[1];
  corrupt.push_back(0);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      corrupt, &event_output, &error));

  corrupt = fixture.chunks[1];
  WriteU64(&corrupt, 56, std::numeric_limits<uint64_t>::max());
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      corrupt, &event_output, &error));
}

TEST_CASE("Guest execution decoders reject forged typed fields and counts",
          "[cpu]") {
  const SessionFixture fixture = MakeSessionFixture();
  std::string error;

  std::vector<uint8_t> forged = fixture.encoded_manifest;
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  GuestExecutionSessionManifest manifest_output;
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  // Version 1 used this participant word as reserved space and never produced
  // an accepted real-title artifact. Both envelope copies must remain rejected
  // after the version-2 initial outer-call state reset.
  forged = fixture.encoded_manifest;
  {
    const size_t footer_offset =
        forged.size() - GuestExecutionSessionCodec::kEnvelopeFooterSize;
    WriteU32(&forged, 8, 1);
    WriteU32(&forged, footer_offset + 8, 1);
  }
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.encoded_manifest;
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 176,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.encoded_manifest;
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 240,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.encoded_manifest;
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 244, 1);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.encoded_manifest;
  WriteU64(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 288, 0);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  const size_t participant_offset =
      GuestExecutionSessionCodec::kEnvelopeHeaderSize +
      GuestExecutionSessionCodec::kManifestPayloadHeaderSize;
  forged = fixture.encoded_manifest;
  WriteU64(&forged, participant_offset + 8, 0);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.encoded_manifest;
  WriteU32(&forged, participant_offset + 16, UINT32_MAX);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.encoded_manifest;
  WriteU32(&forged, participant_offset + 20, UINT32_MAX);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeManifest(
      forged, &manifest_output, &error));

  forged = fixture.chunks[1];
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 12,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  GuestExecutionSessionEventChunk event_output;
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  // Keep the envelope closure and hash internally consistent while making the
  // final event repeat its predecessor's sequence.
  forged = fixture.chunks[1];
  {
    const size_t footer_offset =
        forged.size() - GuestExecutionSessionCodec::kEnvelopeFooterSize;
    const size_t last_event_offset =
        GuestExecutionSessionCodec::kEnvelopeHeaderSize +
        (fixture.events.events.size() - 1) *
            GuestExecutionSessionCodec::kEventRecordSize;
    WriteU64(&forged, last_event_offset, 7);
    WriteU64(&forged, 48, 7);
    WriteU64(&forged, footer_offset + 48, 7);
    RewritePayloadHash(&forged);
  }
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[1];
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 24,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[1];
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 88,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[1];
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 92, 1);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[1];
  WriteU64(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 96, 1);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[1];
  WriteU64(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 104, 1);
  RewritePayloadHash(&forged);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[1];
  const size_t footer_offset =
      forged.size() - GuestExecutionSessionCodec::kEnvelopeFooterSize;
  WriteU32(&forged, 36, UINT32_MAX);
  WriteU32(&forged, footer_offset + 32, UINT32_MAX);
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeEventChunk(
      forged, &event_output, &error));

  forged = fixture.chunks[0];
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 8,
           UINT32_MAX);
  RewritePayloadHash(&forged);
  GuestExecutionSessionCheckpointChunk checkpoint_output;
  CHECK_FALSE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      forged, &checkpoint_output, &error));
}

TEST_CASE("Guest execution validators reject ordering and substitution",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  std::string error;

  std::swap(fixture.chunks[0], fixture.chunks[1]);
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));

  fixture = MakeSessionFixture();
  fixture.chunks.pop_back();
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));

  fixture = MakeSessionFixture();
  fixture.manifest.participants[0].initial_state_sha256 = Digest(0xE0);
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));

  fixture = MakeSessionFixture();
  fixture.manifest.chunks[1].first_event_sequence = 2;
  std::vector<uint8_t> output = {1, 2, 3};
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                         &output, &error));
  CHECK(output.empty());

  GuestExecutionSessionEventChunk events = MakeEventChunk();
  events.events[2].global_sequence = 9;
  output = {1, 2, 3};
  CHECK_FALSE(
      GuestExecutionSessionCodec::EncodeEventChunk(events, &output, &error));
  CHECK(output.empty());

  events = MakeEventChunk();
  events.events.back().global_sequence =
      events.events[events.events.size() - 2].global_sequence;
  output = {1, 2, 3};
  CHECK_FALSE(
      GuestExecutionSessionCodec::EncodeEventChunk(events, &output, &error));
  CHECK(output.empty());
}

TEST_CASE("Guest execution checkpoints close the exact preceding event prefix",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  GuestExecutionSessionCheckpointChunk stale_checkpoint =
      MakeCheckpointChunk(2, 2, 0x60);
  GuestExecutionSessionCheckpointChunk final_checkpoint =
      MakeCheckpointChunk(3, 8, 0x50);
  std::vector<uint8_t> encoded_stale;
  std::vector<uint8_t> encoded_final;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      stale_checkpoint, &encoded_stale, &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_checkpoint, &encoded_final, &error));
  fixture.chunks[2] = std::move(encoded_stale);
  fixture.chunks.push_back(std::move(encoded_final));

  fixture.manifest.chunks.clear();
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]));
  fixture.manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 8, 8, fixture.chunks[1]));
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2, 2, 2, 1,
                   fixture.chunks[2]));
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 3, 8, 8, 1,
                   fixture.chunks[3]));

  std::vector<uint8_t> output = {1, 2, 3};
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                         &output, &error));
  CHECK(output.empty());
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
}

TEST_CASE("Guest execution payload digests bind one type and byte size",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  GuestExecutionSessionEventChunk first_events;
  first_events.session_epoch = fixture.events.session_epoch;
  first_events.ordinal = 1;
  first_events.events.assign(fixture.events.events.cbegin(),
                             fixture.events.events.cbegin() + 2);
  GuestExecutionSessionEventChunk second_events;
  second_events.session_epoch = fixture.events.session_epoch;
  second_events.ordinal = 2;
  second_events.events.assign(fixture.events.events.cbegin() + 2,
                              fixture.events.events.cend());
  second_events.events.front().payload_sha256 =
      first_events.events.back().payload_sha256;
  GuestExecutionSessionCheckpointChunk final_checkpoint =
      MakeCheckpointChunk(3, 8, 0x50);

  std::vector<uint8_t> encoded_first;
  std::vector<uint8_t> encoded_second;
  std::vector<uint8_t> encoded_final;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(first_events,
                                                       &encoded_first, &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      second_events, &encoded_second, &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_checkpoint, &encoded_final, &error));
  fixture.chunks[1] = std::move(encoded_first);
  fixture.chunks[2] = std::move(encoded_second);
  fixture.chunks.push_back(std::move(encoded_final));

  fixture.manifest.chunks.clear();
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]));
  fixture.manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 2, 2, fixture.chunks[1]));
  fixture.manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 2, 3, 8, 6, fixture.chunks[2]));
  fixture.manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 3, 8, 8, 1,
                   fixture.chunks[3]));

  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
}

TEST_CASE("Guest execution participant ranges exactly match their events",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  fixture.events.events[0] =
      PlainEvent(1, GuestExecutionSessionEventKind::kInterrupt,
                 kGuestExecutionSessionNoThread);
  fixture.events.events[1] =
      PlainEvent(2, GuestExecutionSessionEventKind::kSegmentBegin);
  fixture.events.events[1].disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  fixture.events.events[2] =
      PlainEvent(3, GuestExecutionSessionEventKind::kSegmentEnd);
  fixture.events.events[2].disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  fixture.events.events[3] =
      PlainEvent(4, GuestExecutionSessionEventKind::kInterrupt,
                 kGuestExecutionSessionNoThread);
  fixture.events.events[4] =
      PlainEvent(5, GuestExecutionSessionEventKind::kInterrupt,
                 kGuestExecutionSessionNoThread);
  fixture.events.events[5] =
      PlainEvent(6, GuestExecutionSessionEventKind::kGuestMarker,
                 kGuestExecutionSessionNoThread);
  fixture.events.events[5].marker_source =
      GuestExecutionSessionMarkerSource::kPm4Swap;
  fixture.events.events[5].marker_identity = 0x99;
  fixture.manifest.participants[0].first_event_sequence = 2;
  fixture.manifest.participants[0].last_event_sequence = 3;
  fixture.manifest.segments[0].first_event_sequence = 2;
  fixture.manifest.segments[0].last_event_sequence = 3;
  fixture.manifest.stop_request_guest_instruction_count = 0;

  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &fixture.chunks[1], &error));
  fixture.manifest.chunks[1] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 8, 8, fixture.chunks[1]);
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(fixture.manifest,
                                                      fixture.chunks, &error));

  fixture.manifest.participants[0].first_event_sequence = 1;
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
  fixture.manifest.participants[0].first_event_sequence = 2;
  fixture.manifest.participants[0].last_event_sequence = 4;
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
}

TEST_CASE("Guest execution digest has one cross-namespace byte size", "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  fixture.events.events[1].payload_sha256 =
      fixture.initial_checkpoint.checkpoint.thread_states[0].sha256;

  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &fixture.chunks[1], &error));
  fixture.manifest.chunks[1] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 8, 8, fixture.chunks[1]);
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));

  fixture = MakeSessionFixture();
  fixture.initial_checkpoint.checkpoint.thread_states[0].sha256 =
      fixture.initial_checkpoint.checkpoint.content[0].sha256;
  fixture.manifest.participants[0].initial_state_sha256 =
      fixture.initial_checkpoint.checkpoint.thread_states[0].sha256;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      fixture.initial_checkpoint, &fixture.chunks[0], &error));
  fixture.manifest.chunks[0] =
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]);
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
}

TEST_CASE("Guest execution session rejects unsupported or rejected work",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  GuestExecutionSessionEvent& rejected = fixture.events.events[1];
  rejected.kind = GuestExecutionSessionEventKind::kUnsupported;
  rejected.thread_ordinal = kGuestExecutionSessionNoThread;
  rejected.disposition = GuestExecutionSessionEventDisposition::kRejectSession;
  rejected.guest_address = 0;
  rejected.payload_kind = GuestExecutionSessionPayloadKind::kNone;
  rejected.payload_size = 0;
  rejected.payload_sha256 = {};

  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &fixture.chunks[1], &error));
  fixture.manifest.chunks[1] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 8, 8, fixture.chunks[1]);
  fixture.manifest.accepted_event_count = 7;
  fixture.manifest.rejected_event_count = 1;
  fixture.manifest.unsupported_event_count = 1;
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      fixture.manifest, &fixture.encoded_manifest, &error));
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
}

TEST_CASE("Guest execution boundary proofs reject policy mismatches", "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  std::string error;
  std::vector<uint8_t> output = {1, 2, 3};

  SECTION("manual policy cannot claim a requested boundary") {
    fixture.manifest.stop_reason =
        GuestExecutionSessionStopReason::kRequestedBoundary;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("requested policy cannot claim a manual stop") {
    fixture = MakeSegmentBoundaryFixture();
    fixture.manifest.stop_reason =
        GuestExecutionSessionStopReason::kManualRequest;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("marker proof must equal the requested count") {
    fixture = MakeGuestMarkerBoundaryFixture();
    fixture.manifest.stop_request_matching_guest_marker_count = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("instruction proof must equal the requested count") {
    fixture = MakeInstructionBoundaryFixture();
    fixture.manifest.stop_request_guest_instruction_count = 14;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("non-marker policy cannot carry marker proof") {
    fixture.manifest.stop_request_matching_guest_marker_count = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("request sequence and tick must be inside the capture prefix") {
    fixture.manifest.stop_request_event_sequence =
        fixture.manifest.last_event_sequence;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeSessionFixture();
    fixture.manifest.stop_request_tick = 901;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("all configured stop-tail maxima are mandatory") {
    fixture.manifest.maximum_stop_tail_event_count = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeSessionFixture();
    fixture.manifest.maximum_stop_tail_guest_instruction_count = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeSessionFixture();
    fixture.manifest.maximum_stop_tail_ticks = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("duration uses checked ceil conversion") {
    fixture = MakeDurationBoundaryFixture();
    fixture.manifest.stop_request_tick = 101;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));

    fixture = MakeDurationBoundaryFixture();
    fixture.manifest.capture_start_tick = 0;
    fixture.manifest.capture_end_tick = UINT64_MAX;
    fixture.manifest.capture_tick_frequency = UINT64_MAX;
    fixture.manifest.boundary.value = UINT64_MAX;
    fixture.manifest.stop_request_tick = UINT64_MAX;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));

    fixture = MakeDurationBoundaryFixture();
    fixture.manifest.capture_start_tick = 0;
    fixture.manifest.capture_end_tick = UINT64_MAX;
    fixture.manifest.capture_tick_frequency = UINT64_MAX;
    fixture.manifest.boundary.value = 1000000000;
    fixture.manifest.stop_request_tick = UINT64_MAX;
    CheckBoundaryFixtureValid(fixture);
  }
}

TEST_CASE("Guest execution boundary requests bind exact trigger adjacency",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  std::string error;
  std::vector<uint8_t> output;

  SECTION("hard-limit diagnostic sessions are not replayable") {
    fixture.manifest.stop_reason =
        GuestExecutionSessionStopReason::kMaximumEventCount;
    REQUIRE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                       &output, &error));
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("request sequence must contain the unique request event") {
    fixture.events.events[6] =
        PlainEvent(7, GuestExecutionSessionEventKind::kInterrupt,
                   kGuestExecutionSessionNoThread);
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("request and held controls are unique") {
    fixture = MakeAsynchronousRendezvousFixture();
    GuestExecutionSessionEvent& duplicate = fixture.events.events[6];
    duplicate = PlainEvent(7, GuestExecutionSessionEventKind::kBoundaryRequest,
                           kGuestExecutionSessionNoThread);
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));

    fixture = MakeSessionFixture();
    fixture.events.events.back() =
        PlainEvent(8, GuestExecutionSessionEventKind::kInterrupt,
                   kGuestExecutionSessionNoThread);
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("segment trigger must immediately precede request") {
    fixture = MakeSegmentBoundaryFixture();
    GuestExecutionSessionEvent& end = fixture.events.events[4];
    MakeEventCanonicalWithoutPayload(&end);
    end.kind = GuestExecutionSessionEventKind::kSegmentEnd;
    GuestExecutionSessionEvent& trigger = fixture.events.events[5];
    MakeEventCanonicalWithoutPayload(&trigger);
    trigger.kind = GuestExecutionSessionEventKind::kSynchronization;
    fixture.manifest.segments[0].last_event_sequence = 5;
    fixture.manifest.participants[0].last_event_sequence = 6;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("marker count without adjacent matching marker is rejected") {
    fixture = MakeSessionFixture();
    fixture.manifest.boundary.kind =
        GuestExecutionSessionBoundaryKind::kGuestMarkerCount;
    fixture.manifest.boundary.value = 1;
    fixture.manifest.boundary.marker_source =
        GuestExecutionSessionMarkerSource::kPm4Swap;
    fixture.manifest.boundary.marker_identity = 0x99;
    fixture.manifest.stop_reason =
        GuestExecutionSessionStopReason::kRequestedBoundary;
    fixture.manifest.stop_request_matching_guest_marker_count = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("instruction count without adjacent coverage is rejected") {
    fixture.manifest.boundary.kind =
        GuestExecutionSessionBoundaryKind::kGuestInstructionCount;
    fixture.manifest.boundary.value = 10;
    fixture.manifest.stop_reason =
        GuestExecutionSessionStopReason::kRequestedBoundary;
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }
}

TEST_CASE("Guest execution asynchronous rendezvous is fail closed", "[cpu]") {
  SessionFixture fixture = MakeAsynchronousRendezvousFixture();
  std::string error;
  std::vector<uint8_t> output;

  SECTION("staggered participants and checkpoint-only participant validate") {
    CheckBoundaryFixtureValid(fixture);
  }

  SECTION("capture may start inside an active outer host call") {
    // This records capture-time execution state only. Replay starts a fresh
    // outer dispatch at the captured PPC checkpoint; it does not restore a
    // native host stack from this bit.
    GuestExecutionSessionEvent& initial_work = fixture.events.events[1];
    MakeEventCanonicalWithoutPayload(&initial_work);
    initial_work.thread_ordinal = 2;
    initial_work.kind = GuestExecutionSessionEventKind::kSynchronization;
    fixture.manifest.participants[2].initial_outer_call_state =
        GuestExecutionSessionInitialOuterCallState::kActive;
    ReplaceEventChunk(&fixture);
    CheckBoundaryFixtureValid(fixture);
  }

  SECTION("participant event after arrival is rejected") {
    GuestExecutionSessionEvent post_arrival =
        PlainEvent(0, GuestExecutionSessionEventKind::kSynchronization, 1);
    MakeEventCanonicalWithoutPayload(&post_arrival);
    post_arrival.thread_ordinal = 1;
    ExtendHeldBoundary(&fixture, post_arrival);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("missing arrival is rejected") {
    GuestExecutionSessionEvent& arrival = fixture.events.events[11];
    arrival.kind = GuestExecutionSessionEventKind::kSynchronization;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("duplicate arrival is rejected") {
    GuestExecutionSessionEvent& duplicate = fixture.events.events[6];
    MakeEventCanonicalWithoutPayload(&duplicate);
    duplicate.thread_ordinal = 1;
    duplicate.kind = GuestExecutionSessionEventKind::kJitSafepointArrival;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("arrival owned by the wrong participant is rejected") {
    fixture.events.events[11].thread_ordinal = 2;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("new outer host call cannot begin in the stop tail") {
    GuestExecutionSessionEvent& begin = fixture.events.events[7];
    MakeEventCanonicalWithoutPayload(&begin);
    begin.thread_ordinal = 2;
    begin.kind = GuestExecutionSessionEventKind::kOuterHostCallBegin;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("orphan outer return cannot claim a boundary arrival") {
    GuestExecutionSessionEvent& begin = fixture.events.events[1];
    MakeEventCanonicalWithoutPayload(&begin);
    begin.thread_ordinal = 2;
    begin.kind = GuestExecutionSessionEventKind::kSynchronization;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("JIT arrival requires an active call at request and arrival") {
    fixture.manifest.participants[1].initial_outer_call_state =
        GuestExecutionSessionInitialOuterCallState::kOutside;
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("nested outer host calls are rejected") {
    GuestExecutionSessionEvent& nested_begin = fixture.events.events[3];
    MakeEventCanonicalWithoutPayload(&nested_begin);
    nested_begin.thread_ordinal = 2;
    nested_begin.kind = GuestExecutionSessionEventKind::kOuterHostCallBegin;
    fixture.manifest.stop_request_guest_instruction_count = 3;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("unmatched begin cannot be declared already outside") {
    fixture = MakeSessionFixture();
    GuestExecutionSessionEvent& begin = fixture.events.events[1];
    MakeEventCanonicalWithoutPayload(&begin);
    begin.thread_ordinal = 0;
    begin.kind = GuestExecutionSessionEventKind::kOuterHostCallBegin;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("outer host call cannot end while already outside") {
    fixture = MakeSessionFixture();
    GuestExecutionSessionEvent& end = fixture.events.events[1];
    MakeEventCanonicalWithoutPayload(&end);
    end.thread_ordinal = 0;
    end.kind = GuestExecutionSessionEventKind::kOuterHostCallEnd;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("active outer-return participant cannot reach held without end") {
    GuestExecutionSessionEvent& end = fixture.events.events[9];
    MakeEventCanonicalWithoutPayload(&end);
    end.thread_ordinal = 2;
    end.kind = GuestExecutionSessionEventKind::kSynchronization;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("unsupported tail work remains diagnostic only") {
    GuestExecutionSessionEvent& unsupported = fixture.events.events[7];
    unsupported = PlainEvent(8, GuestExecutionSessionEventKind::kUnsupported,
                             kGuestExecutionSessionNoThread);
    unsupported.disposition =
        GuestExecutionSessionEventDisposition::kRejectSession;
    --fixture.manifest.accepted_event_count;
    ++fixture.manifest.rejected_event_count;
    ++fixture.manifest.unsupported_event_count;
    ReplaceEventChunk(&fixture);
    REQUIRE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                       &output, &error));
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("tail event instruction and tick maxima are exact") {
    fixture.manifest.maximum_stop_tail_event_count = 5;
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
    fixture = MakeAsynchronousRendezvousFixture();
    fixture.manifest.maximum_stop_tail_guest_instruction_count = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
    fixture = MakeAsynchronousRendezvousFixture();
    fixture.manifest.maximum_stop_tail_ticks = 299;
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("tail instruction summation cannot overflow") {
    fixture.events.events[6].guest_instruction_delta = UINT64_MAX;
    GuestExecutionSessionEvent& second = fixture.events.events[7];
    MakeEventCanonicalWithoutPayload(&second);
    second.thread_ordinal = 1;
    second.kind = GuestExecutionSessionEventKind::kInstructionCoverage;
    second.guest_instruction_delta = 1;
    fixture.manifest.maximum_stop_tail_guest_instruction_count = UINT64_MAX;
    ReplaceEventChunk(&fixture);
    CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
        fixture.manifest, fixture.chunks, &error));
  }

  SECTION("participant capture identities are nonzero and unique") {
    fixture.manifest.participants[1].capture_instance_id = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeAsynchronousRendezvousFixture();
    fixture.manifest.participants[1].capture_instance_id =
        fixture.manifest.participants[0].capture_instance_id;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeAsynchronousRendezvousFixture();
    fixture.manifest.participants[1].guest_thread_id =
        fixture.manifest.participants[0].guest_thread_id;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("initial outer-call state is a known canonical value") {
    fixture.manifest.participants[1].initial_outer_call_state =
        static_cast<GuestExecutionSessionInitialOuterCallState>(UINT32_MAX);
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }

  SECTION("arrival disposition and held sequence are canonical") {
    fixture.manifest.participants[1].held_after_event_sequence = 11;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeAsynchronousRendezvousFixture();
    fixture.manifest.participants[1].boundary_arrival_kind =
        GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
    fixture.manifest.participants[1].held_after_event_sequence =
        fixture.manifest.stop_request_event_sequence;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
    fixture = MakeAsynchronousRendezvousFixture();
    fixture.manifest.participants[1].first_event_sequence = 0;
    fixture.manifest.participants[1].last_event_sequence = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                           &output, &error));
  }
}

TEST_CASE("Guest execution coverage and marker fields are canonical", "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  std::string error;
  std::vector<uint8_t> output = {1, 2, 3};

  SECTION("ordinary events cannot carry marker identity") {
    fixture.events.events[0].marker_source =
        GuestExecutionSessionMarkerSource::kKernel;
    fixture.events.events[0].marker_identity = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));
  }

  SECTION("ordinary events cannot carry instruction deltas") {
    fixture.events.events[0].guest_instruction_delta = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));
  }

  SECTION("PM4 markers are no-thread captured guest-command markers") {
    GuestExecutionSessionEvent& marker = fixture.events.events[4];
    marker.thread_ordinal = 0;
    marker.disposition =
        GuestExecutionSessionEventDisposition::kValidateDeterministic;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeSessionFixture();
    fixture.events.events[4].marker_source =
        GuestExecutionSessionMarkerSource::kNone;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));
  }

  SECTION(
      "participant marker sources are deterministic and participant-owned") {
    const std::vector<GuestExecutionSessionMarkerSource> sources = {
        GuestExecutionSessionMarkerSource::kGuestDefined,
        GuestExecutionSessionMarkerSource::kKernel,
        GuestExecutionSessionMarkerSource::kOtherInstrumented,
    };
    for (GuestExecutionSessionMarkerSource source : sources) {
      INFO("source = " << static_cast<uint32_t>(source));
      fixture = MakeSessionFixture();
      GuestExecutionSessionEvent& marker = fixture.events.events[4];
      marker.thread_ordinal = 0;
      marker.disposition =
          GuestExecutionSessionEventDisposition::kValidateDeterministic;
      marker.marker_source = source;
      REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                           &output, &error));
    }

    fixture = MakeSessionFixture();
    GuestExecutionSessionEvent& marker = fixture.events.events[4];
    marker.marker_source = GuestExecutionSessionMarkerSource::kGuestDefined;
    marker.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
    marker.payload_size = 1;
    marker.payload_sha256 = Digest(0x70);
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));
  }

  SECTION("coverage requires a nonzero delta and no payload") {
    fixture.events.events[3].guest_instruction_delta = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeSessionFixture();
    fixture.events.events[3].payload_kind =
        GuestExecutionSessionPayloadKind::kGuestBytes;
    fixture.events.events[3].payload_size = 1;
    fixture.events.events[3].payload_sha256 = Digest(0x80);
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));
  }

  SECTION("structural controls reject payload and field smuggling") {
    fixture.events.events[0].payload_kind =
        GuestExecutionSessionPayloadKind::kGuestBytes;
    fixture.events.events[0].payload_size = 1;
    fixture.events.events[0].payload_sha256 = Digest(0x90);
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeSessionFixture();
    fixture.events.events[5].disposition =
        GuestExecutionSessionEventDisposition::kReplayCaptured;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeSessionFixture();
    fixture.events.events[6].payload_kind =
        GuestExecutionSessionPayloadKind::kGuestBytes;
    fixture.events.events[6].payload_size = 1;
    fixture.events.events[6].payload_sha256 = Digest(0x91);
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeSessionFixture();
    fixture.events.events[7].thread_ordinal = 0;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeAsynchronousRendezvousFixture();
    fixture.events.events[1].guest_address = 4;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeAsynchronousRendezvousFixture();
    fixture.events.events[9].guest_instruction_delta = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));

    fixture = MakeAsynchronousRendezvousFixture();
    fixture.events.events[11].marker_source =
        GuestExecutionSessionMarkerSource::kKernel;
    fixture.events.events[11].marker_identity = 1;
    CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(fixture.events,
                                                             &output, &error));
  }
}

TEST_CASE("Guest execution codecs enforce semantic and resource limits",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  std::string error;
  std::vector<uint8_t> output = {1, 2, 3};

  GuestExecutionSessionLimits limits;
  limits.maximum_events_per_chunk = 3;
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &output, &error, limits));
  CHECK(output.empty());

  limits = {};
  limits.maximum_manifest_bytes = fixture.encoded_manifest.size() - 1;
  output = {1, 2, 3};
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(
      fixture.manifest, &output, &error, limits));
  CHECK(output.empty());

  GuestExecutionSessionCheckpointChunk checkpoint = fixture.initial_checkpoint;
  checkpoint.checkpoint.content[1].sha256 =
      checkpoint.checkpoint.content[0].sha256;
  output = {1, 2, 3};
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      checkpoint, &output, &error));
  CHECK(output.empty());

  checkpoint = fixture.initial_checkpoint;
  checkpoint.checkpoint.content[1].guest_address = 0x1800;
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      checkpoint, &output, &error));

  GuestExecutionSessionEventChunk events = fixture.events;
  events.events[2].guest_address = UINT32_MAX;
  events.events[2].byte_count = 2;
  CHECK_FALSE(
      GuestExecutionSessionCodec::EncodeEventChunk(events, &output, &error));

  events = MakeEventChunk();
  events.events[2].payload_kind = GuestExecutionSessionPayloadKind::kNone;
  events.events[2].payload_size = 0;
  events.events[2].payload_sha256 = {};
  CHECK_FALSE(
      GuestExecutionSessionCodec::EncodeEventChunk(events, &output, &error));

  fixture.manifest.capture_end_tick = fixture.manifest.capture_start_tick - 1;
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                         &output, &error));

  fixture = MakeSessionFixture();
  fixture.manifest.boundary.kind =
      GuestExecutionSessionBoundaryKind::kGuestMarkerCount;
  fixture.manifest.boundary.value = 1;
  fixture.manifest.boundary.marker_source =
      GuestExecutionSessionMarkerSource::kPm4Swap;
  fixture.manifest.boundary.marker_identity = 0;
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                         &output, &error));

  fixture = MakeSessionFixture();
  fixture.manifest.module_identity_sha256 = {};
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                         &output, &error));

  fixture = MakeSessionFixture();
  fixture.manifest.rejected_segment_count =
      std::numeric_limits<uint64_t>::max();
  CHECK_FALSE(GuestExecutionSessionCodec::EncodeManifest(fixture.manifest,
                                                         &output, &error));
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
