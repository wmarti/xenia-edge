/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_bundle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_execution_continuous_event.h"

namespace xe {
namespace cpu {
namespace test {

namespace {

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
              ("xenia-session-bundle-test-" + std::to_string(nonce) + "-" +
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

GuestExecutionSessionSha256 IdentityDigest(uint8_t seed) {
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

std::vector<uint8_t> EncodeThreadCheckpoint(
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(
      ppc::GuestPPCThreadCheckpointCodec::Encode(checkpoint, &encoded, &error));
  REQUIRE(error.empty());
  return encoded;
}

std::vector<uint8_t> ThreadCheckpointBytes(
    uint32_t participant_ordinal, uint32_t guest_thread_id, uint32_t resume_pc,
    uint64_t register_seed,
    ppc::GuestPPCThreadResumeKind resume_kind =
        ppc::GuestPPCThreadResumeKind::kGuestBlockHead,
    uint64_t pending_event_sequence = 0, uint32_t pending_export_address = 0) {
  ppc::GuestPPCThreadCheckpoint checkpoint;
  checkpoint.participant_ordinal = participant_ordinal;
  checkpoint.guest_thread_id = guest_thread_id;
  checkpoint.resume_kind = resume_kind;
  checkpoint.resume_pc = resume_pc;
  checkpoint.owning_function_address = 0x82000000;
  checkpoint.owning_function_end_address = 0x820000FC;
  checkpoint.outer_guest_return_address = 0x82000100;
  checkpoint.pending_external_event_sequence = pending_event_sequence;
  checkpoint.pending_export_guest_address = pending_export_address;
  checkpoint.registers.gpr.front() = register_seed;
  return EncodeThreadCheckpoint(checkpoint);
}

ppc::GuestPPCThreadCheckpointBinding BindingForCheckpoint(
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

std::optional<ppc::GuestPPCThreadCheckpointBinding> DecodeCheckpointBinding(
    const std::vector<uint8_t>& bytes) {
  ppc::GuestPPCThreadCheckpoint checkpoint;
  if (!ppc::GuestPPCThreadCheckpointCodec::Decode(bytes, &checkpoint)) {
    return std::nullopt;
  }
  return BindingForCheckpoint(checkpoint);
}

void ReplaceContinuousEvents(
    GuestExecutionSessionBundle* bundle,
    const std::vector<GuestExecutionContinuousEvent>& events) {
  REQUIRE(bundle);
  REQUIRE(bundle->chunks.size() == 4);
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(events, &bundle->chunks[2],
                                                     &error));
  REQUIRE(error.empty());
  bundle->manifest.chunks[2] = ReferenceFor(
      GuestExecutionSessionChunkKind::kContinuousEvents, 2,
      events.front().global_sequence, events.back().global_sequence,
      static_cast<uint32_t>(events.size()), bundle->chunks[2]);
}

std::vector<GuestExecutionContinuousEvent> DecodeContinuousEvents(
    const GuestExecutionSessionBundle& bundle) {
  const auto reference = std::find_if(
      bundle.manifest.chunks.cbegin(), bundle.manifest.chunks.cend(),
      [](const GuestExecutionSessionChunkReference& chunk) {
        return chunk.kind == GuestExecutionSessionChunkKind::kContinuousEvents;
      });
  REQUIRE(reference != bundle.manifest.chunks.cend());
  const size_t chunk_index =
      static_cast<size_t>(reference - bundle.manifest.chunks.cbegin());
  std::vector<GuestExecutionContinuousEvent> events;
  std::string error;
  REQUIRE(GuestExecutionContinuousEventCodec::Decode(bundle.chunks[chunk_index],
                                                     &events, &error));
  REQUIRE(error.empty());
  return events;
}

std::vector<GuestExecutionSessionEvent> DecodeCanonicalEvents(
    const GuestExecutionSessionBundle& bundle) {
  GuestExecutionSessionEventChunk chunk;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(bundle.chunks[1], &chunk,
                                                       &error));
  REQUIRE(error.empty());
  return chunk.events;
}

void ReplaceCanonicalEvents(GuestExecutionSessionBundle* bundle,
                            std::vector<GuestExecutionSessionEvent> events) {
  REQUIRE(bundle);
  GuestExecutionSessionEventChunk chunk;
  chunk.session_epoch = bundle->manifest.session_epoch;
  chunk.ordinal = 1;
  chunk.events = std::move(events);
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      chunk, &bundle->chunks[1], &error));
  REQUIRE(error.empty());
  bundle->manifest.chunks[1] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1,
      chunk.events.front().global_sequence, chunk.events.back().global_sequence,
      static_cast<uint32_t>(chunk.events.size()), bundle->chunks[1]);
}

void SplitContinuousEvents(GuestExecutionSessionBundle* bundle,
                           size_t split_index) {
  REQUIRE(bundle);
  const auto reference = std::find_if(
      bundle->manifest.chunks.cbegin(), bundle->manifest.chunks.cend(),
      [](const GuestExecutionSessionChunkReference& chunk) {
        return chunk.kind == GuestExecutionSessionChunkKind::kContinuousEvents;
      });
  REQUIRE(reference != bundle->manifest.chunks.cend());
  const size_t continuous_index =
      static_cast<size_t>(reference - bundle->manifest.chunks.cbegin());
  REQUIRE(continuous_index + 2 == bundle->chunks.size());
  std::vector<GuestExecutionContinuousEvent> events =
      DecodeContinuousEvents(*bundle);
  REQUIRE(split_index > 0);
  REQUIRE(split_index < events.size());
  std::vector<GuestExecutionContinuousEvent> first(
      events.begin(), events.begin() + split_index);
  std::vector<GuestExecutionContinuousEvent> second(
      events.begin() + split_index, events.end());

  std::vector<uint8_t> first_bytes;
  std::vector<uint8_t> second_bytes;
  std::string error;
  REQUIRE(
      GuestExecutionContinuousEventCodec::Encode(first, &first_bytes, &error));
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(second, &second_bytes,
                                                     &error));

  GuestExecutionSessionCheckpointChunk final;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
      bundle->chunks.back(), &final, &error));
  final.ordinal = static_cast<uint32_t>(continuous_index + 2);
  std::vector<uint8_t> final_bytes;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(final, &final_bytes,
                                                            &error));

  std::vector<std::vector<uint8_t>> chunks;
  std::vector<GuestExecutionSessionChunkReference> references;
  chunks.reserve(bundle->chunks.size() + 1);
  references.reserve(bundle->manifest.chunks.size() + 1);
  for (size_t i = 0; i < continuous_index; ++i) {
    chunks.push_back(std::move(bundle->chunks[i]));
    references.push_back(bundle->manifest.chunks[i]);
  }
  chunks.push_back(std::move(first_bytes));
  references.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kContinuousEvents,
                   static_cast<uint32_t>(continuous_index),
                   first.front().global_sequence, first.back().global_sequence,
                   static_cast<uint32_t>(first.size()), chunks.back()));
  chunks.push_back(std::move(second_bytes));
  references.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kContinuousEvents,
      static_cast<uint32_t>(continuous_index + 1),
      second.front().global_sequence, second.back().global_sequence,
      static_cast<uint32_t>(second.size()), chunks.back()));
  chunks.push_back(std::move(final_bytes));
  references.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, final.ordinal,
                   final.checkpoint.global_sequence,
                   final.checkpoint.global_sequence, 1, chunks.back()));
  bundle->chunks = std::move(chunks);
  bundle->manifest.chunks = std::move(references);
}

void RemoveContinuousEvents(GuestExecutionSessionBundle* bundle) {
  REQUIRE(bundle);
  REQUIRE(bundle->chunks.size() == 4);
  GuestExecutionSessionCheckpointChunk final;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(bundle->chunks[3],
                                                            &final, &error));
  final.ordinal = 2;
  std::vector<uint8_t> final_bytes;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(final, &final_bytes,
                                                            &error));
  bundle->chunks = {std::move(bundle->chunks[0]), std::move(bundle->chunks[1]),
                    std::move(final_bytes)};
  bundle->manifest.chunks = {
      bundle->manifest.chunks[0], bundle->manifest.chunks[1],
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2,
                   final.checkpoint.global_sequence,
                   final.checkpoint.global_sequence, 1, bundle->chunks[2])};
}

void ReplaceFinalThreadStateBlob(GuestExecutionSessionBundle* bundle,
                                 std::vector<uint8_t> replacement,
                                 bool update_binding = true) {
  REQUIRE(bundle);
  GuestExecutionSessionCheckpointChunk final;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(bundle->chunks[3],
                                                            &final, &error));
  REQUIRE(final.checkpoint.thread_states.size() == 1);
  GuestExecutionSessionThreadStateReference& state =
      final.checkpoint.thread_states.front();
  const GuestExecutionSessionSha256 old_digest = state.sha256;
  const GuestExecutionSessionSha256 replacement_digest =
      GuestExecutionSessionCodec::HashBytes(replacement);
  const std::optional<ppc::GuestPPCThreadCheckpointBinding>
      replacement_binding = DecodeCheckpointBinding(replacement);
  const auto old_blob =
      std::find_if(bundle->content_blobs.begin(), bundle->content_blobs.end(),
                   [&](const GuestExecutionSessionContentBlob& blob) {
                     return blob.sha256 == old_digest;
                   });
  REQUIRE(old_blob != bundle->content_blobs.end());
  old_blob->sha256 = replacement_digest;
  old_blob->bytes = std::move(replacement);
  state.byte_size = old_blob->bytes.size();
  state.sha256 = replacement_digest;
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final, &bundle->chunks[3], &error));
  REQUIRE(error.empty());
  bundle->manifest.chunks[3] =
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 3, 6, 6, 1,
                   bundle->chunks[3]);

  std::vector<GuestExecutionContinuousEvent> events =
      DecodeContinuousEvents(*bundle);
  events.back().checkpoint.state_size = state.byte_size;
  events.back().checkpoint.state_sha256 = replacement_digest;
  if (update_binding && replacement_binding) {
    events.back().checkpoint.binding = *replacement_binding;
  }
  ReplaceContinuousEvents(bundle, events);
}

GuestExecutionSessionBundle MakeBundle() {
  GuestExecutionSessionBundle bundle;
  const std::vector<uint8_t> initial_state_bytes =
      ThreadCheckpointBytes(0, 7, 0x82000040, 1);
  const std::vector<uint8_t> final_state_bytes =
      ThreadCheckpointBytes(0, 7, 0x82000044, 2);
  const GuestExecutionSessionSha256 initial_state =
      AddBlob(&bundle, initial_state_bytes);
  const GuestExecutionSessionSha256 final_state =
      AddBlob(&bundle, final_state_bytes);
  const GuestExecutionSessionSha256 page = AddBlob(&bundle, Bytes(4096, 3));
  const GuestExecutionSessionSha256 code = AddBlob(&bundle, Bytes(256, 4));
  const GuestExecutionSessionSha256 scalar = AddBlob(&bundle, Bytes(8, 5));
  const GuestExecutionSessionSha256 mutation = AddBlob(&bundle, Bytes(4, 6));
  const GuestExecutionSessionSha256 corpus = AddBlob(&bundle, Bytes(128, 7));
  const GuestExecutionSessionSha256 segment_blob =
      AddBlob(&bundle, Bytes(96, 8));

  constexpr uint64_t kEpoch = 0x123456789ABCDEF0ull;
  GuestExecutionSessionCheckpointChunk initial;
  initial.session_epoch = kEpoch;
  initial.ordinal = 0;
  initial.checkpoint.global_sequence = 0;
  initial.checkpoint.thread_states.push_back(
      {0, initial_state_bytes.size(), initial_state});
  initial.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestPage, 0x1000, 4096, page});
  initial.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestCode, 0x82000000, 256, code});

  GuestExecutionSessionEventChunk events;
  events.session_epoch = kEpoch;
  events.ordinal = 1;
  GuestExecutionSessionEvent begin;
  begin.global_sequence = 1;
  begin.thread_ordinal = 0;
  begin.kind = GuestExecutionSessionEventKind::kSegmentBegin;
  begin.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  events.events.push_back(begin);
  GuestExecutionSessionEvent external;
  external.global_sequence = 2;
  external.thread_ordinal = 0;
  external.kind = GuestExecutionSessionEventKind::kKernelExport;
  external.guest_address = 0x82000080;
  external.payload_kind =
      GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
  external.payload_size = 8;
  external.payload_sha256 = scalar;
  events.events.push_back(external);
  GuestExecutionSessionEvent write;
  write.global_sequence = 3;
  write.kind = GuestExecutionSessionEventKind::kMemoryMutation;
  write.mutation_source = GuestExecutionSessionMutationSource::kGpu;
  write.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
  write.guest_address = 0x2000;
  write.byte_count = 4;
  write.payload_size = 4;
  write.payload_sha256 = mutation;
  events.events.push_back(write);
  GuestExecutionSessionEvent end;
  end.global_sequence = 4;
  end.thread_ordinal = 0;
  end.kind = GuestExecutionSessionEventKind::kSegmentEnd;
  end.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  events.events.push_back(end);
  GuestExecutionSessionEvent request;
  request.global_sequence = 5;
  request.kind = GuestExecutionSessionEventKind::kBoundaryRequest;
  events.events.push_back(request);
  GuestExecutionSessionEvent held;
  held.global_sequence = 6;
  held.kind = GuestExecutionSessionEventKind::kBoundaryHeld;
  events.events.push_back(held);

  GuestExecutionSessionCheckpointChunk final;
  final.session_epoch = kEpoch;
  final.ordinal = 3;
  final.checkpoint.global_sequence = 6;
  final.checkpoint.thread_states.push_back(
      {0, final_state_bytes.size(), final_state});
  final.checkpoint.content = initial.checkpoint.content;

  std::string error;
  std::vector<GuestExecutionContinuousEvent> continuous_events;
  for (const GuestExecutionSessionEvent& event : events.events) {
    GuestExecutionContinuousEvent continuous;
    continuous.global_sequence = event.global_sequence;
    continuous.kind = event.kind;
    if (event.thread_ordinal != kGuestExecutionSessionNoThread) {
      continuous.actor = {event.thread_ordinal, 7};
    }
    continuous_events.push_back(continuous);
  }
  GuestExecutionContinuousEvent& held_continuous = continuous_events.back();
  held_continuous.subject = {0, 7};
  held_continuous.checkpoint.kind =
      GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
  held_continuous.checkpoint.checkpoint_global_sequence = 6;
  held_continuous.checkpoint.state_size = final_state_bytes.size();
  held_continuous.checkpoint.state_sha256 = final_state;
  const std::optional<ppc::GuestPPCThreadCheckpointBinding> final_binding =
      DecodeCheckpointBinding(final_state_bytes);
  REQUIRE(final_binding.has_value());
  held_continuous.checkpoint.binding = *final_binding;

  bundle.chunks.resize(4);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      events, &bundle.chunks[1], &error));
  REQUIRE(GuestExecutionContinuousEventCodec::Encode(
      continuous_events, &bundle.chunks[2], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final, &bundle.chunks[3], &error));

  GuestExecutionSessionManifest& manifest = bundle.manifest;
  manifest.session_epoch = kEpoch;
  manifest.boundary.kind = GuestExecutionSessionBoundaryKind::kSegmentCount;
  manifest.boundary.value = 1;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 6;
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 900;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = IdentityDigest(0x10);
  manifest.replay_config_sha256 = IdentityDigest(0x20);
  manifest.title_identity_sha256 = IdentityDigest(0x30);
  manifest.module_identity_sha256 = IdentityDigest(0x40);
  manifest.accepted_segment_count = 1;
  manifest.accepted_event_count = 6;
  manifest.stop_reason = GuestExecutionSessionStopReason::kRequestedBoundary;
  manifest.stop_request_event_sequence = 5;
  manifest.stop_request_tick = 800;
  manifest.stop_request_accepted_segment_count = 1;
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
  participant.last_event_sequence = 4;
  participant.held_after_event_sequence = 5;
  participant.initial_state_size = initial_state_bytes.size();
  participant.initial_state_sha256 = initial_state;
  manifest.participants.push_back(participant);
  manifest.segments.push_back(
      {0, 0, 1, 4, 0x82000000, 0x820000FC, corpus, segment_blob});
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   bundle.chunks[0]));
  manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 6, 6, bundle.chunks[1]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kContinuousEvents, 2, 1, 6,
                   6, bundle.chunks[2]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 3, 6, 6, 1,
                   bundle.chunks[3]));
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(manifest, bundle.chunks,
                                                      &error));

  std::reverse(bundle.content_blobs.begin(), bundle.content_blobs.end());
  return bundle;
}

GuestExecutionSessionBundle MakePendingExternBundle(
    uint32_t export_guest_address = 0x82000080) {
  GuestExecutionSessionBundle bundle = MakeBundle();
  ReplaceFinalThreadStateBlob(
      &bundle, ThreadCheckpointBytes(
                   0, 7, 0x82000044, 2,
                   ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern,
                   2, export_guest_address));
  std::vector<GuestExecutionContinuousEvent> events =
      DecodeContinuousEvents(bundle);
  events[1].subject = {0, 7};
  ReplaceContinuousEvents(&bundle, events);
  return bundle;
}

GuestExecutionSessionBundle MakeContinuousBundle(
    bool include_code = true, bool code_matches = true,
    uint32_t function_address = 0x82000000,
    uint32_t function_end_address = 0x820000FC, bool include_overlay = false) {
  GuestExecutionSessionBundle bundle;
  const std::vector<uint8_t> initial_state_bytes =
      ThreadCheckpointBytes(0, 7, 0x82000040, 1);
  const std::vector<uint8_t> final_state_bytes =
      ThreadCheckpointBytes(0, 7, 0x82000044, 2);
  const GuestExecutionSessionSha256 initial_state =
      AddBlob(&bundle, initial_state_bytes);
  const GuestExecutionSessionSha256 final_state =
      AddBlob(&bundle, final_state_bytes);
  const std::vector<uint8_t> corpus_page = Bytes(JitCorpus::kPageSize, 0x30);
  const std::vector<uint8_t> checkpoint_code =
      code_matches ? corpus_page : Bytes(JitCorpus::kPageSize, 0x31);
  GuestExecutionSessionSha256 code = {};
  if (include_code) {
    code = AddBlob(&bundle, checkpoint_code);
  }
  ExecutionJitCorpusBuilder builder(JitCorpus::kConfigGuestScheduler);
  ExecutionJitCorpus::FunctionRecord function = {function_address,
                                                 function_end_address, 64, 0};
  std::vector<uint8_t> corpus_bytes;
  std::string error;
  REQUIRE(builder.AddCodePage(0x82000000, corpus_page.data(),
                              corpus_page.size(), &error));
  REQUIRE(builder.AddFunction(function, &error));
  REQUIRE(builder.Encode(&corpus_bytes, &error));
  const GuestExecutionSessionSha256 corpus =
      AddBlob(&bundle, std::move(corpus_bytes));

  constexpr uint64_t kEpoch = 0x123456789ABCDEF0ull;
  GuestExecutionSessionCheckpointChunk initial;
  initial.session_epoch = kEpoch;
  initial.ordinal = 0;
  initial.checkpoint.thread_states.push_back(
      {0, initial_state_bytes.size(), initial_state});
  if (include_code) {
    initial.checkpoint.content.push_back(
        {GuestExecutionSessionContentKind::kGuestCode, 0x82000000,
         JitCorpus::kPageSize, code});
  }

  GuestExecutionSessionCodeCorpusChunk code_corpus;
  code_corpus.session_epoch = kEpoch;
  code_corpus.ordinal = 1;
  code_corpus.code_corpus_sha256 = corpus;

  GuestExecutionSessionEventChunk events;
  events.session_epoch = kEpoch;
  events.ordinal = 2;
  GuestExecutionSessionEvent begin;
  begin.global_sequence = 1;
  begin.thread_ordinal = 0;
  begin.kind = GuestExecutionSessionEventKind::kOuterHostCallBegin;
  begin.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  events.events.push_back(begin);
  GuestExecutionSessionEvent coverage = begin;
  coverage.global_sequence = 2;
  coverage.kind = GuestExecutionSessionEventKind::kInstructionCoverage;
  coverage.guest_instruction_delta = 10;
  events.events.push_back(coverage);
  GuestExecutionSessionEvent request;
  request.global_sequence = 3;
  request.kind = GuestExecutionSessionEventKind::kBoundaryRequest;
  events.events.push_back(request);
  GuestExecutionSessionEvent arrival = begin;
  arrival.global_sequence = 4;
  arrival.kind = GuestExecutionSessionEventKind::kJitSafepointArrival;
  events.events.push_back(arrival);
  GuestExecutionSessionEvent held;
  held.global_sequence = 5;
  held.kind = GuestExecutionSessionEventKind::kBoundaryHeld;
  events.events.push_back(held);

  GuestExecutionSessionCheckpointChunk final_checkpoint;
  final_checkpoint.session_epoch = kEpoch;
  final_checkpoint.ordinal = include_overlay ? 4 : 3;
  final_checkpoint.checkpoint.global_sequence = 5;
  final_checkpoint.checkpoint.thread_states.push_back(
      {0, final_state_bytes.size(), final_state});
  if (include_code) {
    final_checkpoint.checkpoint.content = initial.checkpoint.content;
  }

  std::vector<GuestExecutionContinuousEvent> continuous_events;
  if (include_overlay) {
    for (const GuestExecutionSessionEvent& event : events.events) {
      GuestExecutionContinuousEvent continuous;
      continuous.global_sequence = event.global_sequence;
      continuous.kind = event.kind;
      if (event.thread_ordinal != kGuestExecutionSessionNoThread) {
        continuous.actor = {event.thread_ordinal, 7};
      }
      continuous_events.push_back(continuous);
    }
    GuestExecutionContinuousEvent& held_continuous = continuous_events.back();
    held_continuous.subject = {0, 7};
    held_continuous.checkpoint.kind =
        GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
    held_continuous.checkpoint.checkpoint_global_sequence = 5;
    held_continuous.checkpoint.state_size = final_state_bytes.size();
    held_continuous.checkpoint.state_sha256 = final_state;
    const std::optional<ppc::GuestPPCThreadCheckpointBinding> final_binding =
        DecodeCheckpointBinding(final_state_bytes);
    REQUIRE(final_binding.has_value());
    held_continuous.checkpoint.binding = *final_binding;
  }

  bundle.chunks.resize(include_overlay ? 5 : 4);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCodeCorpusChunk(
      code_corpus, &bundle.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      events, &bundle.chunks[2], &error));
  if (include_overlay) {
    REQUIRE(GuestExecutionContinuousEventCodec::Encode(
        continuous_events, &bundle.chunks[3], &error));
  }
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final_checkpoint, &bundle.chunks.back(), &error));

  GuestExecutionSessionManifest& manifest = bundle.manifest;
  manifest.session_epoch = kEpoch;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 5;
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 500;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = IdentityDigest(0x10);
  manifest.replay_config_sha256 = IdentityDigest(0x20);
  manifest.title_identity_sha256 = IdentityDigest(0x30);
  manifest.module_identity_sha256 = IdentityDigest(0x40);
  manifest.accepted_event_count = 5;
  manifest.stop_reason = GuestExecutionSessionStopReason::kManualRequest;
  manifest.stop_request_event_sequence = 3;
  manifest.stop_request_tick = 300;
  manifest.stop_request_guest_instruction_count = 10;
  manifest.maximum_stop_tail_event_count = 16;
  manifest.maximum_stop_tail_guest_instruction_count = 64;
  manifest.maximum_stop_tail_ticks = 1000;
  GuestExecutionSessionParticipant participant;
  participant.guest_thread_id = 7;
  participant.capture_instance_id = 0x100;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint;
  participant.first_event_sequence = 1;
  participant.last_event_sequence = 4;
  participant.held_after_event_sequence = 4;
  participant.initial_state_size = initial_state_bytes.size();
  participant.initial_state_sha256 = initial_state;
  manifest.participants.push_back(participant);
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   bundle.chunks[0]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCodeCorpus, 1, 0, 0, 1,
                   bundle.chunks[1]));
  manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 2, 1, 5, 5, bundle.chunks[2]));
  if (include_overlay) {
    manifest.chunks.push_back(
        ReferenceFor(GuestExecutionSessionChunkKind::kContinuousEvents, 3, 1, 5,
                     5, bundle.chunks[3]));
  }
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint,
                   final_checkpoint.ordinal, 5, 5, 1, bundle.chunks.back()));
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(manifest, bundle.chunks,
                                                      &error));
  std::reverse(bundle.content_blobs.begin(), bundle.content_blobs.end());
  return bundle;
}

std::map<GuestExecutionSessionSha256, std::vector<uint8_t>> BlobMap(
    const GuestExecutionSessionBundle& bundle) {
  std::map<GuestExecutionSessionSha256, std::vector<uint8_t>> result;
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    REQUIRE(result.emplace(blob.sha256, blob.bytes).second);
  }
  return result;
}

std::string HexDigest(const GuestExecutionSessionSha256& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (uint8_t value : digest) {
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 0xF]);
  }
  return result;
}

std::filesystem::path BlobName(const GuestExecutionSessionSha256& digest) {
  return "blob-" + HexDigest(digest) + ".xegb";
}

std::filesystem::path ChunkName(
    const GuestExecutionSessionChunkReference& reference) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string ordinal(8, '0');
  for (size_t i = 0; i < ordinal.size(); ++i) {
    ordinal[i] =
        kHex[(reference.ordinal >> ((ordinal.size() - i - 1) * 4)) & 0xF];
  }
  std::string kind;
  switch (reference.kind) {
    case GuestExecutionSessionChunkKind::kEvents:
      kind = "events";
      break;
    case GuestExecutionSessionChunkKind::kCheckpoint:
      kind = "checkpoint";
      break;
    case GuestExecutionSessionChunkKind::kContinuousEvents:
      kind = "continuous";
      break;
    case GuestExecutionSessionChunkKind::kCodeCorpus:
      kind = "code-corpus";
      break;
    default:
      kind = "unknown";
      break;
  }
  return "chunk-" + ordinal + "-" + kind + "-" +
         HexDigest(reference.encoded_sha256) + ".xegc";
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  REQUIRE(stream.is_open());
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  REQUIRE(stream.good());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  WriteFile(path, std::vector<uint8_t>(text.cbegin(), text.cend()));
}

void WriteBundleFilesUnchecked(const std::filesystem::path& output,
                               const GuestExecutionSessionBundle& bundle) {
  REQUIRE(std::filesystem::create_directory(output));
  std::vector<uint8_t> manifest_bytes;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::EncodeManifest(bundle.manifest,
                                                     &manifest_bytes, &error));
  REQUIRE(error.empty());
  WriteFile(output / kGuestExecutionSessionBundleManifestFileName,
            manifest_bytes);
  REQUIRE(bundle.chunks.size() == bundle.manifest.chunks.size());
  for (size_t i = 0; i < bundle.chunks.size(); ++i) {
    WriteFile(output / ChunkName(bundle.manifest.chunks[i]), bundle.chunks[i]);
  }
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    WriteFile(output / BlobName(blob.sha256), blob.bytes);
  }
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream.is_open());
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

std::filesystem::path StagingPath(const std::filesystem::path& output) {
  std::filesystem::path staging = output;
  staging += ".part";
  return staging;
}

}  // namespace

TEST_CASE("session bundle publishes and reads one exact closed object graph",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle bundle = MakeBundle();
  const std::filesystem::path output = temporary_directory.path() / "capture";
  std::string error = "stale";

  REQUIRE(WriteGuestExecutionSessionBundle(output, bundle, &error));
  CHECK(error.empty());
  CHECK(std::filesystem::is_directory(output));
  CHECK_FALSE(std::filesystem::exists(StagingPath(output)));

  GuestExecutionSessionBundle decoded;
  REQUIRE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  CHECK(error.empty());
  CHECK(decoded.manifest == bundle.manifest);
  CHECK(decoded.chunks == bundle.chunks);
  CHECK(BlobMap(decoded) == BlobMap(bundle));
  CHECK(std::is_sorted(decoded.content_blobs.cbegin(),
                       decoded.content_blobs.cend(),
                       [](const GuestExecutionSessionContentBlob& left,
                          const GuestExecutionSessionContentBlob& right) {
                         return left.sha256 < right.sha256;
                       }));

  size_t file_count = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(output)) {
    CHECK(entry.is_regular_file());
    CHECK_FALSE(entry.is_symlink());
    ++file_count;
  }
  CHECK(file_count == 1 + bundle.chunks.size() + bundle.content_blobs.size());
}

TEST_CASE("session bundle accepts an ordered chunked continuous overlay",
          "[guest-execution-session-bundle]") {
  GuestExecutionSessionBundle bundle = MakeBundle();
  SplitContinuousEvents(&bundle, 3);
  std::string error;
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  CHECK(error.empty());
}

TEST_CASE("session bundle bounds aggregate decoded overlay state",
          "[guest-execution-session-bundle]") {
  GuestExecutionSessionBundle bundle = MakeBundle();
  SplitContinuousEvents(&bundle, 3);
  std::string error;

  GuestExecutionSessionBundleLimits limits;
  limits.session.maximum_total_events = 5;
  CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error, limits));
  CHECK_FALSE(error.empty());

  ScopedTestDirectory temporary_directory;
  const std::filesystem::path output = temporary_directory.path() / "limited";
  CHECK_FALSE(WriteGuestExecutionSessionBundle(output, bundle, &error, limits));
  CHECK_FALSE(std::filesystem::exists(output));
  CHECK_FALSE(std::filesystem::exists(StagingPath(output)));

  limits = {};
  limits.session.maximum_total_checkpoint_thread_states = 1;
  CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error, limits));
  CHECK_FALSE(error.empty());
}

TEST_CASE("session bundle preserves the legacy path without an overlay",
          "[guest-execution-session-bundle]") {
  GuestExecutionSessionBundle bundle = MakeBundle();
  REQUIRE(GuestExecutionSessionCodec::kVersion == 2);
  GuestExecutionSessionEventChunk events;
  std::string error;
  REQUIRE(GuestExecutionSessionCodec::DecodeEventChunk(bundle.chunks[1],
                                                       &events, &error));
  REQUIRE(events.events[1].kind ==
          GuestExecutionSessionEventKind::kKernelExport);
  events.events[1].guest_address = 0;
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      events, &bundle.chunks[1], &error));
  bundle.manifest.chunks[1] = ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1,
      events.events.front().global_sequence,
      events.events.back().global_sequence,
      static_cast<uint32_t>(events.events.size()), bundle.chunks[1]);
  RemoveContinuousEvents(&bundle);
  CHECK(std::none_of(
      bundle.manifest.chunks.cbegin(), bundle.manifest.chunks.cend(),
      [](const GuestExecutionSessionChunkReference& chunk) {
        return chunk.kind == GuestExecutionSessionChunkKind::kCodeCorpus ||
               chunk.kind == GuestExecutionSessionChunkKind::kContinuousEvents;
      }));
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
  CHECK(error.empty());
}

TEST_CASE("session bundle closes a pending extern route to its exact event",
          "[guest-execution-session-bundle]") {
  std::string error;

  SECTION("the exact sequence, target, disposition and roles validate") {
    GuestExecutionSessionBundle bundle = MakePendingExternBundle();
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK(error.empty());

    GuestExecutionSessionBundle changed_target =
        MakePendingExternBundle(0x82000084);
    CHECK_FALSE(ValidateGuestExecutionSessionBundle(changed_target, &error));
    CHECK_FALSE(error.empty());

    ReplaceFinalThreadStateBlob(
        &bundle,
        ThreadCheckpointBytes(
            0, 7, 0x82000044, 2,
            ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern, 1,
            0x82000080));
    CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK_FALSE(error.empty());
  }

  SECTION("the referenced event must retain its replay disposition") {
    GuestExecutionSessionBundle bundle = MakePendingExternBundle();
    std::vector<GuestExecutionSessionEvent> canonical =
        DecodeCanonicalEvents(bundle);
    canonical[1].disposition =
        GuestExecutionSessionEventDisposition::kValidateDeterministic;
    ReplaceCanonicalEvents(&bundle, std::move(canonical));
    CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK_FALSE(error.empty());
  }

  SECTION("the referenced event must retain both participant roles") {
    GuestExecutionSessionBundle bundle = MakePendingExternBundle();
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousEvents(bundle);
    events[1].actor = {};
    ReplaceContinuousEvents(&bundle, events);
    CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK_FALSE(error.empty());

    bundle = MakePendingExternBundle();
    events = DecodeContinuousEvents(bundle);
    events[1].subject = {};
    ReplaceContinuousEvents(&bundle, events);
    CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK_FALSE(error.empty());
  }

  SECTION("a pending route closes across split overlay chunks") {
    GuestExecutionSessionBundle bundle = MakePendingExternBundle();
    SplitContinuousEvents(&bundle, 1);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK(error.empty());

    std::vector<GuestExecutionContinuousEvent> second;
    REQUIRE(GuestExecutionContinuousEventCodec::Decode(bundle.chunks[3],
                                                       &second, &error));
    second.front().subject = {};
    REQUIRE(GuestExecutionContinuousEventCodec::Encode(
        second, &bundle.chunks[3], &error));
    bundle.manifest.chunks[3] = ReferenceFor(
        GuestExecutionSessionChunkKind::kContinuousEvents, 3,
        second.front().global_sequence, second.back().global_sequence,
        static_cast<uint32_t>(second.size()), bundle.chunks[3]);
    CHECK_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("continuous session bundle closes exact corpus code",
          "[guest-execution-session-bundle]") {
  std::string error;
  GuestExecutionSessionBundle bundle = MakeContinuousBundle();
  REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));

  SECTION("overlay checkpoint binds to the exact corpus function") {
    bundle = MakeContinuousBundle(true, true, 0x82000000, 0x820000FC, true);
    REQUIRE(bundle.manifest.segments.empty());
    REQUIRE(bundle.manifest.chunks.size() == 5);
    CHECK(bundle.manifest.chunks[0].kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
    CHECK(bundle.manifest.chunks[1].kind ==
          GuestExecutionSessionChunkKind::kCodeCorpus);
    CHECK(bundle.manifest.chunks[2].kind ==
          GuestExecutionSessionChunkKind::kEvents);
    CHECK(bundle.manifest.chunks[3].kind ==
          GuestExecutionSessionChunkKind::kContinuousEvents);
    CHECK(bundle.manifest.chunks[4].kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK(error.empty());
  }

  SECTION("page-covered route without its owning corpus function rejects") {
    bundle = MakeContinuousBundle(true, true, 0x82000100, 0x820001FC, true);
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                        bundle.chunks, &error));
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK(error.find("owning function is absent") != std::string::npos);
  }

  SECTION("owning corpus function extent mismatch rejects") {
    bundle = MakeContinuousBundle(true, true, 0x82000000, 0x820000F8, true);
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                        bundle.chunks, &error));
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK(error.find("owning function extent differs") != std::string::npos);
  }

  SECTION("corpus and split overlay retain canonical chunk order") {
    bundle = MakeContinuousBundle(true, true, 0x82000000, 0x820000FC, true);
    SplitContinuousEvents(&bundle, 3);
    REQUIRE(bundle.manifest.chunks.size() == 6);
    CHECK(bundle.manifest.chunks[0].kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
    CHECK(bundle.manifest.chunks[1].kind ==
          GuestExecutionSessionChunkKind::kCodeCorpus);
    CHECK(bundle.manifest.chunks[2].kind ==
          GuestExecutionSessionChunkKind::kEvents);
    CHECK(bundle.manifest.chunks[3].kind ==
          GuestExecutionSessionChunkKind::kContinuousEvents);
    CHECK(bundle.manifest.chunks[4].kind ==
          GuestExecutionSessionChunkKind::kContinuousEvents);
    CHECK(bundle.manifest.chunks[5].kind ==
          GuestExecutionSessionChunkKind::kCheckpoint);
    REQUIRE(ValidateGuestExecutionSessionBundle(bundle, &error));
    CHECK(error.empty());
  }

  SECTION("missing initial guest code rejects") {
    bundle = MakeContinuousBundle(false);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
  }

  SECTION("initial guest code differing from corpus rejects") {
    bundle = MakeContinuousBundle(true, false);
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
  }

  SECTION("final guest code differing without a model rejects") {
    const GuestExecutionSessionSha256 changed_code =
        AddBlob(&bundle, Bytes(JitCorpus::kPageSize, 0x50));
    GuestExecutionSessionCheckpointChunk final_checkpoint;
    REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
        bundle.chunks.back(), &final_checkpoint, &error));
    REQUIRE(final_checkpoint.checkpoint.content.size() == 1);
    final_checkpoint.checkpoint.content[0].sha256 = changed_code;
    REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
        final_checkpoint, &bundle.chunks.back(), &error));
    bundle.manifest.chunks.back() = ReferenceFor(
        GuestExecutionSessionChunkKind::kCheckpoint, final_checkpoint.ordinal,
        final_checkpoint.checkpoint.global_sequence,
        final_checkpoint.checkpoint.global_sequence, 1, bundle.chunks.back());
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                        bundle.chunks, &error));
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
  }

  SECTION("final code page cannot be relabeled to bypass closure") {
    const GuestExecutionSessionSha256 changed_code =
        AddBlob(&bundle, Bytes(JitCorpus::kPageSize, 0x50));
    GuestExecutionSessionCheckpointChunk final_checkpoint;
    REQUIRE(GuestExecutionSessionCodec::DecodeCheckpointChunk(
        bundle.chunks.back(), &final_checkpoint, &error));
    REQUIRE(final_checkpoint.checkpoint.content.size() == 1);
    final_checkpoint.checkpoint.content[0].kind =
        GuestExecutionSessionContentKind::kGuestPage;
    final_checkpoint.checkpoint.content[0].sha256 = changed_code;
    REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
        final_checkpoint, &bundle.chunks.back(), &error));
    bundle.manifest.chunks.back() = ReferenceFor(
        GuestExecutionSessionChunkKind::kCheckpoint, final_checkpoint.ordinal,
        final_checkpoint.checkpoint.global_sequence,
        final_checkpoint.checkpoint.global_sequence, 1, bundle.chunks.back());
    REQUIRE(GuestExecutionSessionCodec::ValidateSession(bundle.manifest,
                                                        bundle.chunks, &error));
    REQUIRE_FALSE(ValidateGuestExecutionSessionBundle(bundle, &error));
  }
}

TEST_CASE("session bundle rejects an invalid graph before creating staging",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle valid = MakeBundle();
  std::string error;

  auto RejectWithoutStaging =
      [&](std::string_view name, const GuestExecutionSessionBundle& bundle,
          GuestExecutionSessionBundleLimits limits = {}) {
        const std::filesystem::path output =
            temporary_directory.path() / std::string(name);
        CHECK_FALSE(
            WriteGuestExecutionSessionBundle(output, bundle, &error, limits));
        CHECK_FALSE(std::filesystem::exists(output));
        CHECK_FALSE(std::filesystem::exists(StagingPath(output)));
      };

  GuestExecutionSessionBundle invalid = valid;
  invalid.content_blobs.pop_back();
  RejectWithoutStaging("missing-blob", invalid);

  invalid = valid;
  AddBlob(&invalid, Bytes(31, 0xA0));
  RejectWithoutStaging("extra-blob", invalid);

  invalid = valid;
  invalid.content_blobs.push_back(invalid.content_blobs.front());
  RejectWithoutStaging("duplicate-blob", invalid);

  invalid = valid;
  invalid.content_blobs.front().bytes.front() ^= 1;
  RejectWithoutStaging("substituted-blob", invalid);

  invalid = valid;
  invalid.chunks[1][GuestExecutionSessionCodec::kEnvelopeHeaderSize] ^= 1;
  RejectWithoutStaging("substituted-chunk", invalid);

  invalid = valid;
  {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousEvents(invalid);
    events.front().kind = GuestExecutionSessionEventKind::kSynchronization;
    ReplaceContinuousEvents(&invalid, events);
  }
  RejectWithoutStaging("continuous-kind-divergence", invalid);

  invalid = valid;
  {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousEvents(invalid);
    events.front().actor = {};
    events.front().subject = {0, 7};
    ReplaceContinuousEvents(&invalid, events);
  }
  RejectWithoutStaging("continuous-owner-role-swap", invalid);

  invalid = valid;
  {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousEvents(invalid);
    events[4].actor = {0, 7};
    ReplaceContinuousEvents(&invalid, events);
  }
  RejectWithoutStaging("continuous-no-thread-actor", invalid);

  invalid = valid;
  {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousEvents(invalid);
    events.back().checkpoint.state_sha256 = IdentityDigest(0xE0);
    ReplaceContinuousEvents(&invalid, events);
  }
  RejectWithoutStaging("continuous-checkpoint-not-closed", invalid);

  invalid = valid;
  ReplaceFinalThreadStateBlob(
      &invalid, ThreadCheckpointBytes(1, 7, 0x82000044, 2), false);
  RejectWithoutStaging("continuous-checkpoint-subject-mismatch", invalid);

  invalid = valid;
  {
    std::vector<GuestExecutionContinuousEvent> events =
        DecodeContinuousEvents(invalid);
    ppc::GuestPPCThreadCheckpointBinding& binding =
        events.back().checkpoint.binding;
    binding.resume_pc = 0x83000040;
    binding.owning_function_address = 0x83000000;
    binding.owning_function_end_address = 0x830000FC;
    ReplaceContinuousEvents(&invalid, events);
  }
  RejectWithoutStaging("continuous-checkpoint-route-not-captured", invalid);

  invalid = valid;
  {
    std::vector<uint8_t> malformed = ThreadCheckpointBytes(0, 7, 0x82000044, 2);
    malformed.front() ^= 1;
    ReplaceFinalThreadStateBlob(&invalid, std::move(malformed));
  }
  RejectWithoutStaging("continuous-checkpoint-malformed", invalid);

  invalid = valid;
  SplitContinuousEvents(&invalid, 3);
  {
    std::vector<GuestExecutionContinuousEvent> second;
    REQUIRE(
        GuestExecutionContinuousEventCodec::Decode(invalid.chunks[3], &second));
    for (GuestExecutionContinuousEvent& event : second) {
      ++event.global_sequence;
    }
    std::string encode_error;
    REQUIRE(GuestExecutionContinuousEventCodec::Encode(
        second, &invalid.chunks[3], &encode_error));
    invalid.manifest.chunks[3] = ReferenceFor(
        GuestExecutionSessionChunkKind::kContinuousEvents, 3,
        second.front().global_sequence, second.back().global_sequence,
        static_cast<uint32_t>(second.size()), invalid.chunks[3]);
  }
  RejectWithoutStaging("continuous-overlay-gap", invalid);

  GuestExecutionSessionBundleLimits limits;
  limits.maximum_content_blobs =
      static_cast<uint32_t>(valid.content_blobs.size() - 1);
  RejectWithoutStaging("blob-count-limit", valid, limits);

  limits = {};
  limits.maximum_total_content_bytes = 1;
  RejectWithoutStaging("blob-byte-limit", valid, limits);

  limits = {};
  limits.maximum_bundle_bytes = 1;
  RejectWithoutStaging("bundle-byte-limit", valid, limits);
}

TEST_CASE("session bundle preserves existing roots and rejects unsafe paths",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle bundle = MakeBundle();
  std::string error;

  const std::filesystem::path existing = temporary_directory.path() / "exists";
  REQUIRE(std::filesystem::create_directory(existing));
  WriteText(existing / "sentinel", "keep-output");
  CHECK_FALSE(WriteGuestExecutionSessionBundle(existing, bundle, &error));
  CHECK(ReadText(existing / "sentinel") == "keep-output");
  CHECK_FALSE(std::filesystem::exists(StagingPath(existing)));

  const std::filesystem::path output =
      temporary_directory.path() / "staging-exists";
  const std::filesystem::path staging = StagingPath(output);
  REQUIRE(std::filesystem::create_directory(staging));
  WriteText(staging / "sentinel", "keep-staging");
  CHECK_FALSE(WriteGuestExecutionSessionBundle(output, bundle, &error));
  CHECK_FALSE(std::filesystem::exists(output));
  CHECK(ReadText(staging / "sentinel") == "keep-staging");

  const std::filesystem::path traversal =
      temporary_directory.path() / ".." /
      (temporary_directory.path().filename().string() + "-escaped");
  CHECK_FALSE(WriteGuestExecutionSessionBundle(traversal, bundle, &error));
  CHECK_FALSE(std::filesystem::exists(traversal));

  const std::filesystem::path staging_name =
      temporary_directory.path() / "not-a-final.part";
  CHECK_FALSE(WriteGuestExecutionSessionBundle(staging_name, bundle, &error));
  CHECK_FALSE(std::filesystem::exists(staging_name));

  const std::filesystem::path missing_target =
      temporary_directory.path() / "missing-target";
  const std::filesystem::path linked_output =
      temporary_directory.path() / "linked-output";
  std::error_code filesystem_error;
  std::filesystem::create_symlink(missing_target, linked_output,
                                  filesystem_error);
  REQUIRE_FALSE(filesystem_error);
  CHECK_FALSE(WriteGuestExecutionSessionBundle(linked_output, bundle, &error));
  CHECK(std::filesystem::is_symlink(linked_output));

  const std::filesystem::path output_with_linked_staging =
      temporary_directory.path() / "linked-staging";
  const std::filesystem::path linked_staging =
      StagingPath(output_with_linked_staging);
  std::filesystem::create_symlink(missing_target, linked_staging,
                                  filesystem_error);
  REQUIRE_FALSE(filesystem_error);
  CHECK_FALSE(WriteGuestExecutionSessionBundle(output_with_linked_staging,
                                               bundle, &error));
  CHECK_FALSE(std::filesystem::exists(output_with_linked_staging));
  CHECK(std::filesystem::is_symlink(linked_staging));
}

TEST_CASE("session bundle reader fails closed on filesystem substitution",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle bundle = MakeBundle();
  const std::filesystem::path output = temporary_directory.path() / "capture";
  std::string error;
  REQUIRE(WriteGuestExecutionSessionBundle(output, bundle, &error));

  GuestExecutionSessionBundle decoded = bundle;
  SECTION("content bytes changed") {
    const GuestExecutionSessionContentBlob& blob = bundle.content_blobs.front();
    std::vector<uint8_t> changed = blob.bytes;
    changed.front() ^= 1;
    WriteFile(output / BlobName(blob.sha256), changed);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("chunk is missing") {
    REQUIRE(
        std::filesystem::remove(output / ChunkName(bundle.manifest.chunks[1])));
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("chunk bytes changed") {
    std::vector<uint8_t> changed = bundle.chunks[1];
    changed[GuestExecutionSessionCodec::kEnvelopeHeaderSize] ^= 1;
    WriteFile(output / ChunkName(bundle.manifest.chunks[1]), changed);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("manifest closure changed") {
    const std::filesystem::path path =
        output / kGuestExecutionSessionBundleManifestFileName;
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(bytes.empty());
    bytes.back() ^= 1;
    WriteFile(path, bytes);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("extra regular file") {
    WriteText(output / "extra", "unexpected");
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("extra hard-link alias") {
    std::error_code filesystem_error;
    std::filesystem::create_hard_link(
        output / kGuestExecutionSessionBundleManifestFileName,
        output / "manifest-alias", filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("symlink entry") {
    std::error_code filesystem_error;
    std::filesystem::create_symlink(
        output / kGuestExecutionSessionBundleManifestFileName,
        output / "manifest-link", filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("non-regular entry") {
    REQUIRE(std::filesystem::create_directory(output / "nested"));
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("symlink root") {
    const std::filesystem::path alias = temporary_directory.path() / "alias";
    std::error_code filesystem_error;
    std::filesystem::create_directory_symlink(output, alias, filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(alias, &decoded, &error));
  }
  SECTION("renamed object") {
    const std::filesystem::path blob =
        output / BlobName(bundle.content_blobs.front().sha256);
    std::error_code filesystem_error;
    std::filesystem::rename(blob, output / "renamed-blob", filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("published root presented as staging") {
    const std::filesystem::path partial = StagingPath(output);
    REQUIRE_NOTHROW(std::filesystem::rename(output, partial));
    CHECK_FALSE(ReadGuestExecutionSessionBundle(partial, &decoded, &error));
  }
  SECTION("path traversal alias") {
    const std::filesystem::path traversed = output / ".." / output.filename();
    CHECK_FALSE(ReadGuestExecutionSessionBundle(traversed, &decoded, &error));
  }
  SECTION("content size limit") {
    GuestExecutionSessionBundleLimits limits;
    limits.session.maximum_content_blob_bytes = 4095;
    CHECK_FALSE(
        ReadGuestExecutionSessionBundle(output, &decoded, &error, limits));
  }
  SECTION("bundle byte limit") {
    GuestExecutionSessionBundleLimits limits;
    limits.maximum_bundle_bytes = 1;
    CHECK_FALSE(
        ReadGuestExecutionSessionBundle(output, &decoded, &error, limits));
  }
  SECTION("directory entry limit") {
    GuestExecutionSessionBundleLimits limits;
    limits.maximum_content_blobs = 1;
    CHECK_FALSE(
        ReadGuestExecutionSessionBundle(output, &decoded, &error, limits));
  }
  CHECK(decoded == GuestExecutionSessionBundle{});
}

TEST_CASE("session bundle reader validates checkpoint blobs after loading",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  GuestExecutionSessionBundle bundle = MakeBundle();
  std::vector<GuestExecutionContinuousEvent> events =
      DecodeContinuousEvents(bundle);
  events.back().checkpoint.binding.resume_pc += 4;
  ReplaceContinuousEvents(&bundle, events);
  const std::filesystem::path output = temporary_directory.path() / "capture";
  WriteBundleFilesUnchecked(output, bundle);

  GuestExecutionSessionBundle decoded = MakeBundle();
  std::string error;
  CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  CHECK_FALSE(error.empty());
  CHECK(decoded == GuestExecutionSessionBundle{});
}

TEST_CASE("session bundle no-replace publication cleans racing staging",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  GuestExecutionSessionBundle bundle = MakeBundle();
  // Make two size-unconstrained segment objects large enough that the racing
  // creator deterministically observes staging before final publication.
  for (GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    if (blob.sha256 == bundle.manifest.segments[0].code_corpus_sha256) {
      blob.bytes = Bytes(2 * 1024 * 1024, 0x70);
      blob.sha256 = GuestExecutionSessionCodec::HashBytes(blob.bytes);
      bundle.manifest.segments[0].code_corpus_sha256 = blob.sha256;
    } else if (blob.sha256 == bundle.manifest.segments[0].segment_sha256) {
      blob.bytes = Bytes(2 * 1024 * 1024, 0x80);
      blob.sha256 = GuestExecutionSessionCodec::HashBytes(blob.bytes);
      bundle.manifest.segments[0].segment_sha256 = blob.sha256;
    }
  }

  const std::filesystem::path output = temporary_directory.path() / "race";
  const std::filesystem::path staging = StagingPath(output);
  const std::filesystem::path sentinel = output / "sentinel";
  std::atomic<bool> writer_finished = false;
  std::atomic<bool> competitor_created = false;
  std::thread competitor([&]() {
    while (!writer_finished.load(std::memory_order_acquire)) {
      std::error_code filesystem_error;
      if (std::filesystem::exists(staging, filesystem_error) &&
          !filesystem_error &&
          std::filesystem::create_directory(output, filesystem_error)) {
        std::ofstream stream(sentinel, std::ios::binary);
        stream << "keep-racing-output";
        competitor_created.store(stream.good(), std::memory_order_release);
        return;
      }
      std::this_thread::yield();
    }
  });
  std::string error;
  const bool succeeded =
      WriteGuestExecutionSessionBundle(output, bundle, &error);
  writer_finished.store(true, std::memory_order_release);
  competitor.join();

  REQUIRE(competitor_created.load(std::memory_order_acquire));
  CHECK_FALSE(succeeded);
  CHECK(ReadText(sentinel) == "keep-racing-output");
  CHECK_FALSE(std::filesystem::exists(staging));
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
