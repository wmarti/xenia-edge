/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_continuous_event.h"

#include <algorithm>
#include <array>
#include <cstddef>
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

constexpr size_t kHeaderVersionOffset = 8;
constexpr size_t kHeaderSizeOffset = 12;
constexpr size_t kHeaderEncodedSizeOffset = 16;
constexpr size_t kHeaderFlagsOffset = 24;
constexpr size_t kHeaderRecordSizeOffset = 28;
constexpr size_t kHeaderRecordCountOffset = 32;
constexpr size_t kHeaderFirstSequenceOffset = 40;
constexpr size_t kHeaderLastSequenceOffset = 48;
constexpr size_t kHeaderReservedOffset = 56;

constexpr size_t kRecordSequenceOffset = 0;
constexpr size_t kRecordKindOffset = 8;
constexpr size_t kRecordCheckpointKindOffset = 12;
constexpr size_t kRecordActorOrdinalOffset = 16;
constexpr size_t kRecordActorThreadIdOffset = 20;
constexpr size_t kRecordSubjectOrdinalOffset = 24;
constexpr size_t kRecordSubjectThreadIdOffset = 28;
constexpr size_t kRecordCheckpointSequenceOffset = 32;
constexpr size_t kRecordCheckpointSizeOffset = 40;
constexpr size_t kRecordCheckpointShaOffset = 48;
constexpr size_t kRecordReservedOffset = 80;

GuestExecutionContinuousEventIdentity Identity(uint32_t participant_ordinal,
                                               uint32_t guest_thread_id) {
  return {participant_ordinal, guest_thread_id};
}

GuestExecutionContinuousEvent MakeEvent(
    uint64_t global_sequence, GuestExecutionSessionEventKind kind,
    GuestExecutionContinuousEventIdentity actor = {},
    GuestExecutionContinuousEventIdentity subject = {}) {
  GuestExecutionContinuousEvent event;
  event.global_sequence = global_sequence;
  event.kind = kind;
  event.actor = actor;
  event.subject = subject;
  return event;
}

ppc::GuestPPCThreadCheckpoint MakeCheckpoint(
    ppc::GuestPPCThreadResumeKind resume_kind =
        ppc::GuestPPCThreadResumeKind::kGuestBlockHead,
    uint64_t pending_event_sequence = 0) {
  ppc::GuestPPCThreadCheckpoint checkpoint;
  checkpoint.participant_ordinal = 1;
  checkpoint.guest_thread_id = 0x80000001u;
  checkpoint.resume_kind = resume_kind;
  checkpoint.resume_pc = 0x82004000;
  checkpoint.owning_function_address = 0x82003000;
  checkpoint.owning_function_end_address = 0x82004FFC;
  checkpoint.outer_guest_return_address = 0xBCBCBCBC;
  if (resume_kind ==
      ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern) {
    checkpoint.pending_external_event_sequence = pending_event_sequence;
    checkpoint.pending_export_guest_address = 0x80008000;
  }
  checkpoint.registers.gpr.front() = 0;
  checkpoint.registers.gpr.back() = UINT64_MAX;
  checkpoint.registers.fpr_bits.front() = 0x7FF8000000000001ull;
  checkpoint.registers.vector_registers.front().front() = 0x80;
  checkpoint.registers.vector_registers.back().back() = 0xFF;
  checkpoint.registers.condition_register_fields.front() = {0x80, 0, 0x7F,
                                                            0xFF};
  checkpoint.registers.link_register = UINT64_MAX;
  checkpoint.registers.fpscr = UINT32_MAX;
  checkpoint.registers.vscr_vector.front() = 0x80;
  checkpoint.registers.vscr_vector.back() = 0xFF;
  checkpoint.registers.vrsave = UINT32_MAX;
  checkpoint.registers.xer_ca = 1;
  checkpoint.registers.xer_so = 1;
  checkpoint.registers.vscr_sat = 1;
  return checkpoint;
}

ppc::GuestPPCThreadCheckpointBinding BindingFor(
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  ppc::GuestPPCThreadCheckpointBinding binding;
  binding.participant_ordinal = checkpoint.participant_ordinal;
  binding.guest_thread_id = checkpoint.guest_thread_id;
  binding.resume_kind = checkpoint.resume_kind;
  binding.resume_pc = checkpoint.resume_pc;
  binding.owning_function_address = checkpoint.owning_function_address;
  binding.owning_function_end_address = checkpoint.owning_function_end_address;
  binding.outer_guest_return_address = checkpoint.outer_guest_return_address;
  binding.pending_external_event_sequence =
      checkpoint.pending_external_event_sequence;
  binding.pending_export_guest_address =
      checkpoint.pending_export_guest_address;
  return binding;
}

std::vector<uint8_t> EncodeCheckpoint(
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(
      ppc::GuestPPCThreadCheckpointCodec::Encode(checkpoint, &encoded, &error));
  REQUIRE(error.empty());
  return encoded;
}

struct Fixture {
  ppc::GuestPPCThreadCheckpoint checkpoint;
  ppc::GuestPPCThreadCheckpointBinding binding;
  std::vector<uint8_t> checkpoint_bytes;
  std::vector<GuestExecutionContinuousEvent> events;
  std::vector<GuestExecutionSessionParticipant> participants;
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.checkpoint = MakeCheckpoint();
  fixture.binding = BindingFor(fixture.checkpoint);
  fixture.checkpoint_bytes = EncodeCheckpoint(fixture.checkpoint);

  const auto actor = Identity(0, UINT32_MAX);
  const auto subject = Identity(1, 0x80000001u);
  fixture.events.push_back(MakeEvent(
      100, GuestExecutionSessionEventKind::kThreadDispatch, actor, subject));
  fixture.events.push_back(MakeEvent(
      101, GuestExecutionSessionEventKind::kSynchronization, subject, actor));
  fixture.events.push_back(
      MakeEvent(102, GuestExecutionSessionEventKind::kJitSafepointArrival,
                actor, subject));
  GuestExecutionContinuousCheckpointReference& reference =
      fixture.events.back().checkpoint;
  reference.kind =
      GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
  reference.checkpoint_global_sequence = 99;
  reference.state_size = fixture.checkpoint_bytes.size();
  reference.state_sha256 = GuestExecutionSessionCodec::HashBytes(
      fixture.checkpoint_bytes.data(), fixture.checkpoint_bytes.size());

  GuestExecutionSessionParticipant participant;
  participant.ordinal = 0;
  participant.guest_thread_id = UINT32_MAX;
  fixture.participants.push_back(participant);
  participant.ordinal = 1;
  participant.guest_thread_id = 0x80000001u;
  fixture.participants.push_back(participant);
  return fixture;
}

std::vector<uint8_t> EncodeEvents(
    const std::vector<GuestExecutionContinuousEvent>& events) {
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(events, &encoded, &error));
  REQUIRE(error.empty());
  return encoded;
}

void WriteU32(std::vector<uint8_t>* data, size_t offset, uint32_t value) {
  REQUIRE(offset + sizeof(value) <= data->size());
  for (size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
    (*data)[offset + byte_index] =
        static_cast<uint8_t>(value >> (byte_index * 8));
  }
}

void WriteU64(std::vector<uint8_t>* data, size_t offset, uint64_t value) {
  REQUIRE(offset + sizeof(value) <= data->size());
  for (size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
    (*data)[offset + byte_index] =
        static_cast<uint8_t>(value >> (byte_index * 8));
  }
}

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
  REQUIRE(offset + sizeof(uint32_t) <= data.size());
  uint32_t value = 0;
  for (size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
    value |= uint32_t(data[offset + byte_index]) << (byte_index * 8);
  }
  return value;
}

uint64_t ReadU64(const std::vector<uint8_t>& data, size_t offset) {
  REQUIRE(offset + sizeof(uint64_t) <= data.size());
  uint64_t value = 0;
  for (size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
    value |= uint64_t(data[offset + byte_index]) << (byte_index * 8);
  }
  return value;
}

void RequireDecodeFailure(const std::vector<uint8_t>& data) {
  const Fixture fixture = MakeFixture();
  std::vector<GuestExecutionContinuousEvent> output = fixture.events;
  const auto original = output;
  std::string error;
  REQUIRE_FALSE(
      GuestExecutionContinuousEventCodec::Decode(data, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output == original);
}

void RequireEncodeFailure(
    const std::vector<GuestExecutionContinuousEvent>& events) {
  std::vector<uint8_t> output = {0xAA};
  std::string error;
  REQUIRE_FALSE(
      GuestExecutionContinuousEventCodec::Encode(events, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output.empty());
}

void RequireCheckpointFailure(
    const GuestExecutionContinuousEvent& event,
    const std::vector<uint8_t>& checkpoint_bytes,
    const ppc::GuestPPCThreadCheckpointBinding& binding) {
  ppc::GuestPPCThreadCheckpoint output = MakeCheckpoint(
      ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern, 77);
  const ppc::GuestPPCThreadCheckpoint original = output;
  std::string error;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::DecodeAndValidateCheckpoint(
      event, checkpoint_bytes, binding, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output == original);
}

size_t RecordOffset(size_t record_index, size_t field_offset) {
  return GuestExecutionContinuousEventCodec::kHeaderSize +
         record_index * GuestExecutionContinuousEventCodec::kRecordSize +
         field_offset;
}

}  // namespace

TEST_CASE("continuous event v3 round-trips actor and subject independently",
          "[cpu][guest-execution]") {
  const Fixture fixture = MakeFixture();
  const std::vector<uint8_t> first = EncodeEvents(fixture.events);
  const std::vector<uint8_t> second = EncodeEvents(fixture.events);

  REQUIRE(GuestExecutionContinuousEventCodec::kVersion == 3);
  REQUIRE(GuestExecutionContinuousEventCodec::kHeaderSize == 64);
  REQUIRE(GuestExecutionContinuousEventCodec::kRecordSize == 96);
  REQUIRE(first == second);
  REQUIRE(first.size() == 64 + fixture.events.size() * 96);
  REQUIRE(ReadU32(first, kHeaderVersionOffset) == 3);
  REQUIRE(ReadU32(first, kHeaderSizeOffset) == 64);
  REQUIRE(ReadU64(first, kHeaderEncodedSizeOffset) == first.size());
  REQUIRE(ReadU32(first, kHeaderFlagsOffset) == 0);
  REQUIRE(ReadU32(first, kHeaderRecordSizeOffset) == 96);
  REQUIRE(ReadU64(first, kHeaderRecordCountOffset) == fixture.events.size());
  REQUIRE(ReadU64(first, kHeaderFirstSequenceOffset) == 100);
  REQUIRE(ReadU64(first, kHeaderLastSequenceOffset) == 102);
  REQUIRE(ReadU64(first, kHeaderReservedOffset) == 0);
  REQUIRE(ReadU32(first, RecordOffset(0, kRecordActorOrdinalOffset)) == 0);
  REQUIRE(ReadU32(first, RecordOffset(0, kRecordActorThreadIdOffset)) ==
          UINT32_MAX);
  REQUIRE(ReadU32(first, RecordOffset(0, kRecordSubjectOrdinalOffset)) == 1);
  REQUIRE(ReadU32(first, RecordOffset(0, kRecordSubjectThreadIdOffset)) ==
          0x80000001u);
  REQUIRE(ReadU64(first, RecordOffset(2, kRecordSequenceOffset)) == 102);
  REQUIRE(ReadU32(first, RecordOffset(2, kRecordKindOffset)) == 19);
  REQUIRE(ReadU32(first, RecordOffset(2, kRecordCheckpointKindOffset)) == 1);
  REQUIRE(ReadU64(first, RecordOffset(2, kRecordCheckpointSequenceOffset)) ==
          99);
  REQUIRE(ReadU64(first, RecordOffset(2, kRecordCheckpointSizeOffset)) ==
          ppc::GuestPPCThreadCheckpointCodec::kEncodedSize);

  std::vector<GuestExecutionContinuousEvent> decoded;
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::Decode(first, &decoded, &error));
  REQUIRE(error.empty());
  REQUIRE(decoded == fixture.events);
  REQUIRE(decoded.front().actor != decoded.front().subject);
  REQUIRE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      decoded, fixture.participants, &error));
  REQUIRE(error.empty());

  GuestExecutionContinuousEvent edge =
      MakeEvent(UINT64_MAX, GuestExecutionSessionEventKind::kBoundaryHeld,
                Identity(UINT32_MAX - 1, UINT32_MAX),
                Identity(UINT32_MAX - 2, 0x80000000u));
  const std::vector<uint8_t> edge_bytes = EncodeEvents({edge});
  decoded.clear();
  REQUIRE(
      GuestExecutionContinuousEventCodec::Decode(edge_bytes, &decoded, &error));
  REQUIRE(decoded == std::vector<GuestExecutionContinuousEvent>{edge});
}

TEST_CASE("continuous event v3 accepts every defined event kind",
          "[cpu][guest-execution]") {
  const std::array<GuestExecutionSessionEventKind, 19> kinds = {
      GuestExecutionSessionEventKind::kSegmentBegin,
      GuestExecutionSessionEventKind::kSegmentEnd,
      GuestExecutionSessionEventKind::kThreadDispatch,
      GuestExecutionSessionEventKind::kSynchronization,
      GuestExecutionSessionEventKind::kKernelExport,
      GuestExecutionSessionEventKind::kExternOrBuiltin,
      GuestExecutionSessionEventKind::kMmio,
      GuestExecutionSessionEventKind::kClockOrTimebase,
      GuestExecutionSessionEventKind::kInterrupt,
      GuestExecutionSessionEventKind::kAtomicOrReservation,
      GuestExecutionSessionEventKind::kMemoryMutation,
      GuestExecutionSessionEventKind::kUnsupported,
      GuestExecutionSessionEventKind::kInstructionCoverage,
      GuestExecutionSessionEventKind::kGuestMarker,
      GuestExecutionSessionEventKind::kBoundaryRequest,
      GuestExecutionSessionEventKind::kBoundaryHeld,
      GuestExecutionSessionEventKind::kOuterHostCallBegin,
      GuestExecutionSessionEventKind::kOuterHostCallEnd,
      GuestExecutionSessionEventKind::kJitSafepointArrival,
  };
  std::vector<GuestExecutionContinuousEvent> expected;
  for (size_t kind_index = 0; kind_index < kinds.size(); ++kind_index) {
    expected.push_back(MakeEvent(kind_index + 1, kinds[kind_index]));
  }
  const std::vector<uint8_t> encoded = EncodeEvents(expected);
  std::vector<GuestExecutionContinuousEvent> decoded;
  REQUIRE(GuestExecutionContinuousEventCodec::Decode(encoded, &decoded));
  REQUIRE(decoded == expected);
}

TEST_CASE("continuous event v3 is unambiguous with existing v2 event chunks",
          "[cpu][guest-execution]") {
  GuestExecutionSessionEventChunk v2_chunk;
  v2_chunk.session_epoch = 0x1122334455667788ull;
  GuestExecutionSessionEvent v2_event;
  v2_event.global_sequence = 1;
  v2_event.thread_ordinal = 0;
  v2_event.kind = GuestExecutionSessionEventKind::kSegmentBegin;
  v2_event.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  v2_chunk.events.push_back(v2_event);

  std::vector<uint8_t> v2_bytes;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(v2_chunk, &v2_bytes,
                                                       &error));
  REQUIRE(error.empty());

  const Fixture fixture = MakeFixture();
  std::vector<GuestExecutionContinuousEvent> v3_output = fixture.events;
  const auto original = v3_output;
  REQUIRE_FALSE(
      GuestExecutionContinuousEventCodec::Decode(v2_bytes, &v3_output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(v3_output == original);

  GuestExecutionSessionEventChunk v2_output;
  REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(v2_bytes, &v2_output,
                                                       &error));
  REQUIRE(error.empty());
  REQUIRE(v2_output == v2_chunk);
}

TEST_CASE("continuous event checkpoint reference authenticates and binds state",
          "[cpu][guest-execution]") {
  const Fixture fixture = MakeFixture();
  const GuestExecutionContinuousEvent& event = fixture.events.back();
  ppc::GuestPPCThreadCheckpoint decoded;
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::DecodeAndValidateCheckpoint(
      event, fixture.checkpoint_bytes, fixture.binding, &decoded, &error));
  REQUIRE(error.empty());
  REQUIRE(decoded == fixture.checkpoint);
  REQUIRE(event.checkpoint.state_size ==
          ppc::GuestPPCThreadCheckpointCodec::kEncodedSize);

  std::vector<uint8_t> malformed = fixture.checkpoint_bytes;
  malformed.back() ^= 1;
  RequireCheckpointFailure(event, malformed, fixture.binding);

  malformed = fixture.checkpoint_bytes;
  malformed.front() ^= 1;
  GuestExecutionContinuousEvent malformed_event = event;
  malformed_event.checkpoint.state_sha256 =
      GuestExecutionSessionCodec::HashBytes(malformed.data(), malformed.size());
  RequireCheckpointFailure(malformed_event, malformed, fixture.binding);

  ppc::GuestPPCThreadCheckpointBinding mismatch = fixture.binding;
  ++mismatch.participant_ordinal;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);
  mismatch = fixture.binding;
  --mismatch.guest_thread_id;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);
  mismatch = fixture.binding;
  mismatch.resume_pc += 4;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);
  mismatch = fixture.binding;
  mismatch.owning_function_address += 4;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);
  mismatch = fixture.binding;
  mismatch.owning_function_end_address -= 4;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);
  mismatch = fixture.binding;
  mismatch.outer_guest_return_address += 4;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);

  GuestExecutionContinuousEvent no_checkpoint = fixture.events.front();
  RequireCheckpointFailure(no_checkpoint, fixture.checkpoint_bytes,
                           fixture.binding);
  malformed_event = event;
  --malformed_event.checkpoint.state_size;
  RequireCheckpointFailure(malformed_event, fixture.checkpoint_bytes,
                           fixture.binding);
  RequireCheckpointFailure(event, {}, fixture.binding);
}

TEST_CASE("continuous event checkpoint rejects an out-of-order pending route",
          "[cpu][guest-execution]") {
  Fixture fixture = MakeFixture();
  fixture.checkpoint = MakeCheckpoint(
      ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern, 98);
  fixture.binding = BindingFor(fixture.checkpoint);
  fixture.checkpoint_bytes = EncodeCheckpoint(fixture.checkpoint);
  GuestExecutionContinuousEvent event = fixture.events.back();
  event.checkpoint.state_sha256 = GuestExecutionSessionCodec::HashBytes(
      fixture.checkpoint_bytes.data(), fixture.checkpoint_bytes.size());

  ppc::GuestPPCThreadCheckpoint decoded;
  REQUIRE(GuestExecutionContinuousEventCodec::DecodeAndValidateCheckpoint(
      event, fixture.checkpoint_bytes, fixture.binding, &decoded));
  REQUIRE(decoded == fixture.checkpoint);

  ppc::GuestPPCThreadCheckpointBinding mismatch = fixture.binding;
  --mismatch.pending_external_event_sequence;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);
  mismatch = fixture.binding;
  mismatch.pending_export_guest_address += 4;
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, mismatch);

  fixture.checkpoint.pending_external_event_sequence = 100;
  fixture.binding = BindingFor(fixture.checkpoint);
  fixture.checkpoint_bytes = EncodeCheckpoint(fixture.checkpoint);
  event.checkpoint.state_sha256 = GuestExecutionSessionCodec::HashBytes(
      fixture.checkpoint_bytes.data(), fixture.checkpoint_bytes.size());
  RequireCheckpointFailure(event, fixture.checkpoint_bytes, fixture.binding);
}

TEST_CASE(
    "continuous event decode rejects every truncated prefix and trailing data",
    "[cpu][guest-execution]") {
  const std::vector<uint8_t> encoded = EncodeEvents(MakeFixture().events);
  for (size_t prefix_size = 0; prefix_size < encoded.size(); ++prefix_size) {
    CAPTURE(prefix_size);
    RequireDecodeFailure(
        std::vector<uint8_t>(encoded.cbegin(), encoded.cbegin() + prefix_size));
  }
  std::vector<uint8_t> trailing = encoded;
  trailing.push_back(0);
  RequireDecodeFailure(trailing);

  const Fixture fixture = MakeFixture();
  std::vector<GuestExecutionContinuousEvent> output = fixture.events;
  const auto original = output;
  std::string error;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Decode(
      nullptr, encoded.size(), &output, &error));
  REQUIRE(output == original);
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(
      GuestExecutionContinuousEventCodec::Decode(nullptr, 0, &output, &error));
  REQUIRE(output == original);
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Decode(
      encoded.data(), encoded.size(), nullptr, &error));
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Encode(fixture.events,
                                                           nullptr, &error));
}

TEST_CASE("continuous event decode rejects forged headers and reserved bytes",
          "[cpu][guest-execution]") {
  const std::vector<uint8_t> encoded = EncodeEvents(MakeFixture().events);
  std::vector<uint8_t> malformed = encoded;
  malformed.front() ^= 1;
  RequireDecodeFailure(malformed);

  for (uint32_t version : {0u, 2u, 4u, UINT32_MAX}) {
    malformed = encoded;
    WriteU32(&malformed, kHeaderVersionOffset, version);
    RequireDecodeFailure(malformed);
  }
  for (uint32_t size : {0u, 63u, 65u, UINT32_MAX}) {
    malformed = encoded;
    WriteU32(&malformed, kHeaderSizeOffset, size);
    RequireDecodeFailure(malformed);
  }
  for (uint64_t size : {uint64_t{0}, uint64_t{encoded.size() - 1},
                        uint64_t{encoded.size() + 1}, UINT64_MAX}) {
    malformed = encoded;
    WriteU64(&malformed, kHeaderEncodedSizeOffset, size);
    RequireDecodeFailure(malformed);
  }
  for (uint32_t flags : {1u, UINT32_MAX}) {
    malformed = encoded;
    WriteU32(&malformed, kHeaderFlagsOffset, flags);
    RequireDecodeFailure(malformed);
  }
  for (uint32_t size : {0u, 95u, 97u, UINT32_MAX}) {
    malformed = encoded;
    WriteU32(&malformed, kHeaderRecordSizeOffset, size);
    RequireDecodeFailure(malformed);
  }
  for (uint64_t count : {uint64_t{0}, uint64_t{1}, UINT64_MAX}) {
    malformed = encoded;
    WriteU64(&malformed, kHeaderRecordCountOffset, count);
    RequireDecodeFailure(malformed);
  }
  malformed = encoded;
  WriteU64(&malformed, kHeaderFirstSequenceOffset, 99);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU64(&malformed, kHeaderLastSequenceOffset, 103);
  RequireDecodeFailure(malformed);

  for (size_t byte_offset = kHeaderReservedOffset;
       byte_offset < GuestExecutionContinuousEventCodec::kHeaderSize;
       ++byte_offset) {
    CAPTURE(byte_offset);
    malformed = encoded;
    malformed[byte_offset] = 1;
    RequireDecodeFailure(malformed);
  }
  for (size_t record_index = 0; record_index < MakeFixture().events.size();
       ++record_index) {
    for (size_t reserved_index = 0;
         reserved_index <
         GuestExecutionContinuousEventCodec::kRecordReservedSize;
         ++reserved_index) {
      CAPTURE(record_index, reserved_index);
      malformed = encoded;
      malformed[RecordOffset(record_index,
                             kRecordReservedOffset + reserved_index)] = 1;
      RequireDecodeFailure(malformed);
    }
  }
}

TEST_CASE("continuous event codec rejects malformed order identities and kinds",
          "[cpu][guest-execution]") {
  const Fixture fixture = MakeFixture();
  const std::vector<uint8_t> encoded = EncodeEvents(fixture.events);
  std::vector<GuestExecutionContinuousEvent> malformed_events = fixture.events;
  malformed_events[1].global_sequence = 100;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events[1].global_sequence = 102;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.front().global_sequence = 0;
  RequireEncodeFailure(malformed_events);
  GuestExecutionContinuousEvent maximum_sequence =
      MakeEvent(UINT64_MAX, GuestExecutionSessionEventKind::kBoundaryHeld);
  RequireEncodeFailure({maximum_sequence, maximum_sequence});
  malformed_events = fixture.events;
  malformed_events.front().kind =
      static_cast<GuestExecutionSessionEventKind>(0);
  RequireEncodeFailure(malformed_events);
  malformed_events.front().kind =
      static_cast<GuestExecutionSessionEventKind>(20);
  RequireEncodeFailure(malformed_events);
  malformed_events.front().kind =
      static_cast<GuestExecutionSessionEventKind>(UINT32_MAX);
  RequireEncodeFailure(malformed_events);

  malformed_events = fixture.events;
  malformed_events.front().actor.participant_ordinal =
      kGuestExecutionSessionNoThread;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.front().actor.guest_thread_id = 0;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.front().subject.participant_ordinal =
      kGuestExecutionSessionNoThread;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.front().subject.guest_thread_id = 0;
  RequireEncodeFailure(malformed_events);

  std::vector<uint8_t> malformed = encoded;
  WriteU64(&malformed, RecordOffset(1, kRecordSequenceOffset), 100);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU64(&malformed, RecordOffset(1, kRecordSequenceOffset), 102);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU64(&malformed, RecordOffset(0, kRecordSequenceOffset), 0);
  RequireDecodeFailure(malformed);
  for (uint32_t kind : {0u, 20u, UINT32_MAX}) {
    malformed = encoded;
    WriteU32(&malformed, RecordOffset(0, kRecordKindOffset), kind);
    RequireDecodeFailure(malformed);
  }
  malformed = encoded;
  WriteU32(&malformed, RecordOffset(0, kRecordActorOrdinalOffset),
           kGuestExecutionSessionNoThread);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU32(&malformed, RecordOffset(0, kRecordActorThreadIdOffset), 0);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU32(&malformed, RecordOffset(0, kRecordSubjectOrdinalOffset),
           kGuestExecutionSessionNoThread);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU32(&malformed, RecordOffset(0, kRecordSubjectThreadIdOffset), 0);
  RequireDecodeFailure(malformed);
}

TEST_CASE("continuous event codec enforces canonical checkpoint references",
          "[cpu][guest-execution]") {
  const Fixture fixture = MakeFixture();
  const std::vector<uint8_t> encoded = EncodeEvents(fixture.events);
  std::vector<GuestExecutionContinuousEvent> malformed_events = fixture.events;

  malformed_events.front().checkpoint.checkpoint_global_sequence = 1;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.front().checkpoint.state_size = 1;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.front().checkpoint.state_sha256.front() = 1;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.back().checkpoint.kind =
      static_cast<GuestExecutionContinuousCheckpointReferenceKind>(2);
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.back().checkpoint.checkpoint_global_sequence = 103;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.back().checkpoint.state_size = 0;
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.back().checkpoint.state_sha256 = {};
  RequireEncodeFailure(malformed_events);
  malformed_events = fixture.events;
  malformed_events.back().subject = {};
  RequireEncodeFailure(malformed_events);

  std::vector<uint8_t> malformed = encoded;
  WriteU64(&malformed, RecordOffset(0, kRecordCheckpointSequenceOffset), 1);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU64(&malformed, RecordOffset(0, kRecordCheckpointSizeOffset), 1);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  malformed[RecordOffset(0, kRecordCheckpointShaOffset)] = 1;
  RequireDecodeFailure(malformed);
  for (uint32_t kind : {2u, UINT32_MAX}) {
    malformed = encoded;
    WriteU32(&malformed, RecordOffset(2, kRecordCheckpointKindOffset), kind);
    RequireDecodeFailure(malformed);
  }
  malformed = encoded;
  WriteU32(&malformed, RecordOffset(2, kRecordSubjectOrdinalOffset),
           kGuestExecutionSessionNoThread);
  WriteU32(&malformed, RecordOffset(2, kRecordSubjectThreadIdOffset), 0);
  RequireDecodeFailure(malformed);
  malformed = encoded;
  WriteU64(&malformed, RecordOffset(2, kRecordCheckpointSequenceOffset), 103);
  RequireDecodeFailure(malformed);
  for (uint64_t size :
       {uint64_t{0}, uint64_t{2779}, uint64_t{2781}, UINT64_MAX}) {
    malformed = encoded;
    WriteU64(&malformed, RecordOffset(2, kRecordCheckpointSizeOffset), size);
    RequireDecodeFailure(malformed);
  }
  malformed = encoded;
  std::fill_n(malformed.begin() + RecordOffset(2, kRecordCheckpointShaOffset),
              GuestExecutionSessionSha256{}.size(), 0);
  RequireDecodeFailure(malformed);
}

TEST_CASE("continuous event participant bindings are exact and dense",
          "[cpu][guest-execution]") {
  const Fixture fixture = MakeFixture();
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      fixture.events, fixture.participants, &error));
  REQUIRE(error.empty());

  std::vector<GuestExecutionSessionParticipant> participants =
      fixture.participants;
  participants.front().ordinal = 1;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      fixture.events, participants, &error));
  participants = fixture.participants;
  participants.front().guest_thread_id = 0;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      fixture.events, participants, &error));
  participants = fixture.participants;
  participants.back().guest_thread_id = participants.front().guest_thread_id;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      fixture.events, participants, &error));
  participants = fixture.participants;
  --participants.back().guest_thread_id;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      fixture.events, participants, &error));
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      fixture.events, {}, &error));

  std::vector<GuestExecutionContinuousEvent> events = fixture.events;
  events.front().actor = Identity(2, 0x1234);
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      events, fixture.participants, &error));
  events = fixture.events;
  events.front().subject = Identity(2, 0x1234);
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
      events, fixture.participants, &error));
}

TEST_CASE("continuous event limits fail closed without partial output",
          "[cpu][guest-execution]") {
  const Fixture fixture = MakeFixture();
  const std::vector<uint8_t> encoded = EncodeEvents(fixture.events);
  GuestExecutionContinuousEventLimits limits;
  limits.maximum_records = fixture.events.size() - 1;
  std::vector<uint8_t> encoded_output = {0xAA};
  std::string error;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Encode(
      fixture.events, &encoded_output, &error, limits));
  REQUIRE(encoded_output.empty());

  std::vector<GuestExecutionContinuousEvent> decoded_output = fixture.events;
  const auto original = decoded_output;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Decode(
      encoded, &decoded_output, &error, limits));
  REQUIRE(decoded_output == original);
  limits = {};
  limits.maximum_encoded_bytes = encoded.size() - 1;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Decode(
      encoded, &decoded_output, &error, limits));
  REQUIRE(decoded_output == original);

  std::vector<uint8_t> malformed = encoded;
  WriteU64(&malformed, kHeaderRecordCountOffset, UINT64_MAX);
  RequireDecodeFailure(malformed);

  limits.maximum_records = UINT64_MAX;
  limits.maximum_encoded_bytes = UINT64_MAX;
  decoded_output = fixture.events;
  REQUIRE_FALSE(GuestExecutionContinuousEventCodec::Decode(
      malformed, &decoded_output, &error, limits));
  REQUIRE_FALSE(error.empty());
  REQUIRE(decoded_output == original);
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
