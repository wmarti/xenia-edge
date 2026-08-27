/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_CONTINUOUS_EVENT_H_
#define XENIA_CPU_GUEST_EXECUTION_CONTINUOUS_EVENT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_invocation_artifact.h"

namespace xe {
namespace cpu {

// One exact participant lifecycle in the session manifest. The no-participant
// representation is canonical only when both fields have their sentinel value.
struct GuestExecutionContinuousEventIdentity {
  uint32_t participant_ordinal = kGuestExecutionSessionNoThread;
  uint32_t guest_thread_id = 0;

  bool operator==(const GuestExecutionContinuousEventIdentity&) const = default;
};

enum class GuestExecutionContinuousCheckpointReferenceKind : uint32_t {
  kNone = 0,
  kThreadState = 1,
};

// Content-addressed reference to one GuestPPCThreadCheckpointCodec blob. The
// binding is an independent producer declaration: validation must match the
// decoded blob to it and close its owning extent to captured code before a
// replay may attach the state to a ThreadState.
struct GuestExecutionContinuousCheckpointReference {
  GuestExecutionContinuousCheckpointReferenceKind kind =
      GuestExecutionContinuousCheckpointReferenceKind::kNone;
  // The sparse checkpoint whose thread-state catalog owns this blob. Zero is
  // the canonical initial checkpoint and is valid when kind is kThreadState.
  uint64_t checkpoint_global_sequence = 0;
  uint64_t state_size = 0;
  GuestExecutionSessionSha256 state_sha256 = {};
  ppc::GuestPPCThreadCheckpointBinding binding = {};

  bool operator==(const GuestExecutionContinuousCheckpointReference&) const =
      default;
};

// Version-4 control-tape identity for one globally ordered event. Actor is the
// participant executing or publishing the transition; subject is the
// participant affected by it. They may be equal, different, or canonically
// absent independently. A checkpoint reference, when present, always belongs
// to subject.
struct GuestExecutionContinuousEvent {
  uint64_t global_sequence = 0;
  GuestExecutionSessionEventKind kind =
      GuestExecutionSessionEventKind::kSegmentBegin;
  GuestExecutionContinuousEventIdentity actor = {};
  GuestExecutionContinuousEventIdentity subject = {};
  GuestExecutionContinuousCheckpointReference checkpoint = {};

  bool operator==(const GuestExecutionContinuousEvent&) const = default;
};

struct GuestExecutionContinuousEventLimits {
  uint64_t maximum_encoded_bytes = 64ull * 1024ull * 1024ull;
  uint64_t maximum_records = 1ull << 20;
};

// A standalone, fixed-record version-4 control tape. Earlier 96-byte v3 tapes
// and version-2 GuestExecutionSessionCodec artifacts remain distinguishable by
// their own magic/version, but are never accepted here as resumable v4 records.
// Every integer is little-endian; no C++ padding or native pointer is emitted.
class GuestExecutionContinuousEventCodec {
 public:
  static constexpr uint32_t kVersion = 4;
  static constexpr uint32_t kHeaderSize = 64;
  static constexpr uint32_t kRecordSize = 128;
  static constexpr uint32_t kRecordReservedSize = 8;

  // Records must be nonempty and globally contiguous. On failure, output is
  // cleared and error, when non-null, names the rejected invariant.
  static bool Encode(const std::vector<GuestExecutionContinuousEvent>& records,
                     std::vector<uint8_t>* output, std::string* error = nullptr,
                     GuestExecutionContinuousEventLimits limits = {});

  // Decodes through a temporary. On every failure, output is left unchanged,
  // including truncation, trailing data, allocation-independent wire errors,
  // and a non-v4 header.
  static bool Decode(const uint8_t* data, size_t data_size,
                     std::vector<GuestExecutionContinuousEvent>* output,
                     std::string* error = nullptr,
                     GuestExecutionContinuousEventLimits limits = {});
  static bool Decode(const std::vector<uint8_t>& data,
                     std::vector<GuestExecutionContinuousEvent>* output,
                     std::string* error = nullptr,
                     GuestExecutionContinuousEventLimits limits = {}) {
    return Decode(data.data(), data.size(), output, error, limits);
  }

  // Binds every present actor and subject pair to the dense fixed-participant
  // manifest catalog. This does not infer one identity from the other.
  static bool ValidateParticipantBindings(
      const std::vector<GuestExecutionContinuousEvent>& records,
      const std::vector<GuestExecutionSessionParticipant>& participants,
      std::string* error = nullptr);

  // Verifies the event's checkpoint reference, exact bytes and SHA-256, decodes
  // the canonical thread checkpoint through its existing codec, and requires
  // the caller's independent participant/catalog route binding. On failure,
  // output is left unchanged.
  static bool DecodeAndValidateCheckpoint(
      const GuestExecutionContinuousEvent& event, const uint8_t* data,
      size_t data_size, const ppc::GuestPPCThreadCheckpointBinding& binding,
      ppc::GuestPPCThreadCheckpoint* output, std::string* error = nullptr);
  static bool DecodeAndValidateCheckpoint(
      const GuestExecutionContinuousEvent& event,
      const std::vector<uint8_t>& data,
      const ppc::GuestPPCThreadCheckpointBinding& binding,
      ppc::GuestPPCThreadCheckpoint* output, std::string* error = nullptr) {
    return DecodeAndValidateCheckpoint(event, data.data(), data.size(), binding,
                                       output, error);
  }
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_EXECUTION_CONTINUOUS_EVENT_H_
