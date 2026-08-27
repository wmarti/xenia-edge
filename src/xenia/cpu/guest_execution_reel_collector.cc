/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_reel_collector.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace xe {
namespace cpu {

namespace {

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

bool IsKnownBoundaryKind(GuestExecutionReelBoundaryKind kind) {
  switch (kind) {
    case GuestExecutionReelBoundaryKind::kManual:
    case GuestExecutionReelBoundaryKind::kSegmentCount:
    case GuestExecutionReelBoundaryKind::kGuestMarkerCount:
    case GuestExecutionReelBoundaryKind::kGuestInstructionCount:
    case GuestExecutionReelBoundaryKind::kCaptureDurationTicks:
      return true;
    default:
      return false;
  }
}

bool IsKnownMarkerSource(GuestExecutionReelMarkerSource source) {
  switch (source) {
    case GuestExecutionReelMarkerSource::kNone:
    case GuestExecutionReelMarkerSource::kGuestDefined:
    case GuestExecutionReelMarkerSource::kPm4Swap:
    case GuestExecutionReelMarkerSource::kKernel:
    case GuestExecutionReelMarkerSource::kOtherInstrumented:
      return true;
    default:
      return false;
  }
}

bool IsKnownCoverageMode(GuestExecutionReelCoverageMode mode) {
  switch (mode) {
    case GuestExecutionReelCoverageMode::kInvocationSegments:
    case GuestExecutionReelCoverageMode::kContinuousInstructions:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::unique_ptr<GuestExecutionReelCollector>
GuestExecutionReelCollector::Create(const GuestExecutionReelConfig& config,
                                    uint64_t capture_start_tick,
                                    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!IsKnownBoundaryKind(config.boundary.kind)) {
    Fail(error, "capture reel boundary kind is invalid");
    return nullptr;
  }
  if (!IsKnownMarkerSource(config.boundary.marker_source)) {
    Fail(error, "capture reel marker source is invalid");
    return nullptr;
  }
  if (!IsKnownCoverageMode(config.coverage_mode)) {
    Fail(error, "capture reel coverage mode is invalid");
    return nullptr;
  }
  if (config.coverage_mode ==
          GuestExecutionReelCoverageMode::kContinuousInstructions &&
      config.boundary.kind == GuestExecutionReelBoundaryKind::kSegmentCount) {
    Fail(error, "continuous capture reel cannot use a segment-count boundary");
    return nullptr;
  }
  const GuestExecutionReelLimits& limits = config.limits;
  if (!limits.maximum_segment_count || !limits.maximum_event_count ||
      !limits.maximum_guest_instruction_count ||
      !limits.maximum_guest_marker_count || !limits.maximum_duration_ticks) {
    Fail(error, "capture reel hard bounds must all be nonzero");
    return nullptr;
  }
  if (limits.maximum_event_count >
      std::numeric_limits<uint64_t>::max() - config.first_event_sequence) {
    Fail(error, "capture reel event sequence range overflows");
    return nullptr;
  }
  if (limits.maximum_segment_count >
      std::numeric_limits<uint64_t>::max() - config.first_segment_ordinal) {
    Fail(error, "capture reel segment ordinal range overflows");
    return nullptr;
  }

  const bool is_marker_boundary =
      config.boundary.kind == GuestExecutionReelBoundaryKind::kGuestMarkerCount;
  if (is_marker_boundary) {
    if (!config.boundary.value ||
        config.boundary.marker_source ==
            GuestExecutionReelMarkerSource::kNone ||
        !config.boundary.marker_identity) {
      Fail(error,
           "capture reel guest-marker boundary requires a source, identity "
           "and nonzero count");
      return nullptr;
    }
  } else if (config.boundary.marker_source !=
                 GuestExecutionReelMarkerSource::kNone ||
             config.boundary.marker_identity) {
    Fail(error,
         "capture reel non-marker boundary must not select a guest marker");
    return nullptr;
  }

  uint64_t boundary_maximum = 0;
  switch (config.boundary.kind) {
    case GuestExecutionReelBoundaryKind::kManual:
      if (config.boundary.value) {
        Fail(error, "capture reel manual boundary value must be zero");
        return nullptr;
      }
      break;
    case GuestExecutionReelBoundaryKind::kSegmentCount:
      boundary_maximum = limits.maximum_segment_count;
      break;
    case GuestExecutionReelBoundaryKind::kGuestMarkerCount:
      boundary_maximum = limits.maximum_guest_marker_count;
      break;
    case GuestExecutionReelBoundaryKind::kGuestInstructionCount:
      boundary_maximum = limits.maximum_guest_instruction_count;
      break;
    case GuestExecutionReelBoundaryKind::kCaptureDurationTicks:
      boundary_maximum = limits.maximum_duration_ticks;
      break;
    default:
      break;
  }
  if (config.boundary.kind != GuestExecutionReelBoundaryKind::kManual &&
      (!config.boundary.value || config.boundary.value > boundary_maximum)) {
    Fail(error, "capture reel boundary exceeds its hard bound");
    return nullptr;
  }

  return std::unique_ptr<GuestExecutionReelCollector>(
      new GuestExecutionReelCollector(config, capture_start_tick));
}

GuestExecutionReelCollector::GuestExecutionReelCollector(
    const GuestExecutionReelConfig& config, uint64_t capture_start_tick)
    : config_(config) {
  status_.coverage_mode = config.coverage_mode;
  status_.capture_start_tick = capture_start_tick;
  status_.last_observed_tick = capture_start_tick;
  status_.first_event_sequence = config.first_event_sequence;
  status_.last_event_sequence = config.first_event_sequence;
  status_.next_event_sequence = config.first_event_sequence;
  status_.next_segment_ordinal = config.first_segment_ordinal;
}

GuestExecutionReelAction GuestExecutionReelCollector::CurrentActionLocked()
    const {
  switch (status_.state) {
    case GuestExecutionReelState::kRecording:
      return GuestExecutionReelAction::kContinue;
    case GuestExecutionReelState::kStopRequested:
    case GuestExecutionReelState::kComplete:
      return GuestExecutionReelAction::kStop;
    case GuestExecutionReelState::kRejected:
      return GuestExecutionReelAction::kReject;
  }
  return GuestExecutionReelAction::kReject;
}

void GuestExecutionReelCollector::RequestStopLocked(
    GuestExecutionReelStopReason reason, uint64_t now_tick) {
  if (status_.state != GuestExecutionReelState::kRecording) {
    return;
  }
  status_.state = GuestExecutionReelState::kStopRequested;
  status_.stop_reason = reason;
  status_.stop_request_tick = now_tick;
}

GuestExecutionReelAction GuestExecutionReelCollector::RejectLocked(
    GuestExecutionReelRejection rejection, std::string message,
    uint64_t now_tick, uint64_t rejected_event_count,
    uint32_t dependency_flags) {
  if (status_.state == GuestExecutionReelState::kRejected) {
    return GuestExecutionReelAction::kReject;
  }
  status_.state = GuestExecutionReelState::kRejected;
  status_.rejection = rejection;
  status_.capture_end_tick = std::max(status_.last_observed_tick, now_tick);
  status_.last_observed_tick = status_.capture_end_tick;
  status_.rejected_event_count = rejected_event_count;
  status_.rejected_dependency_flags |= dependency_flags;
  status_.message = std::move(message);
  return GuestExecutionReelAction::kReject;
}

GuestExecutionReelAction GuestExecutionReelCollector::ObserveTickLocked(
    uint64_t now_tick) {
  const GuestExecutionReelAction current_action = CurrentActionLocked();
  if (current_action != GuestExecutionReelAction::kContinue) {
    return current_action;
  }
  if (now_tick < status_.last_observed_tick) {
    return RejectLocked(GuestExecutionReelRejection::kTickOrdering,
                        "capture reel tick order regressed", now_tick);
  }
  status_.last_observed_tick = now_tick;
  const uint64_t elapsed = now_tick - status_.capture_start_tick;
  if (config_.boundary.kind ==
          GuestExecutionReelBoundaryKind::kCaptureDurationTicks &&
      elapsed >= config_.boundary.value) {
    RequestStopLocked(GuestExecutionReelStopReason::kCaptureDuration, now_tick);
    return GuestExecutionReelAction::kStop;
  }
  if (elapsed >= config_.limits.maximum_duration_ticks) {
    RequestStopLocked(GuestExecutionReelStopReason::kMaximumDuration, now_tick);
    return GuestExecutionReelAction::kStop;
  }
  return GuestExecutionReelAction::kContinue;
}

GuestExecutionReelAction GuestExecutionReelCollector::RecordEvents(
    uint64_t now_tick, uint64_t event_count, uint64_t guest_instruction_count,
    uint64_t matching_guest_marker_count,
    GuestExecutionReelEventRange* out_range) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (out_range) {
    *out_range = {};
  }
  GuestExecutionReelAction action = ObserveTickLocked(now_tick);
  if (action != GuestExecutionReelAction::kContinue) {
    return action;
  }
  if (!event_count || matching_guest_marker_count > event_count) {
    return RejectLocked(GuestExecutionReelRejection::kInvalidCall,
                        "capture reel event batch is invalid", now_tick,
                        event_count);
  }

  uint64_t accepted_events = 0;
  uint64_t instructions = 0;
  uint64_t markers = 0;
  uint64_t next_sequence = 0;
  if (!CheckedAdd(status_.accepted_event_count, event_count,
                  &accepted_events) ||
      !CheckedAdd(status_.guest_instruction_count, guest_instruction_count,
                  &instructions) ||
      !CheckedAdd(status_.guest_marker_count, matching_guest_marker_count,
                  &markers)) {
    return RejectLocked(GuestExecutionReelRejection::kCounterOverflow,
                        "capture reel coverage counter overflows", now_tick,
                        event_count);
  }
  if (!CheckedAdd(status_.next_event_sequence, event_count, &next_sequence)) {
    return RejectLocked(GuestExecutionReelRejection::kEventSequenceOverflow,
                        "capture reel event sequence overflows", now_tick,
                        event_count);
  }

  if ((config_.boundary.kind ==
           GuestExecutionReelBoundaryKind::kGuestInstructionCount &&
       instructions > config_.boundary.value) ||
      (config_.boundary.kind ==
           GuestExecutionReelBoundaryKind::kGuestMarkerCount &&
       markers > config_.boundary.value)) {
    return RejectLocked(GuestExecutionReelRejection::kBoundaryOvershoot,
                        "capture reel event batch crosses its exact boundary",
                        now_tick, event_count);
  }
  if (accepted_events > config_.limits.maximum_event_count ||
      instructions > config_.limits.maximum_guest_instruction_count ||
      markers > config_.limits.maximum_guest_marker_count) {
    return RejectLocked(GuestExecutionReelRejection::kLimitExceeded,
                        "capture reel event batch exceeds a hard bound",
                        now_tick, event_count);
  }

  GuestExecutionReelEventRange range;
  range.first_sequence = status_.next_event_sequence;
  range.last_sequence = next_sequence - 1;
  range.event_count = event_count;
  status_.accepted_event_count = accepted_events;
  status_.guest_instruction_count = instructions;
  status_.guest_marker_count = markers;
  status_.last_event_sequence = range.last_sequence;
  status_.next_event_sequence = next_sequence;
  status_.has_events = true;
  if (out_range) {
    *out_range = range;
  }

  if (config_.boundary.kind ==
          GuestExecutionReelBoundaryKind::kGuestInstructionCount &&
      instructions == config_.boundary.value) {
    RequestStopLocked(GuestExecutionReelStopReason::kGuestInstructionCount,
                      now_tick);
  } else if (config_.boundary.kind ==
                 GuestExecutionReelBoundaryKind::kGuestMarkerCount &&
             markers == config_.boundary.value) {
    RequestStopLocked(GuestExecutionReelStopReason::kGuestMarkerCount,
                      now_tick);
  } else if (accepted_events == config_.limits.maximum_event_count) {
    RequestStopLocked(GuestExecutionReelStopReason::kMaximumEventCount,
                      now_tick);
  } else if (instructions == config_.limits.maximum_guest_instruction_count) {
    RequestStopLocked(
        GuestExecutionReelStopReason::kMaximumGuestInstructionCount, now_tick);
  } else if (markers == config_.limits.maximum_guest_marker_count) {
    RequestStopLocked(GuestExecutionReelStopReason::kMaximumGuestMarkerCount,
                      now_tick);
  }
  return CurrentActionLocked();
}

GuestExecutionReelAction GuestExecutionReelCollector::RecordUnsupportedEvents(
    uint64_t now_tick, uint64_t event_count, uint32_t dependency_flags,
    GuestExecutionReelEventRange* out_range) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (out_range) {
    *out_range = {};
  }
  GuestExecutionReelAction action = ObserveTickLocked(now_tick);
  if (action != GuestExecutionReelAction::kContinue) {
    return action;
  }
  if (!event_count || !dependency_flags ||
      (dependency_flags & ~ppc::kGuestInvocationKnownDependencyMask)) {
    return RejectLocked(GuestExecutionReelRejection::kInvalidCall,
                        "capture reel unsupported event batch is invalid",
                        now_tick, event_count, dependency_flags);
  }
  uint64_t next_sequence = 0;
  if (!CheckedAdd(status_.next_event_sequence, event_count, &next_sequence)) {
    return RejectLocked(GuestExecutionReelRejection::kEventSequenceOverflow,
                        "capture reel unsupported event sequence overflows",
                        now_tick, event_count, dependency_flags);
  }
  uint64_t total_events = 0;
  if (!CheckedAdd(status_.accepted_event_count, event_count, &total_events) ||
      total_events > config_.limits.maximum_event_count) {
    return RejectLocked(GuestExecutionReelRejection::kLimitExceeded,
                        "capture reel unsupported events exceed a hard bound",
                        now_tick, event_count, dependency_flags);
  }

  GuestExecutionReelEventRange range;
  range.first_sequence = status_.next_event_sequence;
  range.last_sequence = next_sequence - 1;
  range.event_count = event_count;
  status_.last_event_sequence = range.last_sequence;
  status_.next_event_sequence = next_sequence;
  status_.has_events = true;
  status_.unsupported_event_count = event_count;
  if (out_range) {
    *out_range = range;
  }
  return RejectLocked(GuestExecutionReelRejection::kUnsupportedDependency,
                      "capture reel observed an unsupported dependency",
                      now_tick, event_count, dependency_flags);
}

GuestExecutionReelAction GuestExecutionReelCollector::RecordSegment(
    const GuestInvocationCaptureStatus& segment_status,
    const GuestExecutionReelEventRange& event_range) {
  std::lock_guard<std::mutex> lock(mutex_);
  const GuestExecutionReelAction current_action = CurrentActionLocked();
  if (current_action == GuestExecutionReelAction::kReject ||
      status_.state == GuestExecutionReelState::kComplete) {
    return current_action;
  }
  if (config_.coverage_mode ==
      GuestExecutionReelCoverageMode::kContinuousInstructions) {
    ++status_.rejected_segment_count;
    return RejectLocked(GuestExecutionReelRejection::kInvalidCall,
                        "continuous capture reel received a segment",
                        segment_status.capture_end_tick);
  }

  if (segment_status.segment_ordinal != status_.next_segment_ordinal) {
    ++status_.rejected_segment_count;
    return RejectLocked(GuestExecutionReelRejection::kSegmentOrdering,
                        "capture reel segment ordinal is not contiguous",
                        segment_status.capture_end_tick);
  }
  if (segment_status.capture_end_tick < segment_status.capture_start_tick ||
      segment_status.capture_start_tick < status_.capture_start_tick ||
      segment_status.capture_end_tick < status_.last_observed_tick ||
      (has_segments_ &&
       (segment_status.capture_start_tick < last_segment_end_tick_ ||
        segment_status.capture_end_tick < last_segment_end_tick_))) {
    ++status_.rejected_segment_count;
    return RejectLocked(GuestExecutionReelRejection::kTickOrdering,
                        "capture reel segment tick order is invalid",
                        segment_status.capture_end_tick);
  }
  status_.last_observed_tick = segment_status.capture_end_tick;

  if (segment_status.rejected_dependency_flags &
      ~ppc::kGuestInvocationKnownDependencyMask) {
    ++status_.rejected_segment_count;
    return RejectLocked(
        GuestExecutionReelRejection::kInvalidSegmentStatus,
        "capture reel segment contains unknown dependency flags",
        segment_status.capture_end_tick, 1,
        segment_status.rejected_dependency_flags);
  }

  const bool published =
      segment_status.state == GuestInvocationCaptureState::kPublished &&
      segment_status.recorder_state ==
          ppc::GuestInvocationRecorderState::kComplete &&
      segment_status.rejection ==
          ppc::GuestInvocationRecorderRejection::kNone &&
      !segment_status.rejected_dependency_flags &&
      segment_status.accepted_segment_count == 1 &&
      !segment_status.rejected_segment_count && segment_status.message.empty();
  if (!published) {
    ++status_.rejected_segment_count;
    if (segment_status.rejected_dependency_flags) {
      status_.unsupported_event_count = 1;
      return RejectLocked(
          GuestExecutionReelRejection::kUnsupportedDependency,
          "capture reel segment observed an unsupported dependency",
          segment_status.capture_end_tick, 1,
          segment_status.rejected_dependency_flags);
    }
    const GuestExecutionReelRejection rejection =
        segment_status.state == GuestInvocationCaptureState::kRejected ||
                segment_status.state ==
                    GuestInvocationCaptureState::kPublicationFailed ||
                segment_status.state == GuestInvocationCaptureState::kStopped
            ? GuestExecutionReelRejection::kRejectedSegment
            : GuestExecutionReelRejection::kInvalidSegmentStatus;
    return RejectLocked(rejection,
                        "capture reel received a non-published segment",
                        segment_status.capture_end_tick);
  }

  if (!event_range.event_count ||
      event_range.last_sequence < event_range.first_sequence ||
      event_range.event_count !=
          event_range.last_sequence - event_range.first_sequence + 1 ||
      event_range.first_sequence < status_.first_event_sequence ||
      event_range.last_sequence >= status_.next_event_sequence ||
      (has_segments_ &&
       event_range.first_sequence <= last_segment_event_sequence_)) {
    ++status_.rejected_segment_count;
    return RejectLocked(GuestExecutionReelRejection::kSegmentOrdering,
                        "capture reel segment event range is invalid",
                        segment_status.capture_end_tick);
  }

  uint64_t accepted_segments = 0;
  uint64_t next_ordinal = 0;
  if (!CheckedAdd(status_.accepted_segment_count, 1, &accepted_segments) ||
      !CheckedAdd(status_.next_segment_ordinal, 1, &next_ordinal)) {
    ++status_.rejected_segment_count;
    return RejectLocked(GuestExecutionReelRejection::kCounterOverflow,
                        "capture reel segment counter overflows",
                        segment_status.capture_end_tick);
  }
  if ((config_.boundary.kind == GuestExecutionReelBoundaryKind::kSegmentCount &&
       accepted_segments > config_.boundary.value) ||
      accepted_segments > config_.limits.maximum_segment_count) {
    ++status_.rejected_segment_count;
    return RejectLocked(
        config_.boundary.kind == GuestExecutionReelBoundaryKind::kSegmentCount
            ? GuestExecutionReelRejection::kBoundaryOvershoot
            : GuestExecutionReelRejection::kLimitExceeded,
        "capture reel segment crosses a count boundary",
        segment_status.capture_end_tick);
  }

  status_.accepted_segment_count = accepted_segments;
  status_.next_segment_ordinal = next_ordinal;
  last_segment_event_sequence_ = event_range.last_sequence;
  last_segment_end_tick_ = segment_status.capture_end_tick;
  has_segments_ = true;

  if (config_.boundary.kind == GuestExecutionReelBoundaryKind::kSegmentCount &&
      accepted_segments == config_.boundary.value) {
    RequestStopLocked(GuestExecutionReelStopReason::kSegmentCount,
                      segment_status.capture_end_tick);
  } else if (accepted_segments == config_.limits.maximum_segment_count) {
    RequestStopLocked(GuestExecutionReelStopReason::kMaximumSegmentCount,
                      segment_status.capture_end_tick);
  } else {
    const uint64_t elapsed =
        segment_status.capture_end_tick - status_.capture_start_tick;
    if (config_.boundary.kind ==
            GuestExecutionReelBoundaryKind::kCaptureDurationTicks &&
        elapsed >= config_.boundary.value) {
      RequestStopLocked(GuestExecutionReelStopReason::kCaptureDuration,
                        segment_status.capture_end_tick);
    } else if (elapsed >= config_.limits.maximum_duration_ticks) {
      RequestStopLocked(GuestExecutionReelStopReason::kMaximumDuration,
                        segment_status.capture_end_tick);
    }
  }
  return CurrentActionLocked();
}

GuestExecutionReelAction GuestExecutionReelCollector::Poll(uint64_t now_tick) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ObserveTickLocked(now_tick);
}

GuestExecutionReelAction GuestExecutionReelCollector::RequestManualStop(
    uint64_t now_tick) {
  std::lock_guard<std::mutex> lock(mutex_);
  GuestExecutionReelAction action = ObserveTickLocked(now_tick);
  if (action != GuestExecutionReelAction::kContinue) {
    return action;
  }
  if (config_.boundary.kind != GuestExecutionReelBoundaryKind::kManual) {
    return RejectLocked(GuestExecutionReelRejection::kInvalidCall,
                        "capture reel manual stop does not match its policy",
                        now_tick);
  }
  RequestStopLocked(GuestExecutionReelStopReason::kManual, now_tick);
  return GuestExecutionReelAction::kStop;
}

bool GuestExecutionReelCollector::Complete(uint64_t now_tick,
                                           std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (error) {
    error->clear();
  }
  if (status_.state != GuestExecutionReelState::kStopRequested) {
    return Fail(error,
                "capture reel cannot complete before a clean stop request");
  }
  if (now_tick < status_.last_observed_tick ||
      now_tick < status_.stop_request_tick) {
    RejectLocked(GuestExecutionReelRejection::kTickOrdering,
                 "capture reel completion tick order is invalid", now_tick);
    return Fail(error, status_.message);
  }
  const bool has_coverage =
      config_.coverage_mode ==
              GuestExecutionReelCoverageMode::kContinuousInstructions
          ? status_.guest_instruction_count != 0 &&
                status_.accepted_segment_count == 0 &&
                status_.rejected_segment_count == 0
          : status_.accepted_segment_count != 0;
  if (!status_.accepted_event_count || !has_coverage) {
    RejectLocked(GuestExecutionReelRejection::kIncomplete,
                 "capture reel has no accepted event and execution coverage",
                 now_tick);
    return Fail(error, status_.message);
  }
  status_.state = GuestExecutionReelState::kComplete;
  status_.capture_end_tick = now_tick;
  status_.last_observed_tick = now_tick;
  status_.message.clear();
  return true;
}

GuestExecutionReelStatus GuestExecutionReelCollector::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

}  // namespace cpu
}  // namespace xe

#endif
