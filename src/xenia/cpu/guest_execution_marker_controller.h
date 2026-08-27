/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_MARKER_CONTROLLER_H_
#define XENIA_CPU_GUEST_EXECUTION_MARKER_CONTROLLER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/cpu/guest_execution_session.h"
#include "xenia/gpu/pm4_marker_sink.h"

namespace xe {
namespace cpu {

// The GPU-side source enum is recorded as a session marker source directly.
static_assert(uint32_t(gpu::Pm4MarkerSource::kNone) ==
              uint32_t(GuestExecutionSessionMarkerSource::kNone));
static_assert(uint32_t(gpu::Pm4MarkerSource::kPm4Swap) ==
              uint32_t(GuestExecutionSessionMarkerSource::kPm4Swap));

enum class GuestExecutionMarkerControllerState : uint8_t {
  kIdle,
  kWarming,
  kArmed,
  // Terminal. The stop boundary was handed off.
  kStopped,
  // Terminal. Stop was requested before arming, so no boundary was emitted.
  kAborted,
  // Terminal. See the rejection; nothing further is emitted.
  kFailed,
};

enum class GuestExecutionMarkerControllerRejection : uint8_t {
  kNone,
  kInvalidConfig,
  kInvalidTransition,
  kCallbackReentry,
  kClockRegressed,
  kMarkerOrdinalRegressed,
  // The warmup elapsed without one matching marker, so the marker counter
  // never advanced and the title is not treated as live.
  kMarkerCounterStalled,
  kArmBoundaryUnacknowledged,
  kOutstandingBoundaryOverflow,
  kBoundarySinkRejected,
  kArmedMarkerSinkRejected,
  kMarkerSourceLost,
};

enum class GuestExecutionMarkerBoundaryKind : uint8_t {
  kArm = 1,
  kStop = 2,
};

// One hand-off from the controller to the session owner. Everything is an
// integer; the owner performs its own rendezvous elsewhere and acknowledges
// the sequence when it is done.
struct GuestExecutionMarkerBoundary {
  uint64_t sequence = 0;
  GuestExecutionMarkerBoundaryKind kind =
      GuestExecutionMarkerBoundaryKind::kArm;
  GuestExecutionSessionMarkerSource marker_source =
      GuestExecutionSessionMarkerSource::kNone;
  uint64_t marker_identity = 0;
  uint64_t marker_ordinal = 0;
  uint64_t marker_host_tick = 0;
  uint64_t controller_tick = 0;
  // Matching markers after the arm marker; zero for kArm.
  uint64_t markers_since_arm = 0;
  // Canonical only for kStop.
  GuestExecutionSessionStopReason stop_reason =
      GuestExecutionSessionStopReason::kRequestedBoundary;

  bool operator==(const GuestExecutionMarkerBoundary&) const = default;
};

class GuestExecutionMarkerClock {
 public:
  virtual ~GuestExecutionMarkerClock() = default;
  virtual uint64_t NowTicks() const noexcept = 0;
};

// Runs on the marker source thread with the controller lock held. Neither
// callback may block, allocate or call back into the controller. Once the arm
// boundary is acknowledged, every matching marker is delivered exactly once
// through OnArmedMarker before any stop boundary for that marker. Returning
// false fails the controller closed.
class GuestExecutionMarkerBoundarySink {
 public:
  virtual ~GuestExecutionMarkerBoundarySink() = default;
  virtual bool OnMarkerBoundary(
      const GuestExecutionMarkerBoundary& boundary) noexcept = 0;
  virtual bool OnArmedMarker(const gpu::Pm4MarkerEvent& event) noexcept = 0;
};

struct GuestExecutionMarkerControllerConfig {
  GuestExecutionSessionMarkerSource marker_source =
      GuestExecutionSessionMarkerSource::kPm4Swap;
  uint64_t marker_identity = gpu::kPm4SwapMarkerOpcode;
  // Ticks of the injected clock, required nonzero. The retained Halo 3
  // protocol uses exactly 100 seconds at that clock's frequency.
  uint64_t warmup_ticks = 0;
  // The stop boundary is this many matching markers after the arm marker.
  uint64_t stop_marker_count = 1;
  // Boundaries handed off but not yet acknowledged when the next one is due.
  // The arm boundary must always be acknowledged before any later matching
  // marker; this separate bound applies to all other boundary handoffs.
  uint64_t max_outstanding_boundaries = 1;
};

struct GuestExecutionMarkerControllerStatus {
  GuestExecutionMarkerControllerState state =
      GuestExecutionMarkerControllerState::kIdle;
  GuestExecutionMarkerControllerRejection rejection =
      GuestExecutionMarkerControllerRejection::kNone;
  uint64_t begin_tick = 0;
  uint64_t arm_tick = 0;
  uint64_t warmup_marker_count = 0;
  uint64_t matching_marker_count = 0;
  uint64_t ignored_marker_count = 0;
  uint64_t arm_marker_ordinal = 0;
  uint64_t stop_marker_ordinal = 0;
  uint64_t markers_since_arm = 0;
  uint64_t forwarded_marker_count = 0;
  uint64_t emitted_boundary_count = 0;
  uint64_t acknowledged_boundary_count = 0;
  bool stop_requested = false;
};

// Arm/stop policy for a marker-delimited capture window: wait exactly the
// configured warmup, require at least one matching marker during it, arm at
// the next matching marker, then stop at the selected later marker or at the
// first one after a manual request. Every input is injected, so it runs without
// a GPU. The controller is the command processor's marker sink; the session
// owner is the boundary sink.
class GuestExecutionMarkerController final : public gpu::Pm4MarkerSink {
 public:
  static std::shared_ptr<GuestExecutionMarkerController> Create(
      const GuestExecutionMarkerControllerConfig& config,
      const GuestExecutionMarkerClock& clock,
      GuestExecutionMarkerBoundarySink& boundary_sink,
      std::string* error = nullptr);

  ~GuestExecutionMarkerController() override;
  GuestExecutionMarkerController(const GuestExecutionMarkerController&) =
      delete;
  GuestExecutionMarkerController& operator=(
      const GuestExecutionMarkerController&) = delete;

  // Starts the warmup at the clock's current tick. Only valid once, from kIdle.
  bool Begin();
  // Before arming this aborts without a boundary; once armed the next matching
  // marker becomes the stop boundary. Callable from any thread.
  bool RequestStop();
  // Owner-side completion of one handed-off boundary, in sequence order.
  bool AcknowledgeBoundary(uint64_t sequence);

  bool OnPm4Marker(const gpu::Pm4MarkerEvent& event) noexcept override;
  bool ShouldFenceAfterPm4Marker(
      const gpu::Pm4MarkerEvent& event) noexcept override;
  void OnPm4MarkerSourceShutdown() noexcept override;

  GuestExecutionMarkerControllerStatus status() const;

 private:
  GuestExecutionMarkerController(
      const GuestExecutionMarkerControllerConfig& config,
      const GuestExecutionMarkerClock& clock,
      GuestExecutionMarkerBoundarySink& boundary_sink);

  bool EnterLocked();
  bool IsTerminalLocked() const;
  void FailLocked(GuestExecutionMarkerControllerRejection rejection);
  bool EmitBoundaryLocked(GuestExecutionMarkerBoundaryKind kind,
                          const gpu::Pm4MarkerEvent& event,
                          uint64_t controller_tick,
                          GuestExecutionSessionStopReason stop_reason);
  bool EmitArmedMarkerLocked(const gpu::Pm4MarkerEvent& event);

  // Recursive so a boundary sink that reenters reaches the explicit reentry
  // rejection instead of deadlocking on the source thread.
  mutable std::recursive_mutex mutex_;
  const GuestExecutionMarkerControllerConfig config_;
  const GuestExecutionMarkerClock& clock_;
  GuestExecutionMarkerBoundarySink& boundary_sink_;
  GuestExecutionMarkerControllerStatus status_;
  uint64_t last_marker_ordinal_ = 0;
  bool in_callback_ = false;
  std::thread::id callback_thread_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_MARKER_CONTROLLER_H_
