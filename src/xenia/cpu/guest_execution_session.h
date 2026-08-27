/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace cpu {

using GuestExecutionSessionSha256 = std::array<uint8_t, 32>;

// A capture-time interval policy. Duration selects when capture stops; it does
// not change the artifact or replay time model.
enum class GuestExecutionSessionBoundaryKind : uint32_t {
  kManual = 1,
  kSegmentCount = 2,
  kGuestMarkerCount = 3,
  kGuestInstructionCount = 4,
  kCaptureDurationNanoseconds = 5,
};

enum class GuestExecutionSessionMarkerSource : uint32_t {
  kNone = 0,
  kGuestDefined = 1,
  // A captured guest-command marker, never evidence of a host presentation.
  kPm4Swap = 2,
  kKernel = 3,
  // Version 2 permits this source only when owned by a participant.
  kOtherInstrumented = 4,
};

// The requested policy and the actual stop reason are separate so a safety
// limit can be preserved diagnostically without being accepted as evidence
// that the requested interval was captured.
enum class GuestExecutionSessionStopReason : uint32_t {
  kManualRequest = 1,
  kRequestedBoundary = 2,
  kMaximumSegmentCount = 3,
  kMaximumEventCount = 4,
  kMaximumGuestInstructionCount = 5,
  kMaximumGuestMarkerCount = 6,
  kMaximumDuration = 7,
};

// A fixed capture participant reaches the asynchronous stop rendezvous in
// exactly one of these ways. Version 2 does not support participant lifecycle
// changes while a session is being captured.
enum class GuestExecutionSessionBoundaryArrivalKind : uint32_t {
  kAlreadyOutside = 1,
  kJitSafepoint = 2,
  kOuterHostCallReturn = 3,
};

// Capture may begin while a participant is parked at a real JIT safepoint
// inside an outer host-to-guest call. This state seeds the version-2 outer-call
// validator; it is not inferred from the opaque initial PPC state blob. Native
// host stacks are not serialized or restored: offline replay starts a fresh
// outer dispatch at the participant's captured PPC checkpoint.
enum class GuestExecutionSessionInitialOuterCallState : uint32_t {
  kOutside = 1,
  kActive = 2,
};

struct GuestExecutionSessionBoundaryPolicy {
  GuestExecutionSessionBoundaryKind kind =
      GuestExecutionSessionBoundaryKind::kManual;
  uint64_t value = 0;
  GuestExecutionSessionMarkerSource marker_source =
      GuestExecutionSessionMarkerSource::kNone;
  uint64_t marker_identity = 0;

  bool operator==(const GuestExecutionSessionBoundaryPolicy&) const = default;
};

enum class GuestExecutionSessionChunkKind : uint32_t {
  kEvents = 2,
  kCheckpoint = 3,
};

enum class GuestExecutionSessionEventKind : uint32_t {
  kSegmentBegin = 1,
  kSegmentEnd = 2,
  kThreadDispatch = 3,
  kSynchronization = 4,
  kKernelExport = 5,
  kExternOrBuiltin = 6,
  kMmio = 7,
  kClockOrTimebase = 8,
  kInterrupt = 9,
  kAtomicOrReservation = 10,
  kMemoryMutation = 11,
  kUnsupported = 12,
  // An exact delta of architecturally executed guest PPC instructions. Host
  // instructions, translation and fault/retry work are not included.
  kInstructionCoverage = 13,
  // An instrumented guest-side marker, not evidence of a host presentation.
  kGuestMarker = 14,
  // The timed capture prefix ends at this coordinator request event.
  kBoundaryRequest = 15,
  // The final event, emitted after every participant and external sink is held.
  kBoundaryHeld = 16,
  kOuterHostCallBegin = 17,
  kOuterHostCallEnd = 18,
  kJitSafepointArrival = 19,
};

// The disposition is explicit for every nondeterministic input or mutation.
// kRejectSession is durable diagnostic metadata, but makes a session
// non-replayable rather than allowing an unmodeled event to be ignored.
enum class GuestExecutionSessionEventDisposition : uint32_t {
  kReplayCaptured = 1,
  kValidateDeterministic = 2,
  kRejectSession = 3,
};

enum class GuestExecutionSessionMutationSource : uint32_t {
  kNone = 0,
  kActiveGuestThread = 1,
  kOtherGuestThread = 2,
  kGpu = 3,
  kDma = 4,
  kHost = 5,
};

// Payload bytes are content-addressed and interpreted only according to these
// version-2 formats. No event may smuggle an opaque, unversioned sub-format.
enum class GuestExecutionSessionPayloadKind : uint32_t {
  kNone = 0,
  kGuestBytes = 1,
  kLittleEndianUnsignedInteger = 2,
};

enum class GuestExecutionSessionContentKind : uint32_t {
  kGuestPage = 1,
  kGuestCode = 2,
};

constexpr uint32_t kGuestExecutionSessionNoThread = UINT32_MAX;

// A participant's initial PPC state is stored outside the manifest as an
// opaque, pointer-free content-addressed blob. The initial checkpoint must
// reference the exact same size and digest.
struct GuestExecutionSessionParticipant {
  uint32_t ordinal = 0;
  uint32_t guest_thread_id = 0;
  // Unique for the fixed ThreadState/XThread lifecycle instance in this
  // capture, even if an emulator thread identifier is later reused.
  uint64_t capture_instance_id = 0;
  GuestExecutionSessionInitialOuterCallState initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kOutside;
  GuestExecutionSessionBoundaryArrivalKind boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  // The participant is quiescent immediately after this event. For an
  // already-outside participant this is the no-thread boundary request.
  uint64_t held_after_event_sequence = 0;
  uint64_t initial_state_size = 0;
  GuestExecutionSessionSha256 initial_state_sha256 = {};

  bool operator==(const GuestExecutionSessionParticipant&) const = default;
};

// Ordered segment metadata is independent of the segment producer. The
// segment digest may identify a single-invocation artifact today and a larger
// independently accepted segment later.
struct GuestExecutionSessionSegmentReference {
  uint32_t ordinal = 0;
  uint32_t thread_ordinal = 0;
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint32_t entry_guest_address = 0;
  uint32_t end_guest_address = 0;
  GuestExecutionSessionSha256 code_corpus_sha256 = {};
  GuestExecutionSessionSha256 segment_sha256 = {};

  bool operator==(const GuestExecutionSessionSegmentReference&) const = default;
};

struct GuestExecutionSessionChunkReference {
  GuestExecutionSessionChunkKind kind = GuestExecutionSessionChunkKind::kEvents;
  uint32_t ordinal = 0;
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint32_t record_count = 0;
  uint64_t encoded_size = 0;
  GuestExecutionSessionSha256 encoded_sha256 = {};

  bool operator==(const GuestExecutionSessionChunkReference&) const = default;
};

struct GuestExecutionSessionManifest {
  uint64_t session_epoch = 0;
  GuestExecutionSessionBoundaryPolicy boundary = {};
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint64_t capture_start_tick = 0;
  // Tick at which all participants and external sinks were held and the final
  // checkpoint at kBoundaryHeld was latched. Primary timing ends earlier at
  // stop_request_tick.
  uint64_t capture_end_tick = 0;
  uint64_t capture_tick_frequency = 0;
  GuestExecutionSessionSha256 capture_build_sha256 = {};
  GuestExecutionSessionSha256 replay_config_sha256 = {};
  GuestExecutionSessionSha256 title_identity_sha256 = {};
  GuestExecutionSessionSha256 module_identity_sha256 = {};
  uint64_t accepted_segment_count = 0;
  uint64_t rejected_segment_count = 0;
  uint64_t accepted_event_count = 0;
  uint64_t rejected_event_count = 0;
  uint64_t unsupported_event_count = 0;
  GuestExecutionSessionStopReason stop_reason =
      GuestExecutionSessionStopReason::kManualRequest;
  // The exact no-thread kBoundaryRequest ending the timed capture prefix.
  // Event-driven policies place their triggering event immediately before it.
  uint64_t stop_request_event_sequence = 0;
  // Capture clock tick assigned to that kBoundaryRequest.
  uint64_t stop_request_tick = 0;
  uint64_t stop_request_accepted_segment_count = 0;
  uint64_t stop_request_guest_instruction_count = 0;
  uint64_t stop_request_matching_guest_marker_count = 0;
  // Configured fail-closed maxima for the untimed rendezvous tail. Actual
  // totals are derived from the event stream and capture ticks.
  uint64_t maximum_stop_tail_event_count = 0;
  uint64_t maximum_stop_tail_guest_instruction_count = 0;
  uint64_t maximum_stop_tail_ticks = 0;
  std::vector<GuestExecutionSessionParticipant> participants;
  std::vector<GuestExecutionSessionSegmentReference> segments;
  std::vector<GuestExecutionSessionChunkReference> chunks;

  bool operator==(const GuestExecutionSessionManifest&) const = default;
};

// payload_sha256 identifies the separately stored canonical returned value or
// mutation bytes. A zero payload has an all-zero digest; a nonzero payload must
// have a nonzero digest.
struct GuestExecutionSessionEvent {
  uint64_t global_sequence = 0;
  uint32_t thread_ordinal = kGuestExecutionSessionNoThread;
  GuestExecutionSessionEventKind kind =
      GuestExecutionSessionEventKind::kSegmentBegin;
  GuestExecutionSessionEventDisposition disposition =
      GuestExecutionSessionEventDisposition::kReplayCaptured;
  GuestExecutionSessionMutationSource mutation_source =
      GuestExecutionSessionMutationSource::kNone;
  GuestExecutionSessionPayloadKind payload_kind =
      GuestExecutionSessionPayloadKind::kNone;
  uint64_t guest_address = 0;
  uint64_t byte_count = 0;
  uint64_t payload_size = 0;
  GuestExecutionSessionSha256 payload_sha256 = {};
  // These fields are canonical only for kGuestMarker.
  GuestExecutionSessionMarkerSource marker_source =
      GuestExecutionSessionMarkerSource::kNone;
  uint64_t marker_identity = 0;
  // Canonical and nonzero only for kInstructionCoverage.
  uint64_t guest_instruction_delta = 0;

  bool operator==(const GuestExecutionSessionEvent&) const = default;
};

struct GuestExecutionSessionEventChunk {
  uint64_t session_epoch = 0;
  uint32_t ordinal = 0;
  std::vector<GuestExecutionSessionEvent> events;

  bool operator==(const GuestExecutionSessionEventChunk&) const = default;
};

struct GuestExecutionSessionThreadStateReference {
  uint32_t thread_ordinal = 0;
  uint64_t byte_size = 0;
  GuestExecutionSessionSha256 sha256 = {};

  bool operator==(const GuestExecutionSessionThreadStateReference&) const =
      default;
};

// Guest addresses are serialized as integers rather than host pointers. Page
// references are exactly one aligned 4 KiB page; code references are aligned
// PPC byte ranges. Blob bytes are deliberately outside this metadata codec.
struct GuestExecutionSessionContentReference {
  GuestExecutionSessionContentKind kind =
      GuestExecutionSessionContentKind::kGuestPage;
  uint64_t guest_address = 0;
  uint64_t byte_size = 0;
  GuestExecutionSessionSha256 sha256 = {};

  bool operator==(const GuestExecutionSessionContentReference&) const = default;
};

struct GuestExecutionSessionCheckpoint {
  uint64_t global_sequence = 0;
  std::vector<GuestExecutionSessionThreadStateReference> thread_states;
  std::vector<GuestExecutionSessionContentReference> content;

  bool operator==(const GuestExecutionSessionCheckpoint&) const = default;
};

struct GuestExecutionSessionCheckpointChunk {
  uint64_t session_epoch = 0;
  uint32_t ordinal = 0;
  GuestExecutionSessionCheckpoint checkpoint;

  bool operator==(const GuestExecutionSessionCheckpointChunk&) const = default;
};

// Decoder limits are caller-selectable so capture policy can be stricter than
// the format maxima and tests can prove that limits reject rather than slice.
struct GuestExecutionSessionLimits {
  uint64_t maximum_manifest_bytes = 16ull * 1024ull * 1024ull;
  uint64_t maximum_chunk_bytes = 64ull * 1024ull * 1024ull;
  uint64_t maximum_total_chunk_bytes = 16ull * 1024ull * 1024ull * 1024ull;
  uint64_t maximum_event_payload_bytes = 64ull * 1024ull * 1024ull;
  uint64_t maximum_content_blob_bytes = 64ull * 1024ull * 1024ull;
  uint32_t maximum_participants = 4096;
  uint32_t maximum_segments = 1u << 20;
  uint32_t maximum_chunks = 1u << 20;
  uint32_t maximum_events_per_chunk = 1u << 20;
  uint32_t maximum_checkpoint_thread_states = 4096;
  uint32_t maximum_checkpoint_content_references = 1u << 20;
};

// Portable little-endian version 2 metadata format. Version 1 never produced
// an accepted real-title session and is intentionally rejected after adding
// the participant's initial outer-call state. Every envelope has a header and
// a duplicated closure footer binding its epoch, type, order, sequence range,
// record count, byte count and payload SHA-256. Manifest chunk references
// additionally bind SHA-256 of each complete encoded envelope.
class GuestExecutionSessionCodec {
 public:
  static constexpr uint32_t kVersion = 2;
  static constexpr uint32_t kEnvelopeHeaderSize = 96;
  static constexpr uint32_t kEnvelopeFooterSize = 96;
  static constexpr uint32_t kManifestPayloadHeaderSize = 312;
  static constexpr uint32_t kParticipantRecordSize = 88;
  static constexpr uint32_t kSegmentRecordSize = 96;
  static constexpr uint32_t kChunkReferenceRecordSize = 72;
  static constexpr uint32_t kEventRecordSize = 112;
  static constexpr uint32_t kCheckpointPayloadHeaderSize = 16;
  static constexpr uint32_t kThreadStateReferenceSize = 48;
  static constexpr uint32_t kContentReferenceSize = 56;
  static constexpr uint32_t kGuestPageSize = 4096;

  static GuestExecutionSessionSha256 HashBytes(const uint8_t* data,
                                               size_t data_size);
  static GuestExecutionSessionSha256 HashBytes(
      const std::vector<uint8_t>& data) {
    return HashBytes(data.data(), data.size());
  }

  static bool EncodeManifest(const GuestExecutionSessionManifest& manifest,
                             std::vector<uint8_t>* output,
                             std::string* error = nullptr,
                             GuestExecutionSessionLimits limits = {});
  static bool DecodeManifest(const uint8_t* data, size_t data_size,
                             GuestExecutionSessionManifest* output,
                             std::string* error = nullptr,
                             GuestExecutionSessionLimits limits = {});
  static bool DecodeManifest(const std::vector<uint8_t>& data,
                             GuestExecutionSessionManifest* output,
                             std::string* error = nullptr,
                             GuestExecutionSessionLimits limits = {}) {
    return DecodeManifest(data.data(), data.size(), output, error, limits);
  }

  static bool EncodeEventChunk(const GuestExecutionSessionEventChunk& chunk,
                               std::vector<uint8_t>* output,
                               std::string* error = nullptr,
                               GuestExecutionSessionLimits limits = {});
  static bool DecodeEventChunk(const uint8_t* data, size_t data_size,
                               GuestExecutionSessionEventChunk* output,
                               std::string* error = nullptr,
                               GuestExecutionSessionLimits limits = {});
  static bool DecodeEventChunk(const std::vector<uint8_t>& data,
                               GuestExecutionSessionEventChunk* output,
                               std::string* error = nullptr,
                               GuestExecutionSessionLimits limits = {}) {
    return DecodeEventChunk(data.data(), data.size(), output, error, limits);
  }

  static bool EncodeCheckpointChunk(
      const GuestExecutionSessionCheckpointChunk& chunk,
      std::vector<uint8_t>* output, std::string* error = nullptr,
      GuestExecutionSessionLimits limits = {});
  static bool DecodeCheckpointChunk(
      const uint8_t* data, size_t data_size,
      GuestExecutionSessionCheckpointChunk* output,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {});
  static bool DecodeCheckpointChunk(
      const std::vector<uint8_t>& data,
      GuestExecutionSessionCheckpointChunk* output,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {}) {
    return DecodeCheckpointChunk(data.data(), data.size(), output, error,
                                 limits);
  }

  // Fully decodes and binds each supplied chunk to the corresponding manifest
  // reference. Exact chunk order, global event continuity, initial/final
  // checkpoints, participant state, content-address metadata and reject-event
  // dispositions are all verified before success.
  static bool ValidateSession(
      const GuestExecutionSessionManifest& manifest,
      const std::vector<std::vector<uint8_t>>& encoded_chunks,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {});
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_H_
