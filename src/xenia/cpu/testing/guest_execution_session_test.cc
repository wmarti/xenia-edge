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
  chunk.events.push_back(
      PlainEvent(1, GuestExecutionSessionEventKind::kSegmentBegin));

  GuestExecutionSessionEvent external =
      PlainEvent(2, GuestExecutionSessionEventKind::kKernelExport);
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

  chunk.events.push_back(
      PlainEvent(4, GuestExecutionSessionEventKind::kSegmentEnd));
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
  std::vector<std::vector<uint8_t>> chunks;
  std::vector<uint8_t> encoded_manifest;
};

SessionFixture MakeSessionFixture() {
  SessionFixture fixture;
  fixture.events = MakeEventChunk();
  fixture.initial_checkpoint = MakeCheckpointChunk(0, 0, 0x40);
  fixture.final_checkpoint = MakeCheckpointChunk(2, 4, 0x50);

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
  manifest.boundary.kind = GuestExecutionSessionBoundaryKind::kGuestMarkerCount;
  manifest.boundary.value = 2;
  manifest.boundary.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
  manifest.boundary.marker_identity = 0x99;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 4;
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 900;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Digest(1);
  manifest.replay_config_sha256 = Digest(2);
  manifest.title_identity_sha256 = Digest(3);
  manifest.module_identity_sha256 = Digest(4);
  manifest.accepted_segment_count = 1;
  manifest.accepted_event_count = 4;
  manifest.participants.push_back(
      {0, 7, 1, 4, 2644,
       fixture.initial_checkpoint.checkpoint.thread_states[0].sha256});
  manifest.segments.push_back(
      {0, 0, 1, 4, 0x82000000, 0x820000FC, Digest(5), Digest(6)});
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   fixture.chunks[0]));
  manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 4, 4, fixture.chunks[1]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2, 4, 4, 1,
                   fixture.chunks[2]));
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      manifest, &fixture.encoded_manifest, &error));
  return fixture;
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

  forged = fixture.encoded_manifest;
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 176,
           UINT32_MAX);
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

  forged = fixture.chunks[1];
  WriteU32(&forged, GuestExecutionSessionCodec::kEnvelopeHeaderSize + 24,
           UINT32_MAX);
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
}

TEST_CASE("Guest execution session rejects unsupported or rejected work",
          "[cpu]") {
  SessionFixture fixture = MakeSessionFixture();
  GuestExecutionSessionEvent& rejected = fixture.events.events[1];
  rejected.kind = GuestExecutionSessionEventKind::kUnsupported;
  rejected.thread_ordinal = kGuestExecutionSessionNoThread;
  rejected.disposition = GuestExecutionSessionEventDisposition::kRejectSession;
  rejected.payload_kind = GuestExecutionSessionPayloadKind::kNone;
  rejected.payload_size = 0;
  rejected.payload_sha256 = {};

  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      fixture.events, &fixture.chunks[1], &error));
  fixture.manifest.chunks[1] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 4, 4, fixture.chunks[1]);
  fixture.manifest.accepted_event_count = 3;
  fixture.manifest.rejected_event_count = 1;
  fixture.manifest.unsupported_event_count = 1;
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(
      fixture.manifest, &fixture.encoded_manifest, &error));
  CHECK_FALSE(GuestExecutionSessionCodec::ValidateSession(
      fixture.manifest, fixture.chunks, &error));
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
