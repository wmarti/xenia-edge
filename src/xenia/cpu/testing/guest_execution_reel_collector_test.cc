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

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {

namespace {

GuestExecutionReelLimits MakeLimits() {
  GuestExecutionReelLimits limits;
  limits.maximum_segment_count = 8;
  limits.maximum_event_count = 64;
  limits.maximum_guest_instruction_count = 1024;
  limits.maximum_guest_marker_count = 16;
  limits.maximum_duration_ticks = 1000;
  return limits;
}

GuestExecutionReelConfig MakeConfig(GuestExecutionReelBoundaryKind kind =
                                        GuestExecutionReelBoundaryKind::kManual,
                                    uint64_t value = 0) {
  GuestExecutionReelConfig config;
  config.boundary.kind = kind;
  config.boundary.value = value;
  config.limits = MakeLimits();
  return config;
}

std::unique_ptr<GuestExecutionReelCollector> MakeCollector(
    const GuestExecutionReelConfig& config, uint64_t start_tick = 100) {
  std::string error;
  std::unique_ptr<GuestExecutionReelCollector> collector =
      GuestExecutionReelCollector::Create(config, start_tick, &error);
  REQUIRE(collector);
  REQUIRE(error.empty());
  return collector;
}

GuestInvocationCaptureStatus PublishedSegment(uint64_t ordinal,
                                              uint64_t start_tick,
                                              uint64_t end_tick) {
  GuestInvocationCaptureStatus status;
  status.state = GuestInvocationCaptureState::kPublished;
  status.recorder_state = ppc::GuestInvocationRecorderState::kComplete;
  status.segment_ordinal = ordinal;
  status.accepted_segment_count = 1;
  status.capture_start_tick = start_tick;
  status.capture_end_tick = end_tick;
  return status;
}

GuestExecutionReelEventRange RecordEvents(
    GuestExecutionReelCollector& collector, uint64_t tick, uint64_t events = 1,
    uint64_t instructions = 1, uint64_t markers = 0,
    GuestExecutionReelAction expected = GuestExecutionReelAction::kContinue) {
  GuestExecutionReelEventRange range;
  REQUIRE(collector.RecordEvents(tick, events, instructions, markers, &range) ==
          expected);
  return range;
}

void RequireCreateFails(const GuestExecutionReelConfig& config,
                        const std::string& diagnostic) {
  std::string error;
  REQUIRE_FALSE(GuestExecutionReelCollector::Create(config, 100, &error));
  REQUIRE(error.find(diagnostic) != std::string::npos);
}

}  // namespace

TEST_CASE("guest execution reel validates independent bounded policies",
          "[guest-execution-reel]") {
  GuestExecutionReelConfig config = MakeConfig();
  REQUIRE(MakeCollector(config));
  REQUIRE(MakeCollector(config)->status().coverage_mode ==
          GuestExecutionReelCoverageMode::kInvocationSegments);

  config.limits.maximum_event_count = 0;
  RequireCreateFails(config, "nonzero");

  config = MakeConfig();
  config.boundary.value = 1;
  RequireCreateFails(config, "manual");

  config = MakeConfig(GuestExecutionReelBoundaryKind::kSegmentCount, 2);
  REQUIRE(MakeCollector(config));
  config.boundary.value = 0;
  RequireCreateFails(config, "hard bound");
  config.boundary.value = config.limits.maximum_segment_count + 1;
  RequireCreateFails(config, "hard bound");

  config = MakeConfig(GuestExecutionReelBoundaryKind::kGuestMarkerCount, 3);
  RequireCreateFails(config, "source");
  config.boundary.marker_source = GuestExecutionReelMarkerSource::kPm4Swap;
  config.boundary.marker_identity = 0x584553574150ull;
  REQUIRE(MakeCollector(config));

  config =
      MakeConfig(GuestExecutionReelBoundaryKind::kGuestInstructionCount, 100);
  config.boundary.marker_source = GuestExecutionReelMarkerSource::kKernel;
  config.boundary.marker_identity = 1;
  RequireCreateFails(config, "must not select");

  config =
      MakeConfig(GuestExecutionReelBoundaryKind::kCaptureDurationTicks, 500);
  REQUIRE(MakeCollector(config));

  config = MakeConfig();
  config.first_event_sequence = std::numeric_limits<uint64_t>::max() - 10;
  config.limits.maximum_event_count = 11;
  RequireCreateFails(config, "sequence range");

  config = MakeConfig();
  config.first_segment_ordinal = std::numeric_limits<uint64_t>::max() - 2;
  config.limits.maximum_segment_count = 3;
  RequireCreateFails(config, "ordinal range");

  config = MakeConfig();
  config.boundary.kind = static_cast<GuestExecutionReelBoundaryKind>(99);
  RequireCreateFails(config, "kind");

  config = MakeConfig();
  config.coverage_mode = static_cast<GuestExecutionReelCoverageMode>(99);
  RequireCreateFails(config, "coverage mode");

  config = MakeConfig(GuestExecutionReelBoundaryKind::kSegmentCount, 1);
  config.coverage_mode =
      GuestExecutionReelCoverageMode::kContinuousInstructions;
  RequireCreateFails(config, "segment-count");

  config = MakeConfig(GuestExecutionReelBoundaryKind::kGuestMarkerCount, 1);
  config.boundary.marker_source =
      static_cast<GuestExecutionReelMarkerSource>(99);
  config.boundary.marker_identity = 1;
  RequireCreateFails(config, "marker source");
}

TEST_CASE("guest execution reel retains continuous instruction coverage",
          "[guest-execution-reel]") {
  GuestExecutionReelConfig config = MakeConfig();
  config.coverage_mode =
      GuestExecutionReelCoverageMode::kContinuousInstructions;

  SECTION("instruction coverage completes without a segment") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    RecordEvents(*collector, 101, 1, 9);
    REQUIRE(collector->RequestManualStop(102) ==
            GuestExecutionReelAction::kStop);
    REQUIRE(collector->Complete(103));
    REQUIRE(collector->status().accepted_segment_count == 0);
    REQUIRE(collector->status().guest_instruction_count == 9);
    REQUIRE(collector->status().coverage_mode ==
            GuestExecutionReelCoverageMode::kContinuousInstructions);
  }

  SECTION("control events alone do not establish execution coverage") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    RecordEvents(*collector, 101, 1, 0);
    REQUIRE(collector->RequestManualStop(102) ==
            GuestExecutionReelAction::kStop);
    REQUIRE_FALSE(collector->Complete(103));
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kIncomplete);
  }

  SECTION("segment input rejects the continuous reel") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    GuestExecutionReelEventRange range = RecordEvents(*collector, 101);
    REQUIRE(collector->RecordSegment(PublishedSegment(0, 100, 101), range) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kInvalidCall);
  }

  SECTION("instruction boundary stops exactly without a segment") {
    config.boundary.kind =
        GuestExecutionReelBoundaryKind::kGuestInstructionCount;
    config.boundary.value = 10;
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    RecordEvents(*collector, 101, 1, 9);
    RecordEvents(*collector, 102, 1, 1, 0, GuestExecutionReelAction::kStop);
    REQUIRE(collector->Complete(103));
    REQUIRE(collector->status().stop_reason ==
            GuestExecutionReelStopReason::kGuestInstructionCount);
  }
}

TEST_CASE("guest execution reel manual window has exact ordered coverage",
          "[guest-execution-reel]") {
  GuestExecutionReelConfig config = MakeConfig();
  config.first_event_sequence = 1000;
  config.first_segment_ordinal = 7;
  std::unique_ptr<GuestExecutionReelCollector> collector =
      MakeCollector(config);

  GuestExecutionReelEventRange range = RecordEvents(*collector, 101, 3, 9, 0);
  REQUIRE(range.first_sequence == 1000);
  REQUIRE(range.last_sequence == 1002);
  REQUIRE(range.event_count == 3);
  REQUIRE(collector->RecordSegment(PublishedSegment(7, 100, 102), range) ==
          GuestExecutionReelAction::kContinue);
  REQUIRE(collector->RequestManualStop(103) == GuestExecutionReelAction::kStop);

  GuestExecutionReelEventRange after_stop = {1, 2, 2};
  REQUIRE(collector->RecordEvents(103, 1, 1, 0, &after_stop) ==
          GuestExecutionReelAction::kStop);
  REQUIRE(after_stop.event_count == 0);

  std::string error;
  REQUIRE(collector->Complete(104, &error));
  REQUIRE(error.empty());
  const GuestExecutionReelStatus status = collector->status();
  REQUIRE(status.state == GuestExecutionReelState::kComplete);
  REQUIRE(status.stop_reason == GuestExecutionReelStopReason::kManual);
  REQUIRE(status.accepted_segment_count == 1);
  REQUIRE(status.rejected_segment_count == 0);
  REQUIRE(status.accepted_event_count == 3);
  REQUIRE(status.guest_instruction_count == 9);
  REQUIRE(status.guest_marker_count == 0);
  REQUIRE(status.first_event_sequence == 1000);
  REQUIRE(status.last_event_sequence == 1002);
  REQUIRE(status.next_event_sequence == 1003);
  REQUIRE(status.next_segment_ordinal == 8);
  REQUIRE(status.stop_request_tick == 103);
  REQUIRE(status.capture_end_tick == 104);
  REQUIRE(status.message.empty());
  REQUIRE(collector->Poll(105) == GuestExecutionReelAction::kStop);
}

TEST_CASE("guest execution reel refuses empty or mismatched manual completion",
          "[guest-execution-reel]") {
  SECTION("empty capture") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    REQUIRE(collector->RequestManualStop(101) ==
            GuestExecutionReelAction::kStop);
    std::string error;
    REQUIRE_FALSE(collector->Complete(102, &error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kIncomplete);
  }

  SECTION("invocation mode requires an accepted segment") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    RecordEvents(*collector, 101, 1, 9);
    REQUIRE(collector->RequestManualStop(102) ==
            GuestExecutionReelAction::kStop);
    REQUIRE_FALSE(collector->Complete(103));
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kIncomplete);
  }

  SECTION("manual request under another policy") {
    std::unique_ptr<GuestExecutionReelCollector> collector = MakeCollector(
        MakeConfig(GuestExecutionReelBoundaryKind::kSegmentCount, 1));
    REQUIRE(collector->RequestManualStop(101) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kInvalidCall);
  }
}

TEST_CASE("guest execution reel stops on an exact segment count",
          "[guest-execution-reel]") {
  std::unique_ptr<GuestExecutionReelCollector> collector = MakeCollector(
      MakeConfig(GuestExecutionReelBoundaryKind::kSegmentCount, 2));

  GuestExecutionReelEventRange first = RecordEvents(*collector, 101);
  REQUIRE(collector->RecordSegment(PublishedSegment(0, 100, 101), first) ==
          GuestExecutionReelAction::kContinue);
  GuestExecutionReelEventRange second = RecordEvents(*collector, 102);
  REQUIRE(collector->RecordSegment(PublishedSegment(1, 101, 102), second) ==
          GuestExecutionReelAction::kStop);
  REQUIRE(collector->status().stop_reason ==
          GuestExecutionReelStopReason::kSegmentCount);
  REQUIRE(collector->Complete(103));
}

TEST_CASE("guest execution reel treats guest markers only as selected markers",
          "[guest-execution-reel]") {
  GuestExecutionReelConfig config =
      MakeConfig(GuestExecutionReelBoundaryKind::kGuestMarkerCount, 2);
  config.boundary.marker_source = GuestExecutionReelMarkerSource::kPm4Swap;
  config.boundary.marker_identity = 0x504D3453574150ull;
  std::unique_ptr<GuestExecutionReelCollector> collector =
      MakeCollector(config);

  // These are guest-side PM4 markers selected by identity. They do not assert
  // that the host acquired or presented a drawable.
  GuestExecutionReelEventRange first = RecordEvents(*collector, 101, 1, 10, 1);
  GuestExecutionReelEventRange second =
      RecordEvents(*collector, 102, 1, 10, 1, GuestExecutionReelAction::kStop);
  GuestExecutionReelEventRange segment_range = {first.first_sequence,
                                                second.last_sequence, 2};
  REQUIRE(
      collector->RecordSegment(PublishedSegment(0, 100, 102), segment_range) ==
      GuestExecutionReelAction::kStop);
  REQUIRE(collector->status().stop_reason ==
          GuestExecutionReelStopReason::kGuestMarkerCount);
  REQUIRE(collector->Complete(103));

  config.boundary.value = 1;
  collector = MakeCollector(config);
  REQUIRE(collector->RecordEvents(101, 2, 1, 2) ==
          GuestExecutionReelAction::kReject);
  REQUIRE(collector->status().rejection ==
          GuestExecutionReelRejection::kBoundaryOvershoot);
}

TEST_CASE(
    "guest execution reel instruction boundary is exact and overflow-safe",
    "[guest-execution-reel]") {
  SECTION("exact boundary") {
    std::unique_ptr<GuestExecutionReelCollector> collector = MakeCollector(
        MakeConfig(GuestExecutionReelBoundaryKind::kGuestInstructionCount, 10));
    RecordEvents(*collector, 101, 1, 9);
    RecordEvents(*collector, 102, 1, 1, 0, GuestExecutionReelAction::kStop);
    REQUIRE(collector->status().stop_reason ==
            GuestExecutionReelStopReason::kGuestInstructionCount);
  }

  SECTION("boundary overshoot") {
    std::unique_ptr<GuestExecutionReelCollector> collector = MakeCollector(
        MakeConfig(GuestExecutionReelBoundaryKind::kGuestInstructionCount, 10));
    RecordEvents(*collector, 101, 1, 9);
    REQUIRE(collector->RecordEvents(102, 1, 2, 0) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kBoundaryOvershoot);
    REQUIRE(collector->status().guest_instruction_count == 9);
  }

  SECTION("counter overflow precedes limit classification") {
    GuestExecutionReelConfig config = MakeConfig();
    config.limits.maximum_guest_instruction_count =
        std::numeric_limits<uint64_t>::max();
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    RecordEvents(*collector, 101, 1, std::numeric_limits<uint64_t>::max() - 1);
    REQUIRE(collector->RecordEvents(102, 1, 2, 0) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kCounterOverflow);
  }

  SECTION("event sequence overflow precedes its necessarily crossed limit") {
    GuestExecutionReelConfig config = MakeConfig();
    config.first_event_sequence = std::numeric_limits<uint64_t>::max() - 2;
    config.limits.maximum_event_count = 2;
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    RecordEvents(*collector, 101);
    REQUIRE(collector->RecordEvents(102, 2, 2, 0) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kEventSequenceOverflow);
  }
}

TEST_CASE("guest execution reel duration and tick order fail closed",
          "[guest-execution-reel]") {
  SECTION("policy duration") {
    std::unique_ptr<GuestExecutionReelCollector> collector = MakeCollector(
        MakeConfig(GuestExecutionReelBoundaryKind::kCaptureDurationTicks, 50));
    REQUIRE(collector->Poll(149) == GuestExecutionReelAction::kContinue);
    REQUIRE(collector->Poll(150) == GuestExecutionReelAction::kStop);
    REQUIRE(collector->status().stop_reason ==
            GuestExecutionReelStopReason::kCaptureDuration);
  }

  SECTION("manual hard duration") {
    GuestExecutionReelConfig config = MakeConfig();
    config.limits.maximum_duration_ticks = 25;
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    REQUIRE(collector->Poll(125) == GuestExecutionReelAction::kStop);
    REQUIRE(collector->status().stop_reason ==
            GuestExecutionReelStopReason::kMaximumDuration);
  }

  SECTION("tick regression") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    RecordEvents(*collector, 110);
    REQUIRE(collector->Poll(109) == GuestExecutionReelAction::kReject);
    const GuestExecutionReelStatus status = collector->status();
    REQUIRE(status.rejection == GuestExecutionReelRejection::kTickOrdering);
    REQUIRE(status.capture_end_tick == 110);
  }
}

TEST_CASE("guest execution reel hard bounds stop or reject without truncation",
          "[guest-execution-reel]") {
  SECTION("exact event bound") {
    GuestExecutionReelConfig config = MakeConfig();
    config.limits.maximum_event_count = 2;
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    GuestExecutionReelEventRange range =
        RecordEvents(*collector, 101, 2, 2, 0, GuestExecutionReelAction::kStop);
    REQUIRE(range.event_count == 2);
    REQUIRE(collector->status().stop_reason ==
            GuestExecutionReelStopReason::kMaximumEventCount);
  }

  SECTION("event bound overshoot") {
    GuestExecutionReelConfig config = MakeConfig();
    config.limits.maximum_event_count = 2;
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    REQUIRE(collector->RecordEvents(101, 3, 3, 0) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kLimitExceeded);
    REQUIRE(collector->status().accepted_event_count == 0);
    REQUIRE(collector->status().rejected_event_count == 3);
  }

  SECTION("invalid marker batch") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    REQUIRE(collector->RecordEvents(101, 1, 0, 2) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kInvalidCall);
  }
}

TEST_CASE("guest execution reel rejects unsupported work durably",
          "[guest-execution-reel]") {
  std::unique_ptr<GuestExecutionReelCollector> collector =
      MakeCollector(MakeConfig());
  GuestExecutionReelEventRange range;
  REQUIRE(collector->RecordUnsupportedEvents(
              101, 2, ppc::kGuestInvocationDependencyMmio, &range) ==
          GuestExecutionReelAction::kReject);
  REQUIRE(range.first_sequence == 0);
  REQUIRE(range.last_sequence == 1);
  const GuestExecutionReelStatus status = collector->status();
  REQUIRE(status.rejection ==
          GuestExecutionReelRejection::kUnsupportedDependency);
  REQUIRE(status.rejected_event_count == 2);
  REQUIRE(status.unsupported_event_count == 2);
  REQUIRE(status.rejected_dependency_flags ==
          ppc::kGuestInvocationDependencyMmio);
  REQUIRE(status.accepted_event_count == 0);

  collector = MakeCollector(MakeConfig());
  const uint32_t unknown_dependency =
      ppc::kGuestInvocationKnownDependencyMask + 1;
  REQUIRE(collector->RecordUnsupportedEvents(101, 1, unknown_dependency) ==
          GuestExecutionReelAction::kReject);
  REQUIRE(collector->status().rejection ==
          GuestExecutionReelRejection::kInvalidCall);
  REQUIRE(collector->status().unsupported_event_count == 0);
  REQUIRE(collector->status().rejected_dependency_flags == unknown_dependency);
}

TEST_CASE(
    "guest execution reel validates coordinator outcomes before accepting",
    "[guest-execution-reel]") {
  SECTION("unsupported rejected segment") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestInvocationCaptureStatus segment;
    segment.state = GuestInvocationCaptureState::kRejected;
    segment.recorder_state = ppc::GuestInvocationRecorderState::kRejected;
    segment.rejection =
        ppc::GuestInvocationRecorderRejection::kUnsupportedDependency;
    segment.rejected_dependency_flags =
        ppc::kGuestInvocationDependencyAtomicReservation;
    segment.rejected_segment_count = 1;
    segment.capture_start_tick = 100;
    segment.capture_end_tick = 101;
    REQUIRE(collector->RecordSegment(segment, {}) ==
            GuestExecutionReelAction::kReject);
    const GuestExecutionReelStatus status = collector->status();
    REQUIRE(status.rejection ==
            GuestExecutionReelRejection::kUnsupportedDependency);
    REQUIRE(status.rejected_segment_count == 1);
    REQUIRE(status.rejected_event_count == 1);
    REQUIRE(status.unsupported_event_count == 1);
  }

  SECTION("publication failure") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestInvocationCaptureStatus segment;
    segment.state = GuestInvocationCaptureState::kPublicationFailed;
    segment.recorder_state = ppc::GuestInvocationRecorderState::kComplete;
    segment.rejected_segment_count = 1;
    segment.capture_start_tick = 100;
    segment.capture_end_tick = 101;
    REQUIRE(collector->RecordSegment(segment, {}) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kRejectedSegment);
  }

  SECTION("published status with a diagnostic is malformed") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestExecutionReelEventRange range = RecordEvents(*collector, 101);
    GuestInvocationCaptureStatus segment = PublishedSegment(0, 100, 101);
    segment.message = "unexpected diagnostic";
    REQUIRE(collector->RecordSegment(segment, range) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kInvalidSegmentStatus);
  }

  SECTION("unknown dependency flags are malformed, not supported metadata") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestInvocationCaptureStatus segment;
    segment.state = GuestInvocationCaptureState::kRejected;
    segment.recorder_state = ppc::GuestInvocationRecorderState::kRejected;
    segment.rejected_dependency_flags =
        ppc::kGuestInvocationKnownDependencyMask + 1;
    segment.capture_start_tick = 100;
    segment.capture_end_tick = 101;
    REQUIRE(collector->RecordSegment(segment, {}) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kInvalidSegmentStatus);
    REQUIRE(collector->status().unsupported_event_count == 0);
  }

  SECTION("malformed published status") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestExecutionReelEventRange range = RecordEvents(*collector, 101);
    GuestInvocationCaptureStatus segment = PublishedSegment(0, 100, 101);
    segment.accepted_segment_count = 0;
    REQUIRE(collector->RecordSegment(segment, range) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kInvalidSegmentStatus);
  }
}

TEST_CASE(
    "guest execution reel ordering failures take deterministic precedence",
    "[guest-execution-reel]") {
  SECTION("ordinal before unsupported status") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestInvocationCaptureStatus segment;
    segment.state = GuestInvocationCaptureState::kRejected;
    segment.recorder_state = ppc::GuestInvocationRecorderState::kRejected;
    segment.segment_ordinal = 1;
    segment.rejected_dependency_flags = ppc::kGuestInvocationDependencyMmio;
    segment.capture_start_tick = 100;
    segment.capture_end_tick = 101;
    REQUIRE(collector->RecordSegment(segment, {}) ==
            GuestExecutionReelAction::kReject);
    const GuestExecutionReelStatus status = collector->status();
    REQUIRE(status.rejection == GuestExecutionReelRejection::kSegmentOrdering);
    REQUIRE(status.unsupported_event_count == 0);
  }

  SECTION("event ranges may have gaps but never overlap") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestExecutionReelEventRange first = RecordEvents(*collector, 101, 2, 2);
    REQUIRE(collector->RecordSegment(PublishedSegment(0, 100, 101), first) ==
            GuestExecutionReelAction::kContinue);
    RecordEvents(*collector, 102, 1, 1);
    GuestExecutionReelEventRange third = RecordEvents(*collector, 103, 1, 1);
    REQUIRE(collector->RecordSegment(PublishedSegment(1, 101, 103), third) ==
            GuestExecutionReelAction::kContinue);

    GuestInvocationCaptureStatus overlap = PublishedSegment(2, 103, 104);
    REQUIRE(collector->RecordSegment(overlap, third) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kSegmentOrdering);
  }

  SECTION("segment tick intervals may meet but never overlap") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestExecutionReelEventRange first = RecordEvents(*collector, 101);
    REQUIRE(collector->RecordSegment(PublishedSegment(0, 100, 102), first) ==
            GuestExecutionReelAction::kContinue);
    GuestExecutionReelEventRange second = RecordEvents(*collector, 103);
    REQUIRE(collector->RecordSegment(PublishedSegment(1, 101, 103), second) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kTickOrdering);
  }

  SECTION("segment ordinal overflow precedes count-limit classification") {
    GuestExecutionReelConfig config = MakeConfig();
    config.first_segment_ordinal = std::numeric_limits<uint64_t>::max() - 1;
    config.limits.maximum_segment_count = 1;
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(config);
    GuestExecutionReelEventRange reserved = RecordEvents(*collector, 101, 2, 2);
    GuestExecutionReelEventRange first = {reserved.first_sequence,
                                          reserved.first_sequence, 1};
    REQUIRE(collector->RecordSegment(
                PublishedSegment(std::numeric_limits<uint64_t>::max() - 1, 100,
                                 101),
                first) == GuestExecutionReelAction::kStop);
    GuestExecutionReelEventRange second = {reserved.last_sequence,
                                           reserved.last_sequence, 1};
    REQUIRE(
        collector->RecordSegment(
            PublishedSegment(std::numeric_limits<uint64_t>::max(), 101, 102),
            second) == GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kCounterOverflow);
  }

  SECTION("segment cannot end before observed events") {
    std::unique_ptr<GuestExecutionReelCollector> collector =
        MakeCollector(MakeConfig());
    GuestExecutionReelEventRange range = RecordEvents(*collector, 110);
    REQUIRE(collector->RecordSegment(PublishedSegment(0, 100, 109), range) ==
            GuestExecutionReelAction::kReject);
    REQUIRE(collector->status().rejection ==
            GuestExecutionReelRejection::kTickOrdering);
  }
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

#endif
