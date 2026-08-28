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
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "third_party/crypto/sha256.h"
#include "xenia/cpu/guest_execution_continuous_event.h"
#include "xenia/cpu/guest_scheduler_record.h"

namespace xe {
namespace cpu {

namespace {

constexpr std::array<uint8_t, 8> kEnvelopeMagic = {'X', 'E', 'G', 'S',
                                                   'E', 'S', 'S', 0};
constexpr std::array<uint8_t, 8> kClosureMagic = {'X', 'E', 'G', 'C',
                                                  'L', 'O', 'S', 'E'};
constexpr std::array<uint8_t, 8> kSchedulerTopologyMagic = {'X', 'E', 'G', 'T',
                                                            'O', 'P', 'O', 0};
static_assert(GuestExecutionSessionCodec::kSchedulerEventPayloadVersion ==
              GuestSchedulerRecordCodec::kPayloadVersion);
static_assert(GuestExecutionSessionCodec::kSchedulerEventPayloadSize ==
              GuestSchedulerRecordCodec::kPayloadSize);
constexpr uint32_t kManifestEnvelopeKind = 1;
constexpr uint32_t kManifestOrdinal = UINT32_MAX;
constexpr uint32_t kKnownEnvelopeFlags = 0;
constexpr uint64_t kGuestAddressSpaceSize = uint64_t{1} << 32;
constexpr uint64_t kNanosecondsPerSecond = 1000000000;

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

// Computes ceil(nanoseconds * frequency / 1e9) without overflowing an
// intermediate product. Splitting both operands around 1e9 keeps the one
// remaining product below 1e18; every other term is checked explicitly.
bool ComputeDurationTargetTicks(uint64_t nanoseconds, uint64_t frequency,
                                uint64_t* target_ticks) {
  const uint64_t whole_seconds = nanoseconds / kNanosecondsPerSecond;
  const uint64_t remaining_nanoseconds = nanoseconds % kNanosecondsPerSecond;
  const uint64_t frequency_seconds = frequency / kNanosecondsPerSecond;
  const uint64_t frequency_remainder = frequency % kNanosecondsPerSecond;

  uint64_t whole_ticks = 0;
  uint64_t cross_ticks = 0;
  uint64_t fractional_product = 0;
  if (!CheckedMultiply(whole_seconds, frequency, &whole_ticks) ||
      !CheckedMultiply(remaining_nanoseconds, frequency_seconds,
                       &cross_ticks) ||
      !CheckedMultiply(remaining_nanoseconds, frequency_remainder,
                       &fractional_product)) {
    return false;
  }
  uint64_t fractional_ticks = fractional_product / kNanosecondsPerSecond;
  if (fractional_product % kNanosecondsPerSecond) {
    ++fractional_ticks;
  }
  return CheckedAdd(whole_ticks, cross_ticks, target_ticks) &&
         CheckedAdd(*target_ticks, fractional_ticks, target_ticks);
}

bool IsNonzeroHash(const GuestExecutionSessionSha256& hash) {
  return std::any_of(hash.cbegin(), hash.cend(),
                     [](uint8_t value) { return value != 0; });
}

bool IsZeroHash(const GuestExecutionSessionSha256& hash) {
  return !IsNonzeroHash(hash);
}

bool IsAbsentIdentity(const GuestExecutionContinuousEventIdentity& identity) {
  return identity.participant_ordinal == kGuestExecutionSessionNoThread &&
         identity.guest_thread_id == 0;
}

bool IsCodeExtentCovered(
    const std::vector<GuestExecutionSessionContentReference>& content,
    uint32_t start_address, uint32_t end_address) {
  uint64_t next_address = start_address;
  const uint64_t end_exclusive = uint64_t(end_address) + 4;
  for (const GuestExecutionSessionContentReference& reference : content) {
    if (reference.kind != GuestExecutionSessionContentKind::kGuestCode) {
      continue;
    }
    const uint64_t reference_end =
        reference.guest_address + reference.byte_size;
    if (reference_end <= next_address) {
      continue;
    }
    if (reference.guest_address > next_address) {
      return false;
    }
    next_address = reference_end;
    if (next_address >= end_exclusive) {
      return true;
    }
  }
  return false;
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

bool IsKnownMarkerSource(GuestExecutionSessionMarkerSource source) {
  switch (source) {
    case GuestExecutionSessionMarkerSource::kNone:
    case GuestExecutionSessionMarkerSource::kGuestDefined:
    case GuestExecutionSessionMarkerSource::kPm4Swap:
    case GuestExecutionSessionMarkerSource::kKernel:
    case GuestExecutionSessionMarkerSource::kOtherInstrumented:
      return true;
    default:
      return false;
  }
}

bool IsKnownStopReason(GuestExecutionSessionStopReason reason) {
  switch (reason) {
    case GuestExecutionSessionStopReason::kManualRequest:
    case GuestExecutionSessionStopReason::kRequestedBoundary:
    case GuestExecutionSessionStopReason::kMaximumSegmentCount:
    case GuestExecutionSessionStopReason::kMaximumEventCount:
    case GuestExecutionSessionStopReason::kMaximumGuestInstructionCount:
    case GuestExecutionSessionStopReason::kMaximumGuestMarkerCount:
    case GuestExecutionSessionStopReason::kMaximumDuration:
      return true;
    default:
      return false;
  }
}

bool IsSafetyStopReason(GuestExecutionSessionStopReason reason) {
  return reason != GuestExecutionSessionStopReason::kManualRequest &&
         reason != GuestExecutionSessionStopReason::kRequestedBoundary;
}

bool IsKnownBoundaryArrivalKind(GuestExecutionSessionBoundaryArrivalKind kind) {
  switch (kind) {
    case GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside:
    case GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint:
    case GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn:
      return true;
    default:
      return false;
  }
}

bool IsKnownInitialOuterCallState(
    GuestExecutionSessionInitialOuterCallState state) {
  switch (state) {
    case GuestExecutionSessionInitialOuterCallState::kOutside:
    case GuestExecutionSessionInitialOuterCallState::kActive:
    case GuestExecutionSessionInitialOuterCallState::kParkedBelowOuterCall:
      return true;
    default:
      return false;
  }
}

bool ValidateBoundaryPolicy(const GuestExecutionSessionBoundaryPolicy& policy,
                            std::string* error) {
  if (!IsKnownMarkerSource(policy.marker_source)) {
    return Fail(error, "session boundary marker source is unknown");
  }
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
          policy.marker_source == GuestExecutionSessionMarkerSource::kNone) {
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
    case GuestExecutionSessionChunkKind::kContinuousEvents:
    case GuestExecutionSessionChunkKind::kCodeCorpus:
    case GuestExecutionSessionChunkKind::kSchedulerTopology:
      return true;
    default:
      return false;
  }
}

bool IsKnownSchedulerTopologyBoundary(
    GuestExecutionSessionSchedulerTopologyBoundary boundary) {
  switch (boundary) {
    case GuestExecutionSessionSchedulerTopologyBoundary::kStart:
    case GuestExecutionSessionSchedulerTopologyBoundary::kFinal:
      return true;
    default:
      return false;
  }
}

bool IsKnownSchedulerParticipantState(
    GuestExecutionSessionSchedulerParticipantState state) {
  switch (state) {
    case GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned:
    case GuestExecutionSessionSchedulerParticipantState::kRunning:
    case GuestExecutionSessionSchedulerParticipantState::kReady:
    case GuestExecutionSessionSchedulerParticipantState::kBlocked:
    case GuestExecutionSessionSchedulerParticipantState::kSuspended:
      return true;
    default:
      return false;
  }
}

bool IsKnownSchedulerResumeKind(
    GuestExecutionSessionSchedulerResumeKind resume_kind) {
  switch (resume_kind) {
    case GuestExecutionSessionSchedulerResumeKind::kJitSafepoint:
    case GuestExecutionSessionSchedulerResumeKind::kNativeContinuation:
    case GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport:
    case GuestExecutionSessionSchedulerResumeKind::kNotYetRun:
      return true;
    default:
      return false;
  }
}

bool ValidateSchedulerBlockedWait(
    const GuestExecutionSessionSchedulerBlockedWaitBinding& wait,
    std::string* error) {
  constexpr uint32_t kKnownWaitFlags =
      kGuestExecutionSessionSchedulerWaitFlagGated |
      kGuestExecutionSessionSchedulerWaitFlagAlertable |
      kGuestExecutionSessionSchedulerWaitFlagInterruptible |
      kGuestExecutionSessionSchedulerWaitFlagUserApcPending;
  if (wait.kind == GuestExecutionSessionSchedulerWaitKind::kNone ||
      wait.handle_count > kGuestExecutionSessionSchedulerMaximumWaitHandles ||
      (wait.flags & ~kKnownWaitFlags) ||
      ((wait.flags & kGuestExecutionSessionSchedulerWaitFlagUserApcPending) &&
       !(wait.flags & kGuestExecutionSessionSchedulerWaitFlagAlertable))) {
    return Fail(error, "scheduler blocked-wait binding is malformed");
  }

  uint64_t epochs_before = 0;
  uint64_t epochs_observed = 0;
  bool has_signal_epochs = false;
  for (size_t index = 0;
       index < kGuestExecutionSessionSchedulerMaximumWaitHandles; ++index) {
    if (index < wait.handle_count) {
      if (!wait.handles[index]) {
        return Fail(error, "scheduler blocked-wait handle is invalid");
      }
    } else if (wait.handles[index] || wait.signal_epochs_before[index] ||
               wait.signal_epochs_observed[index]) {
      return Fail(error, "scheduler blocked-wait padding is nonzero");
    }
    epochs_before += wait.signal_epochs_before[index];
    epochs_observed += wait.signal_epochs_observed[index];
    has_signal_epochs |= wait.signal_epochs_before[index] != 0 ||
                         wait.signal_epochs_observed[index] != 0;
  }
  if (epochs_before != wait.wait_epoch ||
      epochs_observed != wait.observed_wait_epoch) {
    return Fail(error, "scheduler blocked-wait epochs are inconsistent");
  }

  switch (wait.kind) {
    case GuestExecutionSessionSchedulerWaitKind::kSingle:
      if (wait.handle_count != 1) {
        return Fail(error, "scheduler single wait has no unique handle");
      }
      break;
    case GuestExecutionSessionSchedulerWaitKind::kMultiAny:
    case GuestExecutionSessionSchedulerWaitKind::kMultiAll:
      if (!wait.handle_count) {
        return Fail(error, "scheduler multi-wait has no handles");
      }
      break;
    case GuestExecutionSessionSchedulerWaitKind::kDelay:
    case GuestExecutionSessionSchedulerWaitKind::kFence:
    case GuestExecutionSessionSchedulerWaitKind::kIoOffload:
    case GuestExecutionSessionSchedulerWaitKind::kSpinBackoff:
      if (wait.handle_count) {
        return Fail(error, "scheduler handle-free wait names an object");
      }
      break;
    case GuestExecutionSessionSchedulerWaitKind::kIoCompletion:
    case GuestExecutionSessionSchedulerWaitKind::kSocketIo:
      if (wait.handle_count != 1) {
        return Fail(error, "scheduler external wait has no unique handle");
      }
      break;
    default:
      return Fail(error, "scheduler blocked-wait kind is unknown");
  }

  if ((wait.kind == GuestExecutionSessionSchedulerWaitKind::kDelay ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kFence ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kIoOffload ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kSpinBackoff ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kIoCompletion ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kSocketIo) &&
      has_signal_epochs) {
    return Fail(error, "scheduler non-object wait carries signal epochs");
  }
  if ((wait.kind == GuestExecutionSessionSchedulerWaitKind::kFence ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kIoOffload ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kSpinBackoff) &&
      wait.deadline_ms) {
    return Fail(error, "scheduler untimed wait carries a deadline");
  }
  if (wait.kind == GuestExecutionSessionSchedulerWaitKind::kDelay &&
      !wait.deadline_ms) {
    return Fail(error, "scheduler delay wait has no deadline");
  }

  const bool gated = wait.flags & kGuestExecutionSessionSchedulerWaitFlagGated;
  const bool alertable =
      wait.flags & kGuestExecutionSessionSchedulerWaitFlagAlertable;
  const bool interruptible =
      wait.flags & kGuestExecutionSessionSchedulerWaitFlagInterruptible;
  if ((wait.kind == GuestExecutionSessionSchedulerWaitKind::kFence ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kIoOffload ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kSpinBackoff ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kIoCompletion ||
       wait.kind == GuestExecutionSessionSchedulerWaitKind::kSocketIo) &&
      gated) {
    return Fail(error, "scheduler polling wait is incorrectly gated");
  }
  switch (wait.kind) {
    case GuestExecutionSessionSchedulerWaitKind::kSingle:
    case GuestExecutionSessionSchedulerWaitKind::kMultiAny:
    case GuestExecutionSessionSchedulerWaitKind::kMultiAll:
      if (!interruptible) {
        return Fail(error,
                    "scheduler object wait is incorrectly non-interruptible");
      }
      break;
    case GuestExecutionSessionSchedulerWaitKind::kDelay:
      if (!interruptible || gated == alertable) {
        return Fail(error, "scheduler delay wait flags are impossible");
      }
      break;
    case GuestExecutionSessionSchedulerWaitKind::kFence:
    case GuestExecutionSessionSchedulerWaitKind::kIoOffload:
      if (alertable || interruptible) {
        return Fail(error, "scheduler stack-owned wait flags are impossible");
      }
      break;
    case GuestExecutionSessionSchedulerWaitKind::kSpinBackoff:
    case GuestExecutionSessionSchedulerWaitKind::kIoCompletion:
    case GuestExecutionSessionSchedulerWaitKind::kSocketIo:
      if (alertable || !interruptible) {
        return Fail(error, "scheduler polling wait flags are impossible");
      }
      break;
    default:
      return Fail(error, "scheduler blocked-wait kind is unknown");
  }
  return true;
}

bool IsBlockedParityWait(
    const GuestExecutionSessionSchedulerBlockedWaitBinding& wait) {
  // A wait is witnessed as unsatisfied by a signalable object's frozen epochs
  // or by a deadline the boundary has not reached; a kind carrying neither
  // states nothing about whether it woke.
  const bool object_wait =
      wait.kind == GuestExecutionSessionSchedulerWaitKind::kSingle ||
      wait.kind == GuestExecutionSessionSchedulerWaitKind::kMultiAny ||
      wait.kind == GuestExecutionSessionSchedulerWaitKind::kMultiAll;
  if (!(object_wait && wait.handle_count) && !wait.deadline_ms) {
    return false;
  }
  constexpr uint32_t kRefusedWaitFlags =
      kGuestExecutionSessionSchedulerWaitFlagAlertable |
      kGuestExecutionSessionSchedulerWaitFlagUserApcPending;
  if (wait.flags & kRefusedWaitFlags) {
    return false;
  }
  if (wait.observed_wait_epoch != wait.wait_epoch ||
      wait.signal_epochs_observed != wait.signal_epochs_before) {
    return false;
  }
  // observed_uptime_ms is a per-snapshot host clock read rather than thread
  // state, so the deadline is the only durable statement of how near this wait
  // is to its own timeout.
  return !wait.deadline_ms || wait.deadline_ms > wait.observed_uptime_ms;
}

bool ValidateSchedulerTopology(
    const GuestExecutionSessionSchedulerTopologyChunk& chunk,
    const GuestExecutionSessionLimits& limits, std::string* error) {
  if (!chunk.session_epoch ||
      !IsKnownSchedulerTopologyBoundary(chunk.boundary) ||
      chunk.participants.empty() ||
      chunk.participants.size() > limits.maximum_participants ||
      (chunk.boundary == GuestExecutionSessionSchedulerTopologyBoundary::kStart
           ? chunk.global_sequence != 0
           : chunk.global_sequence == 0)) {
    return Fail(error, "scheduler topology envelope is invalid");
  }

  std::set<uint32_t> guest_thread_ids;
  std::set<uint64_t> capture_instance_ids;
  std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> ready_orders;
  for (size_t index = 0; index < chunk.participants.size(); ++index) {
    const auto& participant = chunk.participants[index];
    if (participant.ordinal != index || !participant.guest_thread_id ||
        !participant.capture_instance_id ||
        !guest_thread_ids.insert(participant.guest_thread_id).second ||
        !capture_instance_ids.insert(participant.capture_instance_id).second ||
        !IsKnownSchedulerParticipantState(participant.state)) {
      return Fail(error,
                  "scheduler topology participants are not uniquely bound");
    }

    const bool no_cpu =
        participant.cpu == kGuestExecutionSessionSchedulerNoValue;
    const bool no_priority = participant.effective_priority ==
                             kGuestExecutionSessionSchedulerNoValue;
    const bool no_base_priority =
        participant.base_priority == kGuestExecutionSessionSchedulerNoValue;
    const bool no_suspension_count =
        participant.suspension_count == kGuestExecutionSessionSchedulerNoValue;
    const bool no_quantum = participant.quantum_remaining_us ==
                            kGuestExecutionSessionSchedulerNoQuantum;
    const bool no_ready_level =
        participant.ready_queue_level == kGuestExecutionSessionSchedulerNoValue;
    const bool no_ready_ordinal = participant.ready_queue_fifo_ordinal ==
                                  kGuestExecutionSessionSchedulerNoValue;
    const bool no_resume =
        participant.resume_kind ==
            GuestExecutionSessionSchedulerResumeKind::kNone &&
        !participant.guest_pc && !participant.restorable;
    const bool no_wait = participant.blocked_wait ==
                         GuestExecutionSessionSchedulerBlockedWaitBinding{};
    if (participant.state ==
        GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned) {
      if (!no_cpu || !no_priority || !no_base_priority ||
          !no_suspension_count || !no_quantum || !no_ready_level ||
          !no_ready_ordinal || !no_resume || !no_wait) {
        return Fail(error,
                    "scheduler-unowned topology participant has queue state");
      }
      continue;
    }
    if (no_cpu || participant.cpu >= 6 || no_priority ||
        participant.effective_priority >= 32 || no_base_priority ||
        participant.base_priority >= 32 || no_suspension_count ||
        participant.suspension_count > UINT8_MAX || no_quantum ||
        participant.quantum_remaining_us > UINT32_MAX) {
      return Fail(error,
                  "scheduler topology CPU, priority or quantum is invalid");
    }
    if (participant.state ==
            GuestExecutionSessionSchedulerParticipantState::kSuspended &&
        !participant.suspension_count) {
      return Fail(error, "scheduler suspended participant has zero count");
    }
    if (participant.state ==
        GuestExecutionSessionSchedulerParticipantState::kReady) {
      if (no_ready_level || participant.ready_queue_level >= 32 ||
          participant.ready_queue_level != participant.effective_priority ||
          no_ready_ordinal ||
          !ready_orders[{participant.cpu, participant.ready_queue_level}]
               .insert(participant.ready_queue_fifo_ordinal)
               .second) {
        return Fail(error, "scheduler ready topology is invalid");
      }
    } else if (!no_ready_level || !no_ready_ordinal) {
      return Fail(error,
                  "non-ready scheduler topology participant has queue order");
    }
    if (participant.state ==
        GuestExecutionSessionSchedulerParticipantState::kBlocked) {
      if (!ValidateSchedulerBlockedWait(participant.blocked_wait, error)) {
        return false;
      }
    } else if (!no_wait) {
      return Fail(error,
                  "non-blocked scheduler participant has a wait binding");
    }

    if (!IsKnownSchedulerResumeKind(participant.resume_kind)) {
      return Fail(error, "scheduler topology resume kind is unknown");
    }
    switch (participant.resume_kind) {
      case GuestExecutionSessionSchedulerResumeKind::kJitSafepoint:
        if (!participant.restorable || !participant.guest_pc ||
            (participant.guest_pc & 3) ||
            participant.state ==
                GuestExecutionSessionSchedulerParticipantState::kBlocked) {
          return Fail(error, "scheduler JIT resume route is invalid");
        }
        break;
      case GuestExecutionSessionSchedulerResumeKind::kNativeContinuation:
        if (participant.restorable || participant.guest_pc ||
            participant.state ==
                GuestExecutionSessionSchedulerParticipantState::kRunning ||
            participant.state ==
                GuestExecutionSessionSchedulerParticipantState::kBlocked) {
          return Fail(error, "scheduler native resume route is invalid");
        }
        break;
      case GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport:
        if (participant.restorable || !participant.guest_pc ||
            (participant.guest_pc & 3) ||
            participant.state !=
                GuestExecutionSessionSchedulerParticipantState::kBlocked) {
          return Fail(error, "scheduler blocked resume route is invalid");
        }
        break;
      case GuestExecutionSessionSchedulerResumeKind::kNotYetRun:
        if (participant.restorable || participant.guest_pc ||
            (participant.state !=
                 GuestExecutionSessionSchedulerParticipantState::kReady &&
             participant.state !=
                 GuestExecutionSessionSchedulerParticipantState::kSuspended)) {
          return Fail(error, "scheduler not-yet-run route is invalid");
        }
        break;
      default:
        return Fail(error, "scheduler topology resume kind is unknown");
    }
  }
  for (const auto& ready_order : ready_orders) {
    uint32_t expected = 0;
    for (uint32_t ordinal : ready_order.second) {
      if (ordinal != expected++) {
        return Fail(error, "scheduler ready FIFO order is not dense");
      }
    }
  }
  return true;
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
  if (!IsKnownStopReason(manifest.stop_reason) ||
      manifest.stop_request_event_sequence < manifest.first_event_sequence ||
      manifest.stop_request_event_sequence >= manifest.last_event_sequence ||
      manifest.stop_request_tick < manifest.capture_start_tick ||
      manifest.stop_request_tick > manifest.capture_end_tick ||
      manifest.stop_request_accepted_segment_count >
          manifest.accepted_segment_count ||
      !manifest.maximum_stop_tail_event_count ||
      !manifest.maximum_stop_tail_guest_instruction_count ||
      !manifest.maximum_stop_tail_ticks) {
    return Fail(error, "manifest stop proof is invalid");
  }
  const bool manual_policy =
      manifest.boundary.kind == GuestExecutionSessionBoundaryKind::kManual;
  const bool safety_stop = IsSafetyStopReason(manifest.stop_reason);
  if ((!safety_stop &&
       manifest.stop_reason !=
           (manual_policy
                ? GuestExecutionSessionStopReason::kManualRequest
                : GuestExecutionSessionStopReason::kRequestedBoundary)) ||
      (manifest.boundary.kind !=
           GuestExecutionSessionBoundaryKind::kGuestMarkerCount &&
       manifest.stop_request_matching_guest_marker_count)) {
    return Fail(error,
                "manifest stop reason or non-marker proof is inconsistent");
  }
  if (!safety_stop) {
    switch (manifest.boundary.kind) {
      case GuestExecutionSessionBoundaryKind::kManual:
        break;
      case GuestExecutionSessionBoundaryKind::kSegmentCount:
        if (manifest.stop_request_accepted_segment_count !=
            manifest.boundary.value) {
          return Fail(error, "segment boundary stop proof is inconsistent");
        }
        break;
      case GuestExecutionSessionBoundaryKind::kGuestMarkerCount:
        if (manifest.stop_request_matching_guest_marker_count !=
            manifest.boundary.value) {
          return Fail(error,
                      "guest-marker boundary stop proof is inconsistent");
        }
        break;
      case GuestExecutionSessionBoundaryKind::kGuestInstructionCount:
        if (manifest.stop_request_guest_instruction_count !=
            manifest.boundary.value) {
          return Fail(error, "instruction boundary stop proof is inconsistent");
        }
        break;
      case GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds: {
        uint64_t target_ticks = 0;
        if (!ComputeDurationTargetTicks(manifest.boundary.value,
                                        manifest.capture_tick_frequency,
                                        &target_ticks) ||
            manifest.stop_request_tick - manifest.capture_start_tick <
                target_ticks) {
          return Fail(error, "duration boundary stop proof is inconsistent");
        }
        break;
      }
      default:
        break;
    }
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
      total_event_count > limits.maximum_total_events ||
      total_event_count != manifest.last_event_sequence ||
      manifest.unsupported_event_count > manifest.rejected_event_count) {
    return Fail(error, "manifest coverage accounting is inconsistent");
  }
  const bool continuous_instruction_coverage = manifest.segments.empty();
  if (continuous_instruction_coverage &&
      (manifest.boundary.kind ==
           GuestExecutionSessionBoundaryKind::kSegmentCount ||
       manifest.rejected_segment_count || !manifest.accepted_event_count ||
       !manifest.stop_request_guest_instruction_count)) {
    return Fail(error,
                "continuous manifest lacks canonical instruction coverage");
  }
  if (manifest.participants.empty() ||
      manifest.participants.size() > limits.maximum_participants) {
    return Fail(error, "manifest participant count is invalid");
  }
  if (manifest.segments.size() > limits.maximum_segments) {
    return Fail(error, "manifest segment count is invalid");
  }
  if (manifest.chunks.empty() ||
      manifest.chunks.size() > limits.maximum_chunks) {
    return Fail(error, "manifest chunk count is invalid");
  }

  std::set<uint32_t> guest_thread_ids;
  std::set<uint64_t> capture_instance_ids;
  for (size_t i = 0; i < manifest.participants.size(); ++i) {
    const GuestExecutionSessionParticipant& participant =
        manifest.participants[i];
    if (participant.ordinal != i || !participant.guest_thread_id ||
        !guest_thread_ids.insert(participant.guest_thread_id).second ||
        !participant.capture_instance_id ||
        !capture_instance_ids.insert(participant.capture_instance_id).second ||
        !IsKnownInitialOuterCallState(participant.initial_outer_call_state) ||
        !IsKnownBoundaryArrivalKind(participant.boundary_arrival_kind)) {
      return Fail(error,
                  "manifest participants are not dense and uniquely named");
    }
    const bool has_no_events =
        !participant.first_event_sequence && !participant.last_event_sequence;
    if ((!has_no_events &&
         (participant.first_event_sequence < manifest.first_event_sequence ||
          participant.last_event_sequence < participant.first_event_sequence ||
          participant.last_event_sequence > manifest.last_event_sequence)) ||
        (participant.boundary_arrival_kind ==
                 GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside
             ? (participant.held_after_event_sequence !=
                    manifest.stop_request_event_sequence ||
                (!has_no_events && participant.last_event_sequence >
                                       manifest.stop_request_event_sequence))
             : (has_no_events ||
                participant.held_after_event_sequence <=
                    manifest.stop_request_event_sequence ||
                participant.held_after_event_sequence >=
                    manifest.last_event_sequence ||
                participant.last_event_sequence !=
                    participant.held_after_event_sequence))) {
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
  uint64_t next_continuous_event_sequence = manifest.first_event_sequence;
  uint64_t previous_checkpoint_sequence = 0;
  bool has_checkpoint = false;
  bool has_continuous_events = false;
  bool has_scheduler_topology = false;
  uint64_t total_checkpoint_thread_states = 0;
  uint32_t code_corpus_chunk_count = 0;
  uint32_t scheduler_topology_chunk_count = 0;
  uint64_t chunk_bytes = 0;
  // Segmented version-2 sessions are checkpoint, canonical events, optional
  // continuous overlay, checkpoint. Zero-segment continuous sessions insert
  // their single mandatory code corpus immediately after the initial
  // checkpoint. In both shapes all canonical event chunks precede all overlay
  // chunks, and the final checkpoint closes the sequence.
  for (size_t i = 0; i < manifest.chunks.size(); ++i) {
    const GuestExecutionSessionChunkReference& chunk = manifest.chunks[i];
    if (!IsKnownChunkKind(chunk.kind) || chunk.ordinal != i ||
        !chunk.record_count || !IsNonzeroHash(chunk.encoded_sha256)) {
      return Fail(error, "manifest chunk reference is invalid");
    }
    const uint64_t minimum_encoded_size =
        chunk.kind == GuestExecutionSessionChunkKind::kContinuousEvents
            ? GuestExecutionContinuousEventCodec::kHeaderSize +
                  GuestExecutionContinuousEventCodec::kRecordSize
            : GuestExecutionSessionCodec::kEnvelopeHeaderSize +
                  GuestExecutionSessionCodec::kEnvelopeFooterSize;
    if (chunk.encoded_size < minimum_encoded_size ||
        chunk.encoded_size > limits.maximum_chunk_bytes ||
        !CheckedAdd(chunk_bytes, chunk.encoded_size, &chunk_bytes) ||
        chunk_bytes > limits.maximum_total_chunk_bytes) {
      return Fail(error, "manifest chunk byte count is invalid or overflows");
    }
    if (chunk.kind == GuestExecutionSessionChunkKind::kEvents) {
      uint64_t expected_last = 0;
      if (has_continuous_events ||
          (continuous_instruction_coverage && code_corpus_chunk_count != 1) ||
          chunk.record_count > limits.maximum_events_per_chunk ||
          chunk.first_event_sequence != next_event_sequence ||
          !CheckedAdd(chunk.first_event_sequence, chunk.record_count - 1,
                      &expected_last) ||
          chunk.last_event_sequence != expected_last ||
          !CheckedAdd(chunk.last_event_sequence, 1, &next_event_sequence)) {
        return Fail(error, "manifest event chunks are not globally contiguous");
      }
    } else if (chunk.kind ==
               GuestExecutionSessionChunkKind::kContinuousEvents) {
      uint64_t expected_last = 0;
      if (has_scheduler_topology || i + 1 >= manifest.chunks.size() ||
          next_event_sequence == 0 ||
          next_event_sequence - 1 != manifest.last_event_sequence ||
          chunk.record_count > limits.maximum_events_per_chunk ||
          chunk.first_event_sequence != next_continuous_event_sequence ||
          !CheckedAdd(chunk.first_event_sequence, chunk.record_count - 1,
                      &expected_last) ||
          chunk.last_event_sequence != expected_last ||
          !CheckedAdd(chunk.last_event_sequence, 1,
                      &next_continuous_event_sequence)) {
        return Fail(
            error, "manifest continuous event overlay is invalid or misplaced");
      }
      has_continuous_events = true;
    } else if (chunk.kind ==
               GuestExecutionSessionChunkKind::kSchedulerTopology) {
      const uint64_t expected_sequence = scheduler_topology_chunk_count == 0
                                             ? 0
                                             : manifest.last_event_sequence;
      if (!continuous_instruction_coverage || !has_continuous_events ||
          i + 1 >= manifest.chunks.size() ||
          scheduler_topology_chunk_count >= 2 ||
          chunk.record_count != manifest.participants.size() ||
          chunk.first_event_sequence != expected_sequence ||
          chunk.last_event_sequence != expected_sequence) {
        return Fail(error,
                    "manifest scheduler topology is invalid or misplaced");
      }
      ++scheduler_topology_chunk_count;
      has_scheduler_topology = true;
    } else if (chunk.kind == GuestExecutionSessionChunkKind::kCheckpoint) {
      if (!CheckedAdd(total_checkpoint_thread_states,
                      manifest.participants.size(),
                      &total_checkpoint_thread_states) ||
          total_checkpoint_thread_states >
              limits.maximum_total_checkpoint_thread_states ||
          chunk.record_count != 1 ||
          chunk.first_event_sequence != chunk.last_event_sequence ||
          chunk.first_event_sequence != next_event_sequence - 1 ||
          (i != 0 && i + 1 != manifest.chunks.size()) ||
          (has_continuous_events && i + 1 != manifest.chunks.size()) ||
          (has_checkpoint &&
           chunk.first_event_sequence <= previous_checkpoint_sequence)) {
        return Fail(error, "manifest checkpoint order is invalid");
      }
      previous_checkpoint_sequence = chunk.first_event_sequence;
      has_checkpoint = true;
    } else {
      if (!continuous_instruction_coverage || i != 1 ||
          chunk.record_count != 1 || chunk.first_event_sequence ||
          chunk.last_event_sequence || ++code_corpus_chunk_count != 1) {
        return Fail(error, "manifest session code corpus reference is invalid");
      }
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
      (scheduler_topology_chunk_count != 0 &&
       scheduler_topology_chunk_count != 2) ||
      code_corpus_chunk_count != (continuous_instruction_coverage ? 1u : 0u) ||
      next_event_sequence - 1 != manifest.last_event_sequence ||
      (has_continuous_events &&
       (next_continuous_event_sequence == 0 ||
        next_continuous_event_sequence - 1 != manifest.last_event_sequence))) {
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
      !IsKnownPayloadKind(event.payload_kind) ||
      !IsKnownMarkerSource(event.marker_source)) {
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
  const bool no_auxiliary_fields =
      event.mutation_source == GuestExecutionSessionMutationSource::kNone &&
      payload_is_none && !event.guest_address && !event.byte_count &&
      event.marker_source == GuestExecutionSessionMarkerSource::kNone &&
      !event.marker_identity && !event.guest_instruction_delta;
  const bool has_participant =
      event.thread_ordinal != kGuestExecutionSessionNoThread;
  if (event.kind == GuestExecutionSessionEventKind::kInstructionCoverage) {
    if (!has_participant || !event.guest_instruction_delta ||
        event.mutation_source != GuestExecutionSessionMutationSource::kNone ||
        !payload_is_none || event.guest_address || event.byte_count ||
        event.marker_source != GuestExecutionSessionMarkerSource::kNone ||
        event.marker_identity ||
        event.disposition !=
            GuestExecutionSessionEventDisposition::kValidateDeterministic) {
      return Fail(error, "instruction coverage event is not canonical");
    }
    return true;
  }
  if (event.kind == GuestExecutionSessionEventKind::kGuestMarker) {
    if (event.marker_source == GuestExecutionSessionMarkerSource::kNone ||
        !event.marker_identity ||
        event.mutation_source != GuestExecutionSessionMutationSource::kNone ||
        !payload_is_none || event.guest_address || event.byte_count ||
        event.guest_instruction_delta) {
      return Fail(error, "guest-marker event is not canonical");
    }
    const bool pm4_marker =
        event.marker_source == GuestExecutionSessionMarkerSource::kPm4Swap;
    if (pm4_marker
            ? (has_participant ||
               event.disposition !=
                   GuestExecutionSessionEventDisposition::kReplayCaptured)
            : (!has_participant || event.disposition !=
                                       GuestExecutionSessionEventDisposition::
                                           kValidateDeterministic)) {
      return Fail(error, "guest-marker ownership is not canonical");
    }
    return true;
  }
  const bool boundary_control =
      event.kind == GuestExecutionSessionEventKind::kBoundaryRequest ||
      event.kind == GuestExecutionSessionEventKind::kBoundaryHeld;
  const bool participant_control =
      event.kind == GuestExecutionSessionEventKind::kSegmentBegin ||
      event.kind == GuestExecutionSessionEventKind::kSegmentEnd ||
      event.kind == GuestExecutionSessionEventKind::kOuterHostCallBegin ||
      event.kind == GuestExecutionSessionEventKind::kOuterHostCallEnd ||
      event.kind == GuestExecutionSessionEventKind::kJitSafepointArrival;
  if (boundary_control || participant_control) {
    if (!no_auxiliary_fields ||
        (boundary_control
             ? (has_participant ||
                event.disposition !=
                    GuestExecutionSessionEventDisposition::kReplayCaptured)
             : (!has_participant || event.disposition !=
                                        GuestExecutionSessionEventDisposition::
                                            kValidateDeterministic))) {
      return Fail(error, "structural control event is not canonical");
    }
    return true;
  }
  if (event.guest_instruction_delta) {
    return Fail(error, "non-coverage event contains an instruction delta");
  }
  if (event.marker_source != GuestExecutionSessionMarkerSource::kNone ||
      event.marker_identity) {
    return Fail(error, "non-marker event contains guest-marker fields");
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
    const bool modeled_export =
        event.kind == GuestExecutionSessionEventKind::kKernelExport ||
        event.kind == GuestExecutionSessionEventKind::kExternOrBuiltin;
    if (address_event) {
      if (!ValidateGuestRange(event.guest_address, event.byte_count,
                              "addressed event", error)) {
        return false;
      }
    } else if (modeled_export) {
      if (event.guest_address >= kGuestAddressSpaceSize ||
          (event.guest_address & 3) || event.byte_count) {
        return Fail(error,
                    "modeled export event has no canonical guest address");
      }
    } else if (event.guest_address || event.byte_count) {
      return Fail(error, "non-addressed event contains a guest range");
    }
    const bool scheduler_control =
        event.kind == GuestExecutionSessionEventKind::kThreadDispatch ||
        event.kind == GuestExecutionSessionEventKind::kSynchronization;
    if (event.kind != GuestExecutionSessionEventKind::kInterrupt &&
        event.kind != GuestExecutionSessionEventKind::kUnsupported &&
        !scheduler_control &&
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
  for (size_t event_index = 0; event_index < chunk.events.size();
       ++event_index) {
    const GuestExecutionSessionEvent& event = chunk.events[event_index];
    if (event.global_sequence != expected_sequence ||
        !ValidateEvent(event, limits, error)) {
      if (event.global_sequence != expected_sequence) {
        return Fail(error, "event chunk sequence is not contiguous");
      }
      return false;
    }
    if (event_index + 1 != chunk.events.size() &&
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
using EventPayloadIdentity =
    std::pair<GuestExecutionSessionPayloadKind, uint64_t>;

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
  writer->WriteU64(participant.capture_instance_id);
  writer->WriteU32(static_cast<uint32_t>(participant.boundary_arrival_kind));
  writer->WriteU32(static_cast<uint32_t>(participant.initial_outer_call_state));
  writer->WriteU64(participant.first_event_sequence);
  writer->WriteU64(participant.last_event_sequence);
  writer->WriteU64(participant.held_after_event_sequence);
  writer->WriteU64(participant.initial_state_size);
  writer->WriteBytes(participant.initial_state_sha256.data(),
                     participant.initial_state_sha256.size());
}

bool ReadParticipant(Reader* reader,
                     GuestExecutionSessionParticipant* participant) {
  uint32_t boundary_arrival_kind = 0;
  uint32_t initial_outer_call_state = 0;
  if (!reader->ReadU32(&participant->ordinal) ||
      !reader->ReadU32(&participant->guest_thread_id) ||
      !reader->ReadU64(&participant->capture_instance_id) ||
      !reader->ReadU32(&boundary_arrival_kind) ||
      !reader->ReadU32(&initial_outer_call_state) ||
      !reader->ReadU64(&participant->first_event_sequence) ||
      !reader->ReadU64(&participant->last_event_sequence) ||
      !reader->ReadU64(&participant->held_after_event_sequence) ||
      !reader->ReadU64(&participant->initial_state_size) ||
      !reader->ReadBytes(participant->initial_state_sha256.data(),
                         participant->initial_state_sha256.size())) {
    return false;
  }
  participant->boundary_arrival_kind =
      static_cast<GuestExecutionSessionBoundaryArrivalKind>(
          boundary_arrival_kind);
  participant->initial_outer_call_state =
      static_cast<GuestExecutionSessionInitialOuterCallState>(
          initial_outer_call_state);
  return true;
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
  writer->WriteU32(static_cast<uint32_t>(event.marker_source));
  writer->WriteU32(0);
  writer->WriteU64(event.marker_identity);
  writer->WriteU64(event.guest_instruction_delta);
}

bool ReadEvent(Reader* reader, GuestExecutionSessionEvent* event) {
  uint32_t kind = 0;
  uint32_t disposition = 0;
  uint32_t mutation_source = 0;
  uint32_t payload_kind = 0;
  uint32_t marker_source = 0;
  uint32_t reserved = 0;
  uint32_t trailing_reserved = 0;
  if (!reader->ReadU64(&event->global_sequence) ||
      !reader->ReadU32(&event->thread_ordinal) || !reader->ReadU32(&kind) ||
      !reader->ReadU32(&disposition) || !reader->ReadU32(&mutation_source) ||
      !reader->ReadU32(&payload_kind) || !reader->ReadU32(&reserved) ||
      reserved || !reader->ReadU64(&event->guest_address) ||
      !reader->ReadU64(&event->byte_count) ||
      !reader->ReadU64(&event->payload_size) ||
      !reader->ReadBytes(event->payload_sha256.data(),
                         event->payload_sha256.size()) ||
      !reader->ReadU32(&marker_source) ||
      !reader->ReadU32(&trailing_reserved) || trailing_reserved ||
      !reader->ReadU64(&event->marker_identity) ||
      !reader->ReadU64(&event->guest_instruction_delta)) {
    return false;
  }
  event->kind = static_cast<GuestExecutionSessionEventKind>(kind);
  event->disposition =
      static_cast<GuestExecutionSessionEventDisposition>(disposition);
  event->mutation_source =
      static_cast<GuestExecutionSessionMutationSource>(mutation_source);
  event->payload_kind =
      static_cast<GuestExecutionSessionPayloadKind>(payload_kind);
  event->marker_source =
      static_cast<GuestExecutionSessionMarkerSource>(marker_source);
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

const char* SchedulerTopologyFirstDifference(
    const GuestExecutionSessionSchedulerTopologyParticipant& initial,
    const GuestExecutionSessionSchedulerTopologyParticipant& final_row,
    bool compare_ready_fifo_ordinal) {
  if (initial.ordinal != final_row.ordinal) {
    return "ordinal";
  }
  if (initial.guest_thread_id != final_row.guest_thread_id) {
    return "guest_thread_id";
  }
  if (initial.capture_instance_id != final_row.capture_instance_id) {
    return "capture_instance_id";
  }
  if (initial.state != final_row.state) {
    return "state";
  }
  if (initial.cpu != final_row.cpu) {
    return "cpu";
  }
  if (initial.effective_priority != final_row.effective_priority) {
    return "effective_priority";
  }
  if (initial.base_priority != final_row.base_priority) {
    return "base_priority";
  }
  if (initial.suspension_count != final_row.suspension_count) {
    return "suspension_count";
  }
  if (initial.quantum_remaining_us != final_row.quantum_remaining_us) {
    return "quantum_remaining_us";
  }
  if (initial.ready_queue_level != final_row.ready_queue_level) {
    return "ready_queue_level";
  }
  if (compare_ready_fifo_ordinal &&
      initial.ready_queue_fifo_ordinal != final_row.ready_queue_fifo_ordinal) {
    return "ready_queue_fifo_ordinal";
  }
  if (initial.resume_kind != final_row.resume_kind) {
    return "resume_kind";
  }
  if (initial.guest_pc != final_row.guest_pc) {
    return "guest_pc";
  }
  if (initial.restorable != final_row.restorable) {
    return "restorable";
  }
  if (initial.blocked_wait != final_row.blocked_wait) {
    return "blocked_wait";
  }
  return nullptr;
}

std::string HexGuestThreadId(uint32_t guest_thread_id) {
  char text[9];
  std::snprintf(text, sizeof(text), "%08X", guest_thread_id);
  return text;
}

}  // namespace

const char* GuestExecutionSessionSchedulerTopologyFirstDifference(
    const GuestExecutionSessionSchedulerTopologyParticipant& initial,
    const GuestExecutionSessionSchedulerTopologyParticipant& final_row) {
  return SchedulerTopologyFirstDifference(initial, final_row, true);
}

const char* GuestExecutionSessionSchedulerTopologyPassiveRowFirstDifference(
    const GuestExecutionSessionSchedulerTopologyParticipant& initial,
    const GuestExecutionSessionSchedulerTopologyParticipant& final_row) {
  return SchedulerTopologyFirstDifference(initial, final_row, false);
}

bool GuestExecutionSessionSchedulerReadyOrderIsStable(
    const GuestExecutionSessionSchedulerTopologyChunk& initial,
    const GuestExecutionSessionSchedulerTopologyChunk& final_topology,
    const std::set<uint32_t>& scheduler_event_subjects, std::string* error) {
  using ReadyBucket = std::map<uint32_t, uint32_t>;
  std::map<std::pair<uint32_t, uint32_t>, std::pair<ReadyBucket, ReadyBucket>>
      ready_orders;
  const size_t participant_count =
      std::min(initial.participants.size(), final_topology.participants.size());
  for (size_t index = 0; index < participant_count; ++index) {
    const auto& initial_row = initial.participants[index];
    const auto& final_row = final_topology.participants[index];
    // A row a scheduler record names may have been unlinked and relinked
    // anywhere, so only rows no record names carry an order across the pair.
    if (initial_row.state !=
            GuestExecutionSessionSchedulerParticipantState::kReady ||
        final_row.state !=
            GuestExecutionSessionSchedulerParticipantState::kReady ||
        initial_row.guest_thread_id != final_row.guest_thread_id ||
        initial_row.cpu != final_row.cpu ||
        initial_row.ready_queue_level != final_row.ready_queue_level ||
        scheduler_event_subjects.contains(static_cast<uint32_t>(index))) {
      continue;
    }
    auto& ready_order =
        ready_orders[{initial_row.cpu, initial_row.ready_queue_level}];
    ready_order.first[initial_row.ready_queue_fifo_ordinal] =
        initial_row.guest_thread_id;
    ready_order.second[final_row.ready_queue_fifo_ordinal] =
        final_row.guest_thread_id;
  }
  for (const auto& ready_order : ready_orders) {
    auto initial_entry = ready_order.second.first.cbegin();
    auto final_entry = ready_order.second.second.cbegin();
    for (; initial_entry != ready_order.second.first.cend() &&
           final_entry != ready_order.second.second.cend();
         ++initial_entry, ++final_entry) {
      if (initial_entry->second == final_entry->second) {
        continue;
      }
      return Fail(
          error,
          "scheduler ready FIFO order changed between boundaries on cpu " +
              std::to_string(ready_order.first.first) + " level " +
              std::to_string(ready_order.first.second) + ": thread " +
              HexGuestThreadId(final_entry->second) + " overtook thread " +
              HexGuestThreadId(initial_entry->second));
    }
  }
  return true;
}

bool IsGuestExecutionSessionWokenInWaitParticipant(
    const GuestExecutionSessionSchedulerTopologyParticipant& participant) {
  if (participant.state !=
          GuestExecutionSessionSchedulerParticipantState::kReady ||
      participant.resume_kind !=
          GuestExecutionSessionSchedulerResumeKind::kNativeContinuation ||
      participant.restorable || participant.guest_pc) {
    return false;
  }
  // A replay seed reconstructs a ready participant from its queue position
  // alone, so the position has to be the dense one the encoder writes.
  if (participant.effective_priority ==
          kGuestExecutionSessionSchedulerNoValue ||
      participant.ready_queue_level != participant.effective_priority ||
      participant.ready_queue_fifo_ordinal ==
          kGuestExecutionSessionSchedulerNoValue) {
    return false;
  }
  // Only a blocked row serializes a wait binding. Restated so the predicate is
  // self-contained.
  return participant.blocked_wait ==
         GuestExecutionSessionSchedulerBlockedWaitBinding{};
}

bool IsGuestExecutionSessionBlockedParityParticipant(
    const GuestExecutionSessionSchedulerTopologyParticipant& participant) {
  if (participant.state !=
          GuestExecutionSessionSchedulerParticipantState::kBlocked ||
      participant.resume_kind !=
          GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport ||
      participant.restorable || !participant.guest_pc ||
      (participant.guest_pc & 3)) {
    return false;
  }
  return ValidateSchedulerBlockedWait(participant.blocked_wait, nullptr) &&
         IsBlockedParityWait(participant.blocked_wait);
}

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
  payload_writer.WriteU32(static_cast<uint32_t>(manifest.stop_reason));
  payload_writer.WriteU32(0);
  payload_writer.WriteU64(manifest.stop_request_event_sequence);
  payload_writer.WriteU64(manifest.stop_request_tick);
  payload_writer.WriteU64(manifest.stop_request_accepted_segment_count);
  payload_writer.WriteU64(manifest.stop_request_guest_instruction_count);
  payload_writer.WriteU64(manifest.stop_request_matching_guest_marker_count);
  payload_writer.WriteU64(manifest.maximum_stop_tail_event_count);
  payload_writer.WriteU64(manifest.maximum_stop_tail_guest_instruction_count);
  payload_writer.WriteU64(manifest.maximum_stop_tail_ticks);
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
  uint32_t stop_reason = 0;
  uint32_t reserved = 0;
  uint32_t stop_reserved = 0;
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
      !reader.ReadU64(&manifest.unsupported_event_count) ||
      !reader.ReadU32(&stop_reason) || !reader.ReadU32(&stop_reserved) ||
      stop_reserved || !reader.ReadU64(&manifest.stop_request_event_sequence) ||
      !reader.ReadU64(&manifest.stop_request_tick) ||
      !reader.ReadU64(&manifest.stop_request_accepted_segment_count) ||
      !reader.ReadU64(&manifest.stop_request_guest_instruction_count) ||
      !reader.ReadU64(&manifest.stop_request_matching_guest_marker_count) ||
      !reader.ReadU64(&manifest.maximum_stop_tail_event_count) ||
      !reader.ReadU64(&manifest.maximum_stop_tail_guest_instruction_count) ||
      !reader.ReadU64(&manifest.maximum_stop_tail_ticks)) {
    return Fail(error, "manifest payload header is truncated or reserved");
  }
  manifest.boundary.kind =
      static_cast<GuestExecutionSessionBoundaryKind>(boundary_kind);
  manifest.boundary.marker_source =
      static_cast<GuestExecutionSessionMarkerSource>(marker_source);
  manifest.stop_reason =
      static_cast<GuestExecutionSessionStopReason>(stop_reason);

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

bool GuestExecutionSessionCodec::EncodeCodeCorpusChunk(
    const GuestExecutionSessionCodeCorpusChunk& chunk,
    std::vector<uint8_t>* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "code corpus chunk encoded output is null");
  }
  output->clear();
  if (error) {
    error->clear();
  }
  if (!chunk.session_epoch || !IsNonzeroHash(chunk.code_corpus_sha256)) {
    return Fail(error, "code corpus chunk metadata is invalid");
  }
  Writer payload_writer(kCodeCorpusPayloadSize);
  payload_writer.WriteBytes(chunk.code_corpus_sha256.data(),
                            chunk.code_corpus_sha256.size());
  EnvelopeMetadata metadata;
  metadata.kind =
      static_cast<uint32_t>(GuestExecutionSessionChunkKind::kCodeCorpus);
  metadata.session_epoch = chunk.session_epoch;
  metadata.ordinal = chunk.ordinal;
  metadata.record_count = 1;
  return EncodeEnvelope(metadata, payload_writer.TakeData(),
                        limits.maximum_chunk_bytes, output, error);
}

bool GuestExecutionSessionCodec::DecodeCodeCorpusChunk(
    const uint8_t* data, size_t data_size,
    GuestExecutionSessionCodeCorpusChunk* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "code corpus chunk decoded output is null");
  }
  *output = {};
  if (error) {
    error->clear();
  }
  DecodedEnvelope envelope;
  if (!DecodeEnvelope(
          data, data_size,
          static_cast<uint32_t>(GuestExecutionSessionChunkKind::kCodeCorpus),
          limits.maximum_chunk_bytes, &envelope, error)) {
    return false;
  }
  if (!envelope.metadata.session_epoch || envelope.metadata.record_count != 1 ||
      envelope.metadata.first_event_sequence ||
      envelope.metadata.last_event_sequence ||
      envelope.payload_size != kCodeCorpusPayloadSize) {
    return Fail(error, "code corpus envelope metadata is invalid");
  }
  GuestExecutionSessionCodeCorpusChunk chunk;
  chunk.session_epoch = envelope.metadata.session_epoch;
  chunk.ordinal = envelope.metadata.ordinal;
  Reader reader(envelope.payload, envelope.payload_size);
  if (!reader.ReadBytes(chunk.code_corpus_sha256.data(),
                        chunk.code_corpus_sha256.size()) ||
      reader.remaining() || !IsNonzeroHash(chunk.code_corpus_sha256)) {
    return Fail(error, "code corpus payload is invalid");
  }
  *output = chunk;
  return true;
}

bool GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
    const GuestExecutionSessionSchedulerTopologyChunk& chunk,
    std::vector<uint8_t>* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "scheduler topology encoded output is null");
  }
  output->clear();
  if (error) {
    error->clear();
  }
  if (!ValidateSchedulerTopology(chunk, limits, error)) {
    return false;
  }
  uint64_t record_bytes = 0;
  uint64_t payload_size = kSchedulerTopologyPayloadHeaderSize;
  if (!CheckedMultiply(chunk.participants.size(), kSchedulerTopologyRecordSize,
                       &record_bytes) ||
      !CheckedAdd(payload_size, record_bytes, &payload_size) ||
      payload_size > limits.maximum_chunk_bytes ||
      payload_size > std::numeric_limits<size_t>::max()) {
    return Fail(error, "scheduler topology payload byte count is invalid");
  }

  Writer payload_writer(static_cast<size_t>(payload_size));
  payload_writer.WriteBytes(kSchedulerTopologyMagic.data(),
                            kSchedulerTopologyMagic.size());
  payload_writer.WriteU32(kSchedulerTopologyVersion);
  payload_writer.WriteU32(kSchedulerTopologyPayloadHeaderSize);
  payload_writer.WriteU32(static_cast<uint32_t>(chunk.boundary));
  payload_writer.WriteU32(kSchedulerTopologyRecordSize);
  payload_writer.WriteU32(static_cast<uint32_t>(chunk.participants.size()));
  payload_writer.WriteU32(0);
  for (const auto& participant : chunk.participants) {
    payload_writer.WriteU32(participant.ordinal);
    payload_writer.WriteU32(participant.guest_thread_id);
    payload_writer.WriteU64(participant.capture_instance_id);
    payload_writer.WriteU32(static_cast<uint32_t>(participant.state));
    payload_writer.WriteU32(participant.cpu);
    payload_writer.WriteU32(participant.effective_priority);
    payload_writer.WriteU32(participant.base_priority);
    payload_writer.WriteU32(participant.suspension_count);
    payload_writer.WriteU32(participant.ready_queue_level);
    payload_writer.WriteU32(participant.ready_queue_fifo_ordinal);
    payload_writer.WriteU32(static_cast<uint32_t>(participant.resume_kind));
    payload_writer.WriteU32(participant.guest_pc);
    payload_writer.WriteU32(participant.restorable ? 1 : 0);
    payload_writer.WriteU32(
        static_cast<uint32_t>(participant.blocked_wait.kind));
    payload_writer.WriteU32(participant.blocked_wait.handle_count);
    payload_writer.WriteU32(participant.blocked_wait.flags);
    payload_writer.WriteU32(participant.blocked_wait.wait_epoch);
    payload_writer.WriteU32(participant.blocked_wait.observed_wait_epoch);
    payload_writer.WriteU64(participant.quantum_remaining_us);
    payload_writer.WriteU64(participant.blocked_wait.deadline_ms);
    payload_writer.WriteU64(participant.blocked_wait.observed_uptime_ms);
    for (uint32_t value : participant.blocked_wait.handles) {
      payload_writer.WriteU32(value);
    }
    for (uint32_t value : participant.blocked_wait.signal_epochs_before) {
      payload_writer.WriteU32(value);
    }
    for (uint32_t value : participant.blocked_wait.signal_epochs_observed) {
      payload_writer.WriteU32(value);
    }
    payload_writer.WriteU32(0);
  }

  EnvelopeMetadata metadata;
  metadata.kind =
      static_cast<uint32_t>(GuestExecutionSessionChunkKind::kSchedulerTopology);
  metadata.session_epoch = chunk.session_epoch;
  metadata.ordinal = chunk.ordinal;
  metadata.record_count = static_cast<uint32_t>(chunk.participants.size());
  metadata.first_event_sequence = chunk.global_sequence;
  metadata.last_event_sequence = chunk.global_sequence;
  return EncodeEnvelope(metadata, payload_writer.TakeData(),
                        limits.maximum_chunk_bytes, output, error);
}

bool GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
    const uint8_t* data, size_t data_size,
    GuestExecutionSessionSchedulerTopologyChunk* output, std::string* error,
    GuestExecutionSessionLimits limits) {
  if (!output) {
    return Fail(error, "scheduler topology decoded output is null");
  }
  *output = {};
  if (error) {
    error->clear();
  }
  DecodedEnvelope envelope;
  if (!DecodeEnvelope(data, data_size,
                      static_cast<uint32_t>(
                          GuestExecutionSessionChunkKind::kSchedulerTopology),
                      limits.maximum_chunk_bytes, &envelope, error)) {
    return false;
  }
  if (!envelope.metadata.session_epoch || !envelope.metadata.record_count ||
      envelope.metadata.record_count > limits.maximum_participants ||
      envelope.metadata.first_event_sequence !=
          envelope.metadata.last_event_sequence ||
      envelope.payload_size < kSchedulerTopologyPayloadHeaderSize) {
    return Fail(error, "scheduler topology envelope metadata is invalid");
  }

  Reader reader(envelope.payload, envelope.payload_size);
  std::array<uint8_t, 8> magic = {};
  uint32_t version = 0;
  uint32_t header_size = 0;
  uint32_t raw_boundary = 0;
  uint32_t record_size = 0;
  uint32_t participant_count = 0;
  uint32_t reserved = 0;
  if (!reader.ReadBytes(magic.data(), magic.size()) ||
      !reader.ReadU32(&version) || !reader.ReadU32(&header_size) ||
      !reader.ReadU32(&raw_boundary) || !reader.ReadU32(&record_size) ||
      !reader.ReadU32(&participant_count) || !reader.ReadU32(&reserved) ||
      magic != kSchedulerTopologyMagic ||
      version != kSchedulerTopologyVersion ||
      header_size != kSchedulerTopologyPayloadHeaderSize ||
      record_size != kSchedulerTopologyRecordSize || reserved ||
      participant_count != envelope.metadata.record_count) {
    return Fail(error, "scheduler topology payload version is unsupported");
  }
  uint64_t record_bytes = 0;
  uint64_t expected_payload_size = kSchedulerTopologyPayloadHeaderSize;
  if (!CheckedMultiply(participant_count, kSchedulerTopologyRecordSize,
                       &record_bytes) ||
      !CheckedAdd(expected_payload_size, record_bytes,
                  &expected_payload_size) ||
      expected_payload_size != envelope.payload_size) {
    return Fail(error, "scheduler topology payload byte count is invalid");
  }

  GuestExecutionSessionSchedulerTopologyChunk chunk;
  chunk.session_epoch = envelope.metadata.session_epoch;
  chunk.ordinal = envelope.metadata.ordinal;
  chunk.boundary =
      static_cast<GuestExecutionSessionSchedulerTopologyBoundary>(raw_boundary);
  chunk.global_sequence = envelope.metadata.first_event_sequence;
  chunk.participants.resize(participant_count);
  for (auto& participant : chunk.participants) {
    uint32_t raw_state = 0;
    uint32_t raw_resume_kind = 0;
    uint32_t raw_restorable = 0;
    uint32_t raw_wait_kind = 0;
    uint32_t record_reserved = 0;
    if (!reader.ReadU32(&participant.ordinal) ||
        !reader.ReadU32(&participant.guest_thread_id) ||
        !reader.ReadU64(&participant.capture_instance_id) ||
        !reader.ReadU32(&raw_state) || !reader.ReadU32(&participant.cpu) ||
        !reader.ReadU32(&participant.effective_priority) ||
        !reader.ReadU32(&participant.base_priority) ||
        !reader.ReadU32(&participant.suspension_count) ||
        !reader.ReadU32(&participant.ready_queue_level) ||
        !reader.ReadU32(&participant.ready_queue_fifo_ordinal) ||
        !reader.ReadU32(&raw_resume_kind) ||
        !reader.ReadU32(&participant.guest_pc) ||
        !reader.ReadU32(&raw_restorable) || raw_restorable > 1 ||
        !reader.ReadU32(&raw_wait_kind) ||
        !reader.ReadU32(&participant.blocked_wait.handle_count) ||
        !reader.ReadU32(&participant.blocked_wait.flags) ||
        !reader.ReadU32(&participant.blocked_wait.wait_epoch) ||
        !reader.ReadU32(&participant.blocked_wait.observed_wait_epoch) ||
        !reader.ReadU64(&participant.quantum_remaining_us) ||
        !reader.ReadU64(&participant.blocked_wait.deadline_ms) ||
        !reader.ReadU64(&participant.blocked_wait.observed_uptime_ms)) {
      return Fail(error, "scheduler topology participant record is truncated");
    }
    participant.state =
        static_cast<GuestExecutionSessionSchedulerParticipantState>(raw_state);
    participant.resume_kind =
        static_cast<GuestExecutionSessionSchedulerResumeKind>(raw_resume_kind);
    participant.restorable = raw_restorable != 0;
    participant.blocked_wait.kind =
        static_cast<GuestExecutionSessionSchedulerWaitKind>(raw_wait_kind);
    for (uint32_t& value : participant.blocked_wait.handles) {
      if (!reader.ReadU32(&value)) {
        return Fail(error,
                    "scheduler topology participant record is truncated");
      }
    }
    for (uint32_t& value : participant.blocked_wait.signal_epochs_before) {
      if (!reader.ReadU32(&value)) {
        return Fail(error,
                    "scheduler topology participant record is truncated");
      }
    }
    for (uint32_t& value : participant.blocked_wait.signal_epochs_observed) {
      if (!reader.ReadU32(&value)) {
        return Fail(error,
                    "scheduler topology participant record is truncated");
      }
    }
    if (!reader.ReadU32(&record_reserved) || record_reserved) {
      return Fail(error, "scheduler topology participant record is truncated");
    }
  }
  if (reader.remaining() || !ValidateSchedulerTopology(chunk, limits, error)) {
    return false;
  }
  *output = std::move(chunk);
  return true;
}

bool GuestExecutionSessionCodec::ResolveSchedulerEventSubject(
    GuestExecutionSessionEventKind kind, const uint8_t* data, size_t data_size,
    const std::vector<GuestExecutionSessionParticipant>& participants,
    uint32_t* subject_ordinal, std::string* error, uint32_t* record_kind,
    uint32_t* record_reason) {
  if (!subject_ordinal) {
    return Fail(error, "scheduler event subject output is null");
  }
  *subject_ordinal = kGuestExecutionSessionNoThread;
  if (record_kind) {
    *record_kind = 0;
  }
  if (record_reason) {
    *record_reason = 0;
  }
  if (error) {
    error->clear();
  }
  if (kind != GuestExecutionSessionEventKind::kThreadDispatch &&
      kind != GuestExecutionSessionEventKind::kSynchronization) {
    return Fail(error, "scheduler event subject kind is not a scheduler kind");
  }
  if (!data) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  DecodedSchedulerRecord record;
  if (!GuestSchedulerRecordCodec::Decode(
          std::span<const uint8_t>(data, data_size), &record, error)) {
    return false;
  }
  GuestExecutionSessionEventKind canonical_kind;
  switch (record.kind) {
    case GuestSchedulerCaptureEventKind::kSafepoint:
    case GuestSchedulerCaptureEventKind::kBlock:
    case GuestSchedulerCaptureEventKind::kReready:
    case GuestSchedulerCaptureEventKind::kParkSuspended:
    case GuestSchedulerCaptureEventKind::kResume:
      canonical_kind = GuestExecutionSessionEventKind::kSynchronization;
      break;
    default:
      canonical_kind = GuestExecutionSessionEventKind::kThreadDispatch;
      break;
  }
  if (canonical_kind != kind) {
    return Fail(
        error, "scheduler event payload kind differs from its canonical event");
  }
  for (const GuestExecutionSessionParticipant& participant : participants) {
    if (participant.capture_instance_id == record.capture_instance_id &&
        participant.guest_thread_id == record.guest_thread_id) {
      *subject_ordinal = participant.ordinal;
      // The event-bridge static_asserts pin these against the session-side
      // spellings a route validator compares them to.
      if (record_kind) {
        *record_kind = static_cast<uint32_t>(record.kind);
      }
      if (record_reason) {
        *record_reason = static_cast<uint32_t>(record.reason);
      }
      return true;
    }
  }
  return Fail(error, "scheduler event subject is not a session participant");
}

bool GuestExecutionSessionCodec::ValidateSession(
    const GuestExecutionSessionManifest& manifest,
    const std::vector<std::vector<uint8_t>>& encoded_chunks, std::string* error,
    GuestExecutionSessionLimits limits) try {
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
  const bool continuous_instruction_coverage = manifest.segments.empty();

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
  std::map<GuestExecutionSessionSha256, uint64_t> blob_size_catalog;
  std::map<GuestExecutionSessionSha256, EventPayloadIdentity> payload_catalog;
  std::map<GuestExecutionSessionSha256, ContentIdentity> content_catalog;
  struct ObservedParticipantRange {
    bool has_event = false;
    uint64_t first_event_sequence = 0;
    uint64_t last_event_sequence = 0;
    bool outer_host_call_active = false;
  };
  std::vector<ObservedParticipantRange> observed_participant_ranges(
      manifest.participants.size());
  for (size_t i = 0; i < manifest.participants.size(); ++i) {
    // Every initial state other than outside seeds one open outer host call.
    observed_participant_ranges[i].outer_host_call_active =
        manifest.participants[i].initial_outer_call_state !=
        GuestExecutionSessionInitialOuterCallState::kOutside;
  }
  std::vector<uint32_t> observed_arrival_counts(manifest.participants.size());
  uint64_t stop_request_accepted_segment_count = 0;
  uint64_t stop_request_guest_instruction_count = 0;
  uint64_t stop_request_matching_guest_marker_count = 0;
  uint64_t stop_tail_guest_instruction_count = 0;
  GuestExecutionSessionEventKind request_trigger_kind =
      GuestExecutionSessionEventKind::kUnsupported;
  GuestExecutionSessionMarkerSource request_trigger_marker_source =
      GuestExecutionSessionMarkerSource::kNone;
  uint64_t request_trigger_marker_identity = 0;
  bool saw_boundary_request = false;
  bool saw_boundary_held = false;
  bool saw_initial_checkpoint = false;
  bool saw_final_checkpoint = false;
  bool saw_start_scheduler_topology = false;
  bool saw_final_scheduler_topology = false;
  const bool manifest_has_continuous_events = std::any_of(
      manifest.chunks.cbegin(), manifest.chunks.cend(),
      [](const GuestExecutionSessionChunkReference& chunk) {
        return chunk.kind == GuestExecutionSessionChunkKind::kContinuousEvents;
      });
  struct CanonicalEventIdentity {
    uint64_t global_sequence;
    GuestExecutionSessionEventKind kind;
    GuestExecutionSessionEventDisposition disposition;
    uint32_t thread_ordinal;
    uint64_t guest_address;
  };
  std::vector<CanonicalEventIdentity> canonical_event_identities;
  std::vector<GuestExecutionContinuousEvent> continuous_events;
  std::map<std::pair<uint64_t, uint32_t>,
           GuestExecutionSessionThreadStateReference>
      checkpoint_thread_states;
  std::vector<GuestExecutionSessionContentReference> initial_checkpoint_content;
  bool saw_code_corpus = false;

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
        if (continuous_instruction_coverage &&
            (event.kind == GuestExecutionSessionEventKind::kSegmentBegin ||
             event.kind == GuestExecutionSessionEventKind::kSegmentEnd)) {
          return Fail(error,
                      "continuous session contains a segment control event");
        }
        if (manifest_has_continuous_events) {
          canonical_event_identities.push_back(
              {event.global_sequence, event.kind, event.disposition,
               event.thread_ordinal, event.guest_address});
        }
        if (event.payload_size) {
          const EventPayloadIdentity identity = {event.payload_kind,
                                                 event.payload_size};
          const auto [it, inserted] =
              payload_catalog.emplace(event.payload_sha256, identity);
          if (!inserted && it->second != identity) {
            return Fail(
                error,
                "event payload digest has conflicting type or byte size");
          }
          const auto [size_it, size_inserted] = blob_size_catalog.emplace(
              event.payload_sha256, event.payload_size);
          if (!size_inserted && size_it->second != event.payload_size) {
            return Fail(error, "digest has conflicting canonical byte sizes");
          }
        }
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
          ObservedParticipantRange& observed =
              observed_participant_ranges[event.thread_ordinal];
          if (!observed.has_event) {
            observed.has_event = true;
            observed.first_event_sequence = event.global_sequence;
          }
          observed.last_event_sequence = event.global_sequence;
          if (event.global_sequence > participant.held_after_event_sequence) {
            return Fail(error,
                        "participant owns an event after its held boundary");
          }
          if (event.kind ==
              GuestExecutionSessionEventKind::kOuterHostCallBegin) {
            if (observed.outer_host_call_active) {
              return Fail(error, "participant begins a nested outer host call");
            }
            observed.outer_host_call_active = true;
          } else if (event.kind ==
                     GuestExecutionSessionEventKind::kOuterHostCallEnd) {
            if (!observed.outer_host_call_active) {
              return Fail(error,
                          "participant ends an inactive outer host call");
            }
            observed.outer_host_call_active = false;
          } else if (event.kind ==
                         GuestExecutionSessionEventKind::kJitSafepointArrival &&
                     !observed.outer_host_call_active) {
            return Fail(error,
                        "participant reaches a JIT arrival while outside");
          }
          if (continuous_instruction_coverage &&
              event.kind ==
                  GuestExecutionSessionEventKind::kInstructionCoverage &&
              !observed.outer_host_call_active) {
            return Fail(error,
                        "continuous instruction coverage lies outside its "
                        "participant outer guest call");
          }
          if (event.global_sequence > manifest.stop_request_event_sequence) {
            const bool jit_arrival =
                event.kind ==
                GuestExecutionSessionEventKind::kJitSafepointArrival;
            const bool outer_return =
                event.kind == GuestExecutionSessionEventKind::kOuterHostCallEnd;
            if (jit_arrival || outer_return) {
              const bool expected =
                  (jit_arrival && participant.boundary_arrival_kind ==
                                      GuestExecutionSessionBoundaryArrivalKind::
                                          kJitSafepoint) ||
                  (outer_return &&
                   participant.boundary_arrival_kind ==
                       GuestExecutionSessionBoundaryArrivalKind::
                           kOuterHostCallReturn);
              if (!expected ||
                  event.global_sequence !=
                      participant.held_after_event_sequence ||
                  observed_arrival_counts[event.thread_ordinal] == UINT32_MAX) {
                return Fail(error,
                            "participant boundary arrival is inconsistent");
              }
              ++observed_arrival_counts[event.thread_ordinal];
            }
          }
        }

        if (event.kind == GuestExecutionSessionEventKind::kBoundaryRequest) {
          if (event.global_sequence != manifest.stop_request_event_sequence ||
              saw_boundary_request) {
            return Fail(error, "session boundary request is not unique");
          }
          for (size_t participant_index = 0;
               participant_index < manifest.participants.size();
               ++participant_index) {
            const bool expected_active =
                manifest.participants[participant_index]
                    .boundary_arrival_kind !=
                GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
            if (observed_participant_ranges[participant_index]
                    .outer_host_call_active != expected_active) {
              return Fail(
                  error, "participant outer-call state differs at the request");
            }
          }
          saw_boundary_request = true;
        } else if (event.global_sequence ==
                   manifest.stop_request_event_sequence) {
          return Fail(error, "stop request sequence is not a boundary request");
        }
        if (event.kind == GuestExecutionSessionEventKind::kBoundaryHeld) {
          if (event.global_sequence != manifest.last_event_sequence ||
              saw_boundary_held) {
            return Fail(error, "session held boundary is not the final event");
          }
          saw_boundary_held = true;
        } else if (event.global_sequence == manifest.last_event_sequence) {
          return Fail(error, "session final event is not a held boundary");
        }
        if (event.kind ==
                GuestExecutionSessionEventKind::kJitSafepointArrival &&
            event.global_sequence <= manifest.stop_request_event_sequence) {
          return Fail(error, "JIT boundary arrival precedes the request");
        }
        if (event.kind == GuestExecutionSessionEventKind::kOuterHostCallBegin &&
            event.global_sequence > manifest.stop_request_event_sequence) {
          return Fail(error,
                      "outer host call begins after the boundary request");
        }
        if (event.global_sequence == manifest.stop_request_event_sequence - 1) {
          request_trigger_kind = event.kind;
          request_trigger_marker_source = event.marker_source;
          request_trigger_marker_identity = event.marker_identity;
        }

        const bool in_timed_prefix =
            event.global_sequence < manifest.stop_request_event_sequence;
        const bool in_stop_tail =
            event.global_sequence > manifest.stop_request_event_sequence &&
            event.global_sequence < manifest.last_event_sequence;
        if (in_timed_prefix) {
          if (event.kind == GuestExecutionSessionEventKind::kSegmentEnd &&
              !CheckedAdd(stop_request_accepted_segment_count, 1,
                          &stop_request_accepted_segment_count)) {
            return Fail(error, "session request segment count overflows");
          }
          if (event.kind ==
                  GuestExecutionSessionEventKind::kInstructionCoverage &&
              !CheckedAdd(stop_request_guest_instruction_count,
                          event.guest_instruction_delta,
                          &stop_request_guest_instruction_count)) {
            return Fail(error, "session request instruction count overflows");
          }
          if (manifest.boundary.kind ==
                  GuestExecutionSessionBoundaryKind::kGuestMarkerCount &&
              event.kind == GuestExecutionSessionEventKind::kGuestMarker &&
              event.marker_source == manifest.boundary.marker_source &&
              event.marker_identity == manifest.boundary.marker_identity &&
              !CheckedAdd(stop_request_matching_guest_marker_count, 1,
                          &stop_request_matching_guest_marker_count)) {
            return Fail(error, "session request guest-marker count overflows");
          }
        }
        if (in_stop_tail &&
            event.kind ==
                GuestExecutionSessionEventKind::kInstructionCoverage &&
            !CheckedAdd(stop_tail_guest_instruction_count,
                        event.guest_instruction_delta,
                        &stop_tail_guest_instruction_count)) {
          return Fail(error, "session stop-tail instruction count overflows");
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
    } else if (reference.kind ==
               GuestExecutionSessionChunkKind::kContinuousEvents) {
      GuestExecutionContinuousEventLimits continuous_limits;
      continuous_limits.maximum_encoded_bytes = limits.maximum_chunk_bytes;
      continuous_limits.maximum_records = limits.maximum_events_per_chunk;
      std::vector<GuestExecutionContinuousEvent> decoded_events;
      if (!GuestExecutionContinuousEventCodec::Decode(
              encoded, &decoded_events, error, continuous_limits) ||
          !GuestExecutionContinuousEventCodec::ValidateParticipantBindings(
              decoded_events, manifest.participants, error)) {
        return false;
      }
      GuestExecutionSessionChunkReference derived;
      derived.kind = GuestExecutionSessionChunkKind::kContinuousEvents;
      derived.ordinal = reference.ordinal;
      derived.first_event_sequence = decoded_events.front().global_sequence;
      derived.last_event_sequence = decoded_events.back().global_sequence;
      derived.record_count = static_cast<uint32_t>(decoded_events.size());
      derived.encoded_size = encoded.size();
      derived.encoded_sha256 = HashBytes(encoded);
      if (derived != reference ||
          (!continuous_events.empty() &&
           decoded_events.front().global_sequence !=
               continuous_events.back().global_sequence + 1)) {
        return Fail(
            error,
            "continuous event overlay does not match its manifest reference");
      }
      continuous_events.insert(continuous_events.end(),
                               std::make_move_iterator(decoded_events.begin()),
                               std::make_move_iterator(decoded_events.end()));
    } else if (reference.kind ==
               GuestExecutionSessionChunkKind::kSchedulerTopology) {
      GuestExecutionSessionSchedulerTopologyChunk chunk;
      if (!DecodeSchedulerTopologyChunk(encoded, &chunk, error, limits)) {
        return false;
      }
      GuestExecutionSessionChunkReference derived;
      derived.kind = GuestExecutionSessionChunkKind::kSchedulerTopology;
      derived.ordinal = chunk.ordinal;
      derived.first_event_sequence = chunk.global_sequence;
      derived.last_event_sequence = chunk.global_sequence;
      derived.record_count = static_cast<uint32_t>(chunk.participants.size());
      derived.encoded_size = encoded.size();
      derived.encoded_sha256 = HashBytes(encoded);
      const bool is_start =
          chunk.boundary ==
          GuestExecutionSessionSchedulerTopologyBoundary::kStart;
      const bool is_final =
          chunk.boundary ==
          GuestExecutionSessionSchedulerTopologyBoundary::kFinal;
      if (chunk.session_epoch != manifest.session_epoch ||
          derived != reference ||
          (is_start ? chunk.global_sequence != 0
                    : (!is_final || chunk.global_sequence !=
                                        manifest.last_event_sequence)) ||
          (is_start ? saw_start_scheduler_topology
                    : saw_final_scheduler_topology) ||
          chunk.participants.size() != manifest.participants.size()) {
        return Fail(error,
                    "scheduler topology does not match its manifest boundary");
      }
      for (size_t participant_index = 0;
           participant_index < chunk.participants.size(); ++participant_index) {
        const auto& topology = chunk.participants[participant_index];
        const auto& participant = manifest.participants[participant_index];
        if (topology.ordinal != participant.ordinal ||
            topology.guest_thread_id != participant.guest_thread_id ||
            topology.capture_instance_id != participant.capture_instance_id) {
          return Fail(error,
                      "scheduler topology participant differs from roster");
        }
      }
      if (is_start) {
        saw_start_scheduler_topology = true;
      } else {
        saw_final_scheduler_topology = true;
      }
    } else if (reference.kind == GuestExecutionSessionChunkKind::kCheckpoint) {
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
            blob_size_catalog.emplace(state.sha256, state.byte_size);
        if (!inserted && it->second != state.byte_size) {
          return Fail(error, "digest has conflicting canonical byte sizes");
        }
        if (manifest_has_continuous_events &&
            !checkpoint_thread_states
                 .emplace(std::make_pair(chunk.checkpoint.global_sequence,
                                         state.thread_ordinal),
                          state)
                 .second) {
          return Fail(error,
                      "checkpoint participant state identity is duplicated");
        }
      }
      if (manifest_has_continuous_events &&
          chunk.checkpoint.global_sequence == 0) {
        initial_checkpoint_content = chunk.checkpoint.content;
      }
      for (const GuestExecutionSessionContentReference& content :
           chunk.checkpoint.content) {
        const auto [size_it, size_inserted] =
            blob_size_catalog.emplace(content.sha256, content.byte_size);
        if (!size_inserted && size_it->second != content.byte_size) {
          return Fail(error, "digest has conflicting canonical byte sizes");
        }
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
    } else {
      GuestExecutionSessionCodeCorpusChunk chunk;
      if (!DecodeCodeCorpusChunk(encoded, &chunk, error, limits)) {
        return false;
      }
      GuestExecutionSessionChunkReference derived;
      derived.kind = GuestExecutionSessionChunkKind::kCodeCorpus;
      derived.ordinal = chunk.ordinal;
      derived.record_count = 1;
      derived.encoded_size = encoded.size();
      derived.encoded_sha256 = HashBytes(encoded);
      if (chunk.session_epoch != manifest.session_epoch ||
          derived != reference || saw_code_corpus) {
        return Fail(error,
                    "code corpus chunk does not match its manifest reference");
      }
      saw_code_corpus = true;
    }
  }

  if (!continuous_events.empty()) {
    if (continuous_events.size() != canonical_event_identities.size()) {
      return Fail(error,
                  "continuous event overlay does not cover the canonical tape");
    }
    for (size_t event_index = 0; event_index < continuous_events.size();
         ++event_index) {
      const GuestExecutionContinuousEvent& continuous =
          continuous_events[event_index];
      const CanonicalEventIdentity& canonical =
          canonical_event_identities[event_index];
      if (continuous.kind != canonical.kind) {
        return Fail(
            error, "continuous event overlay kind differs from canonical tape");
      }
      if (canonical.thread_ordinal == kGuestExecutionSessionNoThread) {
        if (!IsAbsentIdentity(continuous.actor)) {
          return Fail(error,
                      "continuous event overlay invents a canonical actor");
        }
      } else {
        const GuestExecutionSessionParticipant& canonical_actor =
            manifest.participants[canonical.thread_ordinal];
        if (continuous.actor.participant_ordinal != canonical_actor.ordinal ||
            continuous.actor.guest_thread_id !=
                canonical_actor.guest_thread_id) {
          return Fail(error,
                      "continuous event overlay changes the canonical actor");
        }
      }
      if (continuous.checkpoint.kind ==
          GuestExecutionContinuousCheckpointReferenceKind::kThreadState) {
        const auto state = checkpoint_thread_states.find(
            std::make_pair(continuous.checkpoint.checkpoint_global_sequence,
                           continuous.subject.participant_ordinal));
        if (state == checkpoint_thread_states.end() ||
            state->second.byte_size != continuous.checkpoint.state_size ||
            state->second.sha256 != continuous.checkpoint.state_sha256) {
          return Fail(error,
                      "continuous event checkpoint reference is not closed");
        }
        const ppc::GuestPPCThreadCheckpointBinding& binding =
            continuous.checkpoint.binding;
        if (!IsCodeExtentCovered(initial_checkpoint_content,
                                 binding.owning_function_address,
                                 binding.owning_function_end_address)) {
          return Fail(error,
                      "continuous event checkpoint route is outside captured "
                      "code");
        }
        if (binding.resume_kind ==
            ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern) {
          const uint64_t pending_sequence =
              binding.pending_external_event_sequence;
          if (pending_sequence < manifest.first_event_sequence ||
              pending_sequence > manifest.last_event_sequence) {
            return Fail(error,
                        "continuous event pending extern is outside the tape");
          }
          const uint64_t pending_index =
              pending_sequence - manifest.first_event_sequence;
          if (pending_index >= canonical_event_identities.size()) {
            return Fail(error,
                        "continuous event pending extern is outside the tape");
          }
          const CanonicalEventIdentity& pending_canonical =
              canonical_event_identities[static_cast<size_t>(pending_index)];
          const GuestExecutionContinuousEvent& pending_continuous =
              continuous_events[static_cast<size_t>(pending_index)];
          const GuestExecutionContinuousEventIdentity pending_identity = {
              binding.participant_ordinal, binding.guest_thread_id};
          const bool modeled_extern =
              pending_canonical.kind ==
                  GuestExecutionSessionEventKind::kKernelExport ||
              pending_canonical.kind ==
                  GuestExecutionSessionEventKind::kExternOrBuiltin;
          if (pending_canonical.global_sequence != pending_sequence ||
              pending_continuous.global_sequence != pending_sequence ||
              !modeled_extern ||
              pending_canonical.disposition !=
                  GuestExecutionSessionEventDisposition::kReplayCaptured ||
              pending_canonical.guest_address !=
                  binding.pending_export_guest_address ||
              pending_canonical.thread_ordinal != binding.participant_ordinal ||
              pending_continuous.actor != pending_identity ||
              pending_continuous.subject != pending_identity) {
            return Fail(
                error,
                "continuous event pending extern does not name the modeled "
                "participant event");
          }
        }
      }
    }
  }

  if (supplied_chunk_bytes != manifest_chunk_bytes || !segment_starts.empty() ||
      !segment_ends.empty() || !saw_initial_checkpoint ||
      !saw_final_checkpoint ||
      saw_start_scheduler_topology != saw_final_scheduler_topology ||
      saw_code_corpus != continuous_instruction_coverage) {
    return Fail(error,
                "session chunk closure or segment coverage is incomplete");
  }
  for (size_t i = 0; i < manifest.participants.size(); ++i) {
    const GuestExecutionSessionParticipant& participant =
        manifest.participants[i];
    const ObservedParticipantRange& observed = observed_participant_ranges[i];
    const bool has_no_events = !participant.first_event_sequence;
    const uint32_t expected_arrival_count =
        participant.boundary_arrival_kind ==
                GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside
            ? 0
            : 1;
    const bool expected_outer_call_active =
        participant.boundary_arrival_kind ==
        GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint;
    if ((has_no_events ? observed.has_event
                       : (!observed.has_event ||
                          observed.first_event_sequence !=
                              participant.first_event_sequence ||
                          observed.last_event_sequence !=
                              participant.last_event_sequence)) ||
        observed_arrival_counts[i] != expected_arrival_count ||
        observed.outer_host_call_active != expected_outer_call_active) {
      return Fail(error,
                  "participant event range, arrival, or held state differs");
    }
  }
  if (accepted_event_count != manifest.accepted_event_count ||
      rejected_event_count != manifest.rejected_event_count ||
      unsupported_event_count != manifest.unsupported_event_count) {
    return Fail(error, "session event coverage accounting does not match");
  }
  if (continuous_instruction_coverage &&
      !stop_request_guest_instruction_count) {
    return Fail(
        error,
        "continuous session has no timed participant instruction coverage");
  }
  uint64_t first_held_boundary_sequence = 0;
  if (!saw_boundary_request || !saw_boundary_held ||
      stop_request_accepted_segment_count !=
          manifest.stop_request_accepted_segment_count ||
      stop_request_guest_instruction_count !=
          manifest.stop_request_guest_instruction_count ||
      stop_request_matching_guest_marker_count !=
          manifest.stop_request_matching_guest_marker_count ||
      !CheckedAdd(manifest.stop_request_event_sequence, 1,
                  &first_held_boundary_sequence) ||
      manifest.last_event_sequence < first_held_boundary_sequence) {
    return Fail(error, "session stop-request evidence does not match");
  }
  const uint64_t stop_tail_event_count =
      manifest.last_event_sequence - first_held_boundary_sequence;
  const uint64_t stop_tail_ticks =
      manifest.capture_end_tick - manifest.stop_request_tick;
  if (stop_tail_event_count > manifest.maximum_stop_tail_event_count ||
      stop_tail_guest_instruction_count >
          manifest.maximum_stop_tail_guest_instruction_count ||
      stop_tail_ticks > manifest.maximum_stop_tail_ticks) {
    return Fail(error, "session stop tail exceeds a configured hard maximum");
  }
  if (manifest.rejected_segment_count || rejected_event_count ||
      unsupported_event_count) {
    return Fail(error, "session contains rejected or unsupported work");
  }
  if (IsSafetyStopReason(manifest.stop_reason)) {
    return Fail(error, "session stopped at a safety hard limit");
  }
  switch (manifest.boundary.kind) {
    case GuestExecutionSessionBoundaryKind::kManual:
    case GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds:
      break;
    case GuestExecutionSessionBoundaryKind::kSegmentCount:
      if (request_trigger_kind != GuestExecutionSessionEventKind::kSegmentEnd) {
        return Fail(error,
                    "segment boundary does not immediately precede request");
      }
      break;
    case GuestExecutionSessionBoundaryKind::kGuestMarkerCount:
      if (request_trigger_kind !=
              GuestExecutionSessionEventKind::kGuestMarker ||
          request_trigger_marker_source != manifest.boundary.marker_source ||
          request_trigger_marker_identity !=
              manifest.boundary.marker_identity) {
        return Fail(error,
                    "guest-marker boundary does not immediately precede "
                    "request");
      }
      break;
    case GuestExecutionSessionBoundaryKind::kGuestInstructionCount:
      if (request_trigger_kind !=
          GuestExecutionSessionEventKind::kInstructionCoverage) {
        return Fail(error,
                    "instruction boundary does not immediately precede "
                    "request");
      }
      break;
    default:
      return Fail(error, "session boundary kind is unknown");
  }
  return true;
} catch (const std::bad_alloc&) {
  return Fail(error, "session validation allocation failed");
} catch (const std::length_error&) {
  return Fail(error, "session validation collection size is invalid");
}

}  // namespace cpu
}  // namespace xe
