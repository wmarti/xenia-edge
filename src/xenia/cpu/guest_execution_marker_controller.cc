/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_marker_controller.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <limits>

namespace xe {
namespace cpu {

std::shared_ptr<GuestExecutionMarkerController>
GuestExecutionMarkerController::Create(
    const GuestExecutionMarkerControllerConfig& config,
    const GuestExecutionMarkerClock& clock,
    GuestExecutionMarkerBoundarySink& boundary_sink, std::string* error) {
  if (error) {
    error->clear();
  }
  if (config.marker_source == GuestExecutionSessionMarkerSource::kNone) {
    if (error) {
      error->assign("marker controller requires a marker source");
    }
    return nullptr;
  }
  if (!config.stop_marker_count) {
    if (error) {
      error->assign("marker controller requires a nonzero stop marker count");
    }
    return nullptr;
  }
  if (!config.max_outstanding_boundaries) {
    if (error) {
      error->assign(
          "marker controller requires a nonzero outstanding boundary bound");
    }
    return nullptr;
  }
  // A zero warmup leaves no window in which the marker counter can advance,
  // so the first marker would always be rejected as stalled.
  if (!config.warmup_ticks) {
    if (error) {
      error->assign("marker controller requires a nonzero warmup");
    }
    return nullptr;
  }
  return std::shared_ptr<GuestExecutionMarkerController>(
      new GuestExecutionMarkerController(config, clock, boundary_sink));
}

GuestExecutionMarkerController::GuestExecutionMarkerController(
    const GuestExecutionMarkerControllerConfig& config,
    const GuestExecutionMarkerClock& clock,
    GuestExecutionMarkerBoundarySink& boundary_sink)
    : config_(config), clock_(clock), boundary_sink_(boundary_sink) {}

GuestExecutionMarkerController::~GuestExecutionMarkerController() = default;

bool GuestExecutionMarkerController::EnterLocked() {
  if (in_callback_ && callback_thread_ == std::this_thread::get_id()) {
    FailLocked(GuestExecutionMarkerControllerRejection::kCallbackReentry);
    return false;
  }
  return true;
}

bool GuestExecutionMarkerController::IsTerminalLocked() const {
  switch (status_.state) {
    case GuestExecutionMarkerControllerState::kStopped:
    case GuestExecutionMarkerControllerState::kAborted:
    case GuestExecutionMarkerControllerState::kFailed:
      return true;
    default:
      return false;
  }
}

void GuestExecutionMarkerController::FailLocked(
    GuestExecutionMarkerControllerRejection rejection) {
  if (IsTerminalLocked()) {
    return;
  }
  status_.state = GuestExecutionMarkerControllerState::kFailed;
  status_.rejection = rejection;
}

bool GuestExecutionMarkerController::EmitBoundaryLocked(
    GuestExecutionMarkerBoundaryKind kind, const gpu::Pm4MarkerEvent& event,
    uint64_t controller_tick, GuestExecutionSessionStopReason stop_reason) {
  const uint64_t outstanding =
      status_.emitted_boundary_count - status_.acknowledged_boundary_count;
  if (outstanding >= config_.max_outstanding_boundaries ||
      status_.emitted_boundary_count == std::numeric_limits<uint64_t>::max()) {
    FailLocked(
        GuestExecutionMarkerControllerRejection::kOutstandingBoundaryOverflow);
    return false;
  }
  GuestExecutionMarkerBoundary boundary;
  boundary.sequence = status_.emitted_boundary_count + 1;
  boundary.kind = kind;
  boundary.marker_source = config_.marker_source;
  boundary.marker_identity = config_.marker_identity;
  boundary.marker_ordinal = event.ordinal;
  boundary.marker_host_tick = event.host_tick;
  boundary.controller_tick = controller_tick;
  boundary.markers_since_arm = status_.markers_since_arm;
  boundary.stop_reason = stop_reason;

  in_callback_ = true;
  callback_thread_ = std::this_thread::get_id();
  const bool accepted = boundary_sink_.OnMarkerBoundary(boundary);
  in_callback_ = false;
  // A reentrant call has already failed the controller.
  if (IsTerminalLocked()) {
    return false;
  }
  if (!accepted) {
    FailLocked(GuestExecutionMarkerControllerRejection::kBoundarySinkRejected);
    return false;
  }
  status_.emitted_boundary_count = boundary.sequence;
  return true;
}

bool GuestExecutionMarkerController::EmitArmedMarkerLocked(
    const gpu::Pm4MarkerEvent& event) {
  in_callback_ = true;
  callback_thread_ = std::this_thread::get_id();
  const bool accepted = boundary_sink_.OnArmedMarker(event);
  in_callback_ = false;
  if (IsTerminalLocked()) {
    return false;
  }
  if (!accepted) {
    FailLocked(
        GuestExecutionMarkerControllerRejection::kArmedMarkerSinkRejected);
    return false;
  }
  ++status_.forwarded_marker_count;
  return true;
}

bool GuestExecutionMarkerController::Begin() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!EnterLocked()) {
    return false;
  }
  if (status_.state != GuestExecutionMarkerControllerState::kIdle) {
    FailLocked(GuestExecutionMarkerControllerRejection::kInvalidTransition);
    return false;
  }
  status_.begin_tick = clock_.NowTicks();
  status_.state = GuestExecutionMarkerControllerState::kWarming;
  return true;
}

bool GuestExecutionMarkerController::RequestStop() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!EnterLocked()) {
    return false;
  }
  if (IsTerminalLocked()) {
    return false;
  }
  status_.stop_requested = true;
  if (status_.state != GuestExecutionMarkerControllerState::kArmed) {
    status_.state = GuestExecutionMarkerControllerState::kAborted;
  }
  return true;
}

bool GuestExecutionMarkerController::AcknowledgeBoundary(uint64_t sequence) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!EnterLocked()) {
    return false;
  }
  if (sequence != status_.acknowledged_boundary_count + 1 ||
      sequence > status_.emitted_boundary_count) {
    return false;
  }
  status_.acknowledged_boundary_count = sequence;
  return true;
}

bool GuestExecutionMarkerController::OnPm4Marker(
    const gpu::Pm4MarkerEvent& event) noexcept {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!EnterLocked()) {
    return false;
  }
  if (status_.state == GuestExecutionMarkerControllerState::kFailed) {
    return false;
  }
  if (static_cast<uint32_t>(event.source) !=
          static_cast<uint32_t>(config_.marker_source) ||
      event.opcode != config_.marker_identity) {
    ++status_.ignored_marker_count;
    return true;
  }
  if (!event.ordinal || event.ordinal <= last_marker_ordinal_) {
    FailLocked(
        GuestExecutionMarkerControllerRejection::kMarkerOrdinalRegressed);
    return false;
  }
  last_marker_ordinal_ = event.ordinal;
  ++status_.matching_marker_count;

  switch (status_.state) {
    case GuestExecutionMarkerControllerState::kIdle:
    case GuestExecutionMarkerControllerState::kStopped:
    case GuestExecutionMarkerControllerState::kAborted:
      return true;
    case GuestExecutionMarkerControllerState::kWarming: {
      const uint64_t now = clock_.NowTicks();
      if (now < status_.begin_tick) {
        FailLocked(GuestExecutionMarkerControllerRejection::kClockRegressed);
        return false;
      }
      if (now - status_.begin_tick < config_.warmup_ticks) {
        ++status_.warmup_marker_count;
        return true;
      }
      if (!status_.warmup_marker_count) {
        FailLocked(
            GuestExecutionMarkerControllerRejection::kMarkerCounterStalled);
        return false;
      }
      status_.markers_since_arm = 0;
      if (!EmitBoundaryLocked(
              GuestExecutionMarkerBoundaryKind::kArm, event, now,
              GuestExecutionSessionStopReason::kRequestedBoundary)) {
        return false;
      }
      status_.arm_marker_ordinal = event.ordinal;
      status_.arm_tick = now;
      status_.state = GuestExecutionMarkerControllerState::kArmed;
      return true;
    }
    case GuestExecutionMarkerControllerState::kArmed: {
      // Acknowledging the arm boundary is the owner's proof that the session
      // start rendezvous is complete. Forwarding even one marker before that
      // point would leave an unrecorded prefix in the claimed window.
      if (!status_.acknowledged_boundary_count) {
        FailLocked(GuestExecutionMarkerControllerRejection::
                       kArmBoundaryUnacknowledged);
        return false;
      }
      const uint64_t now = clock_.NowTicks();
      if (now < status_.arm_tick) {
        FailLocked(GuestExecutionMarkerControllerRejection::kClockRegressed);
        return false;
      }
      ++status_.markers_since_arm;
      if (!EmitArmedMarkerLocked(event)) {
        return false;
      }
      GuestExecutionSessionStopReason stop_reason =
          GuestExecutionSessionStopReason::kRequestedBoundary;
      if (status_.markers_since_arm >= config_.stop_marker_count) {
        stop_reason = GuestExecutionSessionStopReason::kRequestedBoundary;
      } else if (status_.stop_requested) {
        stop_reason = GuestExecutionSessionStopReason::kManualRequest;
      } else {
        return true;
      }
      if (!EmitBoundaryLocked(GuestExecutionMarkerBoundaryKind::kStop, event,
                              now, stop_reason)) {
        return false;
      }
      status_.stop_marker_ordinal = event.ordinal;
      status_.state = GuestExecutionMarkerControllerState::kStopped;
      return true;
    }
    default:
      return false;
  }
}

void GuestExecutionMarkerController::OnPm4MarkerSourceShutdown() noexcept {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!EnterLocked()) {
    return;
  }
  FailLocked(GuestExecutionMarkerControllerRejection::kMarkerSourceLost);
}

GuestExecutionMarkerControllerStatus GuestExecutionMarkerController::status()
    const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return status_;
}

}  // namespace cpu
}  // namespace xe

#endif
