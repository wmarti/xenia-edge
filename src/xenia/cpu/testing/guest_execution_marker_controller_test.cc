/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

// Both headers reduce to their include guards when capture is compiled out.
#include "xenia/cpu/guest_execution_marker_controller.h"
#include "xenia/gpu/pm4_marker_sink.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

namespace xe {
namespace cpu {
namespace testing {
namespace {

constexpr uint64_t kWarmupTicks = 1000;

class FakeClock final : public GuestExecutionMarkerClock {
 public:
  uint64_t NowTicks() const noexcept override { return now; }
  uint64_t now = 0;
};

class RecordingBoundarySink final : public GuestExecutionMarkerBoundarySink {
 public:
  bool OnMarkerBoundary(
      const GuestExecutionMarkerBoundary& boundary) noexcept override {
    boundaries.push_back(boundary);
    if (reenter) {
      reentry_result = reenter->RequestStop();
    }
    return accept;
  }

  std::vector<GuestExecutionMarkerBoundary> boundaries;
  bool accept = true;
  GuestExecutionMarkerController* reenter = nullptr;
  bool reentry_result = true;
};

gpu::Pm4MarkerEvent SwapMarker(uint64_t ordinal) {
  gpu::Pm4MarkerEvent event;
  event.source = gpu::Pm4MarkerSource::kPm4Swap;
  event.opcode = gpu::kPm4SwapMarkerOpcode;
  event.ordinal = ordinal;
  event.host_tick = ordinal * 10;
  return event;
}

struct Harness {
  explicit Harness(GuestExecutionMarkerControllerConfig config = {}) {
    if (!config.warmup_ticks) {
      config.warmup_ticks = kWarmupTicks;
    }
    std::string error;
    controller =
        GuestExecutionMarkerController::Create(config, clock, sink, &error);
    REQUIRE(controller);
    REQUIRE(error.empty());
  }

  // Begins at tick 100, sees one marker mid-warmup and arms at the edge.
  void ArmAtOrdinal(uint64_t arm_ordinal) {
    clock.now = 100;
    REQUIRE(controller->Begin());
    clock.now = 100 + kWarmupTicks - 1;
    REQUIRE(controller->OnPm4Marker(SwapMarker(arm_ordinal - 1)));
    REQUIRE(controller->status().state ==
            GuestExecutionMarkerControllerState::kWarming);
    clock.now = 100 + kWarmupTicks;
    REQUIRE(controller->OnPm4Marker(SwapMarker(arm_ordinal)));
    REQUIRE(controller->status().state ==
            GuestExecutionMarkerControllerState::kArmed);
    REQUIRE(sink.boundaries.size() == 1);
  }

  FakeClock clock;
  RecordingBoundarySink sink;
  std::shared_ptr<GuestExecutionMarkerController> controller;
};

}  // namespace

TEST_CASE("Guest marker controller rejects an invalid config",
          "[guest-execution-marker]") {
  FakeClock clock;
  RecordingBoundarySink sink;
  std::string error;
  GuestExecutionMarkerControllerConfig config;
  config.stop_marker_count = 0;
  REQUIRE_FALSE(
      GuestExecutionMarkerController::Create(config, clock, sink, &error));
  REQUIRE_FALSE(error.empty());
  config = {};
  config.max_outstanding_boundaries = 0;
  REQUIRE_FALSE(
      GuestExecutionMarkerController::Create(config, clock, sink, &error));
  REQUIRE_FALSE(error.empty());
  config = {};
  config.marker_source = GuestExecutionSessionMarkerSource::kNone;
  REQUIRE_FALSE(
      GuestExecutionMarkerController::Create(config, clock, sink, &error));
  REQUIRE_FALSE(error.empty());
}

TEST_CASE("Guest marker controller rejects the default config",
          "[guest-execution-marker]") {
  FakeClock clock;
  RecordingBoundarySink sink;
  std::string error;
  // The default warmup is zero, which every caller must override.
  const GuestExecutionMarkerControllerConfig config;
  REQUIRE(config.warmup_ticks == 0);
  REQUIRE_FALSE(
      GuestExecutionMarkerController::Create(config, clock, sink, &error));
  REQUIRE(error == "marker controller requires a nonzero warmup");
  GuestExecutionMarkerControllerConfig warmed = config;
  warmed.warmup_ticks = 1;
  REQUIRE(GuestExecutionMarkerController::Create(warmed, clock, sink, &error));
  REQUIRE(error.empty());
}

TEST_CASE("Guest marker controller rejects clock regression while armed",
          "[guest-execution-marker]") {
  Harness harness;
  harness.ArmAtOrdinal(8);
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE(harness.controller->status().arm_tick == 100 + kWarmupTicks);
  harness.clock.now = 100 + kWarmupTicks - 1;
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(9)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kClockRegressed);
  REQUIRE(harness.sink.boundaries.size() == 1);
}

TEST_CASE("Guest marker controller stays warming without markers",
          "[guest-execution-marker]") {
  Harness harness;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  harness.clock.now = 100 + kWarmupTicks * 3;
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kWarming);
  REQUIRE(status.begin_tick == 100);
  REQUIRE(status.matching_marker_count == 0);
  REQUIRE(harness.sink.boundaries.empty());
}

TEST_CASE("Guest marker controller fails closed on a second Begin",
          "[guest-execution-marker]") {
  Harness harness;
  REQUIRE(harness.controller->Begin());
  REQUIRE_FALSE(harness.controller->Begin());
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kInvalidTransition);
}

TEST_CASE("Guest marker controller fails closed when the counter stalls",
          "[guest-execution-marker]") {
  Harness harness;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  harness.clock.now = 100 + kWarmupTicks * 3;
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kWarming);
  REQUIRE(harness.sink.boundaries.empty());
  // The first marker ever arrives after the warmup: nothing advanced during
  // it, so this is not a live title and must not become the arm marker.
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(1)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kMarkerCounterStalled);
  REQUIRE(harness.sink.boundaries.empty());
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(2)));
}

TEST_CASE("Guest marker controller ignores non-matching markers",
          "[guest-execution-marker]") {
  Harness harness;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  gpu::Pm4MarkerEvent wrong_opcode = SwapMarker(1);
  wrong_opcode.opcode = gpu::kPm4SwapMarkerOpcode + 1;
  gpu::Pm4MarkerEvent wrong_source = SwapMarker(2);
  wrong_source.source = gpu::Pm4MarkerSource::kNone;
  REQUIRE(harness.controller->OnPm4Marker(wrong_opcode));
  harness.clock.now = 100 + kWarmupTicks * 2;
  REQUIRE(harness.controller->OnPm4Marker(wrong_source));
  REQUIRE(harness.controller->OnPm4Marker(wrong_opcode));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kWarming);
  REQUIRE(status.ignored_marker_count == 3);
  REQUIRE(status.warmup_marker_count == 0);
  REQUIRE(status.matching_marker_count == 0);
  REQUIRE(harness.sink.boundaries.empty());
}

TEST_CASE("Guest marker controller arms exactly at the warmup edge",
          "[guest-execution-marker]") {
  Harness harness;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  // Markers before Begin are not counted toward the warmup.
  harness.clock.now = 100 + kWarmupTicks - 1;
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(7)));
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kWarming);
  REQUIRE(harness.controller->status().warmup_marker_count == 1);
  REQUIRE(harness.sink.boundaries.empty());
  harness.clock.now = 100 + kWarmupTicks;
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(8)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kArmed);
  REQUIRE(status.arm_marker_ordinal == 8);
  REQUIRE(status.emitted_boundary_count == 1);
  REQUIRE(harness.sink.boundaries.size() == 1);
  GuestExecutionMarkerBoundary expected;
  expected.sequence = 1;
  expected.kind = GuestExecutionMarkerBoundaryKind::kArm;
  expected.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
  expected.marker_identity = gpu::kPm4SwapMarkerOpcode;
  expected.marker_ordinal = 8;
  expected.marker_host_tick = 80;
  expected.controller_tick = 100 + kWarmupTicks;
  expected.markers_since_arm = 0;
  expected.stop_reason = GuestExecutionSessionStopReason::kRequestedBoundary;
  REQUIRE(harness.sink.boundaries[0] == expected);
}

TEST_CASE("Guest marker controller stops at consecutive markers",
          "[guest-execution-marker]") {
  Harness harness;
  harness.ArmAtOrdinal(8);
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  harness.clock.now += 33;
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(9)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kStopped);
  REQUIRE(status.arm_marker_ordinal == 8);
  REQUIRE(status.stop_marker_ordinal == 9);
  REQUIRE(status.markers_since_arm == 1);
  REQUIRE(harness.sink.boundaries.size() == 2);
  const GuestExecutionMarkerBoundary& stop = harness.sink.boundaries[1];
  REQUIRE(stop.sequence == 2);
  REQUIRE(stop.kind == GuestExecutionMarkerBoundaryKind::kStop);
  REQUIRE(stop.marker_ordinal == 9);
  REQUIRE(stop.markers_since_arm == 1);
  REQUIRE(stop.stop_reason ==
          GuestExecutionSessionStopReason::kRequestedBoundary);
  // Later markers keep the source alive but change nothing.
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(10)));
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kStopped);
  REQUIRE(harness.sink.boundaries.size() == 2);
  REQUIRE(harness.controller->AcknowledgeBoundary(2));
  REQUIRE_FALSE(harness.controller->AcknowledgeBoundary(3));
  REQUIRE_FALSE(harness.controller->RequestStop());
}

TEST_CASE("Guest marker controller rejects an unacknowledged arm as overflow",
          "[guest-execution-marker]") {
  Harness harness;
  harness.ArmAtOrdinal(8);
  harness.clock.now += 33;
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(9)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(
      status.rejection ==
      GuestExecutionMarkerControllerRejection::kOutstandingBoundaryOverflow);
  REQUIRE(status.stop_marker_ordinal == 0);
  REQUIRE(harness.sink.boundaries.size() == 1);
  // Acknowledging late does not revive a failed controller.
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(10)));
  REQUIRE(harness.sink.boundaries.size() == 1);
}

TEST_CASE("Guest marker controller honors a wider outstanding bound",
          "[guest-execution-marker]") {
  GuestExecutionMarkerControllerConfig config;
  config.max_outstanding_boundaries = 2;
  Harness harness(config);
  harness.ArmAtOrdinal(8);
  harness.clock.now += 33;
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(9)));
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kStopped);
  REQUIRE(harness.sink.boundaries.size() == 2);
  REQUIRE_FALSE(harness.controller->AcknowledgeBoundary(2));
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE(harness.controller->AcknowledgeBoundary(2));
}

TEST_CASE("Guest marker controller stops at the selected marker count",
          "[guest-execution-marker]") {
  GuestExecutionMarkerControllerConfig config;
  config.stop_marker_count = 3;
  Harness harness(config);
  harness.ArmAtOrdinal(8);
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(9)));
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(10)));
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kArmed);
  REQUIRE(harness.controller->status().markers_since_arm == 2);
  REQUIRE(harness.sink.boundaries.size() == 1);
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(11)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kStopped);
  REQUIRE(status.stop_marker_ordinal == 11);
  REQUIRE(harness.sink.boundaries.size() == 2);
  REQUIRE(harness.sink.boundaries[1].markers_since_arm == 3);
  REQUIRE(harness.sink.boundaries[1].stop_reason ==
          GuestExecutionSessionStopReason::kRequestedBoundary);
}

TEST_CASE("Guest marker controller stops at the next marker on request",
          "[guest-execution-marker]") {
  GuestExecutionMarkerControllerConfig config;
  config.stop_marker_count = 100;
  Harness harness(config);
  harness.ArmAtOrdinal(8);
  REQUIRE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(9)));
  REQUIRE(harness.controller->RequestStop());
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kArmed);
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(10)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kStopped);
  REQUIRE(status.stop_marker_ordinal == 10);
  REQUIRE(harness.sink.boundaries.size() == 2);
  REQUIRE(harness.sink.boundaries[1].markers_since_arm == 2);
  REQUIRE(harness.sink.boundaries[1].stop_reason ==
          GuestExecutionSessionStopReason::kManualRequest);
}

TEST_CASE("Guest marker controller aborts on a request before arming",
          "[guest-execution-marker]") {
  Harness harness;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(1)));
  REQUIRE(harness.controller->RequestStop());
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kAborted);
  harness.clock.now = 100 + kWarmupTicks;
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(2)));
  REQUIRE(harness.controller->status().state ==
          GuestExecutionMarkerControllerState::kAborted);
  REQUIRE(harness.sink.boundaries.empty());
  REQUIRE_FALSE(harness.controller->Begin());
}

TEST_CASE("Guest marker controller fails closed when the sink rejects",
          "[guest-execution-marker]") {
  Harness harness;
  harness.sink.accept = false;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(1)));
  harness.clock.now = 100 + kWarmupTicks;
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(2)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kBoundarySinkRejected);
  REQUIRE(status.arm_marker_ordinal == 0);
  REQUIRE(status.emitted_boundary_count == 0);
  REQUIRE(harness.sink.boundaries.size() == 1);
  REQUIRE_FALSE(harness.controller->AcknowledgeBoundary(1));
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(3)));
}

TEST_CASE("Guest marker controller fails closed on sink reentry",
          "[guest-execution-marker]") {
  Harness harness;
  harness.sink.reenter = harness.controller.get();
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(1)));
  harness.clock.now = 100 + kWarmupTicks;
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(2)));
  REQUIRE_FALSE(harness.sink.reentry_result);
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kCallbackReentry);
}

TEST_CASE("Guest marker controller rejects ordinal regression",
          "[guest-execution-marker]") {
  Harness harness;
  harness.clock.now = 100;
  REQUIRE(harness.controller->Begin());
  REQUIRE(harness.controller->OnPm4Marker(SwapMarker(5)));
  REQUIRE_FALSE(harness.controller->OnPm4Marker(SwapMarker(5)));
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kMarkerOrdinalRegressed);
}

TEST_CASE("Guest marker controller fails closed when the source shuts down",
          "[guest-execution-marker]") {
  Harness harness;
  harness.ArmAtOrdinal(8);
  harness.controller->OnPm4MarkerSourceShutdown();
  GuestExecutionMarkerControllerStatus status = harness.controller->status();
  REQUIRE(status.state == GuestExecutionMarkerControllerState::kFailed);
  REQUIRE(status.rejection ==
          GuestExecutionMarkerControllerRejection::kMarkerSourceLost);
  REQUIRE(harness.sink.boundaries.size() == 1);

  // A completed window is unaffected.
  Harness stopped;
  stopped.ArmAtOrdinal(8);
  REQUIRE(stopped.controller->AcknowledgeBoundary(1));
  REQUIRE(stopped.controller->OnPm4Marker(SwapMarker(9)));
  stopped.controller->OnPm4MarkerSourceShutdown();
  REQUIRE(stopped.controller->status().state ==
          GuestExecutionMarkerControllerState::kStopped);
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#else

namespace xe {
namespace cpu {
namespace testing {

// Everything above this guard, including both headers, compiles to nothing in
// a normal build; this only proves the guard is the numeric CMake definition.
TEST_CASE("Guest marker controller compiles out of normal builds",
          "[guest-execution-marker]") {
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE)
  REQUIRE(XE_ENABLE_GUEST_INVOCATION_CAPTURE == 0);
#else
  SUCCEED();
#endif
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
