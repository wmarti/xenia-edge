/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_REEL_COLLECTOR_H_
#define XENIA_CPU_GUEST_EXECUTION_REEL_COLLECTOR_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/cpu/guest_invocation_capture.h"

namespace xe {
namespace cpu {

// Capture-window policy only. The session codec converts its portable duration
// to host ticks before creating the collector.
enum class GuestExecutionReelBoundaryKind : uint32_t {
  kManual = 1,
  kSegmentCount = 2,
  kGuestMarkerCount = 3,
  kGuestInstructionCount = 4,
  kCaptureDurationTicks = 5,
};

// A guest marker is an explicitly instrumented guest-side event. In
// particular, a PM4 swap marker is not evidence that the host presented a
// drawable.
enum class GuestExecutionReelMarkerSource : uint32_t {
  kNone = 0,
  kGuestDefined = 1,
  kPm4Swap = 2,
  kKernel = 3,
  kOtherInstrumented = 4,
};

struct GuestExecutionReelBoundaryPolicy {
  GuestExecutionReelBoundaryKind kind = GuestExecutionReelBoundaryKind::kManual;
  uint64_t value = 0;
  GuestExecutionReelMarkerSource marker_source =
      GuestExecutionReelMarkerSource::kNone;
  uint64_t marker_identity = 0;
};

// Independent hard bounds are mandatory even for a manual window. Reaching a
// bound exactly requests a clean stop; an event batch that would cross a bound
// rejects instead of being truncated.
struct GuestExecutionReelLimits {
  uint64_t maximum_segment_count = 0;
  uint64_t maximum_event_count = 0;
  uint64_t maximum_guest_instruction_count = 0;
  uint64_t maximum_guest_marker_count = 0;
  uint64_t maximum_duration_ticks = 0;
};

struct GuestExecutionReelConfig {
  GuestExecutionReelBoundaryPolicy boundary;
  GuestExecutionReelLimits limits;
  uint64_t first_event_sequence = 0;
  uint64_t first_segment_ordinal = 0;
};

struct GuestExecutionReelEventRange {
  uint64_t first_sequence = 0;
  uint64_t last_sequence = 0;
  uint64_t event_count = 0;
};

enum class GuestExecutionReelState : uint8_t {
  kRecording,
  kStopRequested,
  kComplete,
  kRejected,
};

enum class GuestExecutionReelAction : uint8_t {
  kContinue,
  kStop,
  kReject,
};

enum class GuestExecutionReelStopReason : uint8_t {
  kNone,
  kManual,
  kSegmentCount,
  kGuestMarkerCount,
  kGuestInstructionCount,
  kCaptureDuration,
  kMaximumSegmentCount,
  kMaximumEventCount,
  kMaximumGuestInstructionCount,
  kMaximumGuestMarkerCount,
  kMaximumDuration,
};

enum class GuestExecutionReelRejection : uint8_t {
  kNone,
  kInvalidCall,
  kTickOrdering,
  kEventSequenceOverflow,
  kCounterOverflow,
  kBoundaryOvershoot,
  kLimitExceeded,
  kSegmentOrdering,
  kInvalidSegmentStatus,
  kRejectedSegment,
  kUnsupportedDependency,
  kIncomplete,
};

struct GuestExecutionReelStatus {
  GuestExecutionReelState state = GuestExecutionReelState::kRecording;
  GuestExecutionReelStopReason stop_reason =
      GuestExecutionReelStopReason::kNone;
  GuestExecutionReelRejection rejection = GuestExecutionReelRejection::kNone;
  uint32_t rejected_dependency_flags = 0;
  uint64_t capture_start_tick = 0;
  uint64_t stop_request_tick = 0;
  uint64_t capture_end_tick = 0;
  uint64_t last_observed_tick = 0;
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint64_t next_event_sequence = 0;
  uint64_t next_segment_ordinal = 0;
  uint64_t accepted_segment_count = 0;
  uint64_t rejected_segment_count = 0;
  uint64_t accepted_event_count = 0;
  uint64_t rejected_event_count = 0;
  uint64_t unsupported_event_count = 0;
  uint64_t guest_instruction_count = 0;
  uint64_t guest_marker_count = 0;
  bool has_events = false;
  std::string message;
};

// Metadata-only capture-window state machine above independently bounded
// GuestInvocationCaptureCoordinator segments. It neither owns nor serializes
// title code, pages, checkpoints or event payloads.
class GuestExecutionReelCollector final {
 public:
  static std::unique_ptr<GuestExecutionReelCollector> Create(
      const GuestExecutionReelConfig& config, uint64_t capture_start_tick,
      std::string* error = nullptr);

  GuestExecutionReelCollector(const GuestExecutionReelCollector&) = delete;
  GuestExecutionReelCollector& operator=(const GuestExecutionReelCollector&) =
      delete;

  // Reserves one contiguous global sequence range and accounts exact captured
  // CPU coverage. Matching_guest_marker_count includes only markers selected
  // by the configured source and identity.
  GuestExecutionReelAction RecordEvents(
      uint64_t now_tick, uint64_t event_count, uint64_t guest_instruction_count,
      uint64_t matching_guest_marker_count,
      GuestExecutionReelEventRange* out_range = nullptr);

  // Allocates durable diagnostic sequence numbers, then rejects the entire
  // reel. Unsupported work is never silently omitted from coverage.
  GuestExecutionReelAction RecordUnsupportedEvents(
      uint64_t now_tick, uint64_t event_count, uint32_t dependency_flags,
      GuestExecutionReelEventRange* out_range = nullptr);

  // Consumes the terminal status of one coordinator. Published segments must
  // be ordinal-contiguous and reference a non-overlapping subset of previously
  // reserved events. Any other coordinator outcome rejects the reel.
  GuestExecutionReelAction RecordSegment(
      const GuestInvocationCaptureStatus& segment_status,
      const GuestExecutionReelEventRange& event_range);

  GuestExecutionReelAction Poll(uint64_t now_tick);
  GuestExecutionReelAction RequestManualStop(uint64_t now_tick);

  // Completes only after a policy requested stop and at least one event and one
  // accepted segment were retained. The future session owner calls this only
  // after its final checkpoint and chunks are durable.
  bool Complete(uint64_t now_tick, std::string* error = nullptr);

  GuestExecutionReelStatus status() const;

 private:
  GuestExecutionReelCollector(const GuestExecutionReelConfig& config,
                              uint64_t capture_start_tick);

  GuestExecutionReelAction ObserveTickLocked(uint64_t now_tick);
  GuestExecutionReelAction RejectLocked(GuestExecutionReelRejection rejection,
                                        std::string message, uint64_t now_tick,
                                        uint64_t rejected_event_count = 0,
                                        uint32_t dependency_flags = 0);
  void RequestStopLocked(GuestExecutionReelStopReason reason,
                         uint64_t now_tick);
  GuestExecutionReelAction CurrentActionLocked() const;

  mutable std::mutex mutex_;
  GuestExecutionReelConfig config_;
  GuestExecutionReelStatus status_;
  uint64_t last_segment_event_sequence_ = 0;
  uint64_t last_segment_end_tick_ = 0;
  bool has_segments_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_REEL_COLLECTOR_H_
