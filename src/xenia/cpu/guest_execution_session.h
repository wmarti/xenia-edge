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
#include <set>
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
  // Parked below an outer host call the participant never arrived at. A fiber
  // inside a modeled blocking export and one on a native continuation below its
  // own root dispatch are both this shape. No capture produces it yet.
  kParkedBelowOuterCall = 3,
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
  // An auxiliary version-4 actor/subject/checkpoint control overlay. The
  // canonical version-2 event chunks remain authoritative for payloads and
  // dispositions; session validation requires both views to agree exactly on
  // every global sequence and event kind.
  kContinuousEvents = 4,
  // A session-level reference to the exact execution JIT corpus blob. Version
  // 2 continuous sessions require exactly one immediately after the initial
  // checkpoint. Segmented version-2 sessions continue to bind their corpus
  // through each segment reference and reject this extension.
  kCodeCorpus = 5,
  // Versioned start or final cooperative-scheduler topology. These chunks
  // bind the durable participant roster to one authoritative quiescent
  // scheduler snapshot without treating PPC register state as queue state.
  kSchedulerTopology = 6,
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

// The corpus bytes remain a separately stored content-addressed blob. This
// envelope makes that blob part of the canonical continuous-session chunk
// closure without changing the version-2 manifest wire layout. Readers that
// predate this chunk kind reject it as unsupported; current readers reject a
// zero-segment version-2 session that omits it.
struct GuestExecutionSessionCodeCorpusChunk {
  uint64_t session_epoch = 0;
  uint32_t ordinal = 0;
  GuestExecutionSessionSha256 code_corpus_sha256 = {};

  bool operator==(const GuestExecutionSessionCodeCorpusChunk&) const = default;
};

enum class GuestExecutionSessionSchedulerTopologyBoundary : uint32_t {
  kStart = 1,
  kFinal = 2,
};

enum class GuestExecutionSessionSchedulerParticipantState : uint32_t {
  // The participant belongs to the fixed session roster but was not yet owned
  // by any scheduler CPU or queue at this boundary.
  kSchedulerUnowned = 1,
  kRunning = 2,
  kReady = 3,
  kBlocked = 4,
  kSuspended = 5,
};

enum class GuestExecutionSessionSchedulerResumeKind : uint32_t {
  kNone = 0,
  kJitSafepoint = 1,
  kNativeContinuation = 2,
  kAfterBlockingExport = 3,
  kNotYetRun = 4,
};

enum class GuestExecutionSessionSchedulerWaitKind : uint32_t {
  kNone = 0,
  kSingle = 1,
  kMultiAny = 2,
  kMultiAll = 3,
  kDelay = 4,
  kFence = 5,
  kIoOffload = 6,
  kSpinBackoff = 7,
  kIoCompletion = 8,
  kSocketIo = 9,
};

enum GuestExecutionSessionSchedulerWaitFlags : uint32_t {
  kGuestExecutionSessionSchedulerWaitFlagGated = 1u << 0,
  kGuestExecutionSessionSchedulerWaitFlagAlertable = 1u << 1,
  kGuestExecutionSessionSchedulerWaitFlagInterruptible = 1u << 2,
  kGuestExecutionSessionSchedulerWaitFlagUserApcPending = 1u << 3,
};

constexpr size_t kGuestExecutionSessionSchedulerMaximumWaitHandles = 8;
constexpr uint32_t kGuestExecutionSessionSchedulerNoValue = UINT32_MAX;
constexpr uint64_t kGuestExecutionSessionSchedulerNoQuantum = UINT64_MAX;

struct GuestExecutionSessionSchedulerBlockedWaitBinding {
  GuestExecutionSessionSchedulerWaitKind kind =
      GuestExecutionSessionSchedulerWaitKind::kNone;
  uint64_t deadline_ms = 0;
  uint64_t observed_uptime_ms = 0;
  uint32_t wait_epoch = 0;
  uint32_t observed_wait_epoch = 0;
  uint32_t handle_count = 0;
  uint32_t flags = 0;
  std::array<uint32_t, kGuestExecutionSessionSchedulerMaximumWaitHandles>
      handles = {};
  std::array<uint32_t, kGuestExecutionSessionSchedulerMaximumWaitHandles>
      signal_epochs_before = {};
  std::array<uint32_t, kGuestExecutionSessionSchedulerMaximumWaitHandles>
      signal_epochs_observed = {};

  bool operator==(
      const GuestExecutionSessionSchedulerBlockedWaitBinding&) const = default;
};

struct GuestExecutionSessionSchedulerTopologyParticipant {
  uint32_t ordinal = 0;
  uint32_t guest_thread_id = 0;
  uint64_t capture_instance_id = 0;
  GuestExecutionSessionSchedulerParticipantState state =
      GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned;
  uint32_t cpu = kGuestExecutionSessionSchedulerNoValue;
  uint32_t effective_priority = kGuestExecutionSessionSchedulerNoValue;
  // Cooperative-scheduler decay floor. KeSetPriorityThread may make this
  // differ from the guest KTHREAD base queried by KeQueryBasePriorityThread.
  uint32_t base_priority = kGuestExecutionSessionSchedulerNoValue;
  uint32_t suspension_count = kGuestExecutionSessionSchedulerNoValue;
  uint64_t quantum_remaining_us = kGuestExecutionSessionSchedulerNoQuantum;
  uint32_t ready_queue_level = kGuestExecutionSessionSchedulerNoValue;
  uint32_t ready_queue_fifo_ordinal = kGuestExecutionSessionSchedulerNoValue;
  GuestExecutionSessionSchedulerResumeKind resume_kind =
      GuestExecutionSessionSchedulerResumeKind::kNone;
  uint32_t guest_pc = 0;
  bool restorable = false;
  GuestExecutionSessionSchedulerBlockedWaitBinding blocked_wait;

  bool operator==(
      const GuestExecutionSessionSchedulerTopologyParticipant&) const = default;
};

// Names the first field differing between two boundary rows of one scheduler
// topology participant, or nullptr when every serialized field matches.
const char* GuestExecutionSessionSchedulerTopologyFirstDifference(
    const GuestExecutionSessionSchedulerTopologyParticipant& initial,
    const GuestExecutionSessionSchedulerTopologyParticipant& final_row);

// The same comparison across a boundary pair, where the ready FIFO position is
// not a statement about this row: a queue mate inserted at the head or dequeued
// renumbers a row the interval never touched. Ready order is instead carried by
// GuestExecutionSessionSchedulerReadyOrderIsStable.
const char* GuestExecutionSessionSchedulerTopologyPassiveRowFirstDifference(
    const GuestExecutionSessionSchedulerTopologyParticipant& initial,
    const GuestExecutionSessionSchedulerTopologyParticipant& final_row);

// The row-side counterpart of the woken-in-wait checkpoint classifier: a
// participant this boundary carries as ready with no durable resume route, at
// a densified queue position. The wait itself is not serialized for a row that
// is not blocked, so whether it is parked inside a modeled export is a
// capture-side question this row cannot answer.
bool IsGuestExecutionSessionWokenInWaitParticipant(
    const GuestExecutionSessionSchedulerTopologyParticipant& participant);

struct GuestExecutionSessionSchedulerTopologyChunk {
  uint64_t session_epoch = 0;
  uint32_t ordinal = 0;
  GuestExecutionSessionSchedulerTopologyBoundary boundary =
      GuestExecutionSessionSchedulerTopologyBoundary::kStart;
  uint64_t global_sequence = 0;
  std::vector<GuestExecutionSessionSchedulerTopologyParticipant> participants;

  bool operator==(const GuestExecutionSessionSchedulerTopologyChunk&) const =
      default;
};

// Ready FIFO order across a boundary pair, over the participants no scheduler
// record names as its subject. A ready queue is only ever inserted at one of
// its ends or unlinked one node at a time, and every unlink is recorded, so two
// unnamed rows sharing a queue cannot change places however far either absolute
// ordinal moved. A ready-list mutator that splices or sorts would break this.
bool GuestExecutionSessionSchedulerReadyOrderIsStable(
    const GuestExecutionSessionSchedulerTopologyChunk& initial,
    const GuestExecutionSessionSchedulerTopologyChunk& final_topology,
    const std::set<uint32_t>& scheduler_event_subjects, std::string* error);

// A thread parked in a blocking export whose wait this row alone witnesses as
// unsatisfied. Holding at both boundaries of an interval is a separate
// obligation and is not decided here.
bool IsGuestExecutionSessionBlockedParityParticipant(
    const GuestExecutionSessionSchedulerTopologyParticipant& participant);

// Durable scheduler tape identifiers carried inside a canonical
// kThreadDispatch/kSynchronization payload. Only the values a validator has to
// recognize are named here; the capture-side encoder static-asserts them
// against kernel::GuestSchedulerCaptureEventKind and
// kernel::GuestSchedulerCaptureReason so the two cannot drift.
enum class GuestExecutionSessionSchedulerEventKind : uint32_t {
  kNone = 0,
  kBlock = 8,
  kReready = 9,
};

enum class GuestExecutionSessionSchedulerEventReason : uint32_t {
  kNone = 0,
  kPolled = 10,
  kSignalEpoch = 11,
  kDeadline = 12,
  kUserApc = 13,
  kBackstop = 14,
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
  // Bounds aggregate decoded collections, not merely each encoded chunk.
  uint64_t maximum_total_events = 1u << 20;
  uint64_t maximum_total_checkpoint_thread_states = 1u << 20;
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
  static constexpr uint32_t kCodeCorpusPayloadSize = 32;
  static constexpr uint32_t kSchedulerTopologyVersion = 1;
  static constexpr uint32_t kSchedulerTopologyPayloadHeaderSize = 32;
  static constexpr uint32_t kSchedulerTopologyRecordSize = 200;
  static constexpr uint32_t kSchedulerEventPayloadVersion = 2;
  static constexpr uint32_t kSchedulerEventPayloadSize = 192;
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

  static bool EncodeCodeCorpusChunk(
      const GuestExecutionSessionCodeCorpusChunk& chunk,
      std::vector<uint8_t>* output, std::string* error = nullptr,
      GuestExecutionSessionLimits limits = {});
  static bool DecodeCodeCorpusChunk(
      const uint8_t* data, size_t data_size,
      GuestExecutionSessionCodeCorpusChunk* output,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {});
  static bool DecodeCodeCorpusChunk(
      const std::vector<uint8_t>& data,
      GuestExecutionSessionCodeCorpusChunk* output,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {}) {
    return DecodeCodeCorpusChunk(data.data(), data.size(), output, error,
                                 limits);
  }

  static bool EncodeSchedulerTopologyChunk(
      const GuestExecutionSessionSchedulerTopologyChunk& chunk,
      std::vector<uint8_t>* output, std::string* error = nullptr,
      GuestExecutionSessionLimits limits = {});
  static bool DecodeSchedulerTopologyChunk(
      const uint8_t* data, size_t data_size,
      GuestExecutionSessionSchedulerTopologyChunk* output,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {});
  static bool DecodeSchedulerTopologyChunk(
      const std::vector<uint8_t>& data,
      GuestExecutionSessionSchedulerTopologyChunk* output,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {}) {
    return DecodeSchedulerTopologyChunk(data.data(), data.size(), output, error,
                                        limits);
  }

  // Resolves the roster ordinal of the one participant a kThreadDispatch or
  // kSynchronization scheduler payload subjects. Unknown envelope versions,
  // malformed records, kind/label mismatches and off-roster subjects reject.
  static bool ResolveSchedulerEventSubject(
      GuestExecutionSessionEventKind kind, const uint8_t* data,
      size_t data_size,
      const std::vector<GuestExecutionSessionParticipant>& participants,
      uint32_t* subject_ordinal, std::string* error = nullptr,
      uint32_t* record_kind = nullptr, uint32_t* record_reason = nullptr);
  static bool ResolveSchedulerEventSubject(
      GuestExecutionSessionEventKind kind, const std::vector<uint8_t>& data,
      const std::vector<GuestExecutionSessionParticipant>& participants,
      uint32_t* subject_ordinal, std::string* error = nullptr,
      uint32_t* record_kind = nullptr, uint32_t* record_reason = nullptr) {
    return ResolveSchedulerEventSubject(kind, data.data(), data.size(),
                                        participants, subject_ordinal, error,
                                        record_kind, record_reason);
  }

  // Fully decodes and binds each supplied chunk to the corresponding manifest
  // reference. Zero-segment continuous order is initial checkpoint, code
  // corpus, canonical event chunks, optional continuous overlay chunks, final
  // checkpoint. Segmented version-2 order omits the session-level corpus.
  // Global event continuity, participant state, content-address metadata and
  // reject-event dispositions are all verified before success.
  static bool ValidateSession(
      const GuestExecutionSessionManifest& manifest,
      const std::vector<std::vector<uint8_t>>& encoded_chunks,
      std::string* error = nullptr, GuestExecutionSessionLimits limits = {});
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_H_
