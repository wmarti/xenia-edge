/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler_capture_observer.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

namespace xe {
namespace kernel {

GuestSchedulerCaptureEventRecorder::GuestSchedulerCaptureEventRecorder(
    size_t capacity)
    : capacity_(capacity) {
  events_.reserve(capacity_);
  signal_witnesses_.reserve(capacity_);
}

GuestSchedulerCaptureEventRecorder::~GuestSchedulerCaptureEventRecorder() =
    default;

bool GuestSchedulerCaptureEventRecorder::Arm() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (armed_ || shutdown_seen_ ||
      rejection_ != GuestSchedulerCaptureRecorderRejection::kNone) {
    return false;
  }
  armed_ = true;
  return true;
}

bool GuestSchedulerCaptureEventRecorder::Disarm() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!armed_) {
    return false;
  }
  armed_ = false;
  return true;
}

bool GuestSchedulerCaptureEventRecorder::OnSchedulerEvent(
    const GuestSchedulerCaptureEvent& event) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rejection_ != GuestSchedulerCaptureRecorderRejection::kNone) {
    return false;
  }
  ++delivered_count_;
  const bool shutdown = event.kind == GuestSchedulerCaptureEventKind::kShutdown;
  if (event.kind == GuestSchedulerCaptureEventKind::kNone ||
      (!shutdown && !event.capture_instance_id)) {
    rejection_ = GuestSchedulerCaptureRecorderRejection::kInvalidEvent;
    return false;
  }
  if (shutdown_seen_) {
    rejection_ = GuestSchedulerCaptureRecorderRejection::kEventAfterShutdown;
    return false;
  }
  if (event.sequence <= last_sequence_) {
    rejection_ = GuestSchedulerCaptureRecorderRejection::kSequenceRegression;
    return false;
  }
  if (event.sequence != last_sequence_ + 1) {
    rejection_ = GuestSchedulerCaptureRecorderRejection::kSequenceGap;
    return false;
  }
  last_sequence_ = event.sequence;
  if (armed_) {
    if (events_.size() >= capacity_) {
      rejection_ = GuestSchedulerCaptureRecorderRejection::kOverflow;
      return false;
    }
    events_.push_back(event);
  }
  if (shutdown) {
    shutdown_seen_ = true;
    armed_ = false;
  }
  return true;
}

bool GuestSchedulerCaptureEventRecorder::OnSchedulerSignalWitness(
    const GuestSchedulerCaptureSignalWitness& witness) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rejection_ != GuestSchedulerCaptureRecorderRejection::kNone) {
    return false;
  }
  if (!witness.signal_epoch ||
      witness.after_scheduler_sequence > last_sequence_ ||
      (witness.capture_instance_id && !witness.guest_thread_id)) {
    rejection_ = GuestSchedulerCaptureRecorderRejection::kInvalidSignalWitness;
    return false;
  }
  if (shutdown_seen_) {
    rejection_ = GuestSchedulerCaptureRecorderRejection::kEventAfterShutdown;
    return false;
  }
  if (armed_) {
    if (signal_witnesses_.size() >= capacity_) {
      rejection_ = GuestSchedulerCaptureRecorderRejection::kOverflow;
      return false;
    }
    signal_witnesses_.push_back(witness);
  }
  return true;
}

bool GuestSchedulerCaptureEventRecorder::CanDetach() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return !armed_;
}

GuestSchedulerCaptureRecorderSnapshot
GuestSchedulerCaptureEventRecorder::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  GuestSchedulerCaptureRecorderSnapshot snapshot;
  snapshot.rejection = rejection_;
  snapshot.armed = armed_;
  snapshot.shutdown_seen = shutdown_seen_;
  snapshot.delivered_count = delivered_count_;
  snapshot.last_sequence = last_sequence_;
  snapshot.events = events_;
  snapshot.signal_witnesses = signal_witnesses_;
  return snapshot;
}

}  // namespace kernel
}  // namespace xe

#endif
