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
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace xe {
namespace cpu {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {'X', 'E', 'G', 'C',
                                           'E', 'V', '3', 0};
constexpr uint32_t kKnownFlags = 0;

static_assert(8 + 4 + 4 + 8 + 4 + 4 + 8 + 8 + 8 + 8 ==
              GuestExecutionContinuousEventCodec::kHeaderSize);
static_assert(8 + 4 + 4 + 4 * 4 + 8 + 8 + 32 +
                  GuestExecutionContinuousEventCodec::kRecordReservedSize ==
              GuestExecutionContinuousEventCodec::kRecordSize);
static_assert(GuestExecutionContinuousEventCodec::kRecordSize == 96);

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

bool IsZeroHash(const GuestExecutionSessionSha256& hash) {
  return std::all_of(hash.cbegin(), hash.cend(),
                     [](uint8_t value) { return value == 0; });
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
    case GuestExecutionSessionEventKind::kInstructionCoverage:
    case GuestExecutionSessionEventKind::kGuestMarker:
    case GuestExecutionSessionEventKind::kBoundaryRequest:
    case GuestExecutionSessionEventKind::kBoundaryHeld:
    case GuestExecutionSessionEventKind::kOuterHostCallBegin:
    case GuestExecutionSessionEventKind::kOuterHostCallEnd:
    case GuestExecutionSessionEventKind::kJitSafepointArrival:
      return true;
    default:
      return false;
  }
}

bool ValidateIdentity(const GuestExecutionContinuousEventIdentity& identity,
                      std::string_view name, std::string* error) {
  const bool has_participant =
      identity.participant_ordinal != kGuestExecutionSessionNoThread;
  if (has_participant != (identity.guest_thread_id != 0)) {
    return Fail(error, std::string(name) +
                           " identity has a noncanonical ordinal/thread pair");
  }
  return true;
}

bool ValidateCheckpointReference(const GuestExecutionContinuousEvent& event,
                                 std::string* error) {
  const GuestExecutionContinuousCheckpointReference& checkpoint =
      event.checkpoint;
  switch (checkpoint.kind) {
    case GuestExecutionContinuousCheckpointReferenceKind::kNone:
      if (checkpoint.checkpoint_global_sequence || checkpoint.state_size ||
          !IsZeroHash(checkpoint.state_sha256)) {
        return Fail(error,
                    "event without a checkpoint has nonzero reference fields");
      }
      return true;
    case GuestExecutionContinuousCheckpointReferenceKind::kThreadState:
      if (event.subject.participant_ordinal == kGuestExecutionSessionNoThread ||
          checkpoint.checkpoint_global_sequence > event.global_sequence ||
          checkpoint.state_size !=
              ppc::GuestPPCThreadCheckpointCodec::kEncodedSize ||
          IsZeroHash(checkpoint.state_sha256)) {
        return Fail(error, "event checkpoint reference is invalid");
      }
      return true;
    default:
      return Fail(error, "event checkpoint reference kind is unknown");
  }
}

bool ValidateRecord(const GuestExecutionContinuousEvent& event,
                    std::string* error) {
  if (!event.global_sequence) {
    return Fail(error, "continuous event global sequence is zero");
  }
  if (!IsKnownEventKind(event.kind)) {
    return Fail(error, "continuous event kind is unknown");
  }
  return ValidateIdentity(event.actor, "actor", error) &&
         ValidateIdentity(event.subject, "subject", error) &&
         ValidateCheckpointReference(event, error);
}

bool ValidateRecords(const std::vector<GuestExecutionContinuousEvent>& records,
                     GuestExecutionContinuousEventLimits limits,
                     uint64_t* encoded_size, std::string* error) {
  if (records.empty() || records.size() > limits.maximum_records) {
    return Fail(error, "continuous event record count is invalid");
  }

  uint64_t expected_sequence = records.front().global_sequence;
  for (size_t record_index = 0; record_index < records.size(); ++record_index) {
    const GuestExecutionContinuousEvent& event = records[record_index];
    if (event.global_sequence != expected_sequence) {
      return Fail(error, "continuous event sequence is not contiguous");
    }
    if (!ValidateRecord(event, error)) {
      return false;
    }
    if (record_index + 1 != records.size() &&
        !CheckedAdd(expected_sequence, 1, &expected_sequence)) {
      return Fail(error, "continuous event sequence overflows");
    }
  }

  uint64_t record_bytes = 0;
  if (!CheckedMultiply(records.size(),
                       GuestExecutionContinuousEventCodec::kRecordSize,
                       &record_bytes) ||
      !CheckedAdd(GuestExecutionContinuousEventCodec::kHeaderSize, record_bytes,
                  encoded_size) ||
      *encoded_size > limits.maximum_encoded_bytes ||
      *encoded_size > std::numeric_limits<size_t>::max()) {
    return Fail(error, "continuous event encoded size is invalid or overflows");
  }
  return true;
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

void WriteRecord(Writer* writer, const GuestExecutionContinuousEvent& event) {
  writer->WriteU64(event.global_sequence);
  writer->WriteU32(static_cast<uint32_t>(event.kind));
  writer->WriteU32(static_cast<uint32_t>(event.checkpoint.kind));
  writer->WriteU32(event.actor.participant_ordinal);
  writer->WriteU32(event.actor.guest_thread_id);
  writer->WriteU32(event.subject.participant_ordinal);
  writer->WriteU32(event.subject.guest_thread_id);
  writer->WriteU64(event.checkpoint.checkpoint_global_sequence);
  writer->WriteU64(event.checkpoint.state_size);
  writer->WriteBytes(event.checkpoint.state_sha256.data(),
                     event.checkpoint.state_sha256.size());
  for (uint32_t i = 0;
       i < GuestExecutionContinuousEventCodec::kRecordReservedSize /
               sizeof(uint64_t);
       ++i) {
    writer->WriteU64(0);
  }
}

bool ReadRecord(Reader* reader, GuestExecutionContinuousEvent* event) {
  uint32_t kind = 0;
  uint32_t checkpoint_kind = 0;
  std::array<uint64_t, GuestExecutionContinuousEventCodec::kRecordReservedSize /
                           sizeof(uint64_t)>
      reserved = {};
  if (!reader->ReadU64(&event->global_sequence) || !reader->ReadU32(&kind) ||
      !reader->ReadU32(&checkpoint_kind) ||
      !reader->ReadU32(&event->actor.participant_ordinal) ||
      !reader->ReadU32(&event->actor.guest_thread_id) ||
      !reader->ReadU32(&event->subject.participant_ordinal) ||
      !reader->ReadU32(&event->subject.guest_thread_id) ||
      !reader->ReadU64(&event->checkpoint.checkpoint_global_sequence) ||
      !reader->ReadU64(&event->checkpoint.state_size) ||
      !reader->ReadBytes(event->checkpoint.state_sha256.data(),
                         event->checkpoint.state_sha256.size())) {
    return false;
  }
  for (uint64_t& value : reserved) {
    if (!reader->ReadU64(&value)) {
      return false;
    }
  }
  if (std::any_of(reserved.cbegin(), reserved.cend(),
                  [](uint64_t value) { return value != 0; })) {
    return false;
  }
  event->kind = static_cast<GuestExecutionSessionEventKind>(kind);
  event->checkpoint.kind =
      static_cast<GuestExecutionContinuousCheckpointReferenceKind>(
          checkpoint_kind);
  return true;
}

bool BindIdentity(
    const GuestExecutionContinuousEventIdentity& identity,
    const std::vector<GuestExecutionSessionParticipant>& participants,
    std::string_view name, std::string* error) {
  if (identity.participant_ordinal == kGuestExecutionSessionNoThread) {
    return true;
  }
  if (identity.participant_ordinal >= participants.size()) {
    return Fail(error, std::string(name) +
                           " participant ordinal is outside the catalog");
  }
  const GuestExecutionSessionParticipant& participant =
      participants[identity.participant_ordinal];
  if (participant.ordinal != identity.participant_ordinal ||
      participant.guest_thread_id != identity.guest_thread_id) {
    return Fail(error, std::string(name) +
                           " participant/thread identity does not match");
  }
  return true;
}

}  // namespace

bool GuestExecutionContinuousEventCodec::Encode(
    const std::vector<GuestExecutionContinuousEvent>& records,
    std::vector<uint8_t>* output, std::string* error,
    GuestExecutionContinuousEventLimits limits) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "continuous event encoded output is null");
  }
  output->clear();

  uint64_t encoded_size = 0;
  if (!ValidateRecords(records, limits, &encoded_size, error)) {
    return false;
  }

  Writer writer(static_cast<size_t>(encoded_size));
  writer.WriteBytes(kMagic.data(), kMagic.size());
  writer.WriteU32(kVersion);
  writer.WriteU32(kHeaderSize);
  writer.WriteU64(encoded_size);
  writer.WriteU32(kKnownFlags);
  writer.WriteU32(kRecordSize);
  writer.WriteU64(records.size());
  writer.WriteU64(records.front().global_sequence);
  writer.WriteU64(records.back().global_sequence);
  writer.WriteU64(0);
  for (const GuestExecutionContinuousEvent& event : records) {
    WriteRecord(&writer, event);
  }
  *output = writer.TakeData();
  if (output->size() != encoded_size) {
    output->clear();
    return Fail(error, "continuous event internal encoded-size mismatch");
  }
  return true;
}

bool GuestExecutionContinuousEventCodec::Decode(
    const uint8_t* data, size_t data_size,
    std::vector<GuestExecutionContinuousEvent>* output, std::string* error,
    GuestExecutionContinuousEventLimits limits) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "continuous event decoded output is null");
  }
  if (!data && data_size) {
    return Fail(error, "continuous event input data is null");
  }
  if (data_size > limits.maximum_encoded_bytes) {
    return Fail(error, "continuous event input exceeds the byte limit");
  }
  if (data_size < kHeaderSize) {
    return Fail(error, "continuous event header is truncated");
  }

  Reader reader(data, data_size);
  std::array<uint8_t, kMagic.size()> magic = {};
  uint32_t version = 0;
  uint32_t header_size = 0;
  uint64_t encoded_size = 0;
  uint32_t flags = 0;
  uint32_t record_size = 0;
  uint64_t record_count = 0;
  uint64_t first_sequence = 0;
  uint64_t last_sequence = 0;
  uint64_t reserved = 0;
  if (!reader.ReadBytes(magic.data(), magic.size()) ||
      !reader.ReadU32(&version) || !reader.ReadU32(&header_size) ||
      !reader.ReadU64(&encoded_size) || !reader.ReadU32(&flags) ||
      !reader.ReadU32(&record_size) || !reader.ReadU64(&record_count) ||
      !reader.ReadU64(&first_sequence) || !reader.ReadU64(&last_sequence) ||
      !reader.ReadU64(&reserved)) {
    return Fail(error, "continuous event header is truncated");
  }
  if (magic != kMagic) {
    return Fail(error, "continuous event magic is invalid");
  }
  if (version != kVersion) {
    return Fail(error, "continuous event version is unsupported");
  }
  if (header_size != kHeaderSize || record_size != kRecordSize) {
    return Fail(error, "continuous event header or record size is unsupported");
  }
  if (flags & ~kKnownFlags) {
    return Fail(error, "continuous event header contains unknown flags");
  }
  if (reserved) {
    return Fail(error, "continuous event header reserved field is nonzero");
  }
  if (!record_count || record_count > limits.maximum_records ||
      record_count > std::numeric_limits<size_t>::max()) {
    return Fail(error, "continuous event record count is invalid");
  }

  uint64_t record_bytes = 0;
  uint64_t expected_size = 0;
  if (!CheckedMultiply(record_count, kRecordSize, &record_bytes) ||
      !CheckedAdd(kHeaderSize, record_bytes, &expected_size) ||
      expected_size > limits.maximum_encoded_bytes ||
      expected_size > std::numeric_limits<size_t>::max() ||
      encoded_size != expected_size) {
    return Fail(error, "continuous event encoded size is noncanonical");
  }
  if (data_size != expected_size) {
    return Fail(error, data_size < expected_size
                           ? "continuous event input is truncated"
                           : "continuous event input has trailing data");
  }

  std::vector<GuestExecutionContinuousEvent> records;
  records.resize(static_cast<size_t>(record_count));
  for (GuestExecutionContinuousEvent& event : records) {
    if (!ReadRecord(&reader, &event)) {
      return Fail(error, "continuous event record is truncated or reserved");
    }
  }

  uint64_t validated_size = 0;
  if (reader.remaining() ||
      !ValidateRecords(records, limits, &validated_size, error) ||
      validated_size != expected_size ||
      records.front().global_sequence != first_sequence ||
      records.back().global_sequence != last_sequence) {
    if (error && error->empty()) {
      error->assign("continuous event header does not match its records");
    }
    return false;
  }
  *output = std::move(records);
  return true;
}

bool GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
    const std::vector<GuestExecutionContinuousEvent>& records,
    const std::vector<GuestExecutionSessionParticipant>& participants,
    std::string* error) {
  if (error) {
    error->clear();
  }
  uint64_t encoded_size = 0;
  if (!ValidateRecords(records, {}, &encoded_size, error)) {
    return false;
  }
  if (participants.empty()) {
    return Fail(error, "continuous event participant catalog is empty");
  }
  std::set<uint32_t> guest_thread_ids;
  for (size_t participant_index = 0; participant_index < participants.size();
       ++participant_index) {
    const GuestExecutionSessionParticipant& participant =
        participants[participant_index];
    if (participant.ordinal != participant_index ||
        !participant.guest_thread_id ||
        !guest_thread_ids.insert(participant.guest_thread_id).second) {
      return Fail(error,
                  "continuous event participant catalog is not dense/unique");
    }
  }
  for (const GuestExecutionContinuousEvent& event : records) {
    if (!BindIdentity(event.actor, participants, "actor", error) ||
        !BindIdentity(event.subject, participants, "subject", error)) {
      return false;
    }
  }
  return true;
}

bool GuestExecutionContinuousEventCodec::DecodeAndValidateCheckpoint(
    const GuestExecutionContinuousEvent& event, const uint8_t* data,
    size_t data_size, const ppc::GuestPPCThreadCheckpointBinding& binding,
    ppc::GuestPPCThreadCheckpoint* output, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "continuous event checkpoint output is null");
  }
  if (!ValidateRecord(event, error)) {
    return false;
  }
  if (event.checkpoint.kind !=
      GuestExecutionContinuousCheckpointReferenceKind::kThreadState) {
    return Fail(error, "continuous event has no thread checkpoint reference");
  }
  if (event.subject.participant_ordinal != binding.participant_ordinal ||
      event.subject.guest_thread_id != binding.guest_thread_id) {
    return Fail(error,
                "continuous event checkpoint subject binding mismatches");
  }
  if (!data || data_size != event.checkpoint.state_size ||
      data_size != ppc::GuestPPCThreadCheckpointCodec::kEncodedSize) {
    return Fail(error, "continuous event checkpoint bytes have the wrong size");
  }
  if (GuestExecutionSessionCodec::HashBytes(data, data_size) !=
      event.checkpoint.state_sha256) {
    return Fail(error, "continuous event checkpoint SHA-256 mismatches");
  }

  ppc::GuestPPCThreadCheckpoint checkpoint;
  std::string checkpoint_error;
  if (!ppc::GuestPPCThreadCheckpointCodec::Decode(data, data_size, &checkpoint,
                                                  &checkpoint_error)) {
    return Fail(error, "continuous event checkpoint decode failed: " +
                           checkpoint_error);
  }
  if (checkpoint.pending_external_event_sequence >
      event.checkpoint.checkpoint_global_sequence) {
    return Fail(error,
                "continuous event checkpoint pending route is out of order");
  }
  if (!ppc::GuestPPCThreadCheckpointCodec::ValidateBinding(checkpoint, binding,
                                                           &checkpoint_error)) {
    return Fail(error, "continuous event checkpoint binding failed: " +
                           checkpoint_error);
  }
  *output = std::move(checkpoint);
  return true;
}

}  // namespace cpu
}  // namespace xe
