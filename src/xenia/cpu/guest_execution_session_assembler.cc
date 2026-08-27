/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_assembler.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "xenia/cpu/execution_jit_corpus.h"

namespace xe {
namespace cpu {

namespace {

using Action = GuestExecutionSessionAssemblerAction;
using Rejection = GuestExecutionSessionAssemblerRejection;
using State = GuestExecutionSessionAssemblerState;

constexpr uint64_t kGuestAddressSpaceSize = uint64_t{1} << 32;
constexpr uint64_t kNanosecondsPerSecond = 1000000000;
constexpr uint32_t kMaximumExternalSinks = 4096;

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

// Mirrors the session codec's duration proof: ceil(ns * frequency / 1e9).
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

bool ValidateGuestRange(uint64_t guest_address, uint64_t byte_count) {
  uint64_t range_end = 0;
  return byte_count && guest_address < kGuestAddressSpaceSize &&
         CheckedAdd(guest_address, byte_count, &range_end) &&
         range_end <= kGuestAddressSpaceSize;
}

bool IsPolicyStopReason(GuestExecutionReelStopReason reason) {
  switch (reason) {
    case GuestExecutionReelStopReason::kManual:
    case GuestExecutionReelStopReason::kSegmentCount:
    case GuestExecutionReelStopReason::kGuestMarkerCount:
    case GuestExecutionReelStopReason::kGuestInstructionCount:
    case GuestExecutionReelStopReason::kCaptureDuration:
      return true;
    default:
      return false;
  }
}

bool ConvertBoundaryKind(GuestExecutionSessionBoundaryKind kind,
                         GuestExecutionReelBoundaryKind* output) {
  switch (kind) {
    case GuestExecutionSessionBoundaryKind::kManual:
      *output = GuestExecutionReelBoundaryKind::kManual;
      return true;
    case GuestExecutionSessionBoundaryKind::kSegmentCount:
      *output = GuestExecutionReelBoundaryKind::kSegmentCount;
      return true;
    case GuestExecutionSessionBoundaryKind::kGuestMarkerCount:
      *output = GuestExecutionReelBoundaryKind::kGuestMarkerCount;
      return true;
    case GuestExecutionSessionBoundaryKind::kGuestInstructionCount:
      *output = GuestExecutionReelBoundaryKind::kGuestInstructionCount;
      return true;
    case GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds:
      *output = GuestExecutionReelBoundaryKind::kCaptureDurationTicks;
      return true;
    default:
      return false;
  }
}

bool ConvertMarkerSource(GuestExecutionSessionMarkerSource source,
                         GuestExecutionReelMarkerSource* output) {
  switch (source) {
    case GuestExecutionSessionMarkerSource::kNone:
      *output = GuestExecutionReelMarkerSource::kNone;
      return true;
    case GuestExecutionSessionMarkerSource::kGuestDefined:
      *output = GuestExecutionReelMarkerSource::kGuestDefined;
      return true;
    case GuestExecutionSessionMarkerSource::kPm4Swap:
      *output = GuestExecutionReelMarkerSource::kPm4Swap;
      return true;
    case GuestExecutionSessionMarkerSource::kKernel:
      *output = GuestExecutionReelMarkerSource::kKernel;
      return true;
    case GuestExecutionSessionMarkerSource::kOtherInstrumented:
      *output = GuestExecutionReelMarkerSource::kOtherInstrumented;
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

bool IsExternalEventKind(GuestExecutionSessionEventKind kind) {
  switch (kind) {
    case GuestExecutionSessionEventKind::kThreadDispatch:
    case GuestExecutionSessionEventKind::kSynchronization:
    case GuestExecutionSessionEventKind::kKernelExport:
    case GuestExecutionSessionEventKind::kExternOrBuiltin:
    case GuestExecutionSessionEventKind::kMmio:
    case GuestExecutionSessionEventKind::kClockOrTimebase:
    case GuestExecutionSessionEventKind::kInterrupt:
    case GuestExecutionSessionEventKind::kAtomicOrReservation:
      return true;
    default:
      return false;
  }
}

bool IsAsynchronousMutationSource(GuestExecutionSessionMutationSource source) {
  return source == GuestExecutionSessionMutationSource::kGpu ||
         source == GuestExecutionSessionMutationSource::kDma ||
         source == GuestExecutionSessionMutationSource::kHost;
}

bool IsGuestThreadMutationSource(GuestExecutionSessionMutationSource source) {
  return source == GuestExecutionSessionMutationSource::kActiveGuestThread ||
         source == GuestExecutionSessionMutationSource::kOtherGuestThread;
}

bool IsPublishedSegmentStatus(const GuestInvocationCaptureStatus& status) {
  return status.state == GuestInvocationCaptureState::kPublished &&
         status.recorder_state ==
             ppc::GuestInvocationRecorderState::kComplete &&
         status.rejection == ppc::GuestInvocationRecorderRejection::kNone &&
         !status.rejected_dependency_flags &&
         status.accepted_segment_count == 1 && !status.rejected_segment_count &&
         status.message.empty();
}

GuestExecutionSessionEvent MakeControlEvent(
    GuestExecutionSessionEventKind kind, uint32_t thread_ordinal,
    GuestExecutionSessionEventDisposition disposition) {
  GuestExecutionSessionEvent event;
  event.kind = kind;
  event.thread_ordinal = thread_ordinal;
  event.disposition = disposition;
  return event;
}

GuestExecutionSessionEvent MakeParticipantControlEvent(
    GuestExecutionSessionEventKind kind, uint32_t thread_ordinal) {
  return MakeControlEvent(
      kind, thread_ordinal,
      GuestExecutionSessionEventDisposition::kValidateDeterministic);
}

GuestExecutionSessionEvent MakeBoundaryEvent(
    GuestExecutionSessionEventKind kind) {
  return MakeControlEvent(
      kind, kGuestExecutionSessionNoThread,
      GuestExecutionSessionEventDisposition::kReplayCaptured);
}

bool BuildReelConfig(const GuestExecutionSessionAssemblerConfig& config,
                     GuestExecutionReelConfig* output, std::string* error) {
  GuestExecutionReelConfig reel_config;
  if (!ConvertBoundaryKind(config.boundary.kind, &reel_config.boundary.kind)) {
    return Fail(error, "capture session boundary kind is unknown");
  }
  if (!ConvertMarkerSource(config.boundary.marker_source,
                           &reel_config.boundary.marker_source)) {
    return Fail(error, "capture session marker source is unknown");
  }
  reel_config.boundary.marker_identity = config.boundary.marker_identity;
  reel_config.boundary.value = config.boundary.value;
  reel_config.coverage_mode = config.coverage_mode;
  if (config.boundary.kind ==
      GuestExecutionSessionBoundaryKind::kCaptureDurationNanoseconds) {
    if (!config.boundary.value ||
        !ComputeDurationTargetTicks(config.boundary.value,
                                    config.capture_tick_frequency,
                                    &reel_config.boundary.value) ||
        !reel_config.boundary.value) {
      return Fail(error, "capture session duration does not convert to ticks");
    }
  }
  reel_config.limits = config.limits;
  reel_config.first_event_sequence = 1;
  reel_config.first_segment_ordinal = 0;
  *output = reel_config;
  return true;
}

}  // namespace

GuestExecutionSessionDirectoryPublisher::
    GuestExecutionSessionDirectoryPublisher(
        std::filesystem::path output_directory,
        GuestExecutionSessionBundleLimits limits)
    : output_directory_(std::move(output_directory)), limits_(limits) {}

bool GuestExecutionSessionDirectoryPublisher::Publish(
    const GuestExecutionSessionBundle& bundle, std::string* error) noexcept {
  try {
    return WriteGuestExecutionSessionBundle(output_directory_, bundle, error,
                                            limits_);
  } catch (...) {
    return Fail(error, "session bundle publication raised an exception");
  }
}

struct GuestExecutionSessionAssembler::Impl {
  struct Participant {
    GuestExecutionCaptureParticipantIdentity identity;
    uint32_t ordinal = 0;
    uint32_t host_call_depth = 0;
    bool arrived = false;
    bool held = false;
    GuestExecutionSessionInitialOuterCallState initial_outer_call_state =
        GuestExecutionSessionInitialOuterCallState::kOutside;
    GuestExecutionSessionBoundaryArrivalKind boundary_arrival_kind =
        GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
    uint64_t first_event_sequence = 0;
    uint64_t last_event_sequence = 0;
    uint64_t held_after_event_sequence = 0;
    uint64_t initial_state_size = 0;
    GuestExecutionSessionSha256 initial_state_sha256 = {};
    // Set while an outer dispatch admitted during the session is active; such
    // a dispatch owns exactly one segment.
    bool dispatch_in_session = false;
    uint32_t dispatch_segment_count = 0;
  };

  struct ExternalSink {
    std::string name;
    bool held = false;
  };

  struct OpenSegment {
    size_t participant_index = 0;
    uint64_t first_event_sequence = 0;
    uint64_t begin_tick = 0;
    uint32_t entry_guest_address = 0;
    uint32_t end_guest_address = 0;
  };

  enum class RecordResult : uint8_t {
    kPrefix,
    kTail,
    kRejected,
  };

  // Recursive so a dependency that reenters the assembler reaches the
  // explicit reentry rejection instead of deadlocking.
  class Scope {
   public:
    explicit Scope(Impl& impl) : impl_(impl), lock_(impl.mutex) {
      reentered_ = impl_.callback_depth != 0;
      ++impl_.callback_depth;
    }
    ~Scope() { --impl_.callback_depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    bool reentered() const { return reentered_; }

   private:
    Impl& impl_;
    std::lock_guard<std::recursive_mutex> lock_;
    bool reentered_ = false;
  };

  Impl(const GuestExecutionSessionAssemblerConfig& config,
       const GuestExecutionSessionAssemblerDependencies& dependencies,
       const GuestExecutionReelConfig& reel_config)
      : config(config), dependencies(dependencies), reel_config(reel_config) {}

  bool IsTerminal() const {
    return state == State::kComplete || state == State::kRejected;
  }

  Action TerminalAction() const {
    return state == State::kRejected ? Action::kReject : Action::kContinue;
  }

  bool IsInSession() const {
    return state == State::kStartRendezvous || state == State::kRecording ||
           state == State::kStopRequested || state == State::kStopRendezvous ||
           state == State::kPublishing;
  }

  Action RejectLocked(Rejection value, std::string text) {
    if (state == State::kRejected) {
      return Action::kReject;
    }
    state = State::kRejected;
    rejection = value;
    message = std::move(text);
    pending_events.clear();
    encoded_chunks.clear();
    blobs.clear();
    encoded_chunk_bytes = 0;
    blob_bytes = 0;
    return Action::kReject;
  }

  Action RejectFromReelLocked() {
    const GuestExecutionReelStatus reel_status = reel->status();
    stop_reason = reel_status.stop_reason;
    Rejection value = Rejection::kReelRejected;
    switch (reel_status.rejection) {
      case GuestExecutionReelRejection::kUnsupportedDependency:
        value = Rejection::kUnsupportedDependency;
        break;
      case GuestExecutionReelRejection::kRejectedSegment:
      case GuestExecutionReelRejection::kInvalidSegmentStatus:
      case GuestExecutionReelRejection::kSegmentOrdering:
        value = Rejection::kRejectedSegment;
        break;
      case GuestExecutionReelRejection::kLimitExceeded:
      case GuestExecutionReelRejection::kBoundaryOvershoot:
      case GuestExecutionReelRejection::kEventSequenceOverflow:
      case GuestExecutionReelRejection::kCounterOverflow:
        value = Rejection::kHardLimit;
        break;
      default:
        break;
    }
    return RejectLocked(value, reel_status.message);
  }

  // Returns false when the call must not proceed; a rejected reentry leaves
  // the session terminal so TerminalAction() reports it.
  bool BeginCallLocked(const Scope& scope, uint64_t* now) {
    if (scope.reentered()) {
      RejectLocked(Rejection::kCallbackReentry,
                   "capture session callback reentered from a dependency");
      return false;
    }
    if (IsTerminal()) {
      return false;
    }
    *now = dependencies.clock->NowTicks();
    if (has_observed_tick && *now < last_observed_tick) {
      RejectLocked(Rejection::kInvalidCall,
                   "capture session tick order regressed");
      return false;
    }
    has_observed_tick = true;
    last_observed_tick = *now;
    return true;
  }

  Participant* FindParticipantLocked(
      const GuestExecutionCaptureParticipantIdentity& identity) {
    for (Participant& participant : participants) {
      if (participant.identity.capture_instance_id ==
          identity.capture_instance_id) {
        if (participant.identity != identity) {
          RejectLocked(Rejection::kInvalidCall,
                       "capture session participant identity mismatch");
          return nullptr;
        }
        return &participant;
      }
    }
    RejectLocked(Rejection::kInvalidCall,
                 "capture session received work from an unknown participant");
    return nullptr;
  }

  size_t IndexOf(const Participant* participant) const {
    return static_cast<size_t>(participant - participants.data());
  }

  bool AddBlobLocked(std::vector<uint8_t> bytes,
                     GuestExecutionSessionSha256* digest, uint64_t* size) {
    const GuestExecutionSessionBundleLimits& limits = config.bundle_limits;
    if (bytes.empty()) {
      RejectLocked(Rejection::kInvalidCall,
                   "capture session content blob is empty");
      return false;
    }
    if (bytes.size() > limits.session.maximum_content_blob_bytes) {
      RejectLocked(Rejection::kStorageLimit,
                   "capture session content blob exceeds the byte limit");
      return false;
    }
    *digest = GuestExecutionSessionCodec::HashBytes(bytes);
    *size = bytes.size();
    if (blobs.count(*digest)) {
      return true;
    }
    uint64_t total_bytes = 0;
    uint64_t bundle_bytes = 0;
    if (blobs.size() >= limits.maximum_content_blobs ||
        !CheckedAdd(blob_bytes, bytes.size(), &total_bytes) ||
        total_bytes > limits.maximum_total_content_bytes ||
        !CheckedAdd(total_bytes, encoded_chunk_bytes, &bundle_bytes) ||
        bundle_bytes > limits.maximum_bundle_bytes) {
      RejectLocked(Rejection::kStorageLimit,
                   "capture session content storage exceeds its limit");
      return false;
    }
    blob_bytes = total_bytes;
    ++blob_count;
    blobs.emplace(*digest, std::move(bytes));
    return true;
  }

  // Chunks are encoded only at publication so no input thread hashes under
  // the session lock; ordinals follow encode order.
  bool AddEncodedChunkLocked(GuestExecutionSessionChunkKind kind,
                             uint64_t first_sequence, uint64_t last_sequence,
                             uint32_t record_count,
                             std::vector<uint8_t> encoded) {
    const GuestExecutionSessionBundleLimits& limits = config.bundle_limits;
    uint64_t total_bytes = 0;
    uint64_t bundle_bytes = 0;
    if (chunk_references.size() >= limits.session.maximum_chunks ||
        !CheckedAdd(encoded_chunk_bytes, encoded.size(), &total_bytes) ||
        total_bytes > limits.session.maximum_total_chunk_bytes ||
        !CheckedAdd(total_bytes, blob_bytes, &bundle_bytes) ||
        bundle_bytes > limits.maximum_bundle_bytes) {
      RejectLocked(Rejection::kStorageLimit,
                   "capture session chunk storage exceeds its limit");
      return false;
    }
    GuestExecutionSessionChunkReference reference;
    reference.kind = kind;
    reference.ordinal = static_cast<uint32_t>(chunk_references.size());
    reference.first_event_sequence = first_sequence;
    reference.last_event_sequence = last_sequence;
    reference.record_count = record_count;
    reference.encoded_size = encoded.size();
    reference.encoded_sha256 = GuestExecutionSessionCodec::HashBytes(encoded);
    encoded_chunk_bytes = total_bytes;
    chunk_references.push_back(reference);
    encoded_chunks.push_back(std::move(encoded));
    return true;
  }

  bool EncodeEventChunksLocked() {
    for (size_t offset = 0; offset < pending_events.size();
         offset += config.maximum_events_per_chunk) {
      const size_t count = std::min<size_t>(config.maximum_events_per_chunk,
                                            pending_events.size() - offset);
      GuestExecutionSessionEventChunk chunk;
      chunk.session_epoch = config.session_epoch;
      chunk.ordinal = static_cast<uint32_t>(chunk_references.size());
      chunk.events.assign(pending_events.begin() + offset,
                          pending_events.begin() + offset + count);
      std::vector<uint8_t> encoded;
      std::string error;
      if (!GuestExecutionSessionCodec::EncodeEventChunk(
              chunk, &encoded, &error, config.bundle_limits.session)) {
        RejectLocked(Rejection::kEncodingFailure, error);
        return false;
      }
      if (!AddEncodedChunkLocked(GuestExecutionSessionChunkKind::kEvents,
                                 chunk.events.front().global_sequence,
                                 chunk.events.back().global_sequence,
                                 static_cast<uint32_t>(count),
                                 std::move(encoded))) {
        return false;
      }
    }
    return true;
  }

  bool EncodeCheckpointChunkLocked(
      const GuestExecutionSessionCheckpoint& checkpoint) {
    GuestExecutionSessionCheckpointChunk chunk;
    chunk.session_epoch = config.session_epoch;
    chunk.ordinal = static_cast<uint32_t>(chunk_references.size());
    chunk.checkpoint = checkpoint;
    std::vector<uint8_t> encoded;
    std::string error;
    if (!GuestExecutionSessionCodec::EncodeCheckpointChunk(
            chunk, &encoded, &error, config.bundle_limits.session)) {
      RejectLocked(Rejection::kEncodingFailure, error);
      return false;
    }
    return AddEncodedChunkLocked(
        GuestExecutionSessionChunkKind::kCheckpoint, checkpoint.global_sequence,
        checkpoint.global_sequence, 1, std::move(encoded));
  }

  bool EncodeCodeCorpusChunkLocked(
      const GuestExecutionSessionSha256& corpus_digest) {
    GuestExecutionSessionCodeCorpusChunk chunk;
    chunk.session_epoch = config.session_epoch;
    chunk.ordinal = static_cast<uint32_t>(chunk_references.size());
    chunk.code_corpus_sha256 = corpus_digest;
    std::vector<uint8_t> encoded;
    std::string error;
    if (!GuestExecutionSessionCodec::EncodeCodeCorpusChunk(
            chunk, &encoded, &error, config.bundle_limits.session)) {
      RejectLocked(Rejection::kEncodingFailure, error);
      return false;
    }
    return AddEncodedChunkLocked(GuestExecutionSessionChunkKind::kCodeCorpus, 0,
                                 0, 1, std::move(encoded));
  }

  bool CollectSessionCodeCorpusLocked(GuestExecutionSessionSha256* digest) {
    const State expected_state = state;
    std::vector<uint8_t> corpus;
    std::string error;
    if (!dependencies.content_provider->CollectSessionCodeCorpus(&corpus,
                                                                 &error)) {
      RejectLocked(Rejection::kContentFailure, error);
      return false;
    }
    if (state != expected_state) {
      return false;
    }
    ExecutionJitCorpus decoded_corpus;
    if (!ExecutionJitCorpus::Decode(corpus, &decoded_corpus, &error)) {
      RejectLocked(Rejection::kContentFailure,
                   "capture session code corpus is invalid: " + error);
      return false;
    }
    uint64_t size = 0;
    if (!AddBlobLocked(std::move(corpus), digest, &size)) {
      return false;
    }
    for (size_t i = 0; i < segments.size(); ++i) {
      if (IsNonzeroHash(segment_corpus_digests[i]) &&
          segment_corpus_digests[i] != *digest) {
        RejectLocked(Rejection::kRejectedSegment,
                     "capture session segment names a different code corpus");
        return false;
      }
      segments[i].code_corpus_sha256 = *digest;
    }
    return true;
  }

  bool AppendEventLocked(GuestExecutionSessionEvent event,
                         Participant* participant) {
    if (next_event_sequence == std::numeric_limits<uint64_t>::max() ||
        next_event_sequence >
            config.bundle_limits.session.maximum_total_events) {
      RejectLocked(Rejection::kHardLimit,
                   "capture session aggregate event limit is exceeded");
      return false;
    }
    event.global_sequence = next_event_sequence;
    try {
      pending_events.push_back(event);
    } catch (const std::bad_alloc&) {
      state = State::kRejected;
      rejection = Rejection::kHardLimit;
      message.clear();
      pending_events.clear();
      encoded_chunks.clear();
      blobs.clear();
      encoded_chunk_bytes = 0;
      blob_bytes = 0;
      return false;
    } catch (const std::length_error&) {
      RejectLocked(Rejection::kHardLimit,
                   "capture session event storage limit is exceeded");
      return false;
    }
    last_event_sequence = next_event_sequence;
    ++next_event_sequence;
    if (participant) {
      if (!participant->first_event_sequence) {
        participant->first_event_sequence = event.global_sequence;
      }
      participant->last_event_sequence = event.global_sequence;
    }
    return true;
  }

  void HoldParticipantLocked(Participant& participant,
                             GuestExecutionSessionBoundaryArrivalKind kind,
                             uint64_t held_after_event_sequence) {
    participant.held = true;
    participant.boundary_arrival_kind = kind;
    participant.held_after_event_sequence = held_after_event_sequence;
    ++held_participant_count;
  }

  bool EmitRequestLocked(const GuestExecutionReelStatus& reel_status) {
    if (state != State::kRecording) {
      RejectLocked(Rejection::kInvalidCall,
                   "capture session stop request outside recording");
      return false;
    }
    session_stop_reason =
        stop_reason == GuestExecutionReelStopReason::kManual
            ? GuestExecutionSessionStopReason::kManualRequest
            : GuestExecutionSessionStopReason::kRequestedBoundary;
    stop_request_event_sequence = next_event_sequence;
    stop_request_tick = reel_status.stop_request_tick;
    stop_request_accepted_segment_count = segments.size();
    stop_request_guest_instruction_count = reel_status.guest_instruction_count;
    stop_request_matching_guest_marker_count = reel_status.guest_marker_count;
    if (!AppendEventLocked(
            MakeBoundaryEvent(GuestExecutionSessionEventKind::kBoundaryRequest),
            nullptr)) {
      return false;
    }
    state = State::kStopRequested;
    for (size_t i = 0; i < participants.size(); ++i) {
      Participant& participant = participants[i];
      participant.held = false;
      if (participant.host_call_depth ||
          &participant == outer_return_in_flight) {
        continue;
      }
      if (open_segment && open_segment->participant_index == i) {
        RejectLocked(Rejection::kInvalidCall,
                     "capture session segment is open outside guest code");
        return false;
      }
      HoldParticipantLocked(
          participant,
          GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside,
          stop_request_event_sequence);
    }
    // Completion waits for the caller so a triggering event lands before
    // the held boundary.
    return state != State::kRejected;
  }

  bool HandleReelStopLocked() {
    const GuestExecutionReelStatus reel_status = reel->status();
    stop_reason = reel_status.stop_reason;
    if (!IsPolicyStopReason(stop_reason)) {
      RejectLocked(Rejection::kHardLimit,
                   "capture session stopped at a safety hard limit");
      return false;
    }
    return EmitRequestLocked(reel_status);
  }

  RecordResult RecordEventLocked(const GuestExecutionSessionEvent& event,
                                 uint64_t tick, uint64_t instruction_delta,
                                 bool matching_marker,
                                 Participant* participant) {
    if (state == State::kRecording) {
      GuestExecutionReelEventRange range;
      const GuestExecutionReelAction action = reel->RecordEvents(
          tick, 1, instruction_delta, matching_marker ? 1 : 0, &range);
      if (action == GuestExecutionReelAction::kReject) {
        RejectFromReelLocked();
        return RecordResult::kRejected;
      }
      if (range.event_count == 1) {
        if (range.first_sequence != next_event_sequence ||
            !CheckedAdd(guest_instruction_count, instruction_delta,
                        &guest_instruction_count)) {
          RejectLocked(Rejection::kInvalidCall,
                       "capture session sequence accounting diverged");
          return RecordResult::kRejected;
        }
        if (!AppendEventLocked(event, participant)) {
          return RecordResult::kRejected;
        }
        // Once the outer return is on the tape its participant is outside.
        outer_return_in_flight = nullptr;
        if (action == GuestExecutionReelAction::kStop) {
          if (!HandleReelStopLocked()) {
            return RecordResult::kRejected;
          }
          TryCompleteStopLocked(tick);
          if (state == State::kRejected) {
            return RecordResult::kRejected;
          }
        }
        return RecordResult::kPrefix;
      }
      // The reel stopped at this tick before admitting the event, so the
      // request precedes it and the event belongs to the stop tail.
      if (!HandleReelStopLocked()) {
        return RecordResult::kRejected;
      }
    }
    if (state != State::kStopRequested && state != State::kStopRendezvous) {
      RejectLocked(Rejection::kInvalidCall,
                   "capture session event outside the capture window");
      return RecordResult::kRejected;
    }
    uint64_t tail_events = 0;
    uint64_t tail_instructions = 0;
    uint64_t total_instructions = 0;
    if (!CheckedAdd(stop_tail_event_count, 1, &tail_events) ||
        tail_events > config.maximum_stop_tail_event_count ||
        !CheckedAdd(stop_tail_guest_instruction_count, instruction_delta,
                    &tail_instructions) ||
        tail_instructions > config.maximum_stop_tail_guest_instruction_count ||
        tick < stop_request_tick ||
        tick - stop_request_tick > config.maximum_stop_tail_ticks ||
        !CheckedAdd(guest_instruction_count, instruction_delta,
                    &total_instructions)) {
      RejectLocked(Rejection::kStopTailLimit,
                   "capture session stop tail exceeds a configured maximum");
      return RecordResult::kRejected;
    }
    if (!AppendEventLocked(event, participant)) {
      return RecordResult::kRejected;
    }
    stop_tail_event_count = tail_events;
    stop_tail_guest_instruction_count = tail_instructions;
    guest_instruction_count = total_instructions;
    TryCompleteStopLocked(tick);
    return state == State::kRejected ? RecordResult::kRejected
                                     : RecordResult::kTail;
  }

  bool CollectContentLocked(
      bool initial_checkpoint,
      std::vector<GuestExecutionSessionContentReference>* output) {
    const State expected_state = state;
    std::vector<GuestExecutionSessionAssemblerContent> entries;
    std::string error;
    if (!dependencies.content_provider->CollectCheckpointContent(
            initial_checkpoint, &entries, &error)) {
      RejectLocked(Rejection::kContentFailure, error);
      return false;
    }
    if (state != expected_state) {
      return false;
    }
    if (entries.size() >
        config.bundle_limits.session.maximum_checkpoint_content_references) {
      RejectLocked(Rejection::kStorageLimit,
                   "capture session checkpoint content exceeds its limit");
      return false;
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const GuestExecutionSessionAssemblerContent& left,
                        const GuestExecutionSessionAssemblerContent& right) {
                       return left.guest_address < right.guest_address;
                     });
    uint64_t previous_end = 0;
    bool has_previous = false;
    output->clear();
    for (GuestExecutionSessionAssemblerContent& entry : entries) {
      const uint64_t byte_size = entry.bytes.size();
      bool canonical = ValidateGuestRange(entry.guest_address, byte_size);
      if (entry.kind == GuestExecutionSessionContentKind::kGuestPage) {
        canonical = canonical &&
                    !(entry.guest_address &
                      (GuestExecutionSessionCodec::kGuestPageSize - 1)) &&
                    byte_size == GuestExecutionSessionCodec::kGuestPageSize;
      } else if (entry.kind == GuestExecutionSessionContentKind::kGuestCode) {
        canonical = canonical && !(entry.guest_address & 3) && !(byte_size & 3);
      } else {
        canonical = false;
      }
      if (!canonical || (has_previous && entry.guest_address < previous_end)) {
        RejectLocked(Rejection::kContentFailure,
                     "capture session checkpoint content is not canonical");
        return false;
      }
      previous_end = entry.guest_address + byte_size;
      has_previous = true;
      GuestExecutionSessionContentReference reference;
      reference.kind = entry.kind;
      reference.guest_address = entry.guest_address;
      if (!AddBlobLocked(std::move(entry.bytes), &reference.sha256,
                         &reference.byte_size)) {
        return false;
      }
      output->push_back(reference);
    }
    return true;
  }

  bool EncodeParticipantStatesLocked(
      bool initial_checkpoint,
      std::vector<GuestExecutionSessionThreadStateReference>* output) {
    const State expected_state = state;
    output->clear();
    for (Participant& participant : participants) {
      std::vector<uint8_t> bytes;
      std::string error;
      if (!dependencies.state_provider->EncodeParticipantState(
              participant.identity, &bytes, &error)) {
        RejectLocked(Rejection::kCheckpointFailure, error);
        return false;
      }
      if (state != expected_state) {
        return false;
      }
      if (!initial_checkpoint &&
          bytes.size() != participant.initial_state_size) {
        RejectLocked(Rejection::kCheckpointFailure,
                     "capture session participant state size changed");
        return false;
      }
      GuestExecutionSessionThreadStateReference reference;
      reference.thread_ordinal = participant.ordinal;
      if (!AddBlobLocked(std::move(bytes), &reference.sha256,
                         &reference.byte_size)) {
        return false;
      }
      if (initial_checkpoint) {
        participant.initial_state_size = reference.byte_size;
        participant.initial_state_sha256 = reference.sha256;
      }
      output->push_back(reference);
    }
    return true;
  }

  void TryCompleteStartLocked(uint64_t now) {
    if (state != State::kStartRendezvous ||
        arrived_participant_count != participants.size() ||
        held_sink_count != external_sinks.size()) {
      return;
    }
    // Only participant state is captured now; the initial checkpoint's page
    // preimages accumulate over the session and are encoded at stop.
    if (!EncodeParticipantStatesLocked(true, &initial_thread_states)) {
      return;
    }
    std::string error;
    reel = GuestExecutionReelCollector::Create(reel_config, now, &error);
    if (!reel) {
      RejectLocked(Rejection::kInvalidCall, error);
      return;
    }
    capture_start_tick = now;
    for (ExternalSink& sink : external_sinks) {
      sink.held = false;
    }
    held_sink_count = 0;
    state = State::kRecording;
  }

  void TryCompleteStopLocked(uint64_t now) {
    if (state == State::kStopRequested &&
        held_participant_count == participants.size()) {
      state = State::kStopRendezvous;
    }
    if (state != State::kStopRendezvous ||
        held_sink_count != external_sinks.size()) {
      return;
    }
    if (now < stop_request_tick ||
        now - stop_request_tick > config.maximum_stop_tail_ticks) {
      RejectLocked(Rejection::kStopTailLimit,
                   "capture session stop tail exceeds its tick maximum");
      return;
    }
    if (open_segment) {
      RejectLocked(Rejection::kInvalidCall,
                   "capture session segment is open at the held boundary");
      return;
    }
    if (!AppendEventLocked(
            MakeBoundaryEvent(GuestExecutionSessionEventKind::kBoundaryHeld),
            nullptr)) {
      return;
    }
    // Providers and encoding wait for Publish() so the last-arriving thread
    // does no checkpoint work; every input stays held until then.
    std::string error;
    if (!reel->Complete(now, &error)) {
      RejectLocked(Rejection::kReelRejected, error);
      return;
    }
    capture_end_tick = now;
    state = State::kPublishing;
  }

  bool AssembleBundleLocked(GuestExecutionSessionBundle* bundle) {
    const bool continuous_coverage =
        config.coverage_mode ==
        GuestExecutionReelCoverageMode::kContinuousInstructions;
    if (continuous_coverage != segments.empty()) {
      RejectLocked(Rejection::kInvalidCall,
                   "capture session coverage mode and segment shape diverged");
      return false;
    }
    GuestExecutionSessionCheckpoint initial;
    initial.global_sequence = 0;
    initial.thread_states = initial_thread_states;
    GuestExecutionSessionCheckpoint final_checkpoint;
    final_checkpoint.global_sequence = last_event_sequence;
    GuestExecutionSessionSha256 corpus_digest = {};
    if (!EncodeParticipantStatesLocked(false,
                                       &final_checkpoint.thread_states) ||
        !CollectContentLocked(true, &initial.content) ||
        !CollectContentLocked(false, &final_checkpoint.content) ||
        !CollectSessionCodeCorpusLocked(&corpus_digest) ||
        !EncodeCheckpointChunkLocked(initial) ||
        (continuous_coverage && !EncodeCodeCorpusChunkLocked(corpus_digest)) ||
        !EncodeEventChunksLocked() ||
        !EncodeCheckpointChunkLocked(final_checkpoint)) {
      return false;
    }
    bundle->manifest = BuildManifestLocked();
    bundle->chunks = std::move(encoded_chunks);
    encoded_chunks.clear();
    for (auto& [digest, bytes] : blobs) {
      GuestExecutionSessionContentBlob blob;
      blob.sha256 = digest;
      blob.bytes = std::move(bytes);
      bundle->content_blobs.push_back(std::move(blob));
    }
    blobs.clear();
    pending_events.clear();
    std::string error;
    std::vector<uint8_t> manifest_bytes;
    if (!ValidateGuestExecutionSessionBundle(*bundle, &error,
                                             config.bundle_limits) ||
        !GuestExecutionSessionCodec::EncodeManifest(
            bundle->manifest, &manifest_bytes, &error,
            config.bundle_limits.session)) {
      RejectLocked(Rejection::kEncodingFailure,
                   "capture session failed validation: " + error);
      return false;
    }
    uint64_t bundle_bytes = 0;
    if (!CheckedAdd(encoded_chunk_bytes, blob_bytes, &bundle_bytes) ||
        !CheckedAdd(bundle_bytes, manifest_bytes.size(), &bundle_bytes) ||
        bundle_bytes > config.bundle_limits.maximum_bundle_bytes) {
      RejectLocked(Rejection::kStorageLimit,
                   "capture session bundle exceeds its byte limit");
      return false;
    }
    return true;
  }

  GuestExecutionSessionManifest BuildManifestLocked() const {
    GuestExecutionSessionManifest manifest;
    manifest.session_epoch = config.session_epoch;
    manifest.boundary = config.boundary;
    manifest.first_event_sequence = 1;
    manifest.last_event_sequence = last_event_sequence;
    manifest.capture_start_tick = capture_start_tick;
    manifest.capture_end_tick = capture_end_tick;
    manifest.capture_tick_frequency = config.capture_tick_frequency;
    manifest.capture_build_sha256 = config.capture_build_sha256;
    manifest.replay_config_sha256 = config.replay_config_sha256;
    manifest.title_identity_sha256 = config.title_identity_sha256;
    manifest.module_identity_sha256 = config.module_identity_sha256;
    manifest.accepted_segment_count = segments.size();
    manifest.accepted_event_count = last_event_sequence;
    manifest.stop_reason = session_stop_reason;
    manifest.stop_request_event_sequence = stop_request_event_sequence;
    manifest.stop_request_tick = stop_request_tick;
    manifest.stop_request_accepted_segment_count =
        stop_request_accepted_segment_count;
    manifest.stop_request_guest_instruction_count =
        stop_request_guest_instruction_count;
    manifest.stop_request_matching_guest_marker_count =
        config.boundary.kind ==
                GuestExecutionSessionBoundaryKind::kGuestMarkerCount
            ? stop_request_matching_guest_marker_count
            : 0;
    manifest.maximum_stop_tail_event_count =
        config.maximum_stop_tail_event_count;
    manifest.maximum_stop_tail_guest_instruction_count =
        config.maximum_stop_tail_guest_instruction_count;
    manifest.maximum_stop_tail_ticks = config.maximum_stop_tail_ticks;
    for (const Participant& participant : participants) {
      GuestExecutionSessionParticipant record;
      record.ordinal = participant.ordinal;
      record.guest_thread_id = participant.identity.guest_thread_id;
      record.capture_instance_id = participant.identity.capture_instance_id;
      record.initial_outer_call_state = participant.initial_outer_call_state;
      record.boundary_arrival_kind = participant.boundary_arrival_kind;
      record.first_event_sequence = participant.first_event_sequence;
      record.last_event_sequence = participant.last_event_sequence;
      record.held_after_event_sequence = participant.held_after_event_sequence;
      record.initial_state_size = participant.initial_state_size;
      record.initial_state_sha256 = participant.initial_state_sha256;
      manifest.participants.push_back(record);
    }
    manifest.segments = segments;
    manifest.chunks = chunk_references;
    return manifest;
  }

  Action RendezvousActionLocked() const {
    if (state == State::kRejected) {
      return Action::kReject;
    }
    return state == State::kRecording ? Action::kContinue : Action::kHold;
  }

  mutable std::recursive_mutex mutex;
  uint32_t callback_depth = 0;

  const GuestExecutionSessionAssemblerConfig config;
  const GuestExecutionSessionAssemblerDependencies dependencies;
  const GuestExecutionReelConfig reel_config;

  State state = State::kIdle;
  Rejection rejection = Rejection::kNone;
  std::string message;
  GuestExecutionReelStopReason stop_reason =
      GuestExecutionReelStopReason::kNone;
  GuestExecutionSessionStopReason session_stop_reason =
      GuestExecutionSessionStopReason::kManualRequest;

  std::vector<Participant> participants;
  bool seeded = false;
  std::vector<ExternalSink> external_sinks;
  uint32_t arrived_participant_count = 0;
  uint32_t held_participant_count = 0;
  uint32_t held_sink_count = 0;
  const Participant* outer_return_in_flight = nullptr;

  std::unique_ptr<GuestExecutionReelCollector> reel;
  bool has_observed_tick = false;
  uint64_t last_observed_tick = 0;
  uint64_t start_request_tick = 0;
  uint64_t capture_start_tick = 0;
  uint64_t stop_request_tick = 0;
  uint64_t capture_end_tick = 0;

  uint64_t next_event_sequence = 1;
  uint64_t last_event_sequence = 0;
  uint64_t stop_request_event_sequence = 0;
  uint64_t stop_request_accepted_segment_count = 0;
  uint64_t stop_request_guest_instruction_count = 0;
  uint64_t stop_request_matching_guest_marker_count = 0;
  uint64_t guest_instruction_count = 0;
  uint64_t stop_tail_event_count = 0;
  uint64_t stop_tail_guest_instruction_count = 0;

  std::vector<GuestExecutionSessionEvent> pending_events;
  std::vector<GuestExecutionSessionThreadStateReference> initial_thread_states;
  std::vector<std::vector<uint8_t>> encoded_chunks;
  std::vector<GuestExecutionSessionChunkReference> chunk_references;
  uint64_t encoded_chunk_bytes = 0;
  std::vector<GuestExecutionSessionSha256> segment_corpus_digests;
  std::map<GuestExecutionSessionSha256, std::vector<uint8_t>> blobs;
  uint64_t blob_count = 0;
  uint64_t blob_bytes = 0;
  std::vector<GuestExecutionSessionSegmentReference> segments;
  std::optional<OpenSegment> open_segment;
  bool publish_in_progress = false;
  bool published = false;
};

std::unique_ptr<GuestExecutionSessionAssembler>
GuestExecutionSessionAssembler::Create(
    const GuestExecutionSessionAssemblerConfig& config,
    const GuestExecutionSessionAssemblerDependencies& dependencies,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!dependencies.clock || !dependencies.state_provider ||
      !dependencies.content_provider || !dependencies.publisher) {
    Fail(error, "capture session dependencies are missing");
    return nullptr;
  }
  if (!config.session_epoch) {
    Fail(error, "capture session epoch must be nonzero");
    return nullptr;
  }
  if (!config.capture_tick_frequency) {
    Fail(error, "capture session tick frequency must be nonzero");
    return nullptr;
  }
  if (!config.maximum_stop_tail_event_count ||
      !config.maximum_stop_tail_guest_instruction_count ||
      !config.maximum_stop_tail_ticks ||
      !config.maximum_start_rendezvous_ticks) {
    Fail(error, "capture session rendezvous maxima must all be nonzero");
    return nullptr;
  }
  if (!IsNonzeroHash(config.capture_build_sha256) ||
      !IsNonzeroHash(config.replay_config_sha256) ||
      !IsNonzeroHash(config.title_identity_sha256) ||
      !IsNonzeroHash(config.module_identity_sha256)) {
    Fail(error, "capture session build/config/title/module SHA-256 is missing");
    return nullptr;
  }
  const GuestExecutionSessionLimits& session_limits =
      config.bundle_limits.session;
  if (!session_limits.maximum_total_events ||
      config.limits.maximum_event_count > session_limits.maximum_total_events ||
      !session_limits.maximum_total_checkpoint_thread_states) {
    Fail(error,
         "capture session aggregate event/checkpoint limits are out of range");
    return nullptr;
  }
  uint64_t chunk_payload_bytes = 0;
  if (!config.maximum_events_per_chunk ||
      config.maximum_events_per_chunk >
          session_limits.maximum_events_per_chunk ||
      !CheckedMultiply(config.maximum_events_per_chunk,
                       GuestExecutionSessionCodec::kEventRecordSize,
                       &chunk_payload_bytes) ||
      chunk_payload_bytes + GuestExecutionSessionCodec::kEnvelopeHeaderSize +
              GuestExecutionSessionCodec::kEnvelopeFooterSize >
          session_limits.maximum_chunk_bytes) {
    Fail(error, "capture session events per chunk are out of range");
    return nullptr;
  }
  GuestExecutionReelConfig reel_config;
  if (!BuildReelConfig(config, &reel_config, error)) {
    return nullptr;
  }
  // Probe the reel policy now so a bad boundary fails before any roster.
  if (!GuestExecutionReelCollector::Create(reel_config, 0, error)) {
    return nullptr;
  }
  return std::unique_ptr<GuestExecutionSessionAssembler>(
      new GuestExecutionSessionAssembler(
          std::make_unique<Impl>(config, dependencies, reel_config)));
}

GuestExecutionSessionAssembler::GuestExecutionSessionAssembler(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestExecutionSessionAssembler::~GuestExecutionSessionAssembler() = default;

bool GuestExecutionSessionAssembler::SeedParticipants(
    std::span<const GuestExecutionCaptureThreadStateLifecycleEvent> seeds,
    const GuestExecutionCaptureHostCallRosterSnapshot& roster) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return false;
  }
  if (impl_->state != State::kIdle || impl_->seeded ||
      !impl_->participants.empty()) {
    impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session roster can be seeded only once while idle");
    return false;
  }
  if (seeds.empty() ||
      seeds.size() > impl_->config.bundle_limits.session.maximum_participants ||
      roster.rejection != GuestExecutionCaptureHostCallRosterRejection::kNone) {
    return false;
  }
  if (seeds.size() > impl_->config.bundle_limits.session
                             .maximum_total_checkpoint_thread_states /
                         2) {
    impl_->RejectLocked(
        Rejection::kHardLimit,
        "capture session checkpoint participant total exceeds its limit");
    return false;
  }
  for (size_t i = 0; i < seeds.size(); ++i) {
    const GuestExecutionCaptureParticipantIdentity& identity =
        seeds[i].participant;
    if (seeds[i].state !=
            GuestExecutionCaptureThreadStateLifecycleState::kReady ||
        !identity.capture_instance_id || !identity.guest_thread_id) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (seeds[j].participant.capture_instance_id ==
              identity.capture_instance_id ||
          seeds[j].participant.guest_thread_id == identity.guest_thread_id) {
        return false;
      }
    }
  }
  std::vector<Impl::Participant> participants;
  for (const GuestExecutionCaptureThreadStateLifecycleEvent& seed : seeds) {
    Impl::Participant participant;
    participant.identity = seed.participant;
    participants.push_back(participant);
  }
  // Depth comes from the roster taken in the same transaction; every active
  // call must belong to a seeded participant and nest contiguously.
  for (const GuestExecutionCaptureActiveHostCall& call : roster.active_calls) {
    Impl::Participant* owner = nullptr;
    for (Impl::Participant& participant : participants) {
      if (participant.identity == call.participant) {
        owner = &participant;
        break;
      }
    }
    if (!owner || !call.participant_depth ||
        call.participant_depth != owner->host_call_depth + 1) {
      return false;
    }
    owner->host_call_depth = call.participant_depth;
  }
  impl_->participants = std::move(participants);
  impl_->seeded = true;
  return true;
}

Action GuestExecutionSessionAssembler::OnParticipantLifecycle(
    const GuestExecutionCaptureThreadStateLifecycleEvent& event) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  if (impl_->state != State::kIdle) {
    return impl_->RejectLocked(
        Rejection::kLifecycleChanged,
        "capture session participant lifecycle changed after arming");
  }
  const GuestExecutionCaptureParticipantIdentity& identity = event.participant;
  switch (event.state) {
    case GuestExecutionCaptureThreadStateLifecycleState::kPending:
      return Action::kContinue;
    case GuestExecutionCaptureThreadStateLifecycleState::kReady: {
      if (!identity.capture_instance_id || !identity.guest_thread_id) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session participant identity is invalid");
      }
      for (const Impl::Participant& participant : impl_->participants) {
        if (participant.identity.capture_instance_id ==
                identity.capture_instance_id ||
            participant.identity.guest_thread_id == identity.guest_thread_id) {
          return impl_->RejectLocked(
              Rejection::kLifecycleChanged,
              "capture session participant registered twice");
        }
      }
      if (impl_->participants.size() >=
          impl_->config.bundle_limits.session.maximum_participants) {
        return impl_->RejectLocked(
            Rejection::kHardLimit,
            "capture session participant count exceeds its limit");
      }
      if (impl_->participants.size() >=
          impl_->config.bundle_limits.session
                  .maximum_total_checkpoint_thread_states /
              2) {
        return impl_->RejectLocked(
            Rejection::kHardLimit,
            "capture session checkpoint participant total exceeds its limit");
      }
      Impl::Participant participant;
      participant.identity = identity;
      impl_->participants.push_back(participant);
      return Action::kContinue;
    }
    case GuestExecutionCaptureThreadStateLifecycleState::kDestroying: {
      const auto it =
          std::find_if(impl_->participants.begin(), impl_->participants.end(),
                       [&identity](const Impl::Participant& participant) {
                         return participant.identity.capture_instance_id ==
                                identity.capture_instance_id;
                       });
      if (it == impl_->participants.end()) {
        return impl_->RejectLocked(
            Rejection::kLifecycleChanged,
            "capture session unknown participant destroyed");
      }
      if (it->host_call_depth) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session participant destroyed inside a host call");
      }
      impl_->participants.erase(it);
      return Action::kContinue;
    }
    default:
      return impl_->RejectLocked(
          Rejection::kInvalidCall,
          "capture session participant lifecycle state is unknown");
  }
}

bool GuestExecutionSessionAssembler::RegisterExternalSink(
    std::string name, uint32_t* out_ordinal) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return false;
  }
  if (impl_->state != State::kIdle || name.empty() || !out_ordinal) {
    impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session external sinks register only while idle");
    return false;
  }
  if (impl_->external_sinks.size() >= kMaximumExternalSinks) {
    impl_->RejectLocked(
        Rejection::kHardLimit,
        "capture session external sink count exceeds its limit");
    return false;
  }
  *out_ordinal = static_cast<uint32_t>(impl_->external_sinks.size());
  impl_->external_sinks.push_back({std::move(name), false});
  return true;
}

bool GuestExecutionSessionAssembler::Arm(std::string* error) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return Fail(error, impl_->message);
  }
  if (impl_->state != State::kIdle) {
    impl_->RejectLocked(Rejection::kInvalidCall,
                        "capture session can be armed only while idle");
    return Fail(error, impl_->message);
  }
  if (impl_->participants.empty()) {
    impl_->RejectLocked(Rejection::kInvalidCall,
                        "capture session requires at least one participant");
    return Fail(error, impl_->message);
  }
  if (impl_->participants.size() >
      impl_->config.bundle_limits.session.maximum_checkpoint_thread_states) {
    impl_->RejectLocked(Rejection::kHardLimit,
                        "capture session participant count exceeds its limit");
    return Fail(error, impl_->message);
  }
  if (impl_->config.pm4_marker_sink_ordinal != kNoExternalSink &&
      impl_->config.pm4_marker_sink_ordinal >= impl_->external_sinks.size()) {
    impl_->RejectLocked(Rejection::kInvalidCall,
                        "capture session PM4 marker sink is not registered");
    return Fail(error, impl_->message);
  }
  std::sort(impl_->participants.begin(), impl_->participants.end(),
            [](const Impl::Participant& left, const Impl::Participant& right) {
              return left.identity.capture_instance_id <
                     right.identity.capture_instance_id;
            });
  for (size_t i = 0; i < impl_->participants.size(); ++i) {
    impl_->participants[i].ordinal = static_cast<uint32_t>(i);
  }
  impl_->state = State::kArming;
  return true;
}

bool GuestExecutionSessionAssembler::RequestStart(std::string* error) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return Fail(error, impl_->message);
  }
  if (impl_->state != State::kArming) {
    impl_->RejectLocked(Rejection::kInvalidCall,
                        "capture session start requires an armed session");
    return Fail(error, impl_->message);
  }
  impl_->start_request_tick = now;
  impl_->arrived_participant_count = 0;
  for (Impl::Participant& participant : impl_->participants) {
    participant.arrived = participant.host_call_depth == 0;
    if (participant.arrived) {
      participant.initial_outer_call_state =
          GuestExecutionSessionInitialOuterCallState::kOutside;
      ++impl_->arrived_participant_count;
    }
  }
  for (Impl::ExternalSink& sink : impl_->external_sinks) {
    sink.held = false;
  }
  impl_->held_sink_count = 0;
  impl_->state = State::kStartRendezvous;
  impl_->TryCompleteStartLocked(now);
  if (impl_->state == State::kRejected) {
    return Fail(error, impl_->message);
  }
  return true;
}

Action GuestExecutionSessionAssembler::RequestStop() {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  switch (impl_->state) {
    case State::kRecording: {
      const GuestExecutionReelAction action =
          impl_->reel->RequestManualStop(now);
      if (action == GuestExecutionReelAction::kReject) {
        return impl_->RejectFromReelLocked();
      }
      if (!impl_->HandleReelStopLocked()) {
        return Action::kReject;
      }
      impl_->TryCompleteStopLocked(now);
      return impl_->RendezvousActionLocked();
    }
    case State::kStopRequested:
    case State::kStopRendezvous:
    case State::kPublishing:
      return Action::kHold;
    default:
      return impl_->RejectLocked(
          Rejection::kInvalidCall,
          "capture session manual stop outside recording");
  }
}

Action GuestExecutionSessionAssembler::Poll() {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  switch (impl_->state) {
    case State::kStartRendezvous:
      if (now - impl_->start_request_tick >
          impl_->config.maximum_start_rendezvous_ticks) {
        return impl_->RejectLocked(
            Rejection::kStartRendezvousTimeout,
            "capture session start rendezvous timed out");
      }
      return Action::kHold;
    case State::kRecording: {
      const GuestExecutionReelAction action = impl_->reel->Poll(now);
      if (action == GuestExecutionReelAction::kReject) {
        return impl_->RejectFromReelLocked();
      }
      if (action == GuestExecutionReelAction::kStop) {
        if (!impl_->HandleReelStopLocked()) {
          return Action::kReject;
        }
        impl_->TryCompleteStopLocked(now);
      }
      return impl_->RendezvousActionLocked();
    }
    case State::kStopRequested:
    case State::kStopRendezvous:
      if (now - impl_->stop_request_tick >
          impl_->config.maximum_stop_tail_ticks) {
        return impl_->RejectLocked(
            Rejection::kStopTailLimit,
            "capture session stop tail exceeds its tick maximum");
      }
      return Action::kHold;
    case State::kPublishing:
      return Action::kHold;
    default:
      return Action::kContinue;
  }
}

Action GuestExecutionSessionAssembler::OnExternalSinkHeld(
    uint32_t sink_ordinal) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  if (sink_ordinal >= impl_->external_sinks.size()) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session external sink is unknown");
  }
  Impl::ExternalSink& sink = impl_->external_sinks[sink_ordinal];
  switch (impl_->state) {
    case State::kStartRendezvous:
      if (!sink.held) {
        sink.held = true;
        ++impl_->held_sink_count;
        impl_->TryCompleteStartLocked(now);
      }
      return impl_->RendezvousActionLocked();
    case State::kStopRequested:
    case State::kStopRendezvous:
      if (!sink.held) {
        sink.held = true;
        ++impl_->held_sink_count;
        impl_->TryCompleteStopLocked(now);
      }
      return impl_->RendezvousActionLocked();
    case State::kPublishing:
      return Action::kHold;
    default:
      return Action::kContinue;
  }
}

Action GuestExecutionSessionAssembler::ArriveAtSafepoint(
    const GuestExecutionCaptureParticipantIdentity& identity) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  Impl::Participant* participant = impl_->FindParticipantLocked(identity);
  if (!participant) {
    return Action::kReject;
  }
  switch (impl_->state) {
    case State::kStartRendezvous:
      if (participant->arrived) {
        return impl_->RejectLocked(
            Rejection::kParticipantNotHeld,
            "capture session parked participant reached a safepoint");
      }
      if (!participant->host_call_depth) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session safepoint outside guest code");
      }
      participant->arrived = true;
      participant->initial_outer_call_state =
          GuestExecutionSessionInitialOuterCallState::kActive;
      ++impl_->arrived_participant_count;
      impl_->TryCompleteStartLocked(now);
      return impl_->RendezvousActionLocked();
    case State::kStopRequested: {
      if (participant->held) {
        return impl_->RejectLocked(
            Rejection::kParticipantNotHeld,
            "capture session held participant reached a safepoint");
      }
      if (!participant->host_call_depth) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session safepoint outside guest code");
      }
      if (impl_->open_segment && impl_->open_segment->participant_index ==
                                     impl_->IndexOf(participant)) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session participant parked inside an open segment");
      }
      if (impl_->RecordEventLocked(
              MakeParticipantControlEvent(
                  GuestExecutionSessionEventKind::kJitSafepointArrival,
                  participant->ordinal),
              now, 0, false, participant) == Impl::RecordResult::kRejected) {
        return Action::kReject;
      }
      impl_->HoldParticipantLocked(
          *participant, GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint,
          participant->last_event_sequence);
      impl_->TryCompleteStopLocked(now);
      return impl_->RendezvousActionLocked();
    }
    case State::kStopRendezvous:
    case State::kPublishing:
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant reached a safepoint");
    default:
      return Action::kContinue;
  }
}

Action GuestExecutionSessionAssembler::OnOuterHostCallBegin(
    const GuestExecutionCaptureParticipantIdentity& identity, uint32_t,
    uint32_t, uint32_t) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  Impl::Participant* participant = impl_->FindParticipantLocked(identity);
  if (!participant) {
    return Action::kReject;
  }
  if (participant->host_call_depth == std::numeric_limits<uint32_t>::max()) {
    return impl_->RejectLocked(Rejection::kHardLimit,
                               "capture session host call depth overflows");
  }
  switch (impl_->state) {
    case State::kStartRendezvous:
      if (participant->arrived) {
        if (!participant->host_call_depth) {
          return Action::kHold;
        }
        return impl_->RejectLocked(
            Rejection::kParticipantNotHeld,
            "capture session parked participant entered guest code");
      }
      ++participant->host_call_depth;
      return Action::kContinue;
    case State::kRecording: {
      if (participant->host_call_depth) {
        ++participant->host_call_depth;
        return Action::kContinue;
      }
      // A stop due at this tick holds the dispatch outside the session.
      const GuestExecutionReelAction action = impl_->reel->Poll(now);
      if (action == GuestExecutionReelAction::kReject) {
        return impl_->RejectFromReelLocked();
      }
      if (action == GuestExecutionReelAction::kStop) {
        if (!impl_->HandleReelStopLocked()) {
          return Action::kReject;
        }
        impl_->TryCompleteStopLocked(now);
        return impl_->RendezvousActionLocked();
      }
      ++participant->host_call_depth;
      participant->dispatch_in_session = true;
      participant->dispatch_segment_count = 0;
      const Impl::RecordResult result = impl_->RecordEventLocked(
          MakeParticipantControlEvent(
              GuestExecutionSessionEventKind::kOuterHostCallBegin,
              participant->ordinal),
          now, 0, false, participant);
      if (result == Impl::RecordResult::kRejected) {
        return Action::kReject;
      }
      if (result == Impl::RecordResult::kTail) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session outer host call began after the stop request");
      }
      return Action::kContinue;
    }
    case State::kStopRequested:
    case State::kStopRendezvous:
    case State::kPublishing:
      if (participant->held) {
        if (!participant->host_call_depth) {
          return Action::kHold;
        }
        return impl_->RejectLocked(
            Rejection::kParticipantNotHeld,
            "capture session held participant entered guest code");
      }
      ++participant->host_call_depth;
      return Action::kContinue;
    default:
      ++participant->host_call_depth;
      return Action::kContinue;
  }
}

Action GuestExecutionSessionAssembler::OnOuterHostCallEnd(
    const GuestExecutionCaptureParticipantIdentity& identity,
    GuestExecutionCaptureHostCallOutcome outcome) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  Impl::Participant* participant = impl_->FindParticipantLocked(identity);
  if (!participant) {
    return Action::kReject;
  }
  if (!participant->host_call_depth) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session host call end is unbalanced");
  }
  if (outcome != GuestExecutionCaptureHostCallOutcome::kReturnedToHost) {
    if (impl_->IsInSession()) {
      return impl_->RejectLocked(
          Rejection::kAbnormalHostCall,
          "capture session host call did not return normally");
    }
    --participant->host_call_depth;
    return Action::kContinue;
  }
  switch (impl_->state) {
    case State::kStartRendezvous:
      if (participant->arrived) {
        return impl_->RejectLocked(
            Rejection::kParticipantNotHeld,
            "capture session parked participant returned to host");
      }
      --participant->host_call_depth;
      if (participant->host_call_depth) {
        return Action::kContinue;
      }
      participant->arrived = true;
      participant->initial_outer_call_state =
          GuestExecutionSessionInitialOuterCallState::kOutside;
      ++impl_->arrived_participant_count;
      impl_->TryCompleteStartLocked(now);
      return impl_->RendezvousActionLocked();
    case State::kRecording:
    case State::kStopRequested: {
      if (participant->held) {
        return impl_->RejectLocked(
            Rejection::kParticipantNotHeld,
            "capture session held participant returned to host");
      }
      --participant->host_call_depth;
      if (participant->host_call_depth) {
        return Action::kContinue;
      }
      if (impl_->open_segment && impl_->open_segment->participant_index ==
                                     impl_->IndexOf(participant)) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session outer host call returned inside a segment");
      }
      const uint32_t expected_segment_count =
          impl_->config.coverage_mode ==
                  GuestExecutionReelCoverageMode::kInvocationSegments
              ? 1
              : 0;
      if (participant->dispatch_in_session &&
          participant->dispatch_segment_count != expected_segment_count) {
        return impl_->RejectLocked(
            Rejection::kInvalidCall,
            "capture session outer dispatch segment count is invalid");
      }
      participant->dispatch_in_session = false;
      impl_->outer_return_in_flight = participant;
      const Impl::RecordResult result = impl_->RecordEventLocked(
          MakeParticipantControlEvent(
              GuestExecutionSessionEventKind::kOuterHostCallEnd,
              participant->ordinal),
          now, 0, false, participant);
      impl_->outer_return_in_flight = nullptr;
      if (result == Impl::RecordResult::kRejected) {
        return Action::kReject;
      }
      if (result == Impl::RecordResult::kTail) {
        impl_->HoldParticipantLocked(
            *participant,
            GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn,
            participant->last_event_sequence);
        impl_->TryCompleteStopLocked(now);
        return impl_->RendezvousActionLocked();
      }
      return participant->held ? Action::kHold : Action::kContinue;
    }
    case State::kStopRendezvous:
    case State::kPublishing:
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant returned to host");
    default:
      --participant->host_call_depth;
      return Action::kContinue;
  }
}

Action GuestExecutionSessionAssembler::OnSegmentBegin(
    const GuestExecutionCaptureParticipantIdentity& identity,
    uint32_t entry_guest_address, uint32_t end_guest_address) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  Impl::Participant* participant = impl_->FindParticipantLocked(identity);
  if (!participant) {
    return Action::kReject;
  }
  if (impl_->config.coverage_mode ==
      GuestExecutionReelCoverageMode::kContinuousInstructions) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "continuous capture session received a segment begin");
  }
  switch (impl_->state) {
    case State::kRecording:
    case State::kStopRequested:
      break;
    case State::kStopRendezvous:
    case State::kPublishing:
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant began a segment");
    default:
      return participant->arrived
                 ? impl_->RejectLocked(
                       Rejection::kParticipantNotHeld,
                       "capture session parked participant began a segment")
                 : Action::kContinue;
  }
  if (participant->held) {
    return impl_->RejectLocked(
        Rejection::kParticipantNotHeld,
        "capture session held participant began a segment");
  }
  if (!participant->host_call_depth || impl_->open_segment ||
      !entry_guest_address || (entry_guest_address & 3) ||
      (end_guest_address & 3) || end_guest_address < entry_guest_address) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session segment begin is invalid");
  }
  if (!participant->dispatch_in_session ||
      participant->dispatch_segment_count) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session outer dispatch owns exactly one segment");
  }
  if (impl_->segments.size() >= impl_->config.limits.maximum_segment_count ||
      impl_->segments.size() >=
          impl_->config.bundle_limits.session.maximum_segments) {
    return impl_->RejectLocked(
        Rejection::kHardLimit,
        "capture session segment count exceeds its limit");
  }
  if (impl_->RecordEventLocked(
          MakeParticipantControlEvent(
              GuestExecutionSessionEventKind::kSegmentBegin,
              participant->ordinal),
          now, 0, false, participant) == Impl::RecordResult::kRejected) {
    return Action::kReject;
  }
  Impl::OpenSegment open_segment;
  open_segment.participant_index = impl_->IndexOf(participant);
  open_segment.first_event_sequence = participant->last_event_sequence;
  open_segment.begin_tick = now;
  open_segment.entry_guest_address = entry_guest_address;
  open_segment.end_guest_address = end_guest_address;
  impl_->open_segment = open_segment;
  return Action::kContinue;
}

Action GuestExecutionSessionAssembler::OnSegmentEnd(
    const GuestExecutionCaptureParticipantIdentity& identity,
    GuestExecutionSessionAssemblerSegmentEnd segment_end) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  Impl::Participant* participant = impl_->FindParticipantLocked(identity);
  if (!participant) {
    return Action::kReject;
  }
  if (impl_->config.coverage_mode ==
      GuestExecutionReelCoverageMode::kContinuousInstructions) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "continuous capture session received a segment end");
  }
  switch (impl_->state) {
    case State::kRecording:
    case State::kStopRequested:
      break;
    case State::kStopRendezvous:
    case State::kPublishing:
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant ended a segment");
    default:
      return impl_->RejectLocked(
          Rejection::kInvalidCall,
          "capture session segment ended outside the capture window");
  }
  if (participant->held) {
    return impl_->RejectLocked(
        Rejection::kParticipantNotHeld,
        "capture session held participant ended a segment");
  }
  if (!impl_->open_segment ||
      impl_->open_segment->participant_index != impl_->IndexOf(participant)) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session segment end has no matching begin");
  }
  const GuestInvocationCaptureStatus& status = segment_end.status;
  if (status.rejected_dependency_flags) {
    return impl_->RejectLocked(
        Rejection::kUnsupportedDependency,
        "capture session segment observed an unsupported dependency");
  }
  if (!IsPublishedSegmentStatus(status) ||
      status.segment_ordinal != impl_->segments.size()) {
    return impl_->RejectLocked(
        Rejection::kRejectedSegment,
        "capture session received a non-published or unordered segment");
  }
  // The producer's own ticks are validated but the session clock timestamps
  // the terminal event, so another participant's later event cannot trip the
  // reel's tick ordering.
  if (status.capture_end_tick < status.capture_start_tick ||
      status.capture_end_tick > now ||
      status.capture_start_tick < impl_->capture_start_tick) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session segment tick order is invalid");
  }
  GuestExecutionSessionSegmentReference reference;
  reference.ordinal = static_cast<uint32_t>(impl_->segments.size());
  reference.thread_ordinal = participant->ordinal;
  reference.first_event_sequence = impl_->open_segment->first_event_sequence;
  reference.entry_guest_address = impl_->open_segment->entry_guest_address;
  reference.end_guest_address = impl_->open_segment->end_guest_address;
  uint64_t blob_size = 0;
  if (!impl_->AddBlobLocked(std::move(segment_end.segment),
                            &reference.segment_sha256, &blob_size)) {
    return Action::kReject;
  }
  const uint64_t begin_tick = impl_->open_segment->begin_tick;
  const Impl::RecordResult result = impl_->RecordEventLocked(
      MakeParticipantControlEvent(GuestExecutionSessionEventKind::kSegmentEnd,
                                  participant->ordinal),
      now, 0, false, participant);
  if (result == Impl::RecordResult::kRejected) {
    return Action::kReject;
  }
  reference.last_event_sequence = participant->last_event_sequence;
  impl_->segments.push_back(reference);
  impl_->segment_corpus_digests.push_back(segment_end.code_corpus_sha256);
  impl_->open_segment.reset();
  participant->dispatch_segment_count = 1;
  if (result == Impl::RecordResult::kPrefix) {
    GuestExecutionReelEventRange range;
    range.first_sequence = reference.last_event_sequence;
    range.last_sequence = reference.last_event_sequence;
    range.event_count = 1;
    // The reel orders segments by the session-observed begin and end ticks.
    GuestInvocationCaptureStatus observed = status;
    observed.capture_start_tick = begin_tick;
    observed.capture_end_tick = now;
    const GuestExecutionReelAction action =
        impl_->reel->RecordSegment(observed, range);
    if (action == GuestExecutionReelAction::kReject) {
      return impl_->RejectFromReelLocked();
    }
    if (action == GuestExecutionReelAction::kStop &&
        impl_->state == State::kRecording) {
      if (!impl_->HandleReelStopLocked()) {
        return Action::kReject;
      }
      impl_->TryCompleteStopLocked(now);
      if (impl_->state == State::kRejected) {
        return Action::kReject;
      }
    }
  }
  return Action::kContinue;
}

Action GuestExecutionSessionAssembler::OnInstructionCoverage(
    const GuestExecutionCaptureParticipantIdentity& identity,
    uint64_t guest_instruction_delta) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  Impl::Participant* participant = impl_->FindParticipantLocked(identity);
  if (!participant) {
    return Action::kReject;
  }
  if (!guest_instruction_delta) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session instruction delta is zero");
  }
  switch (impl_->state) {
    case State::kRecording:
    case State::kStopRequested:
      break;
    case State::kStopRendezvous:
    case State::kPublishing:
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant executed instructions");
    default:
      return participant->arrived
                 ? impl_->RejectLocked(Rejection::kParticipantNotHeld,
                                       "capture session parked participant "
                                       "executed instructions")
                 : Action::kContinue;
  }
  if (participant->held) {
    return impl_->RejectLocked(
        Rejection::kParticipantNotHeld,
        "capture session held participant executed instructions");
  }
  if (!participant->host_call_depth) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session instruction coverage outside guest code");
  }
  GuestExecutionSessionEvent event = MakeParticipantControlEvent(
      GuestExecutionSessionEventKind::kInstructionCoverage,
      participant->ordinal);
  event.guest_instruction_delta = guest_instruction_delta;
  if (impl_->RecordEventLocked(event, now, guest_instruction_delta, false,
                               participant) == Impl::RecordResult::kRejected) {
    return Action::kReject;
  }
  return Action::kContinue;
}

Action GuestExecutionSessionAssembler::OnGuestMarker(
    const std::optional<GuestExecutionCaptureParticipantIdentity>& identity,
    GuestExecutionSessionMarkerSource source, uint64_t marker_identity) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  GuestExecutionReelMarkerSource reel_source =
      GuestExecutionReelMarkerSource::kNone;
  if (!ConvertMarkerSource(source, &reel_source) ||
      source == GuestExecutionSessionMarkerSource::kNone || !marker_identity) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session guest marker is invalid");
  }
  const bool pm4_marker = source == GuestExecutionSessionMarkerSource::kPm4Swap;
  const uint32_t pm4_sink = impl_->config.pm4_marker_sink_ordinal;
  if (pm4_marker && pm4_sink != kNoExternalSink &&
      pm4_sink < impl_->external_sinks.size() &&
      impl_->external_sinks[pm4_sink].held && impl_->IsInSession()) {
    return impl_->RejectLocked(
        Rejection::kExternalSinkNotHeld,
        "capture session held external sink emitted a PM4 marker");
  }
  if (pm4_marker == identity.has_value()) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session guest marker ownership is not canonical");
  }
  Impl::Participant* participant = nullptr;
  if (identity) {
    participant = impl_->FindParticipantLocked(*identity);
    if (!participant) {
      return Action::kReject;
    }
  }
  switch (impl_->state) {
    case State::kRecording:
    case State::kStopRequested:
    case State::kStopRendezvous:
      break;
    case State::kPublishing:
      return impl_->RejectLocked(
          participant ? Rejection::kParticipantNotHeld
                      : Rejection::kExternalSinkNotHeld,
          "capture session guest marker after the session was held");
    default:
      return participant && participant->arrived
                 ? impl_->RejectLocked(
                       Rejection::kParticipantNotHeld,
                       "capture session parked participant reached a marker")
                 : Action::kContinue;
  }
  if (participant) {
    if (participant->held) {
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant reached a marker");
    }
    if (!participant->host_call_depth) {
      return impl_->RejectLocked(Rejection::kInvalidCall,
                                 "capture session marker outside guest code");
    }
  }
  const GuestExecutionSessionBoundaryPolicy& boundary = impl_->config.boundary;
  const bool matching =
      boundary.kind == GuestExecutionSessionBoundaryKind::kGuestMarkerCount &&
      boundary.marker_source == source &&
      boundary.marker_identity == marker_identity;
  GuestExecutionSessionEvent event = MakeControlEvent(
      GuestExecutionSessionEventKind::kGuestMarker,
      participant ? participant->ordinal : kGuestExecutionSessionNoThread,
      pm4_marker
          ? GuestExecutionSessionEventDisposition::kReplayCaptured
          : GuestExecutionSessionEventDisposition::kValidateDeterministic);
  event.marker_source = source;
  event.marker_identity = marker_identity;
  if (impl_->RecordEventLocked(event, now, 0, matching, participant) ==
      Impl::RecordResult::kRejected) {
    return Action::kReject;
  }
  return Action::kContinue;
}

Action GuestExecutionSessionAssembler::OnExternalEvent(
    const std::optional<GuestExecutionCaptureParticipantIdentity>& identity,
    GuestExecutionSessionAssemblerExternalEvent input) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  const bool interrupt =
      input.kind == GuestExecutionSessionEventKind::kInterrupt;
  const bool addressed =
      input.kind == GuestExecutionSessionEventKind::kMmio ||
      input.kind == GuestExecutionSessionEventKind::kAtomicOrReservation;
  const bool modeled_export =
      input.kind == GuestExecutionSessionEventKind::kKernelExport ||
      input.kind == GuestExecutionSessionEventKind::kExternOrBuiltin;
  const bool payload_none =
      input.payload_kind == GuestExecutionSessionPayloadKind::kNone;
  const bool scalar_payload =
      input.payload_kind ==
      GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
  const size_t payload_size = input.payload.size();
  // No coordinator can produce an observed record for a no-thread event.
  const bool deterministic =
      input.disposition ==
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  if (!IsExternalEventKind(input.kind) ||
      !IsKnownDisposition(input.disposition) ||
      (!interrupt && !identity.has_value()) ||
      (!identity.has_value() && deterministic) ||
      (payload_none != input.payload.empty()) ||
      (!payload_none && !scalar_payload &&
       input.payload_kind != GuestExecutionSessionPayloadKind::kGuestBytes) ||
      (scalar_payload && payload_size != 1 && payload_size != 2 &&
       payload_size != 4 && payload_size != 8) ||
      (addressed
           ? !ValidateGuestRange(input.guest_address, input.byte_count)
           : (modeled_export ? (!input.guest_address ||
                                input.guest_address >= kGuestAddressSpaceSize ||
                                (input.guest_address & 3) || input.byte_count)
                             : (input.guest_address || input.byte_count)))) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session external event is invalid");
  }
  if (payload_size >
      impl_->config.bundle_limits.session.maximum_event_payload_bytes) {
    return impl_->RejectLocked(
        Rejection::kStorageLimit,
        "capture session event payload exceeds the byte limit");
  }
  Impl::Participant* participant = nullptr;
  if (identity) {
    participant = impl_->FindParticipantLocked(*identity);
    if (!participant) {
      return Action::kReject;
    }
  }
  switch (impl_->state) {
    case State::kRecording:
    case State::kStopRequested:
    case State::kStopRendezvous:
      break;
    case State::kPublishing:
      return impl_->RejectLocked(
          participant ? Rejection::kParticipantNotHeld
                      : Rejection::kExternalSinkNotHeld,
          "capture session external event after the session was held");
    default:
      return participant && participant->arrived
                 ? impl_->RejectLocked(Rejection::kParticipantNotHeld,
                                       "capture session parked participant "
                                       "observed an external event")
                 : Action::kContinue;
  }
  if (participant) {
    if (participant->held) {
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant observed an external event");
    }
    if (!participant->host_call_depth) {
      return impl_->RejectLocked(
          Rejection::kInvalidCall,
          "capture session external event outside guest code");
    }
  }
  GuestExecutionSessionEvent event = MakeControlEvent(
      input.kind,
      participant ? participant->ordinal : kGuestExecutionSessionNoThread,
      input.disposition);
  event.guest_address = input.guest_address;
  event.byte_count = input.byte_count;
  event.payload_kind = input.payload_kind;
  if (!payload_none &&
      !impl_->AddBlobLocked(std::move(input.payload), &event.payload_sha256,
                            &event.payload_size)) {
    return Action::kReject;
  }
  if (impl_->RecordEventLocked(event, now, 0, false, participant) ==
      Impl::RecordResult::kRejected) {
    return Action::kReject;
  }
  if (input.disposition ==
      GuestExecutionSessionEventDisposition::kRejectSession) {
    return impl_->RejectLocked(
        Rejection::kUnsupportedDependency,
        "capture session external event rejects the session");
  }
  return Action::kContinue;
}

Action GuestExecutionSessionAssembler::OnMemoryMutation(
    const std::optional<GuestExecutionCaptureParticipantIdentity>& identity,
    GuestExecutionSessionAssemblerMutation input) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  const bool asynchronous = IsAsynchronousMutationSource(input.source);
  const bool rejecting = input.disposition ==
                         GuestExecutionSessionEventDisposition::kRejectSession;
  const bool deterministic =
      input.disposition ==
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  if ((!asynchronous && !IsGuestThreadMutationSource(input.source)) ||
      !IsKnownDisposition(input.disposition) ||
      (!identity.has_value() && deterministic) ||
      (!asynchronous && (!identity.has_value() ||
                         input.external_sink_ordinal != kNoExternalSink)) ||
      (asynchronous &&
       input.external_sink_ordinal >= impl_->external_sinks.size()) ||
      !ValidateGuestRange(input.guest_address, input.byte_count) ||
      (rejecting ? !input.bytes.empty()
                 : input.bytes.size() != input.byte_count)) {
    return impl_->RejectLocked(Rejection::kInvalidCall,
                               "capture session memory mutation is invalid");
  }
  if (input.bytes.size() >
      impl_->config.bundle_limits.session.maximum_event_payload_bytes) {
    return impl_->RejectLocked(
        Rejection::kStorageLimit,
        "capture session mutation payload exceeds the byte limit");
  }
  Impl::Participant* participant = nullptr;
  if (identity) {
    participant = impl_->FindParticipantLocked(*identity);
    if (!participant) {
      return Action::kReject;
    }
  }
  switch (impl_->state) {
    case State::kRecording:
    case State::kStopRequested:
    case State::kStopRendezvous:
      break;
    case State::kPublishing:
      return impl_->RejectLocked(
          participant ? Rejection::kParticipantNotHeld
                      : Rejection::kExternalSinkNotHeld,
          "capture session memory mutated after the session was held");
    case State::kStartRendezvous:
      if (asynchronous &&
          impl_->external_sinks[input.external_sink_ordinal].held) {
        return impl_->RejectLocked(
            Rejection::kExternalSinkNotHeld,
            "capture session held external sink mutated memory");
      }
      return participant && participant->arrived
                 ? impl_->RejectLocked(
                       Rejection::kParticipantNotHeld,
                       "capture session parked participant mutated memory")
                 : Action::kContinue;
    default:
      return participant && participant->arrived
                 ? impl_->RejectLocked(
                       Rejection::kParticipantNotHeld,
                       "capture session parked participant mutated memory")
                 : Action::kContinue;
  }
  if (participant) {
    if (participant->held) {
      return impl_->RejectLocked(
          Rejection::kParticipantNotHeld,
          "capture session held participant mutated memory");
    }
    if (!participant->host_call_depth) {
      return impl_->RejectLocked(
          Rejection::kInvalidCall,
          "capture session guest mutation outside guest code");
    }
  }
  if (asynchronous && impl_->external_sinks[input.external_sink_ordinal].held) {
    return impl_->RejectLocked(
        Rejection::kExternalSinkNotHeld,
        "capture session held external sink mutated memory");
  }
  // An asynchronous write under an open segment cannot be attributed to the
  // segment's recorded inputs, so it fails closed at capture.
  if (asynchronous && impl_->open_segment) {
    return impl_->RejectLocked(
        Rejection::kUnmodeledMutation,
        "capture session asynchronous mutation during an open segment");
  }
  GuestExecutionSessionEvent event = MakeControlEvent(
      GuestExecutionSessionEventKind::kMemoryMutation,
      participant ? participant->ordinal : kGuestExecutionSessionNoThread,
      input.disposition);
  event.mutation_source = input.source;
  event.guest_address = input.guest_address;
  event.byte_count = input.byte_count;
  if (!rejecting) {
    event.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
    if (!impl_->AddBlobLocked(std::move(input.bytes), &event.payload_sha256,
                              &event.payload_size)) {
      return Action::kReject;
    }
  }
  if (impl_->RecordEventLocked(event, now, 0, false, participant) ==
      Impl::RecordResult::kRejected) {
    return Action::kReject;
  }
  if (rejecting) {
    return impl_->RejectLocked(
        Rejection::kUnmodeledMutation,
        "capture session observed an unmodeled memory mutation");
  }
  return Action::kContinue;
}

Action GuestExecutionSessionAssembler::OnUnsupportedDependency(
    const std::optional<GuestExecutionCaptureParticipantIdentity>& identity,
    uint32_t dependency_flags) {
  Impl::Scope scope(*impl_);
  uint64_t now = 0;
  if (!impl_->BeginCallLocked(scope, &now)) {
    return impl_->TerminalAction();
  }
  if (!dependency_flags ||
      (dependency_flags & ~ppc::kGuestInvocationKnownDependencyMask)) {
    return impl_->RejectLocked(
        Rejection::kInvalidCall,
        "capture session unsupported dependency flags are invalid");
  }
  Impl::Participant* participant = nullptr;
  if (identity) {
    participant = impl_->FindParticipantLocked(*identity);
    if (!participant) {
      return Action::kReject;
    }
  }
  if (!impl_->IsInSession() || impl_->state == State::kStartRendezvous) {
    return Action::kContinue;
  }
  GuestExecutionSessionEvent event = MakeControlEvent(
      GuestExecutionSessionEventKind::kUnsupported,
      participant ? participant->ordinal : kGuestExecutionSessionNoThread,
      GuestExecutionSessionEventDisposition::kRejectSession);
  if (impl_->state == State::kRecording) {
    GuestExecutionReelEventRange range;
    impl_->reel->RecordUnsupportedEvents(now, 1, dependency_flags, &range);
    if (range.event_count == 1 &&
        range.first_sequence == impl_->next_event_sequence) {
      impl_->AppendEventLocked(event, participant);
    }
  } else if (impl_->state != State::kPublishing) {
    impl_->RecordEventLocked(event, now, 0, false, participant);
  }
  return impl_->RejectLocked(
      Rejection::kUnsupportedDependency,
      "capture session observed an unsupported dependency");
}

bool GuestExecutionSessionAssembler::Publish(std::string* error) {
  GuestExecutionSessionBundle bundle;
  {
    Impl::Scope scope(*impl_);
    uint64_t now = 0;
    if (!impl_->BeginCallLocked(scope, &now)) {
      return Fail(error, impl_->message.empty()
                             ? "capture session is already terminal"
                             : impl_->message);
    }
    if (impl_->publish_in_progress) {
      return Fail(error, "capture session publication is already in progress");
    }
    if (impl_->state != State::kPublishing) {
      impl_->RejectLocked(
          Rejection::kInvalidCall,
          "capture session publication requires every participant held");
      return Fail(error, impl_->message);
    }
    if (!impl_->AssembleBundleLocked(&bundle)) {
      return Fail(error, impl_->message);
    }
    impl_->publish_in_progress = true;
  }
  // The publisher runs unlocked: every input is already held or rejected, so
  // bundle I/O never blocks a guest or sink thread on the session lock.
  std::string publication_error;
  const bool published =
      impl_->dependencies.publisher->Publish(bundle, &publication_error);
  Impl::Scope scope(*impl_);
  impl_->publish_in_progress = false;
  if (published) {
    impl_->published = true;
  }
  if (!published) {
    if (impl_->state == State::kPublishing) {
      impl_->RejectLocked(Rejection::kPublicationFailure, publication_error);
    }
    return Fail(error, impl_->message);
  }
  if (impl_->state != State::kPublishing) {
    return Fail(error, impl_->message);
  }
  impl_->state = State::kComplete;
  impl_->message.clear();
  return true;
}

void GuestExecutionSessionAssembler::Cancel(std::string message) {
  Impl::Scope scope(*impl_);
  if (impl_->IsTerminal()) {
    return;
  }
  impl_->RejectLocked(
      scope.reentered() ? Rejection::kCallbackReentry : Rejection::kCancelled,
      std::move(message));
}

GuestExecutionSessionAssemblerStatus GuestExecutionSessionAssembler::status()
    const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  GuestExecutionSessionAssemblerStatus result;
  result.state = impl_->state;
  result.coverage_mode = impl_->config.coverage_mode;
  result.rejection = impl_->rejection;
  result.stop_reason = impl_->stop_reason;
  result.next_event_sequence = impl_->next_event_sequence;
  result.next_segment_ordinal = impl_->segments.size();
  result.stop_request_event_sequence = impl_->stop_request_event_sequence;
  result.last_event_sequence = impl_->last_event_sequence;
  result.capture_start_tick = impl_->capture_start_tick;
  result.stop_request_tick = impl_->stop_request_tick;
  result.capture_end_tick = impl_->capture_end_tick;
  result.accepted_segment_count = impl_->segments.size();
  result.guest_instruction_count = impl_->guest_instruction_count;
  result.encoded_chunk_count =
      static_cast<uint32_t>(impl_->chunk_references.size());
  result.content_blob_count = impl_->blob_count;
  result.staged_event_count = impl_->pending_events.size();
  result.staged_chunk_bytes = impl_->encoded_chunk_bytes;
  result.content_blob_bytes = impl_->blob_bytes;
  result.published = impl_->published;
  result.message = impl_->message;
  for (const Impl::Participant& participant : impl_->participants) {
    GuestExecutionSessionAssemblerParticipantStatus entry;
    entry.identity = participant.identity;
    entry.ordinal = participant.ordinal;
    entry.host_call_depth = participant.host_call_depth;
    entry.arrived = participant.arrived;
    entry.held = participant.held;
    entry.initial_outer_call_state = participant.initial_outer_call_state;
    entry.boundary_arrival_kind = participant.boundary_arrival_kind;
    entry.first_event_sequence = participant.first_event_sequence;
    entry.last_event_sequence = participant.last_event_sequence;
    entry.held_after_event_sequence = participant.held_after_event_sequence;
    result.participants.push_back(entry);
  }
  for (const Impl::ExternalSink& sink : impl_->external_sinks) {
    result.external_sinks.push_back({sink.name, sink.held});
  }
  return result;
}

}  // namespace cpu
}  // namespace xe

#endif
