/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_artifact.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xenia/cpu/ppc/ppc_context.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::ppc::test {

namespace {

std::array<uint8_t, 32> MakeHash(uint8_t seed) {
  std::array<uint8_t, 32> hash;
  for (size_t i = 0; i < hash.size(); ++i) {
    hash[i] = static_cast<uint8_t>(seed + i * 13);
  }
  return hash;
}

GuestPPCRegisterState MakeRegisterState(uint8_t seed, uint32_t return_address) {
  GuestPPCRegisterState state;
  for (size_t i = 0; i < state.gpr.size(); ++i) {
    state.gpr[i] = 0x0102030405060708ull + seed + i * 0x111111111111111ull;
    state.fpr_bits[i] = 0x7FF8000000000000ull | (uint64_t(seed) << 32) | i;
  }
  for (size_t i = 0; i < state.vector_registers.size(); ++i) {
    for (size_t byte = 0; byte < state.vector_registers[i].size(); ++byte) {
      state.vector_registers[i][byte] =
          static_cast<uint8_t>(seed + i * 3 + byte * 7);
    }
  }
  for (size_t field = 0; field < state.condition_register_fields.size();
       ++field) {
    for (size_t bit = 0; bit < state.condition_register_fields[field].size();
         ++bit) {
      state.condition_register_fields[field][bit] =
          static_cast<uint8_t>(seed + field * 4 + bit);
    }
  }
  state.link_register = return_address;
  state.count_register = 0x1111222233334444ull + seed;
  state.machine_state_register = 0x5555666677778888ull + seed;
  state.fpscr = 0xA0B0C000u + seed;
  for (size_t i = 0; i < state.vscr_vector.size(); ++i) {
    state.vscr_vector[i] = static_cast<uint8_t>(0xF0 - i - seed);
  }
  state.vrsave = 0x12345600u + seed;
  state.xer_ca = static_cast<uint8_t>(0x10 + seed);
  state.xer_ov = static_cast<uint8_t>(0x20 + seed);
  state.xer_so = static_cast<uint8_t>(0x30 + seed);
  state.vscr_sat = static_cast<uint8_t>(0x40 + seed);
  return state;
}

GuestInvocationPage MakePage(uint32_t guest_address, uint8_t seed) {
  GuestInvocationPage page;
  page.guest_address = guest_address;
  for (size_t i = 0; i < page.data.size(); ++i) {
    page.data[i] = static_cast<uint8_t>(seed + i * 17);
  }
  return page;
}

GuestFunctionInvocation MakeInvocation(uint32_t function_address,
                                       uint8_t seed) {
  GuestFunctionInvocation invocation;
  invocation.function_address = function_address;
  invocation.function_end_address = function_address + 0xFFC;
  invocation.entry_address = function_address;
  invocation.expected_return_address = 0x90000000u + uint32_t(seed) * 0x1000;
  invocation.input =
      MakeRegisterState(seed, invocation.expected_return_address);
  invocation.expected_output = MakeRegisterState(
      static_cast<uint8_t>(seed + 1), invocation.expected_return_address);

  const uint32_t data_address = 0x10000000u + uint32_t(seed) * 0x2000;
  invocation.input_data_pages.push_back(MakePage(data_address, seed));
  invocation.input_data_pages.push_back(
      MakePage(data_address + GuestInvocationArtifactCodec::kPageSize,
               static_cast<uint8_t>(seed + 1)));
  invocation.expected_dirty_pages.push_back(
      MakePage(data_address, static_cast<uint8_t>(seed + 3)));
  invocation.expected_dirty_pages.push_back(
      MakePage(data_address + GuestInvocationArtifactCodec::kPageSize,
               static_cast<uint8_t>(seed + 4)));
  return invocation;
}

GuestInvocationArtifact MakeArtifact() {
  GuestInvocationArtifact artifact;
  artifact.capture_build_sha256 = MakeHash(1);
  artifact.code_corpus_sha256 = MakeHash(2);
  artifact.replay_config_sha256 = MakeHash(3);
  artifact.invocations.push_back(MakeInvocation(0x82001000, 4));
  return artifact;
}

std::vector<uint8_t> EncodeValidArtifact() {
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(
      GuestInvocationArtifactCodec::Encode(MakeArtifact(), &encoded, &error));
  REQUIRE(error.empty());
  return encoded;
}

std::vector<uint8_t> EncodeValidRegisterState() {
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(GuestPPCRegisterStateCodec::Encode(
      MakeRegisterState(0x5A, 0x90005000), &encoded, &error));
  REQUIRE(error.empty());
  return encoded;
}

constexpr size_t kThreadParticipantOrdinalOffset = 32;
constexpr size_t kThreadGuestThreadIdOffset = 36;
constexpr size_t kThreadResumeKindOffset = 40;
constexpr size_t kThreadResumePcOffset = 44;
constexpr size_t kThreadOwningFunctionOffset = 48;
constexpr size_t kThreadOwningFunctionEndOffset = 52;
constexpr size_t kThreadOuterReturnOffset = 56;
constexpr size_t kThreadPendingExportOffset = 60;
constexpr size_t kThreadPendingEventOffset = 64;
constexpr size_t kThreadReservationReservedOffset = 72;
constexpr size_t kThreadLifecycleReservedOffset = 88;

GuestPPCThreadCheckpoint MakeThreadCheckpoint(
    GuestPPCThreadResumeKind resume_kind =
        GuestPPCThreadResumeKind::kGuestBlockHead) {
  GuestPPCThreadCheckpoint checkpoint;
  checkpoint.participant_ordinal = 3;
  checkpoint.guest_thread_id = 0x11223344;
  checkpoint.resume_kind = resume_kind;
  checkpoint.resume_pc = 0x82004000;
  checkpoint.owning_function_address = 0x82003000;
  checkpoint.owning_function_end_address = 0x82004FFC;
  checkpoint.outer_guest_return_address = 0xBCBCBCBC;
  if (resume_kind == GuestPPCThreadResumeKind::kPendingModeledBlockingExtern) {
    checkpoint.pending_external_event_sequence = 0x1020304050607080ull;
    checkpoint.pending_export_guest_address = 0x80008000;
  } else if (resume_kind == GuestPPCThreadResumeKind::kOutsideGuest) {
    checkpoint.resume_pc = 0;
    checkpoint.owning_function_address = 0;
    checkpoint.owning_function_end_address = 0;
    checkpoint.outer_guest_return_address = 0;
  }
  checkpoint.registers = MakeRegisterState(0x6B, 0x90006000);
  return checkpoint;
}

GuestPPCThreadCheckpointBinding MakeThreadCheckpointBinding(
    const GuestPPCThreadCheckpoint& checkpoint) {
  GuestPPCThreadCheckpointBinding binding;
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

std::vector<uint8_t> EncodeValidThreadCheckpoint(
    GuestPPCThreadResumeKind resume_kind =
        GuestPPCThreadResumeKind::kGuestBlockHead) {
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(GuestPPCThreadCheckpointCodec::Encode(
      MakeThreadCheckpoint(resume_kind), &encoded, &error));
  REQUIRE(error.empty());
  return encoded;
}

void WriteU32(std::vector<uint8_t>* data, size_t offset, uint32_t value) {
  REQUIRE(offset + 4 <= data->size());
  for (size_t i = 0; i < 4; ++i) {
    (*data)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void WriteU64(std::vector<uint8_t>* data, size_t offset, uint64_t value) {
  REQUIRE(offset + 8 <= data->size());
  for (size_t i = 0; i < 8; ++i) {
    (*data)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

uint64_t ReadU64(const std::vector<uint8_t>& data, size_t offset) {
  REQUIRE(offset + 8 <= data.size());
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= uint64_t(data[offset + i]) << (i * 8);
  }
  return value;
}

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
  REQUIRE(offset + 4 <= data.size());
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value |= uint32_t(data[offset + i]) << (i * 8);
  }
  return value;
}

void RequireDecodeFailure(const std::vector<uint8_t>& data) {
  GuestInvocationArtifact output = MakeArtifact();
  std::string error;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Decode(data, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output.invocations.empty());
}

void RequireRegisterStateDecodeFailure(const std::vector<uint8_t>& data) {
  GuestPPCRegisterState output = MakeRegisterState(0xA5, 0x9000A000);
  std::string error;
  REQUIRE_FALSE(GuestPPCRegisterStateCodec::Decode(data, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output == GuestPPCRegisterState{});
}

void RequireThreadCheckpointDecodeFailure(const std::vector<uint8_t>& data) {
  GuestPPCThreadCheckpoint output = MakeThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  const GuestPPCThreadCheckpoint original = output;
  std::string error;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::Decode(data, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output == original);
}

void RequireThreadCheckpointEncodeFailure(
    const GuestPPCThreadCheckpoint& checkpoint) {
  std::vector<uint8_t> output = {1, 2, 3};
  std::string error;
  REQUIRE_FALSE(
      GuestPPCThreadCheckpointCodec::Encode(checkpoint, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output.empty());
}

}  // namespace

TEST_CASE("PPC register-state codec round-trips its canonical blob exactly",
          "[guest-invocation-artifact]") {
  const GuestPPCRegisterState expected = MakeRegisterState(0x5A, 0x90005000);
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(GuestPPCRegisterStateCodec::Encode(expected, &encoded, &error));
  REQUIRE(error.empty());
  REQUIRE(encoded.size() == GuestPPCRegisterStateCodec::kEncodedSize);
  constexpr std::array<uint8_t, 8> kExpectedMagic = {'X', 'E', 'P', 'P',
                                                     'C', 'S', 'T', 0};
  REQUIRE(std::equal(encoded.cbegin(), encoded.cbegin() + kExpectedMagic.size(),
                     kExpectedMagic.cbegin()));
  REQUIRE(encoded[8] == GuestPPCRegisterStateCodec::kVersion);
  REQUIRE(ReadU64(encoded, 16) == GuestPPCRegisterStateCodec::kEncodedSize);

  GuestPPCRegisterState decoded;
  REQUIRE(GuestPPCRegisterStateCodec::Decode(encoded, &decoded, &error));
  REQUIRE(error.empty());
  REQUIRE(decoded == expected);

  GuestInvocationArtifact artifact = MakeArtifact();
  artifact.invocations[0].input = expected;
  artifact.invocations[0].expected_return_address =
      static_cast<uint32_t>(expected.link_register);
  std::vector<uint8_t> artifact_bytes;
  REQUIRE(GuestInvocationArtifactCodec::Encode(artifact, &artifact_bytes));
  const size_t artifact_state_offset =
      GuestInvocationArtifactCodec::kHeaderSize +
      GuestInvocationArtifactCodec::kInvocationHeaderSize;
  REQUIRE(std::equal(encoded.cbegin() + GuestPPCRegisterStateCodec::kHeaderSize,
                     encoded.cend(),
                     artifact_bytes.cbegin() + artifact_state_offset));
}

TEST_CASE("PPC register-state codec rejects truncation and trailing data",
          "[guest-invocation-artifact]") {
  const std::vector<uint8_t> encoded = EncodeValidRegisterState();
  for (size_t size = 0; size < encoded.size(); ++size) {
    INFO("truncated size " << size);
    RequireRegisterStateDecodeFailure(
        std::vector<uint8_t>(encoded.cbegin(), encoded.cbegin() + size));
  }

  std::vector<uint8_t> trailing = encoded;
  trailing.push_back(0);
  RequireRegisterStateDecodeFailure(trailing);

  GuestPPCRegisterState output = MakeRegisterState(0xA5, 0x9000A000);
  std::string error;
  REQUIRE_FALSE(GuestPPCRegisterStateCodec::Decode(
      nullptr, GuestPPCRegisterStateCodec::kEncodedSize, &output, &error));
  REQUIRE(output == GuestPPCRegisterState{});
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(GuestPPCRegisterStateCodec::Decode(encoded, nullptr, &error));
  REQUIRE_FALSE(GuestPPCRegisterStateCodec::Encode(output, nullptr, &error));
}

TEST_CASE("PPC register-state codec rejects noncanonical header fields",
          "[guest-invocation-artifact]") {
  std::vector<uint8_t> malformed = EncodeValidRegisterState();
  malformed[0] ^= 1;
  RequireRegisterStateDecodeFailure(malformed);

  malformed = EncodeValidRegisterState();
  WriteU32(&malformed, 8, GuestPPCRegisterStateCodec::kVersion + 1);
  RequireRegisterStateDecodeFailure(malformed);

  malformed = EncodeValidRegisterState();
  WriteU32(&malformed, 12, GuestPPCRegisterStateCodec::kHeaderSize + 4);
  RequireRegisterStateDecodeFailure(malformed);

  malformed = EncodeValidRegisterState();
  WriteU64(&malformed, 16, GuestPPCRegisterStateCodec::kEncodedSize - 1);
  RequireRegisterStateDecodeFailure(malformed);

  malformed = EncodeValidRegisterState();
  WriteU32(&malformed, 24, 1);
  RequireRegisterStateDecodeFailure(malformed);

  malformed = EncodeValidRegisterState();
  WriteU32(&malformed, 28, 1);
  RequireRegisterStateDecodeFailure(malformed);
}

TEST_CASE("PPC register-state codec preserves architectural edge bits",
          "[guest-invocation-artifact]") {
  GuestPPCRegisterState expected;
  for (size_t i = 0; i < expected.gpr.size(); ++i) {
    expected.gpr[i] = i & 1 ? UINT64_MAX : 0;
    expected.fpr_bits[i] =
        i & 1 ? 0xFFF0000000000001ull : 0x8000000000000000ull;
  }
  for (size_t i = 0; i < expected.vector_registers.size(); ++i) {
    expected.vector_registers[i].fill(i & 1 ? UINT8_MAX : 0);
  }
  for (size_t i = 0; i < expected.condition_register_fields.size(); ++i) {
    expected.condition_register_fields[i].fill(i & 1 ? UINT8_MAX : 0);
  }
  expected.link_register = UINT64_MAX;
  expected.count_register = UINT64_MAX - 1;
  expected.machine_state_register = UINT64_MAX - 2;
  expected.fpscr = UINT32_MAX;
  expected.vscr_vector.fill(UINT8_MAX);
  expected.vrsave = UINT32_MAX;
  expected.xer_ca = UINT8_MAX;
  expected.xer_ov = UINT8_MAX - 1;
  expected.xer_so = 1;
  expected.vscr_sat = UINT8_MAX;

  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  REQUIRE(GuestPPCRegisterStateCodec::Encode(expected, &first));
  REQUIRE(GuestPPCRegisterStateCodec::Encode(expected, &second));
  REQUIRE(first == second);

  GuestPPCRegisterState decoded;
  REQUIRE(GuestPPCRegisterStateCodec::Decode(first, &decoded));
  REQUIRE(decoded == expected);
}

TEST_CASE("PPC thread checkpoint wraps one exact canonical register blob",
          "[guest-invocation-artifact]") {
  REQUIRE(GuestPPCRegisterStateCodec::kEncodedSize == 2676);
  REQUIRE(GuestPPCThreadCheckpointCodec::kHeaderSize == 32);
  REQUIRE(GuestPPCThreadCheckpointCodec::kMetadataSize == 72);
  REQUIRE(GuestPPCThreadCheckpointCodec::kReservationReservedSize == 16);
  REQUIRE(GuestPPCThreadCheckpointCodec::kLifecycleReservedSize == 16);
  REQUIRE(GuestPPCThreadCheckpointCodec::kRegisterStateOffset == 104);
  REQUIRE(GuestPPCThreadCheckpointCodec::kEncodedSize == 2780);

  for (GuestPPCThreadResumeKind resume_kind :
       {GuestPPCThreadResumeKind::kGuestBlockHead,
        GuestPPCThreadResumeKind::kPendingModeledBlockingExtern,
        GuestPPCThreadResumeKind::kOutsideGuest}) {
    INFO("resume kind " << static_cast<uint32_t>(resume_kind));
    const GuestPPCThreadCheckpoint expected = MakeThreadCheckpoint(resume_kind);
    std::vector<uint8_t> encoded;
    std::string error;
    REQUIRE(GuestPPCThreadCheckpointCodec::Encode(expected, &encoded, &error));
    REQUIRE(error.empty());
    REQUIRE(encoded.size() == GuestPPCThreadCheckpointCodec::kEncodedSize);
    constexpr std::array<uint8_t, 8> kExpectedMagic = {'X', 'E', 'P', 'P',
                                                       'C', 'T', 'C', 0};
    REQUIRE(std::equal(encoded.cbegin(),
                       encoded.cbegin() + kExpectedMagic.size(),
                       kExpectedMagic.cbegin()));
    REQUIRE(ReadU32(encoded, 8) == GuestPPCThreadCheckpointCodec::kVersion);
    REQUIRE(ReadU32(encoded, 12) == GuestPPCThreadCheckpointCodec::kHeaderSize);
    REQUIRE(ReadU64(encoded, 16) ==
            GuestPPCThreadCheckpointCodec::kEncodedSize);
    REQUIRE(ReadU32(encoded, kThreadParticipantOrdinalOffset) ==
            expected.participant_ordinal);
    REQUIRE(ReadU32(encoded, kThreadGuestThreadIdOffset) ==
            expected.guest_thread_id);
    REQUIRE(ReadU32(encoded, kThreadResumeKindOffset) ==
            static_cast<uint32_t>(expected.resume_kind));
    REQUIRE(ReadU32(encoded, kThreadResumePcOffset) == expected.resume_pc);
    REQUIRE(ReadU32(encoded, kThreadOwningFunctionOffset) ==
            expected.owning_function_address);
    REQUIRE(ReadU32(encoded, kThreadOwningFunctionEndOffset) ==
            expected.owning_function_end_address);
    REQUIRE(ReadU32(encoded, kThreadOuterReturnOffset) ==
            expected.outer_guest_return_address);
    REQUIRE(ReadU32(encoded, kThreadPendingExportOffset) ==
            expected.pending_export_guest_address);
    REQUIRE(ReadU64(encoded, kThreadPendingEventOffset) ==
            expected.pending_external_event_sequence);
    REQUIRE(std::all_of(encoded.cbegin() + kThreadReservationReservedOffset,
                        encoded.cbegin() + kThreadLifecycleReservedOffset,
                        [](uint8_t value) { return value == 0; }));
    REQUIRE(std::all_of(
        encoded.cbegin() + kThreadLifecycleReservedOffset,
        encoded.cbegin() + GuestPPCThreadCheckpointCodec::kRegisterStateOffset,
        [](uint8_t value) { return value == 0; }));

    std::vector<uint8_t> register_blob;
    REQUIRE(
        GuestPPCRegisterStateCodec::Encode(expected.registers, &register_blob));
    REQUIRE(std::equal(
        encoded.cbegin() + GuestPPCThreadCheckpointCodec::kRegisterStateOffset,
        encoded.cend(), register_blob.cbegin(), register_blob.cend()));

    GuestPPCThreadCheckpoint decoded = MakeThreadCheckpoint();
    REQUIRE(GuestPPCThreadCheckpointCodec::Decode(encoded, &decoded, &error));
    REQUIRE(error.empty());
    REQUIRE(decoded == expected);
    REQUIRE(GuestPPCThreadCheckpointCodec::ValidateBinding(
        decoded, MakeThreadCheckpointBinding(expected), &error));
    REQUIRE(error.empty());
  }
}

TEST_CASE("PPC thread checkpoint rejects every truncation and trailing byte",
          "[guest-invocation-artifact]") {
  const std::vector<uint8_t> encoded = EncodeValidThreadCheckpoint();
  for (size_t size = 0; size < encoded.size(); ++size) {
    INFO("truncated size " << size);
    RequireThreadCheckpointDecodeFailure(
        std::vector<uint8_t>(encoded.cbegin(), encoded.cbegin() + size));
  }

  std::vector<uint8_t> trailing = encoded;
  trailing.push_back(0);
  RequireThreadCheckpointDecodeFailure(trailing);

  GuestPPCThreadCheckpoint output = MakeThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  const GuestPPCThreadCheckpoint original = output;
  std::string error;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::Decode(
      nullptr, GuestPPCThreadCheckpointCodec::kEncodedSize, &output, &error));
  REQUIRE(output == original);
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(
      GuestPPCThreadCheckpointCodec::Decode(nullptr, 0, &output, &error));
  REQUIRE(output == original);
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(
      GuestPPCThreadCheckpointCodec::Decode(encoded, nullptr, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::Encode(output, nullptr, &error));
  REQUIRE_FALSE(error.empty());
}

TEST_CASE("PPC thread checkpoint rejects forged headers and reserved space",
          "[guest-invocation-artifact]") {
  std::vector<uint8_t> malformed = EncodeValidThreadCheckpoint();
  malformed[0] ^= 1;
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, 8, GuestPPCThreadCheckpointCodec::kVersion + 1);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, 12, GuestPPCThreadCheckpointCodec::kHeaderSize + 4);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU64(&malformed, 16, GuestPPCThreadCheckpointCodec::kEncodedSize - 1);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU64(&malformed, 16, UINT64_MAX);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, 24, 1);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, 24, UINT32_MAX);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, 28, 1);
  RequireThreadCheckpointDecodeFailure(malformed);

  for (size_t offset = kThreadReservationReservedOffset;
       offset < GuestPPCThreadCheckpointCodec::kRegisterStateOffset; ++offset) {
    INFO("reserved byte offset " << offset);
    malformed = EncodeValidThreadCheckpoint();
    malformed[offset] = 1;
    RequireThreadCheckpointDecodeFailure(malformed);
  }

  malformed = EncodeValidThreadCheckpoint();
  malformed[GuestPPCThreadCheckpointCodec::kRegisterStateOffset] ^= 1;
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU64(&malformed, GuestPPCThreadCheckpointCodec::kRegisterStateOffset + 16,
           GuestPPCRegisterStateCodec::kEncodedSize - 1);
  RequireThreadCheckpointDecodeFailure(malformed);
}

TEST_CASE("PPC thread checkpoint enforces canonical resume identities",
          "[guest-invocation-artifact]") {
  GuestPPCThreadCheckpoint checkpoint = MakeThreadCheckpoint();
  checkpoint.guest_thread_id = 0;
  RequireThreadCheckpointEncodeFailure(checkpoint);

  checkpoint = MakeThreadCheckpoint();
  checkpoint.resume_pc = 0;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.resume_pc = 0x82004002;
  RequireThreadCheckpointEncodeFailure(checkpoint);

  checkpoint = MakeThreadCheckpoint();
  checkpoint.owning_function_address = 0;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint = MakeThreadCheckpoint();
  checkpoint.owning_function_address |= 2;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint = MakeThreadCheckpoint();
  checkpoint.owning_function_end_address |= 2;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint = MakeThreadCheckpoint();
  checkpoint.owning_function_end_address =
      checkpoint.owning_function_address - 4;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint = MakeThreadCheckpoint();
  checkpoint.resume_pc = checkpoint.owning_function_address - 4;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint = MakeThreadCheckpoint();
  checkpoint.resume_pc = checkpoint.owning_function_end_address + 4;
  RequireThreadCheckpointEncodeFailure(checkpoint);

  checkpoint = MakeThreadCheckpoint();
  checkpoint.outer_guest_return_address = 0;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.outer_guest_return_address = 0xBCBCBCBE;
  RequireThreadCheckpointEncodeFailure(checkpoint);

  checkpoint = MakeThreadCheckpoint();
  checkpoint.resume_kind = static_cast<GuestPPCThreadResumeKind>(0);
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.resume_kind = static_cast<GuestPPCThreadResumeKind>(4);
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.resume_kind = static_cast<GuestPPCThreadResumeKind>(UINT32_MAX);
  RequireThreadCheckpointEncodeFailure(checkpoint);

  checkpoint = MakeThreadCheckpoint();
  checkpoint.pending_external_event_sequence = 1;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.pending_external_event_sequence = 0;
  checkpoint.pending_export_guest_address = 0x80008000;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.pending_external_event_sequence = 1;
  RequireThreadCheckpointEncodeFailure(checkpoint);

  checkpoint = MakeThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  checkpoint.pending_external_event_sequence = 0;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint = MakeThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  checkpoint.pending_export_guest_address = 0;
  RequireThreadCheckpointEncodeFailure(checkpoint);
  checkpoint.pending_export_guest_address = 0x80008002;
  RequireThreadCheckpointEncodeFailure(checkpoint);

  for (uint32_t forged_kind : {0u, 4u, UINT32_MAX}) {
    INFO("forged resume kind " << forged_kind);
    std::vector<uint8_t> malformed = EncodeValidThreadCheckpoint();
    WriteU32(&malformed, kThreadResumeKindOffset, forged_kind);
    RequireThreadCheckpointDecodeFailure(malformed);
  }

  std::vector<uint8_t> malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadGuestThreadIdOffset, 0);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadResumePcOffset, 0x82004002);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadOwningFunctionOffset, 0);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadOwningFunctionEndOffset, 0x82002FFC);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadResumePcOffset, 0x82005000);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadOuterReturnOffset, 0);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU64(&malformed, kThreadPendingEventOffset, 1);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint();
  WriteU32(&malformed, kThreadPendingExportOffset, 0x80008000);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  WriteU64(&malformed, kThreadPendingEventOffset, 0);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  WriteU32(&malformed, kThreadPendingExportOffset, 0);
  RequireThreadCheckpointDecodeFailure(malformed);

  malformed = EncodeValidThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  WriteU32(&malformed, kThreadPendingExportOffset, 0x80008002);
  RequireThreadCheckpointDecodeFailure(malformed);
}

TEST_CASE("PPC thread checkpoint binding rejects every identity mismatch",
          "[guest-invocation-artifact]") {
  const GuestPPCThreadCheckpoint checkpoint = MakeThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  const GuestPPCThreadCheckpointBinding expected =
      MakeThreadCheckpointBinding(checkpoint);
  std::string error;
  REQUIRE(GuestPPCThreadCheckpointCodec::ValidateBinding(checkpoint, expected,
                                                         &error));
  REQUIRE(error.empty());

  GuestPPCThreadCheckpointBinding mismatch = expected;
  ++mismatch.participant_ordinal;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  ++mismatch.guest_thread_id;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  mismatch.resume_pc += 4;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  mismatch.owning_function_address += 4;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  mismatch.owning_function_end_address -= 4;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  mismatch.outer_guest_return_address += 4;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  ++mismatch.pending_external_event_sequence;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  mismatch.pending_export_guest_address += 4;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  mismatch = expected;
  mismatch.resume_kind = GuestPPCThreadResumeKind::kGuestBlockHead;
  mismatch.pending_external_event_sequence = 0;
  mismatch.pending_export_guest_address = 0;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      checkpoint, mismatch, &error));
  REQUIRE_FALSE(error.empty());

  GuestPPCThreadCheckpoint invalid = checkpoint;
  invalid.resume_pc |= 2;
  REQUIRE_FALSE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      invalid, expected, &error));
  REQUIRE_FALSE(error.empty());
}

TEST_CASE("PPC thread checkpoint preserves full-width routing and edge bits",
          "[guest-invocation-artifact]") {
  GuestPPCThreadCheckpoint expected = MakeThreadCheckpoint(
      GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  expected.participant_ordinal = UINT32_MAX;
  expected.guest_thread_id = UINT32_MAX;
  expected.owning_function_address = 0xFFFFFFF0;
  expected.owning_function_end_address = 0xFFFFFFFC;
  expected.resume_pc = 0xFFFFFFFC;
  expected.outer_guest_return_address = 0xFFFFFFF8;
  expected.pending_external_event_sequence = UINT64_MAX;
  expected.pending_export_guest_address = 0xFFFFFFF4;
  expected.registers.gpr.front() = UINT64_MAX;
  expected.registers.gpr.back() = 0;
  expected.registers.fpr_bits.front() = 0xFFF0000000000001ull;
  expected.registers.fpr_bits.back() = 0x8000000000000000ull;
  expected.registers.vector_registers.front().fill(UINT8_MAX);
  expected.registers.condition_register_fields.back().fill(UINT8_MAX);
  expected.registers.link_register = UINT64_MAX;
  expected.registers.fpscr = UINT32_MAX;
  expected.registers.vscr_vector.fill(UINT8_MAX);
  expected.registers.vrsave = UINT32_MAX;
  expected.registers.xer_ca = UINT8_MAX;
  expected.registers.xer_ov = UINT8_MAX;
  expected.registers.xer_so = UINT8_MAX;
  expected.registers.vscr_sat = UINT8_MAX;

  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  REQUIRE(GuestPPCThreadCheckpointCodec::Encode(expected, &first));
  REQUIRE(GuestPPCThreadCheckpointCodec::Encode(expected, &second));
  REQUIRE(first == second);

  GuestPPCThreadCheckpoint decoded;
  REQUIRE(GuestPPCThreadCheckpointCodec::Decode(first, &decoded));
  REQUIRE(decoded == expected);
  REQUIRE(GuestPPCThreadCheckpointCodec::ValidateBinding(
      decoded, MakeThreadCheckpointBinding(expected)));
}

TEST_CASE("guest invocation artifact round-trips multiple captures exactly",
          "[guest-invocation-artifact]") {
  GuestInvocationArtifact expected = MakeArtifact();
  expected.invocations.push_back(MakeInvocation(0x83002000, 9));

  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(GuestInvocationArtifactCodec::Encode(expected, &encoded, &error));
  REQUIRE(error.empty());

  GuestInvocationArtifact decoded;
  REQUIRE(GuestInvocationArtifactCodec::Decode(encoded, &decoded, &error));
  REQUIRE(error.empty());
  REQUIRE(decoded == expected);
}

TEST_CASE("guest invocation artifact wire format is exact little endian",
          "[guest-invocation-artifact]") {
  GuestInvocationArtifact artifact = MakeArtifact();
  std::vector<uint8_t> encoded;
  REQUIRE(GuestInvocationArtifactCodec::Encode(artifact, &encoded));

  constexpr uint64_t kExpectedSize =
      GuestInvocationArtifactCodec::kHeaderSize +
      GuestInvocationArtifactCodec::kInvocationHeaderSize +
      2ull * GuestInvocationArtifactCodec::kArchitecturalStateSize +
      4ull * GuestInvocationArtifactCodec::kPageRecordSize;
  REQUIRE(encoded.size() == kExpectedSize);
  REQUIRE(std::equal(
      encoded.cbegin(), encoded.cbegin() + 8,
      std::array<uint8_t, 8>{'X', 'E', 'P', 'P', 'C', 'I', 'R', 0}.cbegin()));
  REQUIRE(encoded[8] == 1);
  REQUIRE(encoded[9] == 0);
  REQUIRE(ReadU64(encoded, 24) == kExpectedSize);

  const size_t invocation_offset = GuestInvocationArtifactCodec::kHeaderSize;
  REQUIRE(encoded[invocation_offset + 8] == 0x00);
  REQUIRE(encoded[invocation_offset + 9] == 0x10);
  REQUIRE(encoded[invocation_offset + 10] == 0x00);
  REQUIRE(encoded[invocation_offset + 11] == 0x82);
  REQUIRE(ReadU64(encoded,
                  invocation_offset +
                      GuestInvocationArtifactCodec::kInvocationHeaderSize) ==
          artifact.invocations[0].input.gpr[0]);
}

TEST_CASE("PPC architectural state excludes replay and host fields",
          "[guest-invocation-artifact]") {
  const GuestPPCRegisterState expected = MakeRegisterState(7, 0x90007000);
  PPCContext context = {};
  context.preempt_requested = 0xA1;
  context.thread_id = 0xA2A3A4A5;
  context.scratch = 0xA6A7A8A9AAABACADull;
  context.last_safepoint_pc = 0xAEAFB0B1;
  context.global_mutex =
      reinterpret_cast<decltype(context.global_mutex)>(uintptr_t(0x1110));
  context.processor =
      reinterpret_cast<decltype(context.processor)>(uintptr_t(0x2220));
  context.kernel_state =
      reinterpret_cast<decltype(context.kernel_state)>(uintptr_t(0x3330));
  context.physical_membase = reinterpret_cast<uint8_t*>(uintptr_t(0x4440));
  context.thread_state =
      reinterpret_cast<decltype(context.thread_state)>(uintptr_t(0x5550));
  context.virtual_membase = reinterpret_cast<uint8_t*>(uintptr_t(0x6660));
  context.trace_counts = reinterpret_cast<uint8_t*>(uintptr_t(0x7770));

  RestoreGuestPPCRegisterState(expected, &context);
  REQUIRE(CaptureGuestPPCRegisterState(context) == expected);

  REQUIRE(context.preempt_requested == 0xA1);
  REQUIRE(context.thread_id == 0xA2A3A4A5);
  REQUIRE(context.scratch == 0xA6A7A8A9AAABACADull);
  REQUIRE(context.last_safepoint_pc == 0xAEAFB0B1);
  REQUIRE(reinterpret_cast<uintptr_t>(context.global_mutex) == 0x1110);
  REQUIRE(reinterpret_cast<uintptr_t>(context.processor) == 0x2220);
  REQUIRE(reinterpret_cast<uintptr_t>(context.kernel_state) == 0x3330);
  REQUIRE(reinterpret_cast<uintptr_t>(context.physical_membase) == 0x4440);
  REQUIRE(reinterpret_cast<uintptr_t>(context.thread_state) == 0x5550);
  REQUIRE(reinterpret_cast<uintptr_t>(context.virtual_membase) == 0x6660);
  REQUIRE(reinterpret_cast<uintptr_t>(context.trace_counts) == 0x7770);

  RestoreGuestPPCRegisterState(expected, nullptr);
}

TEST_CASE("PPC architectural capture maps named context fields independently",
          "[guest-invocation-artifact]") {
  PPCContext context = {};
  context.r[3] = 0x0123456789ABCDEFull;
  context.cr0.cr0_lt = 1;
  context.cr0.cr0_gt = 2;
  context.cr0.cr0_eq = 3;
  context.cr0.cr0_so = 4;
  context.cr1.cr1_fx = 5;
  context.cr1.cr1_fex = 6;
  context.cr1.cr1_vx = 7;
  context.cr1.cr1_ox = 8;
  context.cr2.cr2_0 = 9;
  context.cr2.cr2_1 = 10;
  context.cr2.cr2_2 = 11;
  context.cr2.cr2_3 = 12;
  context.cr3.cr3_0 = 13;
  context.cr3.cr3_1 = 14;
  context.cr3.cr3_2 = 15;
  context.cr3.cr3_3 = 16;
  context.cr4.cr4_0 = 17;
  context.cr4.cr4_1 = 18;
  context.cr4.cr4_2 = 19;
  context.cr4.cr4_3 = 20;
  context.cr5.cr5_0 = 21;
  context.cr5.cr5_1 = 22;
  context.cr5.cr5_2 = 23;
  context.cr5.cr5_3 = 24;
  context.cr6.cr6_all_equal = 25;
  context.cr6.cr6_1 = 26;
  context.cr6.cr6_none_equal = 27;
  context.cr6.cr6_3 = 28;
  context.cr7.cr7_0 = 29;
  context.cr7.cr7_1 = 30;
  context.cr7.cr7_2 = 31;
  context.cr7.cr7_3 = 32;

  constexpr uint64_t kNanBits = 0x7FF8123456789ABCull;
  context.f[7] = std::bit_cast<double>(kNanBits);
  std::array<uint8_t, 16> expected_vector;
  std::array<uint8_t, 16> expected_vscr_vector;
  for (size_t i = 0; i < 16; ++i) {
    expected_vector[i] = static_cast<uint8_t>(0x40 + i);
    expected_vscr_vector[i] = static_cast<uint8_t>(0x80 + i);
    context.v[11].u8[i] = expected_vector[i];
    context.vscr_vec.u8[i] = expected_vscr_vector[i];
  }
  context.lr = 0x1111222233334444ull;
  context.ctr = 0x5555666677778888ull;
  context.msr = 0x9999AAAABBBBCCCCull;
  context.fpscr.value = 0xDDEEFF00u;
  context.xer_ca = 0xA1;
  context.xer_ov = 0xA2;
  context.xer_so = 0xA3;
  context.vscr_sat = 0xA4;
  context.vrsave = 0xB1B2B3B4u;

  const GuestPPCRegisterState state = CaptureGuestPPCRegisterState(context);
  const std::array<std::array<uint8_t, 4>, 8> expected_cr = {
      std::array<uint8_t, 4>{1, 2, 3, 4},
      std::array<uint8_t, 4>{5, 6, 7, 8},
      std::array<uint8_t, 4>{9, 10, 11, 12},
      std::array<uint8_t, 4>{13, 14, 15, 16},
      std::array<uint8_t, 4>{17, 18, 19, 20},
      std::array<uint8_t, 4>{21, 22, 23, 24},
      std::array<uint8_t, 4>{25, 26, 27, 28},
      std::array<uint8_t, 4>{29, 30, 31, 32},
  };
  REQUIRE(state.gpr[3] == 0x0123456789ABCDEFull);
  REQUIRE(state.condition_register_fields == expected_cr);
  REQUIRE(state.fpr_bits[7] == kNanBits);
  REQUIRE(state.vector_registers[11] == expected_vector);
  REQUIRE(state.vscr_vector == expected_vscr_vector);
  REQUIRE(state.link_register == 0x1111222233334444ull);
  REQUIRE(state.count_register == 0x5555666677778888ull);
  REQUIRE(state.machine_state_register == 0x9999AAAABBBBCCCCull);
  REQUIRE(state.fpscr == 0xDDEEFF00u);
  REQUIRE(state.xer_ca == 0xA1);
  REQUIRE(state.xer_ov == 0xA2);
  REQUIRE(state.xer_so == 0xA3);
  REQUIRE(state.vscr_sat == 0xA4);
  REQUIRE(state.vrsave == 0xB1B2B3B4u);
}

TEST_CASE("guest invocation encoder rejects incomplete provenance and work",
          "[guest-invocation-artifact]") {
  GuestInvocationArtifact artifact = MakeArtifact();
  std::vector<uint8_t> output = {1, 2, 3};
  std::string error;

  artifact.capture_build_sha256 = {};
  REQUIRE_FALSE(
      GuestInvocationArtifactCodec::Encode(artifact, &output, &error));
  REQUIRE(output.empty());

  artifact = MakeArtifact();
  artifact.code_corpus_sha256 = {};
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.replay_config_sha256 = {};
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations.clear();
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));
}

TEST_CASE("guest invocation encoder rejects every unsafe dependency",
          "[guest-invocation-artifact]") {
  for (uint32_t bit = 1;
       bit <= kGuestInvocationDependencyUnsupportedMappingOrProtection;
       bit <<= 1) {
    INFO("dependency bit " << bit);
    GuestInvocationArtifact artifact = MakeArtifact();
    artifact.invocations[0].dependency_flags = bit;
    std::vector<uint8_t> output;
    REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));
  }

  GuestInvocationArtifact artifact = MakeArtifact();
  artifact.invocations[0].dependency_flags = 1u << 31;
  std::vector<uint8_t> output;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));
}

TEST_CASE("guest invocation encoder rejects unsafe address and page layouts",
          "[guest-invocation-artifact]") {
  std::vector<uint8_t> output;

  GuestInvocationArtifact artifact = MakeArtifact();
  artifact.invocations[0].entry_address =
      artifact.invocations[0].function_address + 4;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].expected_return_address =
      artifact.invocations[0].function_address;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input.link_register++;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages[1].guest_address =
      artifact.invocations[0].input_data_pages[0].guest_address;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages[0].guest_address++;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages.insert(
      artifact.invocations[0].input_data_pages.begin(), MakePage(0x1000, 0xA0));
  artifact.invocations[0].input_data_pages.push_back(
      MakePage(0x7EFFF000, 0xA1));
  artifact.invocations[0].input_data_pages.push_back(
      MakePage(0x80000000, 0xA2));
  artifact.invocations[0].input_data_pages.push_back(
      MakePage(0x9FFFF000, 0xA3));
  REQUIRE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages[0].guest_address = 0;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages[1].guest_address = 0x7F000000;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages[1].guest_address = 0x7FFFF000;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].input_data_pages[1].guest_address = 0xA0000000;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].expected_dirty_pages[0].guest_address = 0x20000000;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));

  artifact = MakeArtifact();
  artifact.invocations[0].expected_dirty_pages[0].data =
      artifact.invocations[0].input_data_pages[0].data;
  REQUIRE_FALSE(GuestInvocationArtifactCodec::Encode(artifact, &output));
}

TEST_CASE("guest invocation decoder rejects truncation and trailing data",
          "[guest-invocation-artifact]") {
  const std::vector<uint8_t> encoded = EncodeValidArtifact();
  for (size_t size = 0; size < encoded.size(); ++size) {
    INFO("truncated size " << size);
    std::vector<uint8_t> truncated(encoded.cbegin(), encoded.cbegin() + size);
    RequireDecodeFailure(truncated);
  }

  std::vector<uint8_t> trailing = encoded;
  trailing.push_back(0);
  RequireDecodeFailure(trailing);
}

TEST_CASE("guest invocation decoder rejects unknown and nonzero fields",
          "[guest-invocation-artifact]") {
  constexpr size_t kInvocationOffset =
      GuestInvocationArtifactCodec::kHeaderSize;

  std::vector<uint8_t> malformed = EncodeValidArtifact();
  WriteU32(&malformed, 8, GuestInvocationArtifactCodec::kVersion + 1);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, 20, 1);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, 36, 1);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kInvocationOffset + 24, kGuestInvocationDependencyMmio);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kInvocationOffset + 24, 1u << 31);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kInvocationOffset + 36, 1);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU64(&malformed, kInvocationOffset,
           ReadU64(malformed, kInvocationOffset) + 1);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kInvocationOffset + 28,
           GuestInvocationArtifactCodec::kMaxDataPagesPerInvocation + 1);
  RequireDecodeFailure(malformed);
}

TEST_CASE("guest invocation decoder rejects malformed page sets",
          "[guest-invocation-artifact]") {
  constexpr size_t kPageOffset =
      GuestInvocationArtifactCodec::kHeaderSize +
      GuestInvocationArtifactCodec::kInvocationHeaderSize +
      2 * GuestInvocationArtifactCodec::kArchitecturalStateSize;
  constexpr size_t kSecondInputPageOffset =
      kPageOffset + GuestInvocationArtifactCodec::kPageRecordSize;
  constexpr size_t kFirstOutputPageOffset =
      kPageOffset + 2 * GuestInvocationArtifactCodec::kPageRecordSize;
  constexpr size_t kSecondOutputPageOffset =
      kFirstOutputPageOffset + GuestInvocationArtifactCodec::kPageRecordSize;

  std::vector<uint8_t> malformed = EncodeValidArtifact();
  WriteU32(&malformed, kSecondInputPageOffset, 0x10007000);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kSecondInputPageOffset, 0x10008001);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kSecondInputPageOffset, 0x7F000000);
  WriteU32(&malformed, kSecondOutputPageOffset, 0x7F000000);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kSecondInputPageOffset, 0x10008000);
  WriteU32(&malformed, kPageOffset, 0x10008000);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kFirstOutputPageOffset, 0x20000000);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  WriteU32(&malformed, kSecondOutputPageOffset, 0x10008000);
  RequireDecodeFailure(malformed);

  malformed = EncodeValidArtifact();
  std::copy(malformed.cbegin() + kPageOffset + 4,
            malformed.cbegin() + kPageOffset +
                GuestInvocationArtifactCodec::kPageRecordSize,
            malformed.begin() + kFirstOutputPageOffset + 4);
  RequireDecodeFailure(malformed);
}

}  // namespace xe::cpu::ppc::test
