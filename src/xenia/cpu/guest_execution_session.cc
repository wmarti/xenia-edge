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
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "third_party/crypto/sha256.h"

namespace xe {
namespace cpu {

namespace {

constexpr std::array<uint8_t, 8> kEnvelopeMagic = {'X', 'E', 'G', 'S',
                                                   'E', 'S', 'S', 0};
constexpr std::array<uint8_t, 8> kClosureMagic = {'X', 'E', 'G', 'C',
                                                  'L', 'O', 'S', 'E'};
constexpr uint32_t kManifestEnvelopeKind = 1;
constexpr uint32_t kManifestOrdinal = UINT32_MAX;
constexpr uint32_t kKnownEnvelopeFlags = 0;
constexpr uint64_t kGuestAddressSpaceSize = uint64_t{1} << 32;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t* result) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t* result) {
  if (left && right > std::numeric_limits<uint64_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

bool IsNonzeroHash(const GuestExecutionSessionSha256& hash) {
  return std::any_of(hash.cbegin(), hash.cend(),
                     [](uint8_t value) { return value != 0; });
}

bool IsZeroHash(const GuestExecutionSessionSha256& hash) {
  return !IsNonzeroHash(hash);
}

class Writer {
 public:
  explicit Writer(size_t capacity) { data_.reserve(capacity); }

  void WriteU32(uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) {
      data_.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
  }

  void WriteU64(uint64_t value) {
    for (uint32_t i = 0; i < 8; ++i) {
      data_.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
  }

  void WriteBytes(const uint8_t* data, size_t size) {
    data_.insert(data_.end(), data, data + size);
  }

  std::vector<uint8_t> TakeData() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const { return size_ - offset_; }

  bool ReadU32(uint32_t* value) {
    uint8_t bytes[4];
    if (!ReadBytes(bytes, sizeof(bytes))) {
      return false;
    }
    *value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
             (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    return true;
  }

  bool ReadU64(uint64_t* value) {
    uint8_t bytes[8];
    if (!ReadBytes(bytes, sizeof(bytes))) {
      return false;
    }
    *value = uint64_t(bytes[0]) | (uint64_t(bytes[1]) << 8) |
             (uint64_t(bytes[2]) << 16) | (uint64_t(bytes[3]) << 24) |
             (uint64_t(bytes[4]) << 32) | (uint64_t(bytes[5]) << 40) |
             (uint64_t(bytes[6]) << 48) | (uint64_t(bytes[7]) << 56);
    return true;
  }

  bool ReadBytes(uint8_t* output, size_t size) {
    if (size > remaining()) {
      return false;
    }
    std::memcpy(output, data_ + offset_, size);
    offset_ += size;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

struct EnvelopeMetadata {
  uint32_t kind = 0;
  uint64_t session_epoch = 0;
  uint32_t ordinal = 0;
  uint32_t record_count = 0;
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
};

struct DecodedEnvelope {
  EnvelopeMetadata metadata;
  const uint8_t* payload = nullptr;
  size_t payload_size = 0;
};

void WriteEnvelopeMetadata(Writer* writer, const EnvelopeMetadata& metadata,
                           uint64_t payload_size,
                           const GuestExecutionSessionSha256& payload_hash,
                           bool closure) {
  const auto& magic = closure ? kClosureMagic : kEnvelopeMagic;
  writer->WriteBytes(magic.data(), magic.size());
  writer->WriteU32(GuestExecutionSessionCodec::kVersion);
  writer->WriteU32(closure ? GuestExecutionSessionCodec::kEnvelopeFooterSize
                           : GuestExecutionSessionCodec::kEnvelopeHeaderSize);
  if (closure) {
    writer->WriteU32(metadata.kind);
    writer->WriteU32(metadata.ordinal);
    writer->WriteU64(metadata.session_epoch);
    writer->WriteU32(metadata.record_count);
    writer->WriteU32(0);
    writer->WriteU64(metadata.first_event_sequence);
    writer->WriteU64(metadata.last_event_sequence);
    writer->WriteU64(payload_size);
  } else {
    writer->WriteU32(kKnownEnvelopeFlags);
    writer->WriteU32(metadata.kind);
    writer->WriteU64(metadata.session_epoch);
    writer->WriteU32(metadata.ordinal);
    writer->WriteU32(metadata.record_count);
    writer->WriteU64(metadata.first_event_sequence);
    writer->WriteU64(metadata.last_event_sequence);
    writer->WriteU64(payload_size);
  }
  writer->WriteBytes(payload_hash.data(), payload_hash.size());
}

bool EncodeEnvelope(const EnvelopeMetadata& metadata,
                    const std::vector<uint8_t>& payload, uint64_t maximum_size,
                    std::vector<uint8_t>* output, std::string* error) {
  uint64_t encoded_size = 0;
  if (!CheckedAdd(GuestExecutionSessionCodec::kEnvelopeHeaderSize,
                  payload.size(), &encoded_size) ||
      !CheckedAdd(encoded_size, GuestExecutionSessionCodec::kEnvelopeFooterSize,
                  &encoded_size)) {
    return Fail(error, "encoded envelope size overflows");
  }
  if (encoded_size > maximum_size ||
      encoded_size > std::numeric_limits<size_t>::max()) {
    return Fail(error, "encoded envelope exceeds the byte limit");
  }

  const GuestExecutionSessionSha256 payload_hash =
      GuestExecutionSessionCodec::HashBytes(payload);
  Writer writer(static_cast<size_t>(encoded_size));
  WriteEnvelopeMetadata(&writer, metadata, payload.size(), payload_hash, false);
  writer.WriteBytes(payload.data(), payload.size());
  WriteEnvelopeMetadata(&writer, metadata, payload.size(), payload_hash, true);
  *output = writer.TakeData();
  return true;
}

bool DecodeEnvelope(const uint8_t* data, size_t data_size,
                    uint32_t expected_kind, uint64_t maximum_size,
                    DecodedEnvelope* output, std::string* error) {
  if (!data && data_size) {
    return Fail(error, "encoded envelope data is null");
  }
  if (data_size > maximum_size) {
    return Fail(error, "encoded envelope exceeds the byte limit");
  }
  if (data_size < GuestExecutionSessionCodec::kEnvelopeHeaderSize +
                      GuestExecutionSessionCodec::kEnvelopeFooterSize) {
    return Fail(error, "encoded envelope is truncated");
  }

  Reader header_reader(data, GuestExecutionSessionCodec::kEnvelopeHeaderSize);
  std::array<uint8_t, 8> magic = {};
  uint32_t version = 0;
  uint32_t header_size = 0;
  uint32_t flags = 0;
  uint64_t payload_size = 0;
  GuestExecutionSessionSha256 payload_hash = {};
  EnvelopeMetadata metadata;
  if (!header_reader.ReadBytes(magic.data(), magic.size()) ||
      !header_reader.ReadU32(&version) ||
      !header_reader.ReadU32(&header_size) || !header_reader.ReadU32(&flags) ||
      !header_reader.ReadU32(&metadata.kind) ||
      !header_reader.ReadU64(&metadata.session_epoch) ||
      !header_reader.ReadU32(&metadata.ordinal) ||
      !header_reader.ReadU32(&metadata.record_count) ||
      !header_reader.ReadU64(&metadata.first_event_sequence) ||
      !header_reader.ReadU64(&metadata.last_event_sequence) ||
      !header_reader.ReadU64(&payload_size) ||
      !header_reader.ReadBytes(payload_hash.data(), payload_hash.size())) {
    return Fail(error, "encoded envelope header is truncated");
  }
  if (magic != kEnvelopeMagic ||
      version != GuestExecutionSessionCodec::kVersion ||
      header_size != GuestExecutionSessionCodec::kEnvelopeHeaderSize ||
      flags != kKnownEnvelopeFlags || metadata.kind != expected_kind) {
    return Fail(error, "encoded envelope header is unsupported");
  }

  uint64_t expected_size = 0;
  if (!CheckedAdd(GuestExecutionSessionCodec::kEnvelopeHeaderSize, payload_size,
                  &expected_size) ||
      !CheckedAdd(expected_size,
                  GuestExecutionSessionCodec::kEnvelopeFooterSize,
                  &expected_size) ||
      expected_size != data_size) {
    return Fail(error,
                "encoded envelope byte count is inconsistent or overflows");
  }

  const uint8_t* payload =
      data + GuestExecutionSessionCodec::kEnvelopeHeaderSize;
  const uint8_t* footer = payload + payload_size;
  Reader footer_reader(footer, GuestExecutionSessionCodec::kEnvelopeFooterSize);
  std::array<uint8_t, 8> closure_magic = {};
  uint32_t closure_version = 0;
  uint32_t closure_size = 0;
  uint32_t closure_kind = 0;
  uint32_t closure_ordinal = 0;
  uint64_t closure_epoch = 0;
  uint32_t closure_record_count = 0;
  uint32_t closure_reserved = 0;
  uint64_t closure_first_sequence = 0;
  uint64_t closure_last_sequence = 0;
  uint64_t closure_payload_size = 0;
  GuestExecutionSessionSha256 closure_payload_hash = {};
  if (!footer_reader.ReadBytes(closure_magic.data(), closure_magic.size()) ||
      !footer_reader.ReadU32(&closure_version) ||
      !footer_reader.ReadU32(&closure_size) ||
      !footer_reader.ReadU32(&closure_kind) ||
      !footer_reader.ReadU32(&closure_ordinal) ||
      !footer_reader.ReadU64(&closure_epoch) ||
      !footer_reader.ReadU32(&closure_record_count) ||
      !footer_reader.ReadU32(&closure_reserved) ||
      !footer_reader.ReadU64(&closure_first_sequence) ||
      !footer_reader.ReadU64(&closure_last_sequence) ||
      !footer_reader.ReadU64(&closure_payload_size) ||
      !footer_reader.ReadBytes(closure_payload_hash.data(),
                               closure_payload_hash.size())) {
    return Fail(error, "encoded envelope closure is truncated");
  }
  if (closure_magic != kClosureMagic ||
      closure_version != GuestExecutionSessionCodec::kVersion ||
      closure_size != GuestExecutionSessionCodec::kEnvelopeFooterSize ||
      closure_reserved || closure_kind != metadata.kind ||
      closure_ordinal != metadata.ordinal ||
      closure_epoch != metadata.session_epoch ||
      closure_record_count != metadata.record_count ||
      closure_first_sequence != metadata.first_event_sequence ||
      closure_last_sequence != metadata.last_event_sequence ||
      closure_payload_size != payload_size ||
      closure_payload_hash != payload_hash) {
    return Fail(error, "encoded envelope closure does not match its header");
  }
  if (GuestExecutionSessionCodec::HashBytes(payload, payload_size) !=
      payload_hash) {
    return Fail(error, "encoded envelope payload SHA-256 does not match");
  }

  output->metadata = metadata;
  output->payload = payload;
  output->payload_size = static_cast<size_t>(payload_size);
  return true;
}

bool ValidateBoundaryPolicy(const GuestExecutionSessionBoundaryPolicy& policy,
                            std::string* error) {
  switch (policy.kind) {
    case GuestExecutionSessionBoundaryKind::kManual:
      if (policy.value || policy.marker_identity ||
          policy.marker_source != GuestExecutionSessionMarkerSource::kNone) {
        return Fail(
            error,
            "manual session boundary has a value or guest-marker identity");
      }
      return true;
    case GuestExecutionSessionBoundaryKind::kSegmentCount:
    case GuestExecutionSessionBoundaryKind::kGuestInstructionCount:
    case GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds:
      if (!policy.value || policy.marker_identity ||
          policy.marker_source != GuestExecutionSessionMarkerSource::kNone) {
        return Fail(error,
                    "non-marker boundary value or marker fields are invalid");
      }
      return true;
    case GuestExecutionSessionBoundaryKind::kGuestMarkerCount:
      if (!policy.value || !policy.marker_identity ||
          policy.marker_source == GuestExecutionSessionMarkerSource::kNone ||
          static_cast<uint32_t>(policy.marker_source) >
              static_cast<uint32_t>(
                  GuestExecutionSessionMarkerSource::kOtherInstrumented)) {
        return Fail(error, "guest-marker boundary identity is invalid");
      }
      return true;
    default:
      return Fail(error, "session boundary kind is unknown");
  }
}

bool IsKnownChunkKind(GuestExecutionSessionChunkKind kind) {
  switch (kind) {
    case GuestExecutionSessionChunkKind::kEvents:
    case GuestExecutionSessionChunkKind::kCheckpoint:
      return true;
    default:
      return false;
  }
}

bool ValidateGuestRange(uint64_t guest_address, uint64_t byte_count,
                        std::string_view name, std::string* error) {
  uint64_t range_end = 0;
  if (!byte_count || guest_address >= kGuestAddressSpaceSize ||
      !CheckedAdd(guest_address, byte_count, &range_end) ||
      range_end > kGuestAddressSpaceSize) {
    return Fail(error, std::string(name) + " has an invalid guest range");
  }
  return true;
}

bool ValidateManifest(const GuestExecutionSessionManifest& manifest,
                      const GuestExecutionSessionLimits& limits,
                      uint64_t* encoded_size, uint64_t* total_chunk_bytes,
                      std::string* error) {
  if (!manifest.session_epoch) {
    return Fail(error, "session epoch is zero");
  }
  if (!ValidateBoundaryPolicy(manifest.boundary, error)) {
    return false;
  }
  if (manifest.first_event_sequence != 1 ||
      manifest.last_event_sequence < manifest.first_event_sequence) {
    return Fail(error, "manifest event sequence range is invalid");
  }
  if (!manifest.capture_tick_frequency ||
      manifest.capture_end_tick < manifest.capture_start_tick) {
    return Fail(error, "manifest actual capture timing is invalid");
  }
  if (!IsNonzeroHash(manifest.capture_build_sha256) ||
      !IsNonzeroHash(manifest.replay_config_sha256) ||
      !IsNonzeroHash(manifest.title_identity_sha256) ||
      !IsNonzeroHash(manifest.module_identity_sha256)) {
    return Fail(error, "manifest build/config/title/module SHA-256 is missing");
  }
  uint64_t total_event_count = 0;
  uint64_t total_segment_count = 0;
  if (manifest.accepted_segment_count != manifest.segments.size() ||
      !CheckedAdd(manifest.accepted_segment_count,
                  manifest.rejected_segment_count, &total_segment_count) ||
      total_segment_count > limits.maximum_segments ||
      !CheckedAdd(manifest.accepted_event_count, manifest.rejected_event_count,
                  &total_event_count) ||
      total_event_count != manifest.last_event_sequence ||
      manifest.unsupported_event_count > manifest.rejected_event_count) {
    return Fail(error, "manifest coverage accounting is inconsistent");
  }
  if (manifest.participants.empty() ||
      manifest.participants.size() > limits.maximum_participants) {
    return Fail(error, "manifest participant count is invalid");
  }
  if (manifest.segments.empty() ||
      manifest.segments.size() > limits.maximum_segments) {
    return Fail(error, "manifest segment count is invalid");
  }
  if (manifest.chunks.empty() ||
      manifest.chunks.size() > limits.maximum_chunks) {
    return Fail(error, "manifest chunk count is invalid");
  }

  std::set<uint32_t> guest_thread_ids;
  for (size_t i = 0; i < manifest.participants.size(); ++i) {
    const GuestExecutionSessionParticipant& participant =
        manifest.participants[i];
    if (participant.ordinal != i || !participant.guest_thread_id ||
        !guest_thread_ids.insert(participant.guest_thread_id).second) {
      return Fail(error,
                  "manifest participants are not dense and uniquely named");
    }
    if (participant.first_event_sequence < manifest.first_event_sequence ||
        participant.last_event_sequence < participant.first_event_sequence ||
        participant.last_event_sequence > manifest.last_event_sequence) {
      return Fail(error, "manifest participant event range is invalid");
    }
    if (!participant.initial_state_size ||
        participant.initial_state_size > limits.maximum_content_blob_bytes ||
        !IsNonzeroHash(participant.initial_state_sha256)) {
      return Fail(error, "manifest participant initial state is invalid");
    }
  }

  uint64_t previous_segment_start = 0;
  std::set<uint64_t> segment_end_sequences;
  for (size_t i = 0; i < manifest.segments.size(); ++i) {
    const GuestExecutionSessionSegmentReference& segment = manifest.segments[i];
    if (segment.ordinal != i ||
        segment.thread_ordinal >= manifest.participants.size()) {
      return Fail(error, "manifest segment ordinal or thread is invalid");
    }
    if (segment.first_event_sequence < manifest.first_event_sequence ||
        segment.first_event_sequence <= previous_segment_start ||
        segment.last_event_sequence < segment.first_event_sequence ||
        segment.last_event_sequence > manifest.last_event_sequence) {
      return Fail(error,
                  "manifest segment event range is invalid or unordered");
    }
    const GuestExecutionSessionParticipant& participant =
        manifest.participants[segment.thread_ordinal];
    if (segment.first_event_sequence < participant.first_event_sequence ||
        segment.last_event_sequence > participant.last_event_sequence ||
        !segment_end_sequences.insert(segment.last_event_sequence).second) {
      return Fail(error,
                  "manifest segment is outside its participant or duplicates "
                  "an end event");
    }
    if (!segment.entry_guest_address || (segment.entry_guest_address & 3) ||
        (segment.end_guest_address & 3) ||
        segment.end_guest_address < segment.entry_guest_address) {
      return Fail(error, "manifest segment guest code range is invalid");
    }
    if (!IsNonzeroHash(segment.code_corpus_sha256) ||
        !IsNonzeroHash(segment.segment_sha256)) {
      return Fail(error, "manifest segment SHA-256 is missing");
    }
    previous_segment_start = segment.first_event_sequence;
  }

  uint64_t next_event_sequence = manifest.first_event_sequence;
  uint64_t previous_checkpoint_sequence = 0;
  bool has_checkpoint = false;
  uint64_t chunk_bytes = 0;
  for (size_t i = 0; i < manifest.chunks.size(); ++i) {
    const GuestExecutionSessionChunkReference& chunk = manifest.chunks[i];
    if (!IsKnownChunkKind(chunk.kind) || chunk.ordinal != i ||
        !chunk.record_count || !IsNonzeroHash(chunk.encoded_sha256)) {
      return Fail(error, "manifest chunk reference is invalid");
    }
    if (chunk.encoded_size <
            GuestExecutionSessionCodec::kEnvelopeHeaderSize +
                GuestExecutionSessionCodec::kEnvelopeFooterSize ||
        chunk.encoded_size > limits.maximum_chunk_bytes ||
        !CheckedAdd(chunk_bytes, chunk.encoded_size, &chunk_bytes) ||
        chunk_bytes > limits.maximum_total_chunk_bytes) {
      return Fail(error, "manifest chunk byte count is invalid or overflows");
    }
    if (chunk.kind == GuestExecutionSessionChunkKind::kEvents) {
      uint64_t expected_last = 0;
      if (chunk.record_count > limits.maximum_events_per_chunk ||
          chunk.first_event_sequence != next_event_sequence ||
          !CheckedAdd(chunk.first_event_sequence, chunk.record_count - 1,
                      &expected_last) ||
          chunk.last_event_sequence != expected_last ||
          !CheckedAdd(chunk.last_event_sequence, 1, &next_event_sequence)) {
        return Fail(error, "manifest event chunks are not globally contiguous");
      }
    } else {
      if (chunk.record_count != 1 ||
          chunk.first_event_sequence != chunk.last_event_sequence ||
          chunk.first_event_sequence >= next_event_sequence ||
          (has_checkpoint &&
           chunk.first_event_sequence <= previous_checkpoint_sequence)) {
        return Fail(error, "manifest checkpoint order is invalid");
      }
      previous_checkpoint_sequence = chunk.first_event_sequence;
      has_checkpoint = true;
    }
  }
  if (manifest.chunks.front().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint ||
      manifest.chunks.front().first_event_sequence != 0 ||
      manifest.chunks.back().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint ||
      manifest.chunks.back().first_event_sequence !=
          manifest.last_event_sequence ||
      !has_checkpoint ||
      next_event_sequence - 1 != manifest.last_event_sequence) {
    return Fail(error,
                "manifest lacks complete initial/final checkpoint coverage");
  }

  uint64_t participant_bytes = 0;
  uint64_t segment_bytes = 0;
  uint64_t chunk_reference_bytes = 0;
  uint64_t payload_size =
      GuestExecutionSessionCodec::kManifestPayloadHeaderSize;
  uint64_t total_size = 0;
  if (!CheckedMultiply(manifest.participants.size(),
                       GuestExecutionSessionCodec::kParticipantRecordSize,
                       &participant_bytes) ||
      !CheckedMultiply(manifest.segments.size(),
                       GuestExecutionSessionCodec::kSegmentRecordSize,
                       &segment_bytes) ||
      !CheckedMultiply(manifest.chunks.size(),
                       GuestExecutionSessionCodec::kChunkReferenceRecordSize,
                       &chunk_reference_bytes) ||
      !CheckedAdd(payload_size, participant_bytes, &payload_size) ||
      !CheckedAdd(payload_size, segment_bytes, &payload_size) ||
      !CheckedAdd(payload_size, chunk_reference_bytes, &payload_size) ||
      !CheckedAdd(GuestExecutionSessionCodec::kEnvelopeHeaderSize, payload_size,
                  &total_size) ||
      !CheckedAdd(total_size, GuestExecutionSessionCodec::kEnvelopeFooterSize,
                  &total_size) ||
      total_size > limits.maximum_manifest_bytes ||
      total_size > std::numeric_limits<size_t>::max()) {
    return Fail(error, "manifest encoded size is invalid or overflows");
  }

  *encoded_size = total_size;
  *total_chunk_bytes = chunk_bytes;
  return true;
}

bool IsKnownEventKind(GuestExecutionSessionEventKind kind) {
  switch (kind) {
    case GuestExecutionSessionEventKind::kSegmentBegin:
    case GuestExecutionSessionEventKind::kSegmentEnd:
    case GuestExecutionSessionEventKind::kThreadDispatch:
    case GuestExecutionSessionEventKind::kSynchronization:
    case GuestExecutionSessionEventKind::kKernelExport:
    case GuestExecutionSessionEventKind::kExternOrBuiltin:
    case GuestExecutionSessionEventKind::kMmio:
    case GuestExecutionSessionEventKind::kClockOrTimebase:
    case GuestExecutionSessionEventKind::kInterrupt:
    case GuestExecutionSessionEventKind::kAtomicOrReservation:
    case GuestExecutionSessionEventKind::kMemoryMutation:
    case GuestExecutionSessionEventKind::kUnsupported:
      return true;
    default:
      return false;
  }
}

bool IsKnownPayloadKind(GuestExecutionSessionPayloadKind kind) {
  switch (kind) {
    case GuestExecutionSessionPayloadKind::kNone:
    case GuestExecutionSessionPayloadKind::kGuestBytes:
    case GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger:
      return true;
    default:
      return false;
  }
}

bool IsKnownDisposition(GuestExecutionSessionEventDisposition disposition) {
  switch (disposition) {
    case GuestExecutionSessionEventDisposition::kReplayCaptured:
    case GuestExecutionSessionEventDisposition::kValidateDeterministic:
    case GuestExecutionSessionEventDisposition::kRejectSession:
      return true;
    default:
      return false;
  }
}

bool IsKnownMutationSource(GuestExecutionSessionMutationSource source) {
  switch (source) {
    case GuestExecutionSessionMutationSource::kNone:
    case GuestExecutionSessionMutationSource::kActiveGuestThread:
    case GuestExecutionSessionMutationSource::kOtherGuestThread:
    case GuestExecutionSessionMutationSource::kGpu:
    case GuestExecutionSessionMutationSource::kDma:
    case GuestExecutionSessionMutationSource::kHost:
      return true;
    default:
      return false;
  }
}

bool ValidateEvent(const GuestExecutionSessionEvent& event,
                   const GuestExecutionSessionLimits& limits,
                   std::string* error) {
  if (!event.global_sequence) {
    return Fail(error, "event global sequence is zero");
  }
  if (!IsKnownEventKind(event.kind) || !IsKnownDisposition(event.disposition) ||
      !IsKnownMutationSource(event.mutation_source) ||
      !IsKnownPayloadKind(event.payload_kind)) {
    return Fail(error, "event contains an unknown typed field");
  }
  const bool payload_is_none =
      event.payload_kind == GuestExecutionSessionPayloadKind::kNone;
  const bool scalar_size_is_valid =
      event.payload_size == 1 || event.payload_size == 2 ||
      event.payload_size == 4 || event.payload_size == 8;
  if (event.payload_size > limits.maximum_event_payload_bytes ||
      (payload_is_none != (event.payload_size == 0)) ||
      (event.payload_size && !IsNonzeroHash(event.payload_sha256)) ||
      (!event.payload_size && !IsZeroHash(event.payload_sha256)) ||
      (event.payload_kind ==
           GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger &&
       !scalar_size_is_valid)) {
    return Fail(error, "event payload reference is invalid");
  }
  if (event.kind == GuestExecutionSessionEventKind::kUnsupported &&
      event.disposition !=
          GuestExecutionSessionEventDisposition::kRejectSession) {
    return Fail(error, "unsupported event does not reject the session");
  }

  if (event.kind == GuestExecutionSessionEventKind::kMemoryMutation) {
    if (event.mutation_source == GuestExecutionSessionMutationSource::kNone ||
        !ValidateGuestRange(event.guest_address, event.byte_count,
                            "memory mutation event", error)) {
      return false;
    }
    const bool asynchronous_source =
        event.mutation_source == GuestExecutionSessionMutationSource::kGpu ||
        event.mutation_source == GuestExecutionSessionMutationSource::kDma ||
        event.mutation_source == GuestExecutionSessionMutationSource::kHost;
    if (!asynchronous_source &&
        event.thread_ordinal == kGuestExecutionSessionNoThread) {
      return Fail(error, "guest-thread mutation has no participant");
    }
    if (event.disposition !=
            GuestExecutionSessionEventDisposition::kRejectSession &&
        (event.payload_kind != GuestExecutionSessionPayloadKind::kGuestBytes ||
         event.payload_size != event.byte_count)) {
      return Fail(error,
                  "replayable mutation does not identify its exact bytes");
    }
  } else {
    if (event.mutation_source != GuestExecutionSessionMutationSource::kNone) {
      return Fail(error, "non-mutation event names a mutation source");
    }
    const bool address_event =
        event.kind == GuestExecutionSessionEventKind::kMmio ||
        event.kind == GuestExecutionSessionEventKind::kAtomicOrReservation;
    if (address_event) {
      if (!ValidateGuestRange(event.guest_address, event.byte_count,
                              "addressed event", error)) {
        return false;
      }
    } else if (event.guest_address || event.byte_count) {
      return Fail(error, "non-addressed event contains a guest range");
    }
    if (event.kind != GuestExecutionSessionEventKind::kInterrupt &&
        event.kind != GuestExecutionSessionEventKind::kUnsupported &&
        event.thread_ordinal == kGuestExecutionSessionNoThread) {
      return Fail(error, "synchronous event has no participant");
    }
  }
  return true;
}

bool ValidateEventChunk(const GuestExecutionSessionEventChunk& chunk,
                        const GuestExecutionSessionLimits& limits,
                        uint64_t* payload_size, std::string* error) {
  if (!chunk.session_epoch) {
    return Fail(error, "event chunk session epoch is zero");
  }
  if (chunk.events.empty() ||
      chunk.events.size() > limits.maximum_events_per_chunk) {
    return Fail(error, "event chunk record count is invalid");
  }
  uint64_t expected_sequence = chunk.events.front().global_sequence;
  for (const GuestExecutionSessionEvent& event : chunk.events) {
    if (event.global_sequence != expected_sequence ||
        !ValidateEvent(event, limits, error)) {
      if (event.global_sequence != expected_sequence) {
        return Fail(error, "event chunk sequence is not contiguous");
      }
      return false;
    }
    if (event.global_sequence != chunk.events.back().global_sequence &&
        !CheckedAdd(expected_sequence, 1, &expected_sequence)) {
      return Fail(error, "event chunk sequence overflows");
    }
  }
  if (!CheckedMultiply(chunk.events.size(),
                       GuestExecutionSessionCodec::kEventRecordSize,
                       payload_size)) {
    return Fail(error, "event chunk payload size overflows");
  }
  uint64_t encoded_size = 0;
  if (!CheckedAdd(GuestExecutionSessionCodec::kEnvelopeHeaderSize,
                  *payload_size, &encoded_size) ||
      !CheckedAdd(encoded_size, GuestExecutionSessionCodec::kEnvelopeFooterSize,
                  &encoded_size) ||
      encoded_size > limits.maximum_chunk_bytes ||
      encoded_size > std::numeric_limits<size_t>::max()) {
    return Fail(error, "event chunk exceeds the byte limit");
  }
  return true;
}

bool IsKnownContentKind(GuestExecutionSessionContentKind kind) {
  switch (kind) {
    case GuestExecutionSessionContentKind::kGuestPage:
    case GuestExecutionSessionContentKind::kGuestCode:
      return true;
    default:
      return false;
  }
}

using ContentIdentity = std::pair<GuestExecutionSessionContentKind, uint64_t>;

bool ValidateCheckpoint(
    const GuestExecutionSessionCheckpoint& checkpoint,
    const GuestExecutionSessionLimits& limits, uint64_t* payload_size,
    std::map<GuestExecutionSessionSha256, ContentIdentity>* content_catalog,
    std::string* error) {
  if (checkpoint.thread_states.empty() ||
      checkpoint.thread_states.size() >
          limits.maximum_checkpoint_thread_states) {
    return Fail(error, "checkpoint thread-state count is invalid");
  }
  if (checkpoint.content.size() >
      limits.maximum_checkpoint_content_references) {
    return Fail(error, "checkpoint content-reference count is invalid");
  }

  uint32_t previous_thread = 0;
  bool has_previous_thread = false;
  std::map<GuestExecutionSessionSha256, uint64_t> state_catalog;
  for (const GuestExecutionSessionThreadStateReference& state :
       checkpoint.thread_states) {
    if ((has_previous_thread && state.thread_ordinal <= previous_thread) ||
        !state.byte_size ||
        state.byte_size > limits.maximum_content_blob_bytes ||
        !IsNonzeroHash(state.sha256)) {
      return Fail(error,
                  "checkpoint thread states are invalid or not canonical");
    }
    previous_thread = state.thread_ordinal;
    has_previous_thread = true;
    const auto [it, inserted] =
        state_catalog.emplace(state.sha256, state.byte_size);
    if (!inserted && it->second != state.byte_size) {
      return Fail(error,
                  "thread-state digest is reused with conflicting sizes");
    }
  }

  uint64_t previous_end = 0;
  bool has_previous_content = false;
  for (const GuestExecutionSessionContentReference& content :
       checkpoint.content) {
    if (!IsKnownContentKind(content.kind)) {
      return Fail(error, "checkpoint content kind is unknown");
    }
    if (!IsNonzeroHash(content.sha256) ||
        content.byte_size > limits.maximum_content_blob_bytes) {
      return Fail(error, "checkpoint content digest or byte size is invalid");
    }
    if (!ValidateGuestRange(content.guest_address, content.byte_size,
                            "checkpoint content reference", error)) {
      return false;
    }
    if (content.kind == GuestExecutionSessionContentKind::kGuestPage) {
      if ((content.guest_address &
           (GuestExecutionSessionCodec::kGuestPageSize - 1)) ||
          content.byte_size != GuestExecutionSessionCodec::kGuestPageSize) {
        return Fail(error, "checkpoint page reference is not one aligned page");
      }
    } else if ((content.guest_address & 3) || (content.byte_size & 3)) {
      return Fail(error, "checkpoint code reference is not PPC aligned");
    }
    if (has_previous_content && content.guest_address < previous_end) {
      return Fail(error,
                  "checkpoint content references overlap or are unordered");
    }
    previous_end = content.guest_address + content.byte_size;
    has_previous_content = true;

    const ContentIdentity identity = {content.kind, content.byte_size};
    const auto [it, inserted] =
        content_catalog->emplace(content.sha256, identity);
    if (!inserted && it->second != identity) {
      return Fail(error, "content digest is reused with conflicting metadata");
    }
  }

  uint64_t state_bytes = 0;
  uint64_t content_bytes = 0;
  if (!CheckedMultiply(checkpoint.thread_states.size(),
                       GuestExecutionSessionCodec::kThreadStateReferenceSize,
                       &state_bytes) ||
      !CheckedMultiply(checkpoint.content.size(),
                       GuestExecutionSessionCodec::kContentReferenceSize,
                       &content_bytes) ||
      !CheckedAdd(GuestExecutionSessionCodec::kCheckpointPayloadHeaderSize,
                  state_bytes, payload_size) ||
      !CheckedAdd(*payload_size, content_bytes, payload_size)) {
    return Fail(error, "checkpoint payload size overflows");
  }
  return true;
}

bool ValidateCheckpointChunk(
    const GuestExecutionSessionCheckpointChunk& chunk,
    const GuestExecutionSessionLimits& limits, uint64_t* payload_size,
    std::map<GuestExecutionSessionSha256, ContentIdentity>* content_catalog,
    std::string* error) {
  if (!chunk.session_epoch) {
    return Fail(error, "checkpoint chunk session epoch is zero");
  }
  if (!ValidateCheckpoint(chunk.checkpoint, limits, payload_size,
                          content_catalog, error)) {
    return false;
  }
  uint64_t encoded_size = 0;
  if (!CheckedAdd(GuestExecutionSessionCodec::kEnvelopeHeaderSize,
                  *payload_size, &encoded_size) ||
      !CheckedAdd(encoded_size, GuestExecutionSessionCodec::kEnvelopeFooterSize,
                  &encoded_size) ||
      encoded_size > limits.maximum_chunk_bytes ||
      encoded_size > std::numeric_limits<size_t>::max()) {
    return Fail(error, "checkpoint chunk exceeds the byte limit");
  }
  return true;
}

void WriteParticipant(Writer* writer,
                      const GuestExecutionSessionParticipant& participant) {
  writer->WriteU32(participant.ordinal);
  writer->WriteU32(participant.guest_thread_id);
  writer->WriteU64(participant.first_event_sequence);
  writer->WriteU64(participant.last_event_sequence);
  writer->WriteU64(participant.initial_state_size);
  writer->WriteBytes(participant.initial_state_sha256.data(),
                     participant.initial_state_sha256.size());
}

bool ReadParticipant(Reader* reader,
                     GuestExecutionSessionParticipant* participant) {
  return reader->ReadU32(&participant->ordinal) &&
         reader->ReadU32(&participant->guest_thread_id) &&
         reader->ReadU64(&participant->first_event_sequence) &&
         reader->ReadU64(&participant->last_event_sequence) &&
         reader->ReadU64(&participant->initial_state_size) &&
         reader->ReadBytes(participant->initial_state_sha256.data(),
                           participant->initial_state_sha256.size());
}

void WriteSegment(Writer* writer,
                  const GuestExecutionSessionSegmentReference& segment) {
  writer->WriteU32(segment.ordinal);
  writer->WriteU32(segment.thread_ordinal);
  writer->WriteU64(segment.first_event_sequence);
  writer->WriteU64(segment.last_event_sequence);
  writer->WriteU32(segment.entry_guest_address);
  writer->WriteU32(segment.end_guest_address);
  writer->WriteBytes(segment.code_corpus_sha256.data(),
                     segment.code_corpus_sha256.size());
  writer->WriteBytes(segment.segment_sha256.data(),
                     segment.segment_sha256.size());
}

bool ReadSegment(Reader* reader,
                 GuestExecutionSessionSegmentReference* segment) {
  return reader->ReadU32(&segment->ordinal) &&
         reader->ReadU32(&segment->thread_ordinal) &&
         reader->ReadU64(&segment->first_event_sequence) &&
         reader->ReadU64(&segment->last_event_sequence) &&
         reader->ReadU32(&segment->entry_guest_address) &&
         reader->ReadU32(&segment->end_guest_address) &&
         reader->ReadBytes(segment->code_corpus_sha256.data(),
                           segment->code_corpus_sha256.size()) &&
         reader->ReadBytes(segment->segment_sha256.data(),
                           segment->segment_sha256.size());
}

void WriteChunkReference(Writer* writer,
                         const GuestExecutionSessionChunkReference& chunk) {
  writer->WriteU32(static_cast<uint32_t>(chunk.kind));
  writer->WriteU32(chunk.ordinal);
  writer->WriteU64(chunk.first_event_sequence);
  writer->WriteU64(chunk.last_event_sequence);
  writer->WriteU32(chunk.record_count);
  writer->WriteU32(0);
  writer->WriteU64(chunk.encoded_size);
  writer->WriteBytes(chunk.encoded_sha256.data(), chunk.encoded_sha256.size());
}

bool ReadChunkReference(Reader* reader,
                        GuestExecutionSessionChunkReference* chunk) {
  uint32_t kind = 0;
  uint32_t reserved = 0;
  if (!reader->ReadU32(&kind) || !reader->ReadU32(&chunk->ordinal) ||
      !reader->ReadU64(&chunk->first_event_sequence) ||
      !reader->ReadU64(&chunk->last_event_sequence) ||
      !reader->ReadU32(&chunk->record_count) || !reader->ReadU32(&reserved) ||
      !reader->ReadU64(&chunk->encoded_size) ||
      !reader->ReadBytes(chunk->encoded_sha256.data(),
                         chunk->encoded_sha256.size()) ||
      reserved) {
    return false;
  }
  chunk->kind = static_cast<GuestExecutionSessionChunkKind>(kind);
  return true;
}

void WriteEvent(Writer* writer, const GuestExecutionSessionEvent& event) {
  writer->WriteU64(event.global_sequence);
  writer->WriteU32(event.thread_ordinal);
  writer->WriteU32(static_cast<uint32_t>(event.kind));
  writer->WriteU32(static_cast<uint32_t>(event.disposition));
  writer->WriteU32(static_cast<uint32_t>(event.mutation_source));
  writer->WriteU32(static_cast<uint32_t>(event.payload_kind));
  writer->WriteU32(0);
  writer->WriteU64(event.guest_address);
  writer->WriteU64(event.byte_count);
  writer->WriteU64(event.payload_size);
  writer->WriteBytes(event.payload_sha256.data(), event.payload_sha256.size());
}

bool ReadEvent(Reader* reader, GuestExecutionSessionEvent* event) {
  uint32_t kind = 0;
  uint32_t disposition = 0;
  uint32_t mutation_source = 0;
  uint32_t payload_kind = 0;
  uint32_t reserved = 0;
  if (!reader->ReadU64(&event->global_sequence) ||
      !reader->ReadU32(&event->thread_ordinal) || !reader->ReadU32(&kind) ||
      !reader->ReadU32(&disposition) || !reader->ReadU32(&mutation_source) ||
      !reader->ReadU32(&payload_kind) || !reader->ReadU32(&reserved) ||
      reserved || !reader->ReadU64(&event->guest_address) ||
      !reader->ReadU64(&event->byte_count) ||
      !reader->ReadU64(&event->payload_size) ||
      !reader->ReadBytes(event->payload_sha256.data(),
                         event->payload_sha256.size())) {
    return false;
  }
  event->kind = static_cast<GuestExecutionSessionEventKind>(kind);
  event->disposition =
      static_cast<GuestExecutionSessionEventDisposition>(disposition);
  event->mutation_source =
      static_cast<GuestExecutionSessionMutationSource>(mutation_source);
  event->payload_kind =
      static_cast<GuestExecutionSessionPayloadKind>(payload_kind);
  return true;
}

void WriteThreadStateReference(
    Writer* writer, const GuestExecutionSessionThreadStateReference& state) {
  writer->WriteU32(state.thread_ordinal);
  writer->WriteU32(0);
  writer->WriteU64(state.byte_size);
  writer->WriteBytes(state.sha256.data(), state.sha256.size());
}

bool ReadThreadStateReference(
    Reader* reader, GuestExecutionSessionThreadStateReference* state) {
  uint32_t reserved = 0;
  return reader->ReadU32(&state->thread_ordinal) &&
         reader->ReadU32(&reserved) && !reserved &&
         reader->ReadU64(&state->byte_size) &&
         reader->ReadBytes(state->sha256.data(), state->sha256.size());
}

void WriteContentReference(
    Writer* writer, const GuestExecutionSessionContentReference& content) {
  writer->WriteU32(static_cast<uint32_t>(content.kind));
  writer->WriteU32(0);
  writer->WriteU64(content.guest_address);
  writer->WriteU64(content.byte_size);
  writer->WriteBytes(content.sha256.data(), content.sha256.size());
}

bool ReadContentReference(Reader* reader,
                          GuestExecutionSessionContentReference* content) {
  uint32_t kind = 0;
  uint32_t reserved = 0;
  if (!reader->ReadU32(&kind) || !reader->ReadU32(&reserved) || reserved ||
      !reader->ReadU64(&content->guest_address) ||
      !reader->ReadU64(&content->byte_size) ||
      !reader->ReadBytes(content->sha256.data(), content->sha256.size())) {
    return false;
  }
  content->kind = static_cast<GuestExecutionSessionContentKind>(kind);
  return true;
}

}  // namespace

GuestExecutionSessionSha256 GuestExecutionSessionCodec::HashBytes(
    const uint8_t* data, size_t data_size) {
  sha256::SHA256 hasher;
  if (data_size) {
    hasher.add(data, data_size);
  }
  GuestExecutionSessionSha256 digest = {};
  static_assert(sizeof(unsigned char) == sizeof(uint8_t));
  hasher.getHash(reinterpret_cast<unsigned char*>(digest.data()));
  return digest;
}

bool GuestExecutionSessionCodec::EncodeManifest(
    const GuestExecutionSessionManifest& manifest, std::vector<uint8_t>* output,
    std::string* error, GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "manifest encoded output is null");
  }
  output->clear();
  if (error) {
    error->clear();
  }

  uint64_t encoded_size = 0;
  uint64_t total_chunk_bytes = 0;
  if (!ValidateManifest(manifest, limits, &encoded_size, &total_chunk_bytes,
                        error)) {
    return false;
  }
  uint64_t record_count = 0;
  if (!CheckedAdd(manifest.participants.size(), manifest.segments.size(),
                  &record_count) ||
      !CheckedAdd(record_count, manifest.chunks.size(), &record_count) ||
      record_count > UINT32_MAX) {
    return Fail(error, "manifest envelope record count overflows");
  }

  Writer payload_writer(static_cast<size_t>(encoded_size - kEnvelopeHeaderSize -
                                            kEnvelopeFooterSize));
  payload_writer.WriteU32(static_cast<uint32_t>(manifest.boundary.kind));
  payload_writer.WriteU32(
      static_cast<uint32_t>(manifest.boundary.marker_source));
  payload_writer.WriteU64(manifest.boundary.value);
  payload_writer.WriteU64(manifest.boundary.marker_identity);
  payload_writer.WriteU64(manifest.capture_start_tick);
  payload_writer.WriteU64(manifest.capture_end_tick);
  payload_writer.WriteU64(manifest.capture_tick_frequency);
  payload_writer.WriteBytes(manifest.capture_build_sha256.data(),
                            manifest.capture_build_sha256.size());
  payload_writer.WriteBytes(manifest.replay_config_sha256.data(),
                            manifest.replay_config_sha256.size());
  payload_writer.WriteBytes(manifest.title_identity_sha256.data(),
                            manifest.title_identity_sha256.size());
  payload_writer.WriteBytes(manifest.module_identity_sha256.data(),
                            manifest.module_identity_sha256.size());
  payload_writer.WriteU32(static_cast<uint32_t>(manifest.participants.size()));
  payload_writer.WriteU32(static_cast<uint32_t>(manifest.segments.size()));
  payload_writer.WriteU32(static_cast<uint32_t>(manifest.chunks.size()));
  payload_writer.WriteU32(0);
  payload_writer.WriteU64(total_chunk_bytes);
  payload_writer.WriteU64(manifest.accepted_segment_count);
  payload_writer.WriteU64(manifest.rejected_segment_count);
  payload_writer.WriteU64(manifest.accepted_event_count);
  payload_writer.WriteU64(manifest.rejected_event_count);
  payload_writer.WriteU64(manifest.unsupported_event_count);
  for (const GuestExecutionSessionParticipant& participant :
       manifest.participants) {
    WriteParticipant(&payload_writer, participant);
  }
  for (const GuestExecutionSessionSegmentReference& segment :
       manifest.segments) {
    WriteSegment(&payload_writer, segment);
  }
  for (const GuestExecutionSessionChunkReference& chunk : manifest.chunks) {
    WriteChunkReference(&payload_writer, chunk);
  }
  std::vector<uint8_t> payload = payload_writer.TakeData();

  EnvelopeMetadata metadata;
  metadata.kind = kManifestEnvelopeKind;
  metadata.session_epoch = manifest.session_epoch;
  metadata.ordinal = kManifestOrdinal;
  metadata.record_count = static_cast<uint32_t>(record_count);
  metadata.first_event_sequence = manifest.first_event_sequence;
  metadata.last_event_sequence = manifest.last_event_sequence;
  return EncodeEnvelope(metadata, payload, limits.maximum_manifest_bytes,
                        output, error);
}

bool GuestExecutionSessionCodec::DecodeManifest(
    const uint8_t* data, size_t data_size,
    GuestExecutionSessionManifest* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "manifest decoded output is null");
  }
  *output = {};
  if (error) {
    error->clear();
  }

  DecodedEnvelope envelope;
  if (!DecodeEnvelope(data, data_size, kManifestEnvelopeKind,
                      limits.maximum_manifest_bytes, &envelope, error)) {
    return false;
  }
  if (envelope.metadata.ordinal != kManifestOrdinal ||
      envelope.payload_size < kManifestPayloadHeaderSize) {
    return Fail(error, "manifest envelope ordinal or payload size is invalid");
  }

  Reader reader(envelope.payload, envelope.payload_size);
  GuestExecutionSessionManifest manifest;
  manifest.session_epoch = envelope.metadata.session_epoch;
  manifest.first_event_sequence = envelope.metadata.first_event_sequence;
  manifest.last_event_sequence = envelope.metadata.last_event_sequence;
  uint32_t boundary_kind = 0;
  uint32_t marker_source = 0;
  uint32_t participant_count = 0;
  uint32_t segment_count = 0;
  uint32_t chunk_count = 0;
  uint32_t reserved = 0;
  uint64_t wire_total_chunk_bytes = 0;
  if (!reader.ReadU32(&boundary_kind) || !reader.ReadU32(&marker_source) ||
      !reader.ReadU64(&manifest.boundary.value) ||
      !reader.ReadU64(&manifest.boundary.marker_identity) ||
      !reader.ReadU64(&manifest.capture_start_tick) ||
      !reader.ReadU64(&manifest.capture_end_tick) ||
      !reader.ReadU64(&manifest.capture_tick_frequency) ||
      !reader.ReadBytes(manifest.capture_build_sha256.data(),
                        manifest.capture_build_sha256.size()) ||
      !reader.ReadBytes(manifest.replay_config_sha256.data(),
                        manifest.replay_config_sha256.size()) ||
      !reader.ReadBytes(manifest.title_identity_sha256.data(),
                        manifest.title_identity_sha256.size()) ||
      !reader.ReadBytes(manifest.module_identity_sha256.data(),
                        manifest.module_identity_sha256.size()) ||
      !reader.ReadU32(&participant_count) || !reader.ReadU32(&segment_count) ||
      !reader.ReadU32(&chunk_count) || !reader.ReadU32(&reserved) || reserved ||
      !reader.ReadU64(&wire_total_chunk_bytes) ||
      !reader.ReadU64(&manifest.accepted_segment_count) ||
      !reader.ReadU64(&manifest.rejected_segment_count) ||
      !reader.ReadU64(&manifest.accepted_event_count) ||
      !reader.ReadU64(&manifest.rejected_event_count) ||
      !reader.ReadU64(&manifest.unsupported_event_count)) {
    return Fail(error, "manifest payload header is truncated or reserved");
  }
  manifest.boundary.kind =
      static_cast<GuestExecutionSessionBoundaryKind>(boundary_kind);
  manifest.boundary.marker_source =
      static_cast<GuestExecutionSessionMarkerSource>(marker_source);

  uint64_t participant_bytes = 0;
  uint64_t segment_bytes = 0;
  uint64_t chunk_bytes = 0;
  uint64_t expected_payload_size = kManifestPayloadHeaderSize;
  uint64_t wire_record_count = 0;
  if (participant_count > limits.maximum_participants ||
      segment_count > limits.maximum_segments ||
      chunk_count > limits.maximum_chunks ||
      !CheckedMultiply(participant_count, kParticipantRecordSize,
                       &participant_bytes) ||
      !CheckedMultiply(segment_count, kSegmentRecordSize, &segment_bytes) ||
      !CheckedMultiply(chunk_count, kChunkReferenceRecordSize, &chunk_bytes) ||
      !CheckedAdd(expected_payload_size, participant_bytes,
                  &expected_payload_size) ||
      !CheckedAdd(expected_payload_size, segment_bytes,
                  &expected_payload_size) ||
      !CheckedAdd(expected_payload_size, chunk_bytes, &expected_payload_size) ||
      expected_payload_size != envelope.payload_size ||
      !CheckedAdd(participant_count, segment_count, &wire_record_count) ||
      !CheckedAdd(wire_record_count, chunk_count, &wire_record_count) ||
      wire_record_count != envelope.metadata.record_count) {
    return Fail(error,
                "manifest record counts or payload byte count are invalid");
  }

  manifest.participants.resize(participant_count);
  for (GuestExecutionSessionParticipant& participant : manifest.participants) {
    if (!ReadParticipant(&reader, &participant)) {
      return Fail(error, "manifest participant record is truncated");
    }
  }
  manifest.segments.resize(segment_count);
  for (GuestExecutionSessionSegmentReference& segment : manifest.segments) {
    if (!ReadSegment(&reader, &segment)) {
      return Fail(error, "manifest segment record is truncated");
    }
  }
  manifest.chunks.resize(chunk_count);
  for (GuestExecutionSessionChunkReference& chunk : manifest.chunks) {
    if (!ReadChunkReference(&reader, &chunk)) {
      return Fail(error, "manifest chunk reference is truncated or reserved");
    }
  }
  if (reader.remaining()) {
    return Fail(error, "manifest payload has trailing bytes");
  }

  uint64_t validated_encoded_size = 0;
  uint64_t validated_total_chunk_bytes = 0;
  if (!ValidateManifest(manifest, limits, &validated_encoded_size,
                        &validated_total_chunk_bytes, error) ||
      validated_encoded_size != data_size ||
      validated_total_chunk_bytes != wire_total_chunk_bytes) {
    if (error && error->empty()) {
      error->assign("manifest derived byte counts do not match the wire data");
    }
    return false;
  }
  *output = std::move(manifest);
  return true;
}

bool GuestExecutionSessionCodec::EncodeEventChunk(
    const GuestExecutionSessionEventChunk& chunk, std::vector<uint8_t>* output,
    std::string* error, GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "event chunk encoded output is null");
  }
  output->clear();
  if (error) {
    error->clear();
  }
  uint64_t payload_size = 0;
  if (!ValidateEventChunk(chunk, limits, &payload_size, error)) {
    return false;
  }
  Writer payload_writer(static_cast<size_t>(payload_size));
  for (const GuestExecutionSessionEvent& event : chunk.events) {
    WriteEvent(&payload_writer, event);
  }
  std::vector<uint8_t> payload = payload_writer.TakeData();
  EnvelopeMetadata metadata;
  metadata.kind =
      static_cast<uint32_t>(GuestExecutionSessionChunkKind::kEvents);
  metadata.session_epoch = chunk.session_epoch;
  metadata.ordinal = chunk.ordinal;
  metadata.record_count = static_cast<uint32_t>(chunk.events.size());
  metadata.first_event_sequence = chunk.events.front().global_sequence;
  metadata.last_event_sequence = chunk.events.back().global_sequence;
  return EncodeEnvelope(metadata, payload, limits.maximum_chunk_bytes, output,
                        error);
}

bool GuestExecutionSessionCodec::DecodeEventChunk(
    const uint8_t* data, size_t data_size,
    GuestExecutionSessionEventChunk* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "event chunk decoded output is null");
  }
  *output = {};
  if (error) {
    error->clear();
  }
  DecodedEnvelope envelope;
  if (!DecodeEnvelope(
          data, data_size,
          static_cast<uint32_t>(GuestExecutionSessionChunkKind::kEvents),
          limits.maximum_chunk_bytes, &envelope, error)) {
    return false;
  }
  uint64_t expected_payload_size = 0;
  if (!envelope.metadata.record_count ||
      envelope.metadata.record_count > limits.maximum_events_per_chunk ||
      !CheckedMultiply(envelope.metadata.record_count, kEventRecordSize,
                       &expected_payload_size) ||
      expected_payload_size != envelope.payload_size) {
    return Fail(error, "event chunk record count or byte count is invalid");
  }

  GuestExecutionSessionEventChunk chunk;
  chunk.session_epoch = envelope.metadata.session_epoch;
  chunk.ordinal = envelope.metadata.ordinal;
  chunk.events.resize(envelope.metadata.record_count);
  Reader reader(envelope.payload, envelope.payload_size);
  for (GuestExecutionSessionEvent& event : chunk.events) {
    if (!ReadEvent(&reader, &event)) {
      return Fail(error, "event chunk record is truncated or reserved");
    }
  }
  uint64_t validated_payload_size = 0;
  if (reader.remaining() ||
      !ValidateEventChunk(chunk, limits, &validated_payload_size, error) ||
      validated_payload_size != envelope.payload_size ||
      chunk.events.front().global_sequence !=
          envelope.metadata.first_event_sequence ||
      chunk.events.back().global_sequence !=
          envelope.metadata.last_event_sequence) {
    if (error && error->empty()) {
      error->assign("event chunk envelope does not match its records");
    }
    return false;
  }
  *output = std::move(chunk);
  return true;
}

bool GuestExecutionSessionCodec::EncodeCheckpointChunk(
    const GuestExecutionSessionCheckpointChunk& chunk,
    std::vector<uint8_t>* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "checkpoint chunk encoded output is null");
  }
  output->clear();
  if (error) {
    error->clear();
  }
  uint64_t payload_size = 0;
  std::map<GuestExecutionSessionSha256, ContentIdentity> content_catalog;
  if (!ValidateCheckpointChunk(chunk, limits, &payload_size, &content_catalog,
                               error)) {
    return false;
  }
  Writer payload_writer(static_cast<size_t>(payload_size));
  payload_writer.WriteU64(chunk.checkpoint.global_sequence);
  payload_writer.WriteU32(
      static_cast<uint32_t>(chunk.checkpoint.thread_states.size()));
  payload_writer.WriteU32(
      static_cast<uint32_t>(chunk.checkpoint.content.size()));
  for (const GuestExecutionSessionThreadStateReference& state :
       chunk.checkpoint.thread_states) {
    WriteThreadStateReference(&payload_writer, state);
  }
  for (const GuestExecutionSessionContentReference& content :
       chunk.checkpoint.content) {
    WriteContentReference(&payload_writer, content);
  }
  std::vector<uint8_t> payload = payload_writer.TakeData();
  EnvelopeMetadata metadata;
  metadata.kind =
      static_cast<uint32_t>(GuestExecutionSessionChunkKind::kCheckpoint);
  metadata.session_epoch = chunk.session_epoch;
  metadata.ordinal = chunk.ordinal;
  metadata.record_count = 1;
  metadata.first_event_sequence = chunk.checkpoint.global_sequence;
  metadata.last_event_sequence = chunk.checkpoint.global_sequence;
  return EncodeEnvelope(metadata, payload, limits.maximum_chunk_bytes, output,
                        error);
}

bool GuestExecutionSessionCodec::DecodeCheckpointChunk(
    const uint8_t* data, size_t data_size,
    GuestExecutionSessionCheckpointChunk* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "checkpoint chunk decoded output is null");
  }
  *output = {};
  if (error) {
    error->clear();
  }
  DecodedEnvelope envelope;
  if (!DecodeEnvelope(
          data, data_size,
          static_cast<uint32_t>(GuestExecutionSessionChunkKind::kCheckpoint),
          limits.maximum_chunk_bytes, &envelope, error)) {
    return false;
  }
  if (envelope.metadata.record_count != 1 ||
      envelope.metadata.first_event_sequence !=
          envelope.metadata.last_event_sequence ||
      envelope.payload_size < kCheckpointPayloadHeaderSize) {
    return Fail(error, "checkpoint envelope metadata is invalid");
  }

  GuestExecutionSessionCheckpointChunk chunk;
  chunk.session_epoch = envelope.metadata.session_epoch;
  chunk.ordinal = envelope.metadata.ordinal;
  Reader reader(envelope.payload, envelope.payload_size);
  uint32_t state_count = 0;
  uint32_t content_count = 0;
  if (!reader.ReadU64(&chunk.checkpoint.global_sequence) ||
      !reader.ReadU32(&state_count) || !reader.ReadU32(&content_count)) {
    return Fail(error, "checkpoint payload header is truncated");
  }
  uint64_t state_bytes = 0;
  uint64_t content_bytes = 0;
  uint64_t expected_payload_size = kCheckpointPayloadHeaderSize;
  if (state_count > limits.maximum_checkpoint_thread_states ||
      content_count > limits.maximum_checkpoint_content_references ||
      !CheckedMultiply(state_count, kThreadStateReferenceSize, &state_bytes) ||
      !CheckedMultiply(content_count, kContentReferenceSize, &content_bytes) ||
      !CheckedAdd(expected_payload_size, state_bytes, &expected_payload_size) ||
      !CheckedAdd(expected_payload_size, content_bytes,
                  &expected_payload_size) ||
      expected_payload_size != envelope.payload_size) {
    return Fail(error,
                "checkpoint record counts or payload byte count are invalid");
  }
  chunk.checkpoint.thread_states.resize(state_count);
  for (GuestExecutionSessionThreadStateReference& state :
       chunk.checkpoint.thread_states) {
    if (!ReadThreadStateReference(&reader, &state)) {
      return Fail(error,
                  "checkpoint thread-state record is truncated or reserved");
    }
  }
  chunk.checkpoint.content.resize(content_count);
  for (GuestExecutionSessionContentReference& content :
       chunk.checkpoint.content) {
    if (!ReadContentReference(&reader, &content)) {
      return Fail(error, "checkpoint content record is truncated or reserved");
    }
  }

  uint64_t validated_payload_size = 0;
  std::map<GuestExecutionSessionSha256, ContentIdentity> content_catalog;
  if (reader.remaining() ||
      !ValidateCheckpointChunk(chunk, limits, &validated_payload_size,
                               &content_catalog, error) ||
      validated_payload_size != envelope.payload_size ||
      chunk.checkpoint.global_sequence !=
          envelope.metadata.first_event_sequence) {
    if (error && error->empty()) {
      error->assign("checkpoint envelope does not match its records");
    }
    return false;
  }
  *output = std::move(chunk);
  return true;
}

bool GuestExecutionSessionCodec::ValidateSession(
    const GuestExecutionSessionManifest& manifest,
    const std::vector<std::vector<uint8_t>>& encoded_chunks, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (error) {
    error->clear();
  }
  uint64_t manifest_size = 0;
  uint64_t manifest_chunk_bytes = 0;
  if (!ValidateManifest(manifest, limits, &manifest_size, &manifest_chunk_bytes,
                        error)) {
    return false;
  }
  if (encoded_chunks.size() != manifest.chunks.size()) {
    return Fail(error, "session chunk count does not match the manifest");
  }

  std::map<uint64_t, uint32_t> segment_starts;
  std::map<uint64_t, uint32_t> segment_ends;
  for (const GuestExecutionSessionSegmentReference& segment :
       manifest.segments) {
    if (!segment_starts
             .emplace(segment.first_event_sequence, segment.thread_ordinal)
             .second ||
        !segment_ends
             .emplace(segment.last_event_sequence, segment.thread_ordinal)
             .second) {
      return Fail(error, "multiple segments share one boundary event");
    }
  }

  uint64_t accepted_event_count = 0;
  uint64_t rejected_event_count = 0;
  uint64_t unsupported_event_count = 0;
  uint64_t supplied_chunk_bytes = 0;
  std::map<GuestExecutionSessionSha256, ContentIdentity> content_catalog;
  std::map<GuestExecutionSessionSha256, uint64_t> state_catalog;
  bool saw_initial_checkpoint = false;
  bool saw_final_checkpoint = false;

  for (size_t i = 0; i < encoded_chunks.size(); ++i) {
    const GuestExecutionSessionChunkReference& reference = manifest.chunks[i];
    const std::vector<uint8_t>& encoded = encoded_chunks[i];
    if (encoded.size() != reference.encoded_size ||
        HashBytes(encoded) != reference.encoded_sha256 ||
        !CheckedAdd(supplied_chunk_bytes, encoded.size(),
                    &supplied_chunk_bytes) ||
        supplied_chunk_bytes > limits.maximum_total_chunk_bytes) {
      return Fail(error,
                  "session chunk size or whole-envelope SHA-256 differs");
    }

    if (reference.kind == GuestExecutionSessionChunkKind::kEvents) {
      GuestExecutionSessionEventChunk chunk;
      if (!DecodeEventChunk(encoded, &chunk, error, limits)) {
        return false;
      }
      GuestExecutionSessionChunkReference derived;
      derived.kind = GuestExecutionSessionChunkKind::kEvents;
      derived.ordinal = chunk.ordinal;
      derived.first_event_sequence = chunk.events.front().global_sequence;
      derived.last_event_sequence = chunk.events.back().global_sequence;
      derived.record_count = static_cast<uint32_t>(chunk.events.size());
      derived.encoded_size = encoded.size();
      derived.encoded_sha256 = HashBytes(encoded);
      if (chunk.session_epoch != manifest.session_epoch ||
          derived != reference) {
        return Fail(error, "event chunk does not match its manifest reference");
      }

      for (const GuestExecutionSessionEvent& event : chunk.events) {
        if (event.thread_ordinal != kGuestExecutionSessionNoThread) {
          if (event.thread_ordinal >= manifest.participants.size()) {
            return Fail(error, "event names an unknown participant");
          }
          const GuestExecutionSessionParticipant& participant =
              manifest.participants[event.thread_ordinal];
          if (event.global_sequence < participant.first_event_sequence ||
              event.global_sequence > participant.last_event_sequence) {
            return Fail(error, "event lies outside its participant range");
          }
        }

        const auto start = segment_starts.find(event.global_sequence);
        if (event.kind == GuestExecutionSessionEventKind::kSegmentBegin) {
          if (start == segment_starts.end() ||
              start->second != event.thread_ordinal) {
            return Fail(error, "segment-begin event has no matching reference");
          }
          segment_starts.erase(start);
        } else if (start != segment_starts.end()) {
          return Fail(error, "referenced segment does not begin at its event");
        }
        const auto end = segment_ends.find(event.global_sequence);
        if (event.kind == GuestExecutionSessionEventKind::kSegmentEnd) {
          if (end == segment_ends.end() ||
              end->second != event.thread_ordinal) {
            return Fail(error, "segment-end event has no matching reference");
          }
          segment_ends.erase(end);
        } else if (end != segment_ends.end()) {
          return Fail(error, "referenced segment does not end at its event");
        }

        if (event.disposition ==
            GuestExecutionSessionEventDisposition::kRejectSession) {
          ++rejected_event_count;
        } else {
          ++accepted_event_count;
        }
        if (event.kind == GuestExecutionSessionEventKind::kUnsupported) {
          ++unsupported_event_count;
        }
      }
    } else {
      GuestExecutionSessionCheckpointChunk chunk;
      if (!DecodeCheckpointChunk(encoded, &chunk, error, limits)) {
        return false;
      }
      GuestExecutionSessionChunkReference derived;
      derived.kind = GuestExecutionSessionChunkKind::kCheckpoint;
      derived.ordinal = chunk.ordinal;
      derived.first_event_sequence = chunk.checkpoint.global_sequence;
      derived.last_event_sequence = chunk.checkpoint.global_sequence;
      derived.record_count = 1;
      derived.encoded_size = encoded.size();
      derived.encoded_sha256 = HashBytes(encoded);
      if (chunk.session_epoch != manifest.session_epoch ||
          derived != reference) {
        return Fail(error,
                    "checkpoint chunk does not match its manifest reference");
      }
      if (chunk.checkpoint.thread_states.size() !=
          manifest.participants.size()) {
        return Fail(error,
                    "checkpoint does not contain every participant state");
      }
      for (size_t state_index = 0;
           state_index < chunk.checkpoint.thread_states.size(); ++state_index) {
        const GuestExecutionSessionThreadStateReference& state =
            chunk.checkpoint.thread_states[state_index];
        const GuestExecutionSessionParticipant& participant =
            manifest.participants[state_index];
        if (state.thread_ordinal != state_index ||
            state.byte_size != participant.initial_state_size) {
          return Fail(error,
                      "checkpoint participant state size or order differs");
        }
        if (chunk.checkpoint.global_sequence == 0 &&
            state.sha256 != participant.initial_state_sha256) {
          return Fail(error,
                      "initial checkpoint state differs from the manifest");
        }
        const auto [it, inserted] =
            state_catalog.emplace(state.sha256, state.byte_size);
        if (!inserted && it->second != state.byte_size) {
          return Fail(error, "thread-state digest has conflicting byte sizes");
        }
      }
      for (const GuestExecutionSessionContentReference& content :
           chunk.checkpoint.content) {
        const ContentIdentity identity = {content.kind, content.byte_size};
        const auto [it, inserted] =
            content_catalog.emplace(content.sha256, identity);
        if (!inserted && it->second != identity) {
          return Fail(error, "content digest conflicts between checkpoints");
        }
      }
      saw_initial_checkpoint |= chunk.checkpoint.global_sequence == 0;
      saw_final_checkpoint |=
          chunk.checkpoint.global_sequence == manifest.last_event_sequence;
    }
  }

  if (supplied_chunk_bytes != manifest_chunk_bytes || !segment_starts.empty() ||
      !segment_ends.empty() || !saw_initial_checkpoint ||
      !saw_final_checkpoint) {
    return Fail(error,
                "session chunk closure or segment coverage is incomplete");
  }
  if (accepted_event_count != manifest.accepted_event_count ||
      rejected_event_count != manifest.rejected_event_count ||
      unsupported_event_count != manifest.unsupported_event_count) {
    return Fail(error, "session event coverage accounting does not match");
  }
  if (manifest.rejected_segment_count || rejected_event_count ||
      unsupported_event_count) {
    return Fail(error, "session contains rejected or unsupported work");
  }
  if (manifest.boundary.kind ==
          GuestExecutionSessionBoundaryKind::kSegmentCount &&
      manifest.boundary.value != manifest.accepted_segment_count) {
    return Fail(error,
                "session did not reach its requested accepted-segment count");
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
