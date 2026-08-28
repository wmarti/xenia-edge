/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_runner.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/guest_invocation_replay_cli.h"
#include "xenia/cpu/testing/util.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {

namespace {

constexpr uint32_t kCodeAddress = 0x82040000u;
constexpr uint32_t kDataAddress = 0x10000000u;
constexpr uint32_t kReturnAddress = 0x83000000u;
constexpr uint32_t kFunctionEndAddress = kCodeAddress + 28;
constexpr uint32_t kMutationOffset = 0x100;
constexpr uint64_t kLoopCount = 0x40000;
constexpr uint64_t kEpoch = 0x1122334455667788ull;
constexpr uint32_t kGuestPageSize = GuestExecutionSessionCodec::kGuestPageSize;

const std::array<uint8_t, 4> kMutationBytes = {0xDE, 0xAD, 0xBE, 0xEF};

GuestExecutionSessionSha256 Identity(uint8_t seed) {
  GuestExecutionSessionSha256 digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(seed + i);
  }
  return digest;
}

GuestExecutionSessionSha256 AddBlob(GuestExecutionSessionBundle* bundle,
                                    std::vector<uint8_t> bytes) {
  const GuestExecutionSessionSha256 digest =
      GuestExecutionSessionCodec::HashBytes(bytes);
  for (const GuestExecutionSessionContentBlob& blob : bundle->content_blobs) {
    if (blob.sha256 == digest) {
      return digest;
    }
  }
  GuestExecutionSessionContentBlob blob;
  blob.sha256 = digest;
  blob.bytes = std::move(bytes);
  bundle->content_blobs.push_back(std::move(blob));
  return digest;
}

void StoreGuestInstruction(std::array<uint8_t, kGuestPageSize>* page,
                           uint32_t offset, uint32_t instruction) {
  (*page)[offset + 0] = static_cast<uint8_t>(instruction >> 24);
  (*page)[offset + 1] = static_cast<uint8_t>(instruction >> 16);
  (*page)[offset + 2] = static_cast<uint8_t>(instruction >> 8);
  (*page)[offset + 3] = static_cast<uint8_t>(instruction);
}

// lwz/addi/stw on the shared word, then a CTR loop that leaves real guest
// work in r7 without touching CR or XER.
std::vector<uint8_t> EncodeCorpus(uint32_t host_page_size) {
  ExecutionJitCorpusBuilder builder(0);
  std::string error;
  for (uint32_t i = 0; i < host_page_size / kGuestPageSize; ++i) {
    std::array<uint8_t, kGuestPageSize> page = {};
    if (!i) {
      StoreGuestInstruction(&page, 0, 0x80A30000u);   // lwz r5, 0(r3)
      StoreGuestInstruction(&page, 4, 0x38A50001u);   // addi r5, r5, 1
      StoreGuestInstruction(&page, 8, 0x90A30000u);   // stw r5, 0(r3)
      StoreGuestInstruction(&page, 12, 0x3CC00004u);  // lis r6, 4
      StoreGuestInstruction(&page, 16, 0x7CC903A6u);  // mtctr r6
      StoreGuestInstruction(&page, 20, 0x38E70003u);  // addi r7, r7, 3
      StoreGuestInstruction(&page, 24, 0x4200FFFCu);  // bdnz -4
      StoreGuestInstruction(&page, 28, 0x4E800020u);  // blr
    }
    REQUIRE(builder.AddCodePage(kCodeAddress + i * kGuestPageSize, page.data(),
                                page.size(), &error));
  }
  REQUIRE(
      builder.AddFunction({kCodeAddress, kFunctionEndAddress, 64, 0}, &error));
  std::vector<uint8_t> bytes;
  REQUIRE(builder.Encode(&bytes, &error));
  REQUIRE(error.empty());
  return bytes;
}

ppc::GuestInvocationPage MakeDataPage(uint32_t address, uint8_t seed) {
  ppc::GuestInvocationPage page;
  page.guest_address = address;
  for (size_t i = 0; i < page.data.size(); ++i) {
    page.data[i] = static_cast<uint8_t>(seed + i * 13);
  }
  return page;
}

void StoreWord(std::array<uint8_t, kGuestPageSize>* data, uint32_t value) {
  (*data)[0] = static_cast<uint8_t>(value >> 24);
  (*data)[1] = static_cast<uint8_t>(value >> 16);
  (*data)[2] = static_cast<uint8_t>(value >> 8);
  (*data)[3] = static_cast<uint8_t>(value);
}

std::vector<ppc::GuestInvocationPage> MakeDataPages(uint32_t host_page_size,
                                                    uint32_t initial_word,
                                                    bool mutated) {
  std::vector<ppc::GuestInvocationPage> pages;
  for (uint32_t i = 0; i < host_page_size / kGuestPageSize; ++i) {
    pages.push_back(MakeDataPage(kDataAddress + i * kGuestPageSize,
                                 static_cast<uint8_t>(0x20 + i)));
  }
  StoreWord(&pages.front().data, initial_word);
  if (mutated) {
    std::copy(kMutationBytes.cbegin(), kMutationBytes.cend(),
              pages.front().data.begin() + kMutationOffset);
  }
  return pages;
}

ppc::GuestFunctionInvocation MakeSegment(uint32_t host_page_size,
                                         uint32_t initial_word,
                                         uint64_t r7_seed, bool mutated) {
  ppc::GuestFunctionInvocation invocation;
  invocation.function_address = kCodeAddress;
  invocation.function_end_address = kFunctionEndAddress;
  invocation.entry_address = kCodeAddress;
  invocation.expected_return_address = kReturnAddress;
  invocation.input.link_register = kReturnAddress;
  invocation.input.gpr[3] = kDataAddress;
  invocation.input.gpr[7] = r7_seed;
  invocation.expected_output = invocation.input;
  invocation.expected_output.gpr[5] = initial_word + 1;
  invocation.expected_output.gpr[6] = kLoopCount;
  invocation.expected_output.gpr[7] = r7_seed + 3 * kLoopCount;
  invocation.expected_output.count_register = 0;
  invocation.input_data_pages =
      MakeDataPages(host_page_size, initial_word, mutated);
  invocation.expected_dirty_pages.push_back(
      invocation.input_data_pages.front());
  StoreWord(&invocation.expected_dirty_pages.front().data, initial_word + 1);
  return invocation;
}

std::vector<uint8_t> EncodeArtifact(
    const ppc::GuestFunctionInvocation& invocation,
    const GuestExecutionSessionSha256& corpus_sha256,
    const GuestExecutionSessionSha256& replay_config_sha256) {
  ppc::GuestInvocationArtifact artifact;
  artifact.capture_build_sha256 = Identity(0x10);
  artifact.code_corpus_sha256 = corpus_sha256;
  artifact.replay_config_sha256 = replay_config_sha256;
  artifact.invocations.push_back(invocation);
  std::vector<uint8_t> bytes;
  std::string error;
  REQUIRE(ppc::GuestInvocationArtifactCodec::Encode(artifact, &bytes, &error));
  return bytes;
}

std::vector<uint8_t> EncodeState(const ppc::GuestPPCRegisterState& state) {
  std::vector<uint8_t> bytes;
  std::string error;
  REQUIRE(ppc::GuestPPCRegisterStateCodec::Encode(state, &bytes, &error));
  return bytes;
}

GuestExecutionSessionEvent ControlEvent(uint32_t owner,
                                        GuestExecutionSessionEventKind kind) {
  GuestExecutionSessionEvent event;
  event.thread_ordinal = owner;
  event.kind = kind;
  event.disposition =
      owner == kGuestExecutionSessionNoThread
          ? GuestExecutionSessionEventDisposition::kReplayCaptured
          : GuestExecutionSessionEventDisposition::kValidateDeterministic;
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

enum class ExtraEvent {
  kNone,
  kCoverageInsideSegment,
  kKernelExportInsideSegment,
  kMutationInsideSegment,
  kMutationBetweenSegments,
};

struct BundleOptions {
  uint32_t host_page_size = 16 * 1024;
  // Records the second participant's segment before the first one's.
  bool swap_order = false;
  // Begins the second segment before the first one ends.
  bool overlap_segments = false;
  ExtraEvent extra = ExtraEvent::kNone;
  bool corrupt_replay_config = false;
  bool drop_last_initial_page = false;
};

// Two participants share one data word: participant 0 increments 41 to 42 and
// participant 1 then increments 42 to 43. The recorded order is the only
// order in which both captured segment inputs can be observed.
GuestExecutionSessionBundle MakeSessionBundle(const BundleOptions& options) {
  GuestExecutionSessionBundle bundle;
  const GuestExecutionSessionSha256 replay_config_sha256 = Identity(0x20);
  const std::vector<uint8_t> corpus_bytes =
      EncodeCorpus(options.host_page_size);
  const GuestExecutionSessionSha256 corpus_sha256 =
      AddBlob(&bundle, corpus_bytes);
  const bool mutated = options.extra == ExtraEvent::kMutationBetweenSegments;
  const ppc::GuestFunctionInvocation segment_0 =
      MakeSegment(options.host_page_size, 41, 0, false);
  const ppc::GuestFunctionInvocation segment_1 =
      MakeSegment(options.host_page_size, 42, 0x1000, mutated);
  const GuestExecutionSessionSha256 artifact_config_sha256 =
      options.corrupt_replay_config ? Identity(0x21) : replay_config_sha256;
  const GuestExecutionSessionSha256 segment_0_sha256 =
      AddBlob(&bundle,
              EncodeArtifact(segment_0, corpus_sha256, artifact_config_sha256));
  const GuestExecutionSessionSha256 segment_1_sha256 =
      AddBlob(&bundle,
              EncodeArtifact(segment_1, corpus_sha256, artifact_config_sha256));
  const std::vector<uint8_t> initial_state_0 = EncodeState(segment_0.input);
  const std::vector<uint8_t> initial_state_1 = EncodeState(segment_1.input);
  const uint64_t state_size = initial_state_0.size();
  const GuestExecutionSessionSha256 initial_0 =
      AddBlob(&bundle, initial_state_0);
  const GuestExecutionSessionSha256 initial_1 =
      AddBlob(&bundle, initial_state_1);
  const GuestExecutionSessionSha256 final_0 =
      AddBlob(&bundle, EncodeState(segment_0.expected_output));
  const GuestExecutionSessionSha256 final_1 =
      AddBlob(&bundle, EncodeState(segment_1.expected_output));

  GuestExecutionSessionCheckpointChunk initial;
  initial.session_epoch = kEpoch;
  initial.ordinal = 0;
  initial.checkpoint.global_sequence = 0;
  initial.checkpoint.thread_states = {{0, state_size, initial_0},
                                      {1, state_size, initial_1}};
  const std::vector<ppc::GuestInvocationPage>& initial_pages =
      segment_0.input_data_pages;
  for (size_t i = 0; i < initial_pages.size(); ++i) {
    if (options.drop_last_initial_page && i + 1 == initial_pages.size()) {
      break;
    }
    const ppc::GuestInvocationPage& page = initial_pages[i];
    initial.checkpoint.content.push_back(
        {GuestExecutionSessionContentKind::kGuestPage, page.guest_address,
         kGuestPageSize,
         AddBlob(&bundle,
                 std::vector<uint8_t>(page.data.cbegin(), page.data.cend()))});
  }

  const uint32_t first_thread = options.swap_order ? 1 : 0;
  const uint32_t second_thread = options.swap_order ? 0 : 1;
  const GuestExecutionSessionSha256 first_segment_sha256 =
      options.swap_order ? segment_1_sha256 : segment_0_sha256;
  const GuestExecutionSessionSha256 second_segment_sha256 =
      options.swap_order ? segment_0_sha256 : segment_1_sha256;

  GuestExecutionSessionEventChunk events;
  events.session_epoch = kEpoch;
  events.ordinal = 1;
  auto push = [&](GuestExecutionSessionEvent event) {
    event.global_sequence = events.events.size() + 1;
    events.events.push_back(event);
    return event.global_sequence;
  };
  auto push_mutation = [&]() {
    GuestExecutionSessionEvent mutation =
        ControlEvent(kGuestExecutionSessionNoThread,
                     GuestExecutionSessionEventKind::kMemoryMutation);
    mutation.mutation_source = GuestExecutionSessionMutationSource::kGpu;
    mutation.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
    mutation.guest_address = kDataAddress + kMutationOffset;
    mutation.byte_count = kMutationBytes.size();
    mutation.payload_size = kMutationBytes.size();
    mutation.payload_sha256 = AddBlob(
        &bundle,
        std::vector<uint8_t>(kMutationBytes.cbegin(), kMutationBytes.cend()));
    push(mutation);
  };

  const uint64_t first_begin = push(ControlEvent(
      first_thread, GuestExecutionSessionEventKind::kSegmentBegin));
  uint64_t guest_instruction_count = 0;
  switch (options.extra) {
    case ExtraEvent::kCoverageInsideSegment: {
      GuestExecutionSessionEvent coverage = ControlEvent(
          first_thread, GuestExecutionSessionEventKind::kInstructionCoverage);
      coverage.guest_instruction_delta = 8;
      guest_instruction_count = 8;
      push(coverage);
      break;
    }
    case ExtraEvent::kKernelExportInsideSegment: {
      GuestExecutionSessionEvent external = ControlEvent(
          first_thread, GuestExecutionSessionEventKind::kKernelExport);
      external.disposition =
          GuestExecutionSessionEventDisposition::kReplayCaptured;
      external.payload_kind =
          GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
      external.payload_size = 8;
      external.payload_sha256 = AddBlob(&bundle, std::vector<uint8_t>(8, 0x5A));
      push(external);
      break;
    }
    case ExtraEvent::kMutationInsideSegment:
      push_mutation();
      break;
    default:
      break;
  }
  uint64_t second_begin = 0;
  if (options.overlap_segments) {
    second_begin = push(ControlEvent(
        second_thread, GuestExecutionSessionEventKind::kSegmentBegin));
  }
  const uint64_t first_end = push(
      ControlEvent(first_thread, GuestExecutionSessionEventKind::kSegmentEnd));
  if (options.extra == ExtraEvent::kMutationBetweenSegments) {
    push_mutation();
  }
  if (!options.overlap_segments) {
    second_begin = push(ControlEvent(
        second_thread, GuestExecutionSessionEventKind::kSegmentBegin));
  }
  const uint64_t second_end = push(
      ControlEvent(second_thread, GuestExecutionSessionEventKind::kSegmentEnd));
  const uint64_t request =
      push(ControlEvent(kGuestExecutionSessionNoThread,
                        GuestExecutionSessionEventKind::kBoundaryRequest));
  const uint64_t held =
      push(ControlEvent(kGuestExecutionSessionNoThread,
                        GuestExecutionSessionEventKind::kBoundaryHeld));

  GuestExecutionSessionCheckpointChunk final;
  final.session_epoch = kEpoch;
  final.ordinal = 2;
  final.checkpoint.global_sequence = held;
  final.checkpoint.thread_states = {{0, state_size, final_0},
                                    {1, state_size, final_1}};
  const ppc::GuestInvocationPage& final_page =
      segment_1.expected_dirty_pages.front();
  final.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestPage, final_page.guest_address,
       kGuestPageSize,
       AddBlob(&bundle, std::vector<uint8_t>(final_page.data.cbegin(),
                                             final_page.data.cend()))});

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
  manifest.last_event_sequence = held;
  manifest.capture_start_tick = 100;
  manifest.stop_request_tick = 700;
  manifest.capture_end_tick = 800;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Identity(0x10);
  manifest.replay_config_sha256 = replay_config_sha256;
  manifest.title_identity_sha256 = Identity(0x30);
  manifest.module_identity_sha256 = Identity(0x40);
  manifest.accepted_segment_count = 2;
  manifest.accepted_event_count = held;
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = request;
  manifest.stop_request_accepted_segment_count = 2;
  manifest.stop_request_guest_instruction_count = guest_instruction_count;
  manifest.maximum_stop_tail_event_count = 4;
  manifest.maximum_stop_tail_guest_instruction_count = 4;
  manifest.maximum_stop_tail_ticks = 200;

  manifest.participants.resize(2);
  for (uint32_t ordinal = 0; ordinal < 2; ++ordinal) {
    GuestExecutionSessionParticipant& participant =
        manifest.participants[ordinal];
    participant.ordinal = ordinal;
    participant.guest_thread_id = 0x100 + ordinal;
    participant.capture_instance_id = 0x1000 + ordinal;
    participant.boundary_arrival_kind =
        GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
    const bool first = ordinal == first_thread;
    participant.first_event_sequence = first ? first_begin : second_begin;
    participant.last_event_sequence = first ? first_end : second_end;
    participant.held_after_event_sequence = request;
    participant.initial_state_size = state_size;
    participant.initial_state_sha256 = ordinal ? initial_1 : initial_0;
  }
  manifest.segments.push_back({0, first_thread, first_begin, first_end,
                               kCodeAddress, kFunctionEndAddress, corpus_sha256,
                               first_segment_sha256});
  manifest.segments.push_back({1, second_thread, second_begin, second_end,
                               kCodeAddress, kFunctionEndAddress, corpus_sha256,
                               second_segment_sha256});
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                bundle.chunks[0]));
  manifest.chunks.push_back(Reference(GuestExecutionSessionChunkKind::kEvents,
                                      1, 1, held, static_cast<uint32_t>(held),
                                      bundle.chunks[1]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 2, held, held, 1,
                bundle.chunks[2]));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  return bundle;
}

// Adds a participant that declares the durable park to the segmented fixture:
// byte-stable at both checkpoints, owning no segment and no event.
void AppendParkedSegmentedParticipant(GuestExecutionSessionBundle* bundle) {
  std::string error;
  GuestExecutionSessionCheckpointChunk initial;
  GuestExecutionSessionCheckpointChunk final_chunk;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(bundle->chunks[0],
                                                            &initial, &error));
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      bundle->chunks[2], &final_chunk, &error));
  ppc::GuestPPCRegisterState state;
  state.link_register = kReturnAddress;
  state.gpr[3] = kDataAddress;
  state.gpr[9] = 0x5150;
  const std::vector<uint8_t> bytes = EncodeState(state);
  const GuestExecutionSessionSha256 digest = AddBlob(bundle, bytes);
  const uint32_t ordinal =
      static_cast<uint32_t>(bundle->manifest.participants.size());
  const GuestExecutionSessionThreadStateReference reference = {
      ordinal, bytes.size(), digest};
  initial.checkpoint.thread_states.push_back(reference);
  final_chunk.checkpoint.thread_states.push_back(reference);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle->chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_chunk, &bundle->chunks[2], &error));
  bundle->manifest.chunks[0] =
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                bundle->chunks[0]);
  bundle->manifest.chunks[2] =
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 2,
                final_chunk.checkpoint.global_sequence,
                final_chunk.checkpoint.global_sequence, 1, bundle->chunks[2]);
  GuestExecutionSessionParticipant participant;
  participant.ordinal = ordinal;
  participant.guest_thread_id = 0x100 + ordinal;
  participant.capture_instance_id = 0x1000 + ordinal;
  participant.initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kParkedBelowOuterCall;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  participant.held_after_event_sequence =
      bundle->manifest.stop_request_event_sequence;
  participant.initial_state_size = bytes.size();
  participant.initial_state_sha256 = digest;
  bundle->manifest.participants.push_back(participant);
}

ppc::GuestPPCThreadCheckpoint MakeContinuousThreadCheckpoint(
    uint32_t ordinal, uint32_t guest_thread_id, uint32_t resume_pc,
    uint32_t outer_return, uint64_t seed) {
  ppc::GuestPPCThreadCheckpoint checkpoint;
  checkpoint.participant_ordinal = ordinal;
  checkpoint.guest_thread_id = guest_thread_id;
  checkpoint.resume_pc = resume_pc;
  checkpoint.owning_function_address = kCodeAddress;
  checkpoint.owning_function_end_address = kCodeAddress + 0xFC;
  checkpoint.outer_guest_return_address = outer_return;
  checkpoint.registers.gpr[3] = kDataAddress;
  checkpoint.registers.gpr[8] = seed;
  checkpoint.registers.link_register = kCodeAddress + 0xC0;
  return checkpoint;
}

std::vector<uint8_t> EncodeContinuousThreadCheckpoint(
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  std::vector<uint8_t> bytes;
  std::string error;
  REQUIRE(
      ppc::GuestPPCThreadCheckpointCodec::Encode(checkpoint, &bytes, &error));
  REQUIRE(error.empty());
  return bytes;
}

ppc::GuestPPCThreadCheckpointBinding ContinuousBinding(
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

GuestExecutionSessionSchedulerTopologyChunk MakeSchedulerTopology(
    GuestExecutionSessionSchedulerTopologyBoundary boundary,
    uint64_t global_sequence, uint32_t ordinal,
    const std::array<uint32_t, 2>& guest_thread_ids,
    const std::array<ppc::GuestPPCThreadCheckpoint, 2>& checkpoints,
    bool passive_second = false) {
  GuestExecutionSessionSchedulerTopologyChunk topology;
  topology.session_epoch = kEpoch;
  topology.ordinal = ordinal;
  topology.boundary = boundary;
  topology.global_sequence = global_sequence;
  for (uint32_t i = 0; i < guest_thread_ids.size(); ++i) {
    GuestExecutionSessionSchedulerTopologyParticipant participant;
    participant.ordinal = i;
    participant.guest_thread_id = guest_thread_ids[i];
    participant.capture_instance_id = 0x1000 + i;
    participant.cpu = i;
    participant.effective_priority = 8 - i;
    participant.base_priority = 6;
    participant.suspension_count = 0;
    participant.quantum_remaining_us = 500 + i;
    participant.resume_kind =
        GuestExecutionSessionSchedulerResumeKind::kJitSafepoint;
    participant.guest_pc = checkpoints[i].resume_pc;
    participant.restorable = true;
    if (boundary == GuestExecutionSessionSchedulerTopologyBoundary::kStart) {
      participant.state =
          GuestExecutionSessionSchedulerParticipantState::kReady;
      participant.cpu = 0;
      participant.effective_priority = 8;
      participant.ready_queue_level = 8;
      participant.ready_queue_fifo_ordinal = i;
    } else {
      participant.state =
          i == 0 ? GuestExecutionSessionSchedulerParticipantState::kRunning
                 : GuestExecutionSessionSchedulerParticipantState::kSuspended;
      participant.suspension_count = i == 0 ? 0 : 1;
    }
    if (passive_second) {
      participant.state =
          GuestExecutionSessionSchedulerParticipantState::kReady;
      participant.cpu = 0;
      participant.effective_priority = 8;
      participant.suspension_count = 0;
      participant.ready_queue_level = 8;
      participant.ready_queue_fifo_ordinal = i;
      if (i == 1) {
        participant.resume_kind =
            GuestExecutionSessionSchedulerResumeKind::kNativeContinuation;
        participant.guest_pc = 0;
        participant.restorable = false;
      }
    }
    topology.participants.push_back(participant);
  }
  return topology;
}

GuestExecutionSessionBundle MakeContinuousSessionBundle(
    uint32_t host_page_size, bool passive_second = false,
    uint32_t data_address = kDataAddress) {
  GuestExecutionSessionBundle bundle;
  constexpr uint32_t kParticipantCount = 2;
  const std::array<uint32_t, kParticipantCount> guest_thread_ids = {0x101,
                                                                    0x202};
  std::array<ppc::GuestPPCThreadCheckpoint, kParticipantCount> initial = {
      MakeContinuousThreadCheckpoint(0, guest_thread_ids[0],
                                     kCodeAddress + 0x40, kReturnAddress, 1),
      MakeContinuousThreadCheckpoint(
          1, guest_thread_ids[1], kCodeAddress + 0x80, kReturnAddress + 4, 2)};
  std::array<ppc::GuestPPCThreadCheckpoint, kParticipantCount> final = {
      MakeContinuousThreadCheckpoint(0, guest_thread_ids[0],
                                     kCodeAddress + 0x44, kReturnAddress, 11),
      MakeContinuousThreadCheckpoint(
          1, guest_thread_ids[1], kCodeAddress + 0x84, kReturnAddress + 4, 12)};
  if (passive_second) {
    ppc::GuestPPCThreadCheckpoint passive;
    passive.participant_ordinal = 1;
    passive.guest_thread_id = guest_thread_ids[1];
    passive.resume_kind = ppc::GuestPPCThreadResumeKind::kOutsideGuest;
    passive.registers.gpr[8] = 2;
    initial[1] = passive;
    final[1] = passive;
  }

  std::array<GuestExecutionSessionSha256, kParticipantCount> initial_digests;
  std::array<GuestExecutionSessionSha256, kParticipantCount> final_digests;
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    initial_digests[i] =
        AddBlob(&bundle, EncodeContinuousThreadCheckpoint(initial[i]));
    final_digests[i] =
        AddBlob(&bundle, EncodeContinuousThreadCheckpoint(final[i]));
  }

  ExecutionJitCorpusBuilder corpus_builder(JitCorpus::kConfigGuestScheduler);
  GuestExecutionSessionCheckpointChunk initial_chunk;
  initial_chunk.session_epoch = kEpoch;
  initial_chunk.ordinal = 0;
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    initial_chunk.checkpoint.thread_states.push_back(
        {i, ppc::GuestPPCThreadCheckpointCodec::kEncodedSize,
         initial_digests[i]});
  }
  const uint32_t page_count = host_page_size / kGuestPageSize;
  std::string error;
  for (uint32_t i = 0; i < page_count; ++i) {
    std::vector<uint8_t> code(kGuestPageSize, static_cast<uint8_t>(0x30 + i));
    REQUIRE(corpus_builder.AddCodePage(kCodeAddress + i * kGuestPageSize,
                                       code.data(), code.size(), &error));
    initial_chunk.checkpoint.content.push_back(
        {GuestExecutionSessionContentKind::kGuestCode,
         kCodeAddress + i * kGuestPageSize, kGuestPageSize,
         AddBlob(&bundle, std::move(code))});
  }
  REQUIRE(corpus_builder.AddFunction({kCodeAddress, kCodeAddress + 0xFC, 64, 0},
                                     &error));
  std::vector<uint8_t> corpus_bytes;
  REQUIRE(corpus_builder.Encode(&corpus_bytes, &error));
  const GuestExecutionSessionSha256 corpus_digest =
      AddBlob(&bundle, std::move(corpus_bytes));

  std::vector<GuestExecutionSessionSha256> initial_page_digests;
  std::vector<GuestExecutionSessionSha256> final_page_digests;
  for (uint32_t i = 0; i < page_count; ++i) {
    std::vector<uint8_t> initial_page(kGuestPageSize,
                                      static_cast<uint8_t>(0x60 + i));
    std::vector<uint8_t> final_page = initial_page;
    if (!i) {
      final_page[0] ^= 0xFF;
    }
    initial_page_digests.push_back(AddBlob(&bundle, std::move(initial_page)));
    final_page_digests.push_back(AddBlob(&bundle, std::move(final_page)));
    initial_chunk.checkpoint.content.push_back(
        {GuestExecutionSessionContentKind::kGuestPage,
         data_address + i * kGuestPageSize, kGuestPageSize,
         initial_page_digests.back()});
  }
  std::sort(initial_chunk.checkpoint.content.begin(),
            initial_chunk.checkpoint.content.end(),
            [](const GuestExecutionSessionContentReference& left,
               const GuestExecutionSessionContentReference& right) {
              return std::tie(left.kind, left.guest_address) <
                     std::tie(right.kind, right.guest_address);
            });

  GuestExecutionSessionCodeCorpusChunk corpus_chunk;
  corpus_chunk.session_epoch = kEpoch;
  corpus_chunk.ordinal = 1;
  corpus_chunk.code_corpus_sha256 = corpus_digest;

  GuestExecutionSessionEventChunk event_chunk;
  event_chunk.session_epoch = kEpoch;
  event_chunk.ordinal = 2;
  auto push = [&](uint32_t owner, GuestExecutionSessionEventKind kind) {
    GuestExecutionSessionEvent event = ControlEvent(owner, kind);
    event.global_sequence = event_chunk.events.size() + 1;
    event_chunk.events.push_back(event);
  };
  push(0, GuestExecutionSessionEventKind::kOuterHostCallBegin);
  if (!passive_second) {
    push(1, GuestExecutionSessionEventKind::kOuterHostCallBegin);
  }
  push(0, GuestExecutionSessionEventKind::kInstructionCoverage);
  event_chunk.events.back().guest_instruction_delta = 10;
  if (!passive_second) {
    push(1, GuestExecutionSessionEventKind::kInstructionCoverage);
    event_chunk.events.back().guest_instruction_delta = 20;
  }
  push(kGuestExecutionSessionNoThread,
       GuestExecutionSessionEventKind::kBoundaryRequest);
  push(0, GuestExecutionSessionEventKind::kJitSafepointArrival);
  if (!passive_second) {
    push(1, GuestExecutionSessionEventKind::kJitSafepointArrival);
  }
  push(kGuestExecutionSessionNoThread,
       GuestExecutionSessionEventKind::kBoundaryHeld);

  GuestExecutionSessionCheckpointChunk final_chunk;
  final_chunk.session_epoch = kEpoch;
  final_chunk.ordinal = 6;
  final_chunk.checkpoint.global_sequence = event_chunk.events.size();
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    final_chunk.checkpoint.thread_states.push_back(
        {i, ppc::GuestPPCThreadCheckpointCodec::kEncodedSize,
         final_digests[i]});
  }
  for (const GuestExecutionSessionContentReference& content :
       initial_chunk.checkpoint.content) {
    GuestExecutionSessionContentReference final_content = content;
    if (content.kind == GuestExecutionSessionContentKind::kGuestPage) {
      const uint32_t page_index = static_cast<uint32_t>(
          (content.guest_address - data_address) / kGuestPageSize);
      final_content.sha256 = final_page_digests[page_index];
    }
    final_chunk.checkpoint.content.push_back(final_content);
  }

  std::vector<GuestExecutionContinuousEvent> control_events;
  for (const GuestExecutionSessionEvent& event : event_chunk.events) {
    GuestExecutionContinuousEvent control;
    control.global_sequence = event.global_sequence;
    control.kind = event.kind;
    if (event.thread_ordinal != kGuestExecutionSessionNoThread) {
      control.actor = {event.thread_ordinal,
                       guest_thread_ids[event.thread_ordinal]};
    }
    control_events.push_back(control);
  }
  const uint32_t arrival_count = passive_second ? 1 : kParticipantCount;
  for (uint32_t i = 0; i < arrival_count; ++i) {
    GuestExecutionContinuousEvent& arrival =
        control_events[control_events.size() - 1 - arrival_count + i];
    arrival.subject = {i, guest_thread_ids[i]};
    arrival.checkpoint.kind =
        GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
    arrival.checkpoint.checkpoint_global_sequence =
        final_chunk.checkpoint.global_sequence;
    arrival.checkpoint.state_size =
        ppc::GuestPPCThreadCheckpointCodec::kEncodedSize;
    arrival.checkpoint.state_sha256 = final_digests[i];
    arrival.checkpoint.binding = ContinuousBinding(final[i]);
  }

  GuestExecutionSessionSchedulerTopologyChunk start_topology =
      MakeSchedulerTopology(
          GuestExecutionSessionSchedulerTopologyBoundary::kStart, 0, 4,
          guest_thread_ids, initial, passive_second);
  GuestExecutionSessionSchedulerTopologyChunk final_topology =
      MakeSchedulerTopology(
          GuestExecutionSessionSchedulerTopologyBoundary::kFinal,
          final_chunk.checkpoint.global_sequence, 5, guest_thread_ids, final,
          passive_second);

  bundle.chunks.resize(7);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial_chunk, &bundle.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCodeCorpusChunk(
      corpus_chunk, &bundle.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      event_chunk, &bundle.chunks[2], &error));
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(
      control_events, &bundle.chunks[3], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      start_topology, &bundle.chunks[4], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      final_topology, &bundle.chunks[5], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_chunk, &bundle.chunks[6], &error));

  GuestExecutionSessionManifest& manifest = bundle.manifest;
  manifest.session_epoch = kEpoch;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = event_chunk.events.size();
  manifest.capture_start_tick = 100;
  manifest.stop_request_tick = 500;
  manifest.capture_end_tick = 800;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Identity(0x10);
  manifest.replay_config_sha256 = Identity(0x20);
  manifest.title_identity_sha256 = Identity(0x30);
  manifest.module_identity_sha256 = Identity(0x40);
  manifest.accepted_event_count = event_chunk.events.size();
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = passive_second ? 3 : 5;
  manifest.stop_request_tick = 500;
  manifest.stop_request_guest_instruction_count = passive_second ? 10 : 30;
  manifest.maximum_stop_tail_event_count = 4;
  manifest.maximum_stop_tail_guest_instruction_count = 1;
  manifest.maximum_stop_tail_ticks = 400;
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    GuestExecutionSessionParticipant participant;
    participant.ordinal = i;
    participant.guest_thread_id = guest_thread_ids[i];
    participant.capture_instance_id = 0x1000 + i;
    if (passive_second && i == 1) {
      participant.boundary_arrival_kind =
          GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
      participant.held_after_event_sequence =
          manifest.stop_request_event_sequence;
    } else {
      participant.boundary_arrival_kind =
          GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint;
      participant.first_event_sequence = 1 + i;
      participant.last_event_sequence = passive_second ? 4 : 6 + i;
      participant.held_after_event_sequence = participant.last_event_sequence;
    }
    participant.initial_state_size =
        ppc::GuestPPCThreadCheckpointCodec::kEncodedSize;
    participant.initial_state_sha256 = initial_digests[i];
    manifest.participants.push_back(participant);
  }
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                bundle.chunks[0]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCodeCorpus, 1, 0, 0, 1,
                bundle.chunks[1]));
  manifest.chunks.push_back(Reference(
      GuestExecutionSessionChunkKind::kEvents, 2, 1, event_chunk.events.size(),
      static_cast<uint32_t>(event_chunk.events.size()), bundle.chunks[2]));
  manifest.chunks.push_back(Reference(
      GuestExecutionSessionChunkKind::kContinuousEvents, 3, 1,
      control_events.size(), static_cast<uint32_t>(control_events.size()),
      bundle.chunks[3]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kSchedulerTopology, 4, 0, 0,
                kParticipantCount, bundle.chunks[4]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kSchedulerTopology, 5,
                final_chunk.checkpoint.global_sequence,
                final_chunk.checkpoint.global_sequence, kParticipantCount,
                bundle.chunks[5]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 6,
                final_chunk.checkpoint.global_sequence,
                final_chunk.checkpoint.global_sequence, 1, bundle.chunks[6]));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  REQUIRE(error.empty());
  return bundle;
}

std::vector<GuestExecutionContinuousEvent> DecodeContinuousControlEvents(
    const GuestExecutionSessionBundle& bundle) {
  std::vector<GuestExecutionContinuousEvent> events;
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::Decode(bundle.chunks[3], &events,
                                                     &error));
  REQUIRE(error.empty());
  return events;
}

void ReplaceContinuousControlEvents(
    GuestExecutionSessionBundle* bundle,
    const std::vector<GuestExecutionContinuousEvent>& events) {
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(events, &bundle->chunks[3],
                                                     &error));
  bundle->manifest.chunks[3] =
      Reference(GuestExecutionSessionChunkKind::kContinuousEvents, 3,
                events.front().global_sequence, events.back().global_sequence,
                static_cast<uint32_t>(events.size()), bundle->chunks[3]);
  REQUIRE(ValidateGuestExecutionSessionBundle(*bundle, &error));
  REQUIRE(error.empty());
}

void ReplaceInitialContinuousCheckpoint(
    GuestExecutionSessionBundle* bundle, uint32_t ordinal,
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  GuestExecutionSessionCheckpointChunk initial;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      bundle->chunks.front(), &initial, &error));
  GuestExecutionSessionThreadStateReference& state =
      initial.checkpoint.thread_states[ordinal];
  const GuestExecutionSessionSha256 old_digest = state.sha256;
  std::vector<uint8_t> bytes = EncodeContinuousThreadCheckpoint(checkpoint);
  const GuestExecutionSessionSha256 new_digest =
      GuestExecutionSessionCodec::HashBytes(bytes);
  auto blob =
      std::find_if(bundle->content_blobs.begin(), bundle->content_blobs.end(),
                   [&](const GuestExecutionSessionContentBlob& candidate) {
                     return candidate.sha256 == old_digest;
                   });
  REQUIRE(blob != bundle->content_blobs.end());
  blob->bytes = std::move(bytes);
  blob->sha256 = new_digest;
  state.sha256 = new_digest;
  bundle->manifest.participants[ordinal].initial_state_sha256 = new_digest;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle->chunks.front(), &error));
  bundle->manifest.chunks.front() =
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                bundle->chunks.front());
  GuestExecutionSessionSchedulerTopologyChunk topology;
  REQUIRE(GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
      bundle->chunks[4], &topology, &error));
  topology.participants[ordinal].guest_pc = checkpoint.resume_pc;
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      topology, &bundle->chunks[4], &error));
  bundle->manifest.chunks[4] = Reference(
      GuestExecutionSessionChunkKind::kSchedulerTopology, 4, 0, 0,
      static_cast<uint32_t>(topology.participants.size()), bundle->chunks[4]);
  REQUIRE(ValidateGuestExecutionSessionBundle(*bundle, &error));
  REQUIRE(error.empty());
}

void MutateFinalSchedulerTopology(
    GuestExecutionSessionBundle* bundle,
    const std::function<void(GuestExecutionSessionSchedulerTopologyChunk*)>&
        mutate) {
  GuestExecutionSessionSchedulerTopologyChunk topology;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
      bundle->chunks[5], &topology, &error));
  mutate(&topology);
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      topology, &bundle->chunks[5], &error));
  bundle->manifest.chunks[5] = Reference(
      GuestExecutionSessionChunkKind::kSchedulerTopology, 5,
      topology.global_sequence, topology.global_sequence,
      static_cast<uint32_t>(topology.participants.size()), bundle->chunks[5]);
}

void MutateStartSchedulerTopology(
    GuestExecutionSessionBundle* bundle,
    const std::function<void(GuestExecutionSessionSchedulerTopologyChunk*)>&
        mutate) {
  GuestExecutionSessionSchedulerTopologyChunk topology;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
      bundle->chunks[4], &topology, &error));
  mutate(&topology);
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      topology, &bundle->chunks[4], &error));
  bundle->manifest.chunks[4] = Reference(
      GuestExecutionSessionChunkKind::kSchedulerTopology, 4,
      topology.global_sequence, topology.global_sequence,
      static_cast<uint32_t>(topology.participants.size()), bundle->chunks[4]);
}

constexpr uint32_t kSchedulerRecordDispatch = 3;
constexpr uint32_t kSchedulerRecordResume = 11;

std::vector<uint8_t> EncodeSchedulerEventPayload(uint32_t record_kind,
                                                 uint64_t capture_instance_id,
                                                 uint32_t guest_thread_id,
                                                 uint32_t version) {
  std::vector<uint8_t> payload(
      GuestExecutionSessionCodec::kSchedulerEventPayloadSize, 0);
  constexpr std::array<uint8_t, 8> kMagic = {'X', 'E', 'G', 'S',
                                             'C', 'E', '1', 0};
  std::copy(kMagic.cbegin(), kMagic.cend(), payload.begin());
  const auto write_u32 = [&payload](size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
      payload[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
  };
  write_u32(8, version);
  write_u32(12, record_kind);
  write_u32(16, 1);
  write_u32(24, static_cast<uint32_t>(capture_instance_id));
  write_u32(28, static_cast<uint32_t>(capture_instance_id >> 32));
  write_u32(32, guest_thread_id);
  return payload;
}

GuestExecutionSessionBundle MakePassiveContinuousSessionBundle(
    uint32_t host_page_size, uint32_t scheduler_record_kind,
    uint32_t scheduler_subject_ordinal,
    uint32_t scheduler_payload_version =
        GuestExecutionSessionCodec::kSchedulerEventPayloadVersion) {
  GuestExecutionSessionBundle bundle;
  constexpr uint32_t kParticipantCount = 2;
  const std::array<uint32_t, kParticipantCount> guest_thread_ids = {0x101,
                                                                    0x202};
  ppc::GuestPPCThreadCheckpoint passive;
  passive.participant_ordinal = 1;
  passive.guest_thread_id = guest_thread_ids[1];
  passive.resume_kind = ppc::GuestPPCThreadResumeKind::kOutsideGuest;
  passive.resume_pc = 0;
  passive.registers.gpr[8] = 2;
  const std::array<ppc::GuestPPCThreadCheckpoint, kParticipantCount> initial = {
      MakeContinuousThreadCheckpoint(0, guest_thread_ids[0],
                                     kCodeAddress + 0x40, kReturnAddress, 1),
      passive};
  const std::array<ppc::GuestPPCThreadCheckpoint, kParticipantCount> final = {
      MakeContinuousThreadCheckpoint(0, guest_thread_ids[0],
                                     kCodeAddress + 0x44, kReturnAddress, 11),
      passive};

  std::array<GuestExecutionSessionSha256, kParticipantCount> initial_digests;
  std::array<GuestExecutionSessionSha256, kParticipantCount> final_digests;
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    initial_digests[i] =
        AddBlob(&bundle, EncodeContinuousThreadCheckpoint(initial[i]));
    final_digests[i] =
        AddBlob(&bundle, EncodeContinuousThreadCheckpoint(final[i]));
  }

  ExecutionJitCorpusBuilder corpus_builder(JitCorpus::kConfigGuestScheduler);
  GuestExecutionSessionCheckpointChunk initial_chunk;
  initial_chunk.session_epoch = kEpoch;
  initial_chunk.ordinal = 0;
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    initial_chunk.checkpoint.thread_states.push_back(
        {i, ppc::GuestPPCThreadCheckpointCodec::kEncodedSize,
         initial_digests[i]});
  }
  const uint32_t page_count = host_page_size / kGuestPageSize;
  std::string error;
  for (uint32_t i = 0; i < page_count; ++i) {
    std::vector<uint8_t> code(kGuestPageSize, static_cast<uint8_t>(0x30 + i));
    REQUIRE(corpus_builder.AddCodePage(kCodeAddress + i * kGuestPageSize,
                                       code.data(), code.size(), &error));
    initial_chunk.checkpoint.content.push_back(
        {GuestExecutionSessionContentKind::kGuestCode,
         kCodeAddress + i * kGuestPageSize, kGuestPageSize,
         AddBlob(&bundle, std::move(code))});
  }
  REQUIRE(corpus_builder.AddFunction({kCodeAddress, kCodeAddress + 0xFC, 64, 0},
                                     &error));
  std::vector<uint8_t> corpus_bytes;
  REQUIRE(corpus_builder.Encode(&corpus_bytes, &error));
  const GuestExecutionSessionSha256 corpus_digest =
      AddBlob(&bundle, std::move(corpus_bytes));

  std::vector<GuestExecutionSessionSha256> initial_page_digests;
  std::vector<GuestExecutionSessionSha256> final_page_digests;
  for (uint32_t i = 0; i < page_count; ++i) {
    std::vector<uint8_t> initial_page(kGuestPageSize,
                                      static_cast<uint8_t>(0x60 + i));
    std::vector<uint8_t> final_page = initial_page;
    if (!i) {
      final_page[0] ^= 0xFF;
    }
    initial_page_digests.push_back(AddBlob(&bundle, std::move(initial_page)));
    final_page_digests.push_back(AddBlob(&bundle, std::move(final_page)));
    initial_chunk.checkpoint.content.push_back(
        {GuestExecutionSessionContentKind::kGuestPage,
         kDataAddress + i * kGuestPageSize, kGuestPageSize,
         initial_page_digests.back()});
  }
  std::sort(initial_chunk.checkpoint.content.begin(),
            initial_chunk.checkpoint.content.end(),
            [](const GuestExecutionSessionContentReference& left,
               const GuestExecutionSessionContentReference& right) {
              return std::tie(left.kind, left.guest_address) <
                     std::tie(right.kind, right.guest_address);
            });

  GuestExecutionSessionCodeCorpusChunk corpus_chunk;
  corpus_chunk.session_epoch = kEpoch;
  corpus_chunk.ordinal = 1;
  corpus_chunk.code_corpus_sha256 = corpus_digest;

  GuestExecutionSessionEventChunk event_chunk;
  event_chunk.session_epoch = kEpoch;
  event_chunk.ordinal = 2;
  auto push = [&](uint32_t owner, GuestExecutionSessionEventKind kind) {
    GuestExecutionSessionEvent event = ControlEvent(owner, kind);
    event.global_sequence = event_chunk.events.size() + 1;
    event_chunk.events.push_back(event);
  };
  push(0, GuestExecutionSessionEventKind::kOuterHostCallBegin);
  push(0, GuestExecutionSessionEventKind::kInstructionCoverage);
  event_chunk.events.back().guest_instruction_delta = 10;
  const bool synchronization_record =
      scheduler_record_kind >= 7 && scheduler_record_kind <= 11;
  push(kGuestExecutionSessionNoThread,
       synchronization_record
           ? GuestExecutionSessionEventKind::kSynchronization
           : GuestExecutionSessionEventKind::kThreadDispatch);
  {
    GuestExecutionSessionEvent& scheduler_event = event_chunk.events.back();
    std::vector<uint8_t> payload = EncodeSchedulerEventPayload(
        scheduler_record_kind, 0x1000 + scheduler_subject_ordinal,
        guest_thread_ids[scheduler_subject_ordinal], scheduler_payload_version);
    scheduler_event.payload_kind =
        GuestExecutionSessionPayloadKind::kGuestBytes;
    scheduler_event.payload_size = payload.size();
    scheduler_event.payload_sha256 = AddBlob(&bundle, std::move(payload));
  }
  push(kGuestExecutionSessionNoThread,
       GuestExecutionSessionEventKind::kBoundaryRequest);
  push(0, GuestExecutionSessionEventKind::kJitSafepointArrival);
  push(kGuestExecutionSessionNoThread,
       GuestExecutionSessionEventKind::kBoundaryHeld);

  GuestExecutionSessionCheckpointChunk final_chunk;
  final_chunk.session_epoch = kEpoch;
  final_chunk.ordinal = 6;
  final_chunk.checkpoint.global_sequence = event_chunk.events.size();
  for (uint32_t i = 0; i < kParticipantCount; ++i) {
    final_chunk.checkpoint.thread_states.push_back(
        {i, ppc::GuestPPCThreadCheckpointCodec::kEncodedSize,
         final_digests[i]});
  }
  for (const GuestExecutionSessionContentReference& content :
       initial_chunk.checkpoint.content) {
    GuestExecutionSessionContentReference final_content = content;
    if (content.kind == GuestExecutionSessionContentKind::kGuestPage) {
      const uint32_t page_index = static_cast<uint32_t>(
          (content.guest_address - kDataAddress) / kGuestPageSize);
      final_content.sha256 = final_page_digests[page_index];
    }
    final_chunk.checkpoint.content.push_back(final_content);
  }

  std::vector<GuestExecutionContinuousEvent> control_events;
  for (const GuestExecutionSessionEvent& event : event_chunk.events) {
    GuestExecutionContinuousEvent control;
    control.global_sequence = event.global_sequence;
    control.kind = event.kind;
    if (event.thread_ordinal != kGuestExecutionSessionNoThread) {
      control.actor = {event.thread_ordinal,
                       guest_thread_ids[event.thread_ordinal]};
    }
    control_events.push_back(control);
  }
  control_events[2].subject = {scheduler_subject_ordinal,
                               guest_thread_ids[scheduler_subject_ordinal]};
  GuestExecutionContinuousEvent& arrival = control_events[4];
  arrival.subject = {0, guest_thread_ids[0]};
  arrival.checkpoint.kind =
      GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
  arrival.checkpoint.checkpoint_global_sequence =
      final_chunk.checkpoint.global_sequence;
  arrival.checkpoint.state_size =
      ppc::GuestPPCThreadCheckpointCodec::kEncodedSize;
  arrival.checkpoint.state_sha256 = final_digests[0];
  arrival.checkpoint.binding = ContinuousBinding(final[0]);

  GuestExecutionSessionSchedulerTopologyChunk start_topology =
      MakeSchedulerTopology(
          GuestExecutionSessionSchedulerTopologyBoundary::kStart, 0, 4,
          guest_thread_ids, initial);
  GuestExecutionSessionSchedulerTopologyChunk final_topology =
      MakeSchedulerTopology(
          GuestExecutionSessionSchedulerTopologyBoundary::kFinal,
          final_chunk.checkpoint.global_sequence, 5, guest_thread_ids, final);
  for (auto* topology : {&start_topology, &final_topology}) {
    GuestExecutionSessionSchedulerTopologyParticipant& participant =
        topology->participants[1];
    // An untouched passive row is invariant across both boundaries.
    participant.state = GuestExecutionSessionSchedulerParticipantState::kReady;
    participant.cpu = 0;
    participant.effective_priority = 8;
    participant.suspension_count = 0;
    participant.ready_queue_level = 8;
    participant.ready_queue_fifo_ordinal = 0;
    participant.resume_kind =
        GuestExecutionSessionSchedulerResumeKind::kNotYetRun;
    participant.restorable = false;
    participant.guest_pc = 0;
  }
  // The executable row runs at the final boundary and leaves the queue, so the
  // passive row holds ordinal 0 and the start queue stays dense behind it.
  start_topology.participants[0].ready_queue_fifo_ordinal = 1;

  bundle.chunks.resize(7);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial_chunk, &bundle.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCodeCorpusChunk(
      corpus_chunk, &bundle.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      event_chunk, &bundle.chunks[2], &error));
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(
      control_events, &bundle.chunks[3], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      start_topology, &bundle.chunks[4], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
      final_topology, &bundle.chunks[5], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_chunk, &bundle.chunks[6], &error));

  GuestExecutionSessionManifest& manifest = bundle.manifest;
  manifest.session_epoch = kEpoch;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = event_chunk.events.size();
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 800;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = Identity(0x10);
  manifest.replay_config_sha256 = Identity(0x20);
  manifest.title_identity_sha256 = Identity(0x30);
  manifest.module_identity_sha256 = Identity(0x40);
  manifest.accepted_event_count = event_chunk.events.size();
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = 4;
  manifest.stop_request_tick = 500;
  manifest.stop_request_guest_instruction_count = 10;
  manifest.maximum_stop_tail_event_count = 4;
  manifest.maximum_stop_tail_guest_instruction_count = 1;
  manifest.maximum_stop_tail_ticks = 400;
  GuestExecutionSessionParticipant executable;
  executable.ordinal = 0;
  executable.guest_thread_id = guest_thread_ids[0];
  executable.capture_instance_id = 0x1000;
  executable.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint;
  executable.first_event_sequence = 1;
  executable.last_event_sequence = 5;
  executable.held_after_event_sequence = 5;
  executable.initial_state_size =
      ppc::GuestPPCThreadCheckpointCodec::kEncodedSize;
  executable.initial_state_sha256 = initial_digests[0];
  manifest.participants.push_back(executable);
  GuestExecutionSessionParticipant passive_participant;
  passive_participant.ordinal = 1;
  passive_participant.guest_thread_id = guest_thread_ids[1];
  passive_participant.capture_instance_id = 0x1001;
  passive_participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  passive_participant.held_after_event_sequence =
      manifest.stop_request_event_sequence;
  passive_participant.initial_state_size =
      ppc::GuestPPCThreadCheckpointCodec::kEncodedSize;
  passive_participant.initial_state_sha256 = initial_digests[1];
  manifest.participants.push_back(passive_participant);
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                bundle.chunks[0]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCodeCorpus, 1, 0, 0, 1,
                bundle.chunks[1]));
  manifest.chunks.push_back(Reference(
      GuestExecutionSessionChunkKind::kEvents, 2, 1, event_chunk.events.size(),
      static_cast<uint32_t>(event_chunk.events.size()), bundle.chunks[2]));
  manifest.chunks.push_back(Reference(
      GuestExecutionSessionChunkKind::kContinuousEvents, 3, 1,
      control_events.size(), static_cast<uint32_t>(control_events.size()),
      bundle.chunks[3]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kSchedulerTopology, 4, 0, 0,
                kParticipantCount, bundle.chunks[4]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kSchedulerTopology, 5,
                final_chunk.checkpoint.global_sequence,
                final_chunk.checkpoint.global_sequence, kParticipantCount,
                bundle.chunks[5]));
  manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, 6,
                final_chunk.checkpoint.global_sequence,
                final_chunk.checkpoint.global_sequence, 1, bundle.chunks[6]));
  return bundle;
}

// Splices a witness table in front of the final checkpoint, which is where the
// format places it.
void AttachSignalWitnesses(
    GuestExecutionSessionBundle* bundle,
    std::vector<GuestExecutionSessionSignalWitness> witnesses) {
  GuestExecutionSessionCheckpointChunk final_checkpoint;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      bundle->chunks.back(), &final_checkpoint, &error));
  GuestExecutionSessionSignalWitnessChunk chunk;
  chunk.session_epoch = bundle->manifest.session_epoch;
  chunk.ordinal = final_checkpoint.ordinal;
  chunk.witnesses = std::move(witnesses);
  final_checkpoint.ordinal += 1;
  std::vector<uint8_t> witness_bytes;
  std::vector<uint8_t> final_bytes;
  REQUIRE(GuestExecutionSessionCodec::EncodeSignalWitnessChunk(
      chunk, &witness_bytes, &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_checkpoint, &final_bytes, &error));
  const GuestExecutionSessionChunkReference previous_final =
      bundle->manifest.chunks.back();
  bundle->manifest.chunks.back() =
      Reference(GuestExecutionSessionChunkKind::kSignalWitness, chunk.ordinal,
                0, 0, 1, witness_bytes);
  bundle->manifest.chunks.push_back(
      Reference(GuestExecutionSessionChunkKind::kCheckpoint,
                final_checkpoint.ordinal, previous_final.first_event_sequence,
                previous_final.last_event_sequence, 1, final_bytes));
  bundle->chunks.back() = std::move(witness_bytes);
  bundle->chunks.push_back(std::move(final_bytes));
}

enum class ContinuousParkKind {
  kReadyParity,
  kBlockedParity,
};

// Turns the passive fixture's second participant into a fiber the barrier
// caught below the root dispatch it never arrived at: held on a ready queue,
// or blocked in a wait no modeled dispatch owns.
GuestExecutionSessionBundle MakeParkedContinuousSessionBundle(
    uint32_t host_page_size, ContinuousParkKind park) {
  GuestExecutionSessionBundle bundle = MakePassiveContinuousSessionBundle(
      host_page_size, kSchedulerRecordDispatch, 0);
  bundle.manifest.participants[1].initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kParkedBelowOuterCall;
  const auto park_row =
      [park](GuestExecutionSessionSchedulerTopologyChunk* topology) {
        GuestExecutionSessionSchedulerTopologyParticipant& row =
            topology->participants[1];
        row.restorable = false;
        row.guest_pc = 0;
        if (park == ContinuousParkKind::kReadyParity) {
          row.state = GuestExecutionSessionSchedulerParticipantState::kReady;
          row.resume_kind =
              GuestExecutionSessionSchedulerResumeKind::kNativeContinuation;
          return;
        }
        row.state = GuestExecutionSessionSchedulerParticipantState::kBlocked;
        row.ready_queue_level = kGuestExecutionSessionSchedulerNoValue;
        row.ready_queue_fifo_ordinal = kGuestExecutionSessionSchedulerNoValue;
        row.resume_kind =
            GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport;
        // The return address the wait comes back to, which the emulator reads
        // from the link register rather than from a durable route.
        row.guest_pc = kReturnAddress + 8;
        row.blocked_wait.kind = GuestExecutionSessionSchedulerWaitKind::kSingle;
        row.blocked_wait.wait_epoch = 3;
        row.blocked_wait.observed_wait_epoch = 3;
        row.blocked_wait.handle_count = 1;
        row.blocked_wait.flags =
            kGuestExecutionSessionSchedulerWaitFlagGated |
            kGuestExecutionSessionSchedulerWaitFlagInterruptible;
        row.blocked_wait.handles[0] = 0x00110001u;
        row.blocked_wait.signal_epochs_before[0] = 3;
        row.blocked_wait.signal_epochs_observed[0] = 3;
        // The queue the park leaves has to stay dense behind it.
        GuestExecutionSessionSchedulerTopologyParticipant& mate =
            topology->participants[0];
        if (mate.state ==
            GuestExecutionSessionSchedulerParticipantState::kReady) {
          mate.ready_queue_fifo_ordinal = 0;
        }
      };
  MutateStartSchedulerTopology(&bundle, park_row);
  MutateFinalSchedulerTopology(&bundle, park_row);
  return bundle;
}

// Rewrites the state both boundaries of one byte-stable participant share.
void ReplaceParkedBoundaryCheckpoints(
    GuestExecutionSessionBundle* bundle, uint32_t ordinal,
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  std::string error;
  GuestExecutionSessionCheckpointChunk initial;
  GuestExecutionSessionCheckpointChunk final_chunk;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      bundle->chunks.front(), &initial, &error));
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      bundle->chunks.back(), &final_chunk, &error));
  const GuestExecutionSessionSha256 old_digest =
      initial.checkpoint.thread_states[ordinal].sha256;
  const bool boundaries_share_state =
      final_chunk.checkpoint.thread_states[ordinal].sha256 == old_digest;
  REQUIRE(boundaries_share_state);
  std::vector<uint8_t> bytes = EncodeContinuousThreadCheckpoint(checkpoint);
  const GuestExecutionSessionSha256 digest =
      GuestExecutionSessionCodec::HashBytes(bytes);
  auto blob =
      std::find_if(bundle->content_blobs.begin(), bundle->content_blobs.end(),
                   [&old_digest](const GuestExecutionSessionContentBlob& c) {
                     return c.sha256 == old_digest;
                   });
  REQUIRE(blob != bundle->content_blobs.end());
  blob->bytes = bytes;
  blob->sha256 = digest;
  for (GuestExecutionSessionCheckpointChunk* chunk : {&initial, &final_chunk}) {
    chunk->checkpoint.thread_states[ordinal].byte_size = bytes.size();
    chunk->checkpoint.thread_states[ordinal].sha256 = digest;
  }
  bundle->manifest.participants[ordinal].initial_state_size = bytes.size();
  bundle->manifest.participants[ordinal].initial_state_sha256 = digest;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle->chunks.front(), &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_chunk, &bundle->chunks.back(), &error));
  bundle->manifest.chunks.front() =
      Reference(GuestExecutionSessionChunkKind::kCheckpoint, initial.ordinal, 0,
                0, 1, bundle->chunks.front());
  bundle->manifest.chunks.back() = Reference(
      GuestExecutionSessionChunkKind::kCheckpoint, final_chunk.ordinal,
      final_chunk.checkpoint.global_sequence,
      final_chunk.checkpoint.global_sequence, 1, bundle->chunks.back());
}

// Moves the record at |index| onto |ordinal| in the canonical tape and in the
// control overlay, and gives that participant the range it now owns.
void RetargetContinuousEvent(GuestExecutionSessionBundle* bundle, size_t index,
                             uint32_t ordinal, uint32_t guest_thread_id) {
  std::string error;
  GuestExecutionSessionEventChunk events;
  REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(bundle->chunks[2],
                                                       &events, &error));
  const uint64_t sequence = events.events[index].global_sequence;
  events.events[index].thread_ordinal = ordinal;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      events, &bundle->chunks[2], &error));
  bundle->manifest.chunks[2] =
      Reference(GuestExecutionSessionChunkKind::kEvents, 2,
                events.events.front().global_sequence,
                events.events.back().global_sequence,
                static_cast<uint32_t>(events.events.size()), bundle->chunks[2]);
  GuestExecutionContinuousEventLimits limits;
  std::vector<GuestExecutionContinuousEvent> control;
  REQUIRE(GuestExecutionContinuousEventCodec::Decode(bundle->chunks[3],
                                                     &control, &error, limits));
  control[index].actor = {ordinal, guest_thread_id};
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(
      control, &bundle->chunks[3], &error));
  bundle->manifest.chunks[3] =
      Reference(GuestExecutionSessionChunkKind::kContinuousEvents, 3,
                control.front().global_sequence, control.back().global_sequence,
                static_cast<uint32_t>(control.size()), bundle->chunks[3]);
  bundle->manifest.participants[ordinal].first_event_sequence = sequence;
  bundle->manifest.participants[ordinal].last_event_sequence = sequence;
}

}  // namespace

TEST_CASE("continuous replay planner carries the signal witness table",
          "[guest-execution-session-runner][continuous]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize);
  GuestExecutionSessionSignalWitness witness;
  witness.after_scheduler_sequence = 3;
  witness.capture_instance_id = 0x1000;
  witness.guest_thread_id = 0x101;
  witness.object_handle = 0x2000;
  witness.signal_epoch = 5;
  witness.source = GuestExecutionSessionSignalWitnessSource::kParticipant;
  AttachSignalWitnesses(&bundle, {witness});
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                      bundle.chunks, &error));
  GuestExecutionContinuousReplayPlan plan;
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(error.empty());
  REQUIRE(plan.signal_witnesses.size() == 1);
  REQUIRE(plan.signal_witnesses.front() == witness);
}

TEST_CASE("continuous replay planner binds exact participant continuations",
          "[guest-execution-session-runner][continuous]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  const GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(error.empty());
  REQUIRE(plan.participants.size() == 2);
  REQUIRE(plan.events.size() == 8);
  REQUIRE(plan.corpus.functions().size() == 1);
  REQUIRE(plan.initial_session_checkpoint.global_sequence == 0);
  REQUIRE(plan.final_session_checkpoint.global_sequence == 8);
  REQUIRE(plan.initial_scheduler_topology.boundary ==
          GuestExecutionSessionSchedulerTopologyBoundary::kStart);
  REQUIRE(plan.final_scheduler_topology.boundary ==
          GuestExecutionSessionSchedulerTopologyBoundary::kFinal);
  REQUIRE(plan.initial_scheduler_topology.participants.size() == 2);
  REQUIRE(plan.initial_scheduler_topology.participants[0].state ==
          GuestExecutionSessionSchedulerParticipantState::kReady);
  REQUIRE(plan.initial_scheduler_topology.participants[0].base_priority == 6);
  REQUIRE(
      plan.initial_scheduler_topology.participants[0].quantum_remaining_us ==
      500);
  REQUIRE(plan.initial_scheduler_topology.participants[0].resume_kind ==
          GuestExecutionSessionSchedulerResumeKind::kJitSafepoint);
  REQUIRE(plan.initial_scheduler_topology.participants[0].guest_pc ==
          kCodeAddress + 0x40);
  REQUIRE(plan.initial_scheduler_topology.participants[0]
              .ready_queue_fifo_ordinal == 0);
  REQUIRE(plan.initial_scheduler_topology.participants[1]
              .ready_queue_fifo_ordinal == 1);
  REQUIRE(plan.final_scheduler_topology.participants[0].state ==
          GuestExecutionSessionSchedulerParticipantState::kRunning);
  REQUIRE(plan.final_scheduler_topology.participants[1].state ==
          GuestExecutionSessionSchedulerParticipantState::kSuspended);
  REQUIRE(plan.final_scheduler_topology.participants[1].suspension_count == 1);
  REQUIRE(plan.participants[0].initial_checkpoint.resume_pc ==
          kCodeAddress + 0x40);
  REQUIRE(plan.participants[1].initial_checkpoint.resume_pc ==
          kCodeAddress + 0x80);
  REQUIRE(plan.participants[0].final_checkpoint.resume_pc ==
          kCodeAddress + 0x44);
  REQUIRE(plan.participants[1].final_checkpoint.resume_pc ==
          kCodeAddress + 0x84);
  REQUIRE(plan.participants[0].initial_checkpoint.outer_guest_return_address ==
          kReturnAddress);
  REQUIRE(plan.participants[1].initial_checkpoint.outer_guest_return_address ==
          kReturnAddress + 4);
  REQUIRE(plan.resume_entries ==
          std::vector<GuestExecutionContinuousReplayResumeEntry>{
              {kCodeAddress + 0x40, kCodeAddress, kCodeAddress + 0xFC},
              {kCodeAddress + 0x80, kCodeAddress, kCodeAddress + 0xFC}});
  REQUIRE(plan.events[5].control.subject.participant_ordinal == 0);
  REQUIRE(plan.events[6].control.subject.participant_ordinal == 1);
  REQUIRE(plan.events[5].control.checkpoint.binding.resume_pc ==
          kCodeAddress + 0x44);
  REQUIRE(plan.events[6].control.checkpoint.binding.resume_pc ==
          kCodeAddress + 0x84);
  REQUIRE(plan.pages.size() == 8);
  REQUIRE(plan.reset_page_addresses ==
          std::vector<uint32_t>{kDataAddress, kDataAddress + 0x1000,
                                kDataAddress + 0x2000, kDataAddress + 0x3000});
  REQUIRE(plan.protection_granules ==
          std::vector<GuestInvocationReplayProtectionGranule>{
              {kDataAddress, kHostPageSize, true},
              {kCodeAddress, kHostPageSize, false}});
}

TEST_CASE("continuous replay planner plans physical-view data pages",
          "[guest-execution-session-runner][continuous]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  constexpr uint32_t kPhysicalDataAddress = 0xA0000000u;
  const GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize, false, kPhysicalDataAddress);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(error.empty());
  REQUIRE(plan.pages.size() == 2 * (kHostPageSize / kGuestPageSize));
  REQUIRE(plan.protection_granules ==
          std::vector<GuestInvocationReplayProtectionGranule>{
              {kCodeAddress, kHostPageSize, false},
              {kPhysicalDataAddress, kHostPageSize, true}});
}

TEST_CASE("continuous replay planner rejects the reserved first 64k",
          "[guest-execution-session-runner][continuous]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  const GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize, false, 0x1000u);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;
  REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                        &plan, &error));
  REQUIRE(error == "continuous initial checkpoint page closure is invalid");
  REQUIRE(plan.pages.empty());
}

TEST_CASE("continuous replay planner requires complete scheduler topology",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionContinuousReplayPlan plan;
  std::string error;

  SECTION("legacy continuous bundle without topology is not replayable") {
    GuestExecutionSessionBundle bundle =
        MakeContinuousSessionBundle(kHostPageSize);
    GuestExecutionSessionCheckpointChunk final_checkpoint;
    REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
        bundle.chunks.back(), &final_checkpoint, &error));
    final_checkpoint.ordinal = 4;
    bundle.chunks.erase(bundle.chunks.begin() + 4, bundle.chunks.begin() + 6);
    bundle.manifest.chunks.erase(bundle.manifest.chunks.begin() + 4,
                                 bundle.manifest.chunks.begin() + 6);
    REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
        final_checkpoint, &bundle.chunks.back(), &error));
    bundle.manifest.chunks.back() = Reference(
        GuestExecutionSessionChunkKind::kCheckpoint, 4,
        final_checkpoint.checkpoint.global_sequence,
        final_checkpoint.checkpoint.global_sequence, 1, bundle.chunks.back());
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(error == "continuous session chunk closure is missing");
  }

  SECTION("topology missing one roster participant fails bundle closure") {
    GuestExecutionSessionBundle bundle =
        MakeContinuousSessionBundle(kHostPageSize);
    GuestExecutionSessionSchedulerTopologyChunk topology;
    REQUIRE(GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
        bundle.chunks[4], &topology, &error));
    topology.participants.pop_back();
    REQUIRE(GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
        topology, &bundle.chunks[4], &error));
    bundle.manifest.chunks[4] =
        Reference(GuestExecutionSessionChunkKind::kSchedulerTopology, 4, 0, 0,
                  1, bundle.chunks[4]);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
  }

  REQUIRE(plan.participants.empty());
  REQUIRE(plan.initial_scheduler_topology.participants.empty());
  REQUIRE(plan.final_scheduler_topology.participants.empty());
}

TEST_CASE("continuous replay planner plans an unchanged passive participant",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  const GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize, true);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(error.empty());
  REQUIRE(plan.participants.size() == 2);
  REQUIRE(plan.participants[1].final_checkpoint.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  REQUIRE(plan.final_scheduler_topology.participants[1].resume_kind ==
          GuestExecutionSessionSchedulerResumeKind::kNativeContinuation);
  REQUIRE(plan.resume_entries.size() == 1);
}

TEST_CASE("continuous replay planner enforces passive topology parity",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize, true);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;

  SECTION("a changed quantum rejects") {
    MutateFinalSchedulerTopology(
        &bundle, [](GuestExecutionSessionSchedulerTopologyChunk* topology) {
          topology->participants[1].quantum_remaining_us = 0;
        });
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error.find("scheduler topology changed between boundaries: "
                       "quantum_remaining_us") != std::string::npos);
  }

  SECTION("a changed ready-queue FIFO ordinal rejects") {
    MutateFinalSchedulerTopology(
        &bundle, [](GuestExecutionSessionSchedulerTopologyChunk* topology) {
          topology->participants[0].ready_queue_fifo_ordinal = 1;
          topology->participants[1].ready_queue_fifo_ordinal = 0;
        });
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error ==
            "scheduler ready FIFO order changed between boundaries on cpu 0 "
            "level 8: thread 00000202 overtook thread 00000101");
  }

  SECTION("a ready participant suspended between boundaries rejects") {
    MutateFinalSchedulerTopology(
        &bundle, [](GuestExecutionSessionSchedulerTopologyChunk* topology) {
          auto& participant = topology->participants[1];
          participant.state =
              GuestExecutionSessionSchedulerParticipantState::kSuspended;
          participant.suspension_count = 1;
          participant.ready_queue_level =
              kGuestExecutionSessionSchedulerNoValue;
          participant.ready_queue_fifo_ordinal =
              kGuestExecutionSessionSchedulerNoValue;
        });
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error.find("scheduler topology changed between boundaries: "
                       "state") != std::string::npos);
  }

  REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                        &plan, &error));
  REQUIRE(plan.participants.empty());
  REQUIRE(plan.resume_entries.empty());
}

TEST_CASE("continuous replay planner rejects ambiguous continuation routes",
          "[guest-execution-session-runner][continuous]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;

  SECTION("a participant final route is missing") {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousControlEvents(bundle);
    events[6].checkpoint = {};
    events[6].subject = {};
    ReplaceContinuousControlEvents(&bundle, events);
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(error == "continuous executable participant route is missing");
  }

  SECTION("a participant final route is duplicated") {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousControlEvents(bundle);
    events[6].subject = events[5].subject;
    events[6].checkpoint = events[5].checkpoint;
    ReplaceContinuousControlEvents(&bundle, events);
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(error == "continuous final checkpoint route is duplicated");
  }

  SECTION("an initial continuation owner is absent from the exact corpus") {
    ppc::GuestPPCThreadCheckpoint checkpoint = MakeContinuousThreadCheckpoint(
        0, 0x101, kCodeAddress + 0x140, kReturnAddress, 1);
    checkpoint.owning_function_address = kCodeAddress + 0x100;
    checkpoint.owning_function_end_address = kCodeAddress + 0x1FC;
    ReplaceInitialContinuousCheckpoint(&bundle, 0, checkpoint);
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(error ==
            "continuous initial checkpoint owner differs from the corpus");
  }

  REQUIRE(plan.participants.empty());
  REQUIRE(plan.events.empty());
  REQUIRE(plan.pages.empty());
}

TEST_CASE("continuous validation rejects passive scheduler event subjects",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionContinuousReplayPlan plan;
  std::string error;

  SECTION("a dispatch record subjecting the passive participant rejects") {
    const GuestExecutionSessionBundle bundle =
        MakePassiveContinuousSessionBundle(kHostPageSize,
                                           kSchedulerRecordDispatch, 1);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error == "scheduler event subjects an outside-guest participant");
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(error == "scheduler event subjects an outside-guest participant");
    REQUIRE(plan.participants.empty());
  }

  SECTION("a resume record subjecting the passive participant rejects") {
    const GuestExecutionSessionBundle bundle =
        MakePassiveContinuousSessionBundle(kHostPageSize,
                                           kSchedulerRecordResume, 1);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error == "scheduler event subjects an outside-guest participant");
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(plan.participants.empty());
  }

  SECTION("a record subjecting only the executable participant validates") {
    const GuestExecutionSessionBundle bundle =
        MakePassiveContinuousSessionBundle(kHostPageSize,
                                           kSchedulerRecordDispatch, 0);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error.empty());
    REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                    &plan, &error));
    REQUIRE(error.empty());
    REQUIRE(plan.participants.size() == 2);
    REQUIRE(plan.events.size() == 6);
  }

  SECTION("an unknown scheduler payload version fails closed") {
    const GuestExecutionSessionBundle bundle =
        MakePassiveContinuousSessionBundle(kHostPageSize,
                                           kSchedulerRecordDispatch, 0, 3);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    REQUIRE(plan.participants.empty());
  }
}

TEST_CASE("continuous validation admits ready FIFO renumbering",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionContinuousReplayPlan plan;
  std::string error;
  const auto requeue_at_head =
      [](GuestExecutionSessionSchedulerTopologyChunk* topology) {
        GuestExecutionSessionSchedulerTopologyParticipant& dispatched =
            topology->participants[0];
        dispatched.state =
            GuestExecutionSessionSchedulerParticipantState::kReady;
        dispatched.ready_queue_level = 8;
        dispatched.ready_queue_fifo_ordinal = 0;
        topology->participants[1].ready_queue_fifo_ordinal = 1;
      };

  SECTION("a dispatched row inserted at the head renumbers the passive row") {
    GuestExecutionSessionBundle bundle = MakePassiveContinuousSessionBundle(
        kHostPageSize, kSchedulerRecordDispatch, 0);
    MutateFinalSchedulerTopology(&bundle, requeue_at_head);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                    &plan, &error));
    REQUIRE(error.empty());
    REQUIRE(plan.participants.size() == 2);
  }

  SECTION("a running row requeued at the head renumbers the passive row") {
    GuestExecutionSessionBundle bundle = MakePassiveContinuousSessionBundle(
        kHostPageSize, kSchedulerRecordDispatch, 0);
    MutateStartSchedulerTopology(
        &bundle, [](GuestExecutionSessionSchedulerTopologyChunk* topology) {
          GuestExecutionSessionSchedulerTopologyParticipant& running =
              topology->participants[0];
          running.state =
              GuestExecutionSessionSchedulerParticipantState::kRunning;
          running.ready_queue_level = kGuestExecutionSessionSchedulerNoValue;
          running.ready_queue_fifo_ordinal =
              kGuestExecutionSessionSchedulerNoValue;
        });
    MutateFinalSchedulerTopology(&bundle, requeue_at_head);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                    &plan, &error));
    REQUIRE(error.empty());
    REQUIRE(plan.participants.size() == 2);
  }

  SECTION("a dequeued neighbour renumbers the passive row") {
    const GuestExecutionSessionBundle bundle =
        MakePassiveContinuousSessionBundle(kHostPageSize,
                                           kSchedulerRecordDispatch, 0);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                    &plan, &error));
    REQUIRE(error.empty());
  }
}

TEST_CASE("continuous replay planner plans a ready-parity park",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  const GuestExecutionSessionBundle bundle = MakeParkedContinuousSessionBundle(
      kHostPageSize, ContinuousParkKind::kReadyParity);
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                      bundle.chunks, &error));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  GuestExecutionContinuousReplayPlan plan;
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(error.empty());
  REQUIRE(plan.participants.size() == 2);
  REQUIRE(plan.participants[1].initial_outer_call_state ==
          GuestExecutionSessionInitialOuterCallState::kParkedBelowOuterCall);
  REQUIRE(plan.participants[1].initial_checkpoint ==
          plan.participants[1].final_checkpoint);
  REQUIRE(plan.participants[1].initial_checkpoint.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  REQUIRE(IsGuestExecutionSessionReadyParityParticipant(
      plan.initial_scheduler_topology.participants[1]));
  REQUIRE(IsGuestExecutionSessionReadyParityParticipant(
      plan.final_scheduler_topology.participants[1]));
  // Only the executable participant seeds one; the park seeds none.
  REQUIRE(plan.resume_entries.size() == 1);
}

TEST_CASE("continuous replay planner plans a blocked-parity park",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  const GuestExecutionSessionBundle bundle = MakeParkedContinuousSessionBundle(
      kHostPageSize, ContinuousParkKind::kBlockedParity);
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                      bundle.chunks, &error));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  GuestExecutionContinuousReplayPlan plan;
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(error.empty());
  REQUIRE(plan.participants.size() == 2);
  REQUIRE(plan.participants[1].initial_outer_call_state ==
          GuestExecutionSessionInitialOuterCallState::kParkedBelowOuterCall);
  REQUIRE(plan.participants[1].initial_checkpoint.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  // The row carries the link register the wait returns to, and the planner
  // must not read that as a route it could resume.
  REQUIRE(plan.initial_scheduler_topology.participants[1].guest_pc ==
          kReturnAddress + 8);
  REQUIRE_FALSE(plan.initial_scheduler_topology.participants[1].restorable);
  REQUIRE(IsGuestExecutionSessionBlockedParityParticipant(
      plan.initial_scheduler_topology.participants[1]));
  REQUIRE(IsGuestExecutionSessionBlockedParityParticipant(
      plan.final_scheduler_topology.participants[1]));
  // Only the executable participant seeds one; the park seeds none.
  REQUIRE(plan.resume_entries.size() == 1);
}

TEST_CASE("continuous replay planner refuses a park that claims work",
          "[guest-execution-session-runner][continuous]"
          "[guest-execution-scheduler-topology]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestExecutionContinuousReplayPlan plan;
  std::string error;

  SECTION("a parked row that publishes an executable route") {
    GuestExecutionSessionBundle bundle = MakeParkedContinuousSessionBundle(
        kHostPageSize, ContinuousParkKind::kReadyParity);
    const auto restorable_route =
        [](GuestExecutionSessionSchedulerTopologyChunk* topology) {
          GuestExecutionSessionSchedulerTopologyParticipant& row =
              topology->participants[1];
          row.resume_kind =
              GuestExecutionSessionSchedulerResumeKind::kJitSafepoint;
          row.guest_pc = kCodeAddress + 0x40;
          row.restorable = true;
        };
    MutateStartSchedulerTopology(&bundle, restorable_route);
    MutateFinalSchedulerTopology(&bundle, restorable_route);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    INFO(error);
    REQUIRE(error.find("JIT route differs from PPC") != std::string::npos);
  }

  SECTION("a parked state that leaves a modeled export dispatch open") {
    GuestExecutionSessionBundle bundle = MakeParkedContinuousSessionBundle(
        kHostPageSize, ContinuousParkKind::kBlockedParity);
    ppc::GuestPPCThreadCheckpoint pending = MakeContinuousThreadCheckpoint(
        1, 0x202, kCodeAddress + 0x40, kReturnAddress + 4, 2);
    pending.resume_kind =
        ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern;
    pending.pending_external_event_sequence = 2;
    pending.pending_export_guest_address = kCodeAddress + 0x40;
    ReplaceParkedBoundaryCheckpoints(&bundle, 1, pending);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    INFO(error);
    REQUIRE(error.find("parked topology has an executable PPC route") !=
            std::string::npos);
  }

  SECTION("a parked participant that owns a guest execution record") {
    GuestExecutionSessionBundle bundle = MakeParkedContinuousSessionBundle(
        kHostPageSize, ContinuousParkKind::kReadyParity);
    RetargetContinuousEvent(&bundle, 1, 1, 0x202);
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                        bundle.chunks, &error));
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    REQUIRE_FALSE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize,
                                                          &plan, &error));
    INFO(error);
    REQUIRE(error.find("outside-guest participant changed or executed") !=
            std::string::npos);
  }

  REQUIRE(plan.participants.empty());
  REQUIRE(plan.resume_entries.empty());
}

TEST_CASE("guest execution session planner resolves segments and event roles",
          "[guest-execution-session-runner]") {
  const BundleOptions options;
  const GuestExecutionSessionBundle bundle = MakeSessionBundle(options);
  GuestExecutionSessionReplayPlan plan;
  std::string error;
  REQUIRE(BuildGuestExecutionSessionReplayPlan(bundle, options.host_page_size,
                                               &plan, &error));
  REQUIRE(error.empty());
  REQUIRE(plan.participants.size() == 2);
  REQUIRE(plan.segments.size() == 2);
  REQUIRE(plan.events.size() == 6);
  REQUIRE(plan.checkpoints.size() == 2);
  REQUIRE(plan.eager_function_count == 1);
  REQUIRE(plan.eager_guest_code_bytes == 32);
  REQUIRE(plan.captured_host_code_bytes == 64);

  using Role = GuestExecutionSessionReplayEventRole;
  const std::vector<Role> expected_roles = {
      Role::kSegmentBegin, Role::kSegmentEnd,      Role::kSegmentBegin,
      Role::kSegmentEnd,   Role::kBoundaryControl, Role::kBoundaryControl,
  };
  for (size_t i = 0; i < expected_roles.size(); ++i) {
    REQUIRE(plan.events[i].role == expected_roles[i]);
    REQUIRE(plan.events[i].checkpoint_index ==
            kGuestExecutionSessionReplayNoIndex);
  }
  REQUIRE(plan.events[0].segment_index == 0);
  REQUIRE(plan.events[1].segment_index == 0);
  REQUIRE(plan.events[2].segment_index == 1);
  REQUIRE(plan.events[3].segment_index == 1);
  REQUIRE(plan.participants[0].event_indices == std::vector<uint32_t>{0, 1});
  REQUIRE(plan.participants[1].event_indices == std::vector<uint32_t>{2, 3});
  REQUIRE(plan.participants[0].segment_count == 1);
  REQUIRE(plan.participants[1].segment_count == 1);
  REQUIRE(plan.participants[1].initial_state.gpr[7] == 0x1000);
  REQUIRE(plan.participants[1].initial_state_blob);
  REQUIRE(plan.participants[1].initial_state_blob->size() ==
          ppc::GuestPPCRegisterStateCodec::kEncodedSize);
  REQUIRE(plan.coordinator_event_indices == std::vector<uint32_t>{4, 5});
  REQUIRE(plan.segments[0].thread_ordinal == 0);
  REQUIRE(plan.segments[1].thread_ordinal == 1);
  REQUIRE(plan.segments[1].invocation.input.gpr[7] == 0x1000);

  const uint32_t closure_page_count = options.host_page_size / kGuestPageSize;
  REQUIRE(plan.pages.size() == 2 * closure_page_count);
  REQUIRE(plan.pages.front().guest_address == kDataAddress);
  REQUIRE_FALSE(plan.pages.front().code);
  REQUIRE(plan.pages.back().code);
  REQUIRE(plan.reset_page_addresses == std::vector<uint32_t>{kDataAddress});
  REQUIRE(plan.protection_granules ==
          std::vector<GuestInvocationReplayProtectionGranule>{
              {kDataAddress, options.host_page_size, true},
              {kCodeAddress, options.host_page_size, false}});

  SECTION("a reordered tape plans and is rejected only by execution") {
    BundleOptions swapped = options;
    swapped.swap_order = true;
    const GuestExecutionSessionBundle swapped_bundle =
        MakeSessionBundle(swapped);
    REQUIRE(BuildGuestExecutionSessionReplayPlan(
        swapped_bundle, options.host_page_size, &plan, &error));
    REQUIRE(plan.segments[0].thread_ordinal == 1);
    REQUIRE(plan.participants[1].event_indices == std::vector<uint32_t>{0, 1});
  }

  SECTION("a quiescent asynchronous mutation is injected by the coordinator") {
    BundleOptions mutated = options;
    mutated.extra = ExtraEvent::kMutationBetweenSegments;
    const GuestExecutionSessionBundle mutated_bundle =
        MakeSessionBundle(mutated);
    REQUIRE(BuildGuestExecutionSessionReplayPlan(
        mutated_bundle, options.host_page_size, &plan, &error));
    REQUIRE(plan.events.size() == 7);
    REQUIRE(plan.events[2].role == Role::kAsynchronousMutation);
    REQUIRE(plan.coordinator_event_indices == std::vector<uint32_t>{2, 5, 6});
    REQUIRE(plan.reset_page_addresses == std::vector<uint32_t>{kDataAddress});
  }
}

TEST_CASE("guest execution session planner carries a park that runs nothing",
          "[guest-execution-session-runner]") {
  BundleOptions options;
  GuestExecutionSessionBundle bundle = MakeSessionBundle(options);
  AppendParkedSegmentedParticipant(&bundle);
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                      bundle.chunks, &error));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  GuestExecutionSessionReplayPlan plan;
  REQUIRE(BuildGuestExecutionSessionReplayPlan(bundle, options.host_page_size,
                                               &plan, &error));
  REQUIRE(error.empty());
  REQUIRE(plan.participants.size() == 3);
  REQUIRE(plan.participants[2].segment_count == 0);
  REQUIRE(plan.participants[2].event_indices.empty());
}

TEST_CASE("guest execution session planner refuses a park that owns work",
          "[guest-execution-session-runner]") {
  BundleOptions options;
  GuestExecutionSessionBundle bundle = MakeSessionBundle(options);
  bundle.manifest.participants[0].initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kParkedBelowOuterCall;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                      bundle.chunks, &error));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  GuestExecutionSessionReplayPlan plan;
  REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
      bundle, options.host_page_size, &plan, &error));
  REQUIRE(error == "parked participant owns a segment or a recorded event");
  REQUIRE(plan.participants.empty());
}

TEST_CASE("guest execution session planner rejects sessions without a hook",
          "[guest-execution-session-runner]") {
  GuestExecutionSessionReplayPlan plan;
  std::string error;
  BundleOptions options;

  SECTION("instruction coverage needs counters") {
    options.extra = ExtraEvent::kCoverageInsideSegment;
    REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
        MakeSessionBundle(options), options.host_page_size, &plan, &error));
    REQUIRE(error ==
            "instruction coverage events require guest instruction counters "
            "that timed replay does not provide");
  }

  SECTION("kernel export injection is absent") {
    options.extra = ExtraEvent::kKernelExportInsideSegment;
    REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
        MakeSessionBundle(options), options.host_page_size, &plan, &error));
    REQUIRE(error ==
            "participant event kind 5 has no replay hook in this runner");
  }

  SECTION("asynchronous mutation inside a running segment") {
    options.extra = ExtraEvent::kMutationInsideSegment;
    REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
        MakeSessionBundle(options), options.host_page_size, &plan, &error));
    REQUIRE(error == "asynchronous mutation overlaps a running segment");
  }

  SECTION("concurrent segments need recorded scheduling") {
    options.overlap_segments = true;
    REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
        MakeSessionBundle(options), options.host_page_size, &plan, &error));
    REQUIRE(error ==
            "segment begin at sequence 2 overlaps an open segment; concurrent "
            "segments are not replayable without recorded scheduling");
  }

  SECTION("segment provenance must bind the session configuration") {
    options.corrupt_replay_config = true;
    REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
        MakeSessionBundle(options), options.host_page_size, &plan, &error));
    REQUIRE(error ==
            "segment artifact replay configuration differs from the session");
  }

  SECTION("segment pages must come from the initial checkpoint") {
    options.drop_last_initial_page = true;
    REQUIRE_FALSE(BuildGuestExecutionSessionReplayPlan(
        MakeSessionBundle(options), options.host_page_size, &plan, &error));
    REQUIRE(error ==
            "segment input page is absent from the initial checkpoint");
  }

  REQUIRE(plan.participants.empty());
  REQUIRE(plan.events.empty());
  REQUIRE(plan.pages.empty());
}

// This backend fixture is mechanism coverage only until it has been built and
// run on an Apple A64 host.
TEST_CASE("guest execution session runner replays persistent participants",
          "[guest-execution-session-runner][backend]") {
  BundleOptions options;
  options.host_page_size = static_cast<uint32_t>(xe::memory::page_size());
  const GuestExecutionSessionBundle bundle = MakeSessionBundle(options);
  GuestExecutionSessionRunner::Options runner_options;
  // Two 262,144-iteration segments per repetition; the floor is well below
  // that so the gate proves only that it is enforced, not the title budget.
  runner_options.minimum_participant_cpu_nanoseconds_per_repetition = 1000;

  auto backend = testing::CreateBackend();
  if (!backend) {
    WARN("No executable host backend is available");
    return;
  }

  std::string error;
  std::unique_ptr<GuestExecutionSessionRunner> runner =
      GuestExecutionSessionRunner::Create(bundle, std::move(backend),
                                          runner_options, &error);
#if !XE_PLATFORM_MAC || !XE_ARCH_ARM64
  REQUIRE_FALSE(runner);
  REQUIRE(error == "guest execution session replay runner requires Apple A64");
  return;
#else
  REQUIRE(runner);
  REQUIRE(error.empty());
  REQUIRE(runner->WarmAndVerify(&error));
  REQUIRE(error.empty());
  const GuestExecutionSessionReplayMetrics& warmup = runner->warmup_metrics();
  REQUIRE(warmup.repetition_count == 1);
  REQUIRE(warmup.consumed_event_count_per_repetition == 6);
  REQUIRE(warmup.participants.size() == 2);
  REQUIRE(warmup.participants[0].consumed_event_count == 2);
  REQUIRE(warmup.participants[1].consumed_event_count == 2);
  REQUIRE(warmup.in_interval_verification_count == 4);
  REQUIRE(warmup.code_shape.function_count == 1);

  GuestExecutionSessionReplayMetrics metrics;
  REQUIRE(runner->RunTimed(8, &metrics, &error));
  REQUIRE(error.empty());
  REQUIRE(metrics.repetition_count == 8);
  REQUIRE(metrics.consumed_event_count_per_repetition == 6);
  REQUIRE(metrics.intermediate_checkpoint_count == 0);
  REQUIRE(metrics.in_interval_verification_count == 0);
  REQUIRE(metrics.in_interval_verification_thread_cpu_nanoseconds == 0);
  REQUIRE(
      metrics.participant_thread_cpu_nanoseconds >=
      8 * runner_options.minimum_participant_cpu_nanoseconds_per_repetition);
  REQUIRE(metrics.uptime_raw_nanoseconds > 0);
  REQUIRE(metrics.placement_generation_before ==
          metrics.placement_generation_after);
  REQUIRE(metrics.reset_page_count_per_repetition == 1);
  REQUIRE(metrics.reset_bytes_per_repetition == kGuestPageSize);
  REQUIRE(metrics.code_shape == warmup.code_shape);
  REQUIRE(metrics.participants.size() == 2);
  REQUIRE(metrics.participants[0].consumed_event_count == 16);
  REQUIRE(metrics.participants[1].consumed_event_count == 16);

  SECTION("a quiescent asynchronous mutation reaches the next segment") {
    runner.reset();
    BundleOptions mutated = options;
    mutated.extra = ExtraEvent::kMutationBetweenSegments;
    const GuestExecutionSessionBundle mutated_bundle =
        MakeSessionBundle(mutated);
    std::unique_ptr<GuestExecutionSessionRunner> mutated_runner =
        GuestExecutionSessionRunner::Create(
            mutated_bundle, testing::CreateBackend(), runner_options, &error);
    REQUIRE(mutated_runner);
    REQUIRE(mutated_runner->WarmAndVerify(&error));
    REQUIRE(error.empty());
    REQUIRE(mutated_runner->RunTimed(2, &metrics, &error));
    REQUIRE(metrics.consumed_event_count_per_repetition == 7);
  }

  SECTION("a reordered tape rejects at the first real segment entry") {
    runner.reset();
    BundleOptions swapped = options;
    swapped.swap_order = true;
    const GuestExecutionSessionBundle swapped_bundle =
        MakeSessionBundle(swapped);
    std::unique_ptr<GuestExecutionSessionRunner> swapped_runner =
        GuestExecutionSessionRunner::Create(
            swapped_bundle, testing::CreateBackend(), runner_options, &error);
    REQUIRE(swapped_runner);
    REQUIRE_FALSE(swapped_runner->WarmAndVerify(&error));
    REQUIRE(error == "segment input page differs from live guest memory");
  }
#endif  // !XE_PLATFORM_MAC || !XE_ARCH_ARM64
}

// Stage E0 of the continuous executor: the replay CLI's plan attempt is the
// only non-test caller of the continuous planner, so its gate order and its
// verdict records are asserted against the same synthetic bundles.
TEST_CASE("continuous replay CLI reports a planned session",
          "[guest-execution-session-runner][continuous]"
          "[guest-invocation-replay-cli]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  const GuestExecutionSessionBundle bundle =
      MakeContinuousSessionBundle(kHostPageSize);
  GuestExecutionContinuousReplayPlan plan;
  std::string error;
  REQUIRE(BuildGuestExecutionContinuousReplayPlan(bundle, kHostPageSize, &plan,
                                                  &error));
  REQUIRE(plan.participants.size() == 2);
  REQUIRE(plan.events.size() == 8);

  GuestSessionContinuousReplayVerdict verdict;
  REQUIRE(AttemptGuestSessionContinuousReplayPlan(
      bundle, kHostPageSize, true, bundle.manifest.replay_config_sha256,
      &verdict));
  REQUIRE(verdict.planned);
  REQUIRE(verdict.plan_line ==
          "XENIA_GUEST_SESSION_CONTINUOUS_PLAN_V1 status=planned "
          "participants=" +
              std::to_string(plan.participants.size()) +
              " events=" + std::to_string(plan.events.size()) +
              " pages=" + std::to_string(plan.pages.size()) +
              " resume_entries=" + std::to_string(plan.resume_entries.size()));
  REQUIRE(verdict.exec_line ==
          "XENIA_GUEST_SESSION_CONTINUOUS_EXEC_V1 status=rejected "
          "reason=\"continuous executor is not implemented\"");
}

TEST_CASE("continuous replay CLI gates configuration before planning",
          "[guest-execution-session-runner][continuous]"
          "[guest-invocation-replay-cli]") {
  constexpr uint32_t kHostPageSize = 16 * 1024;
  GuestSessionContinuousReplayVerdict verdict;

  SECTION("a segmented session rejects with the zero-segment reason") {
    const BundleOptions options;
    const GuestExecutionSessionBundle bundle = MakeSessionBundle(options);
    REQUIRE_FALSE(AttemptGuestSessionContinuousReplayPlan(
        bundle, kHostPageSize, false, bundle.manifest.replay_config_sha256,
        &verdict));
    REQUIRE(verdict.plan_line ==
            FormatGuestSessionContinuousPlanRejection(
                "continuous replay requires a zero-segment session"));
  }

  SECTION("a mismatched configuration hash rejects before the plan") {
    const GuestExecutionSessionBundle bundle =
        MakeContinuousSessionBundle(kHostPageSize);
    GuestInvocationReplaySha256 other_config_sha256 =
        bundle.manifest.replay_config_sha256;
    other_config_sha256[0] = static_cast<uint8_t>(other_config_sha256[0] + 1);
    REQUIRE_FALSE(AttemptGuestSessionContinuousReplayPlan(
        bundle, kHostPageSize, true, other_config_sha256, &verdict));
    REQUIRE(verdict.plan_line ==
            FormatGuestSessionContinuousPlanRejection(
                "continuous replay configuration SHA-256 does not match the "
                "session"));
  }

  SECTION("a corpus recorded under another scheduler setting rejects first") {
    const GuestExecutionSessionBundle bundle =
        MakeContinuousSessionBundle(kHostPageSize);
    // Both gates would fire; the named scheduler reason has to win, because it
    // is the one that explains why a matching hash would still never poll.
    GuestInvocationReplaySha256 other_config_sha256 =
        bundle.manifest.replay_config_sha256;
    other_config_sha256[0] = static_cast<uint8_t>(other_config_sha256[0] + 1);
    REQUIRE_FALSE(AttemptGuestSessionContinuousReplayPlan(
        bundle, kHostPageSize, false, other_config_sha256, &verdict));
    REQUIRE(verdict.plan_line ==
            FormatGuestSessionContinuousPlanRejection(
                "continuous replay corpus guest_scheduler does not match the "
                "runtime"));
  }

  SECTION("an unsupported host page size rejects at the plan") {
    const GuestExecutionSessionBundle bundle =
        MakeContinuousSessionBundle(kHostPageSize);
    REQUIRE_FALSE(AttemptGuestSessionContinuousReplayPlan(
        bundle, 1024, true, bundle.manifest.replay_config_sha256, &verdict));
    REQUIRE(verdict.plan_line ==
            FormatGuestSessionContinuousPlanRejection(
                "host page size is unsupported for continuous replay"));
  }

  REQUIRE_FALSE(verdict.planned);
  REQUIRE(verdict.exec_line.empty());
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
