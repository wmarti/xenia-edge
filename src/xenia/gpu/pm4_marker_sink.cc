/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/pm4_marker_sink.h"
#include "xenia/base/platform.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <utility>

namespace xe {
namespace gpu {

bool Pm4MarkerDispatcher::AttachSink(std::shared_ptr<Pm4MarkerSink> sink) {
  if (!sink) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_ || sink_failed_ || shut_down_) {
    return false;
  }
  sink_ = std::move(sink);
  return true;
}

bool Pm4MarkerDispatcher::DetachSink(
    const std::shared_ptr<Pm4MarkerSink>& sink) {
  if (!sink) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (sink_ != sink) {
    return false;
  }
  if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
    return false;
  }
  dispatch_condition_.wait(lock, [this]() { return !dispatching_; });
  if (sink_ != sink) {
    return false;
  }
  sink_.reset();
  return true;
}

bool Pm4MarkerDispatcher::sink_failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sink_failed_;
}

void Pm4MarkerDispatcher::NotifyPm4Swap(uint64_t host_tick) noexcept {
  Pm4MarkerEvent event;
  event.source = Pm4MarkerSource::kPm4Swap;
  event.opcode = kPm4SwapMarkerOpcode;
  event.ordinal = marker_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
  event.host_tick = host_tick;

  std::shared_ptr<Pm4MarkerSink> sink;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sink_ || sink_failed_ || shut_down_) {
      return;
    }
    sink = sink_;
    dispatching_ = true;
    dispatch_thread_ = std::this_thread::get_id();
  }

  const bool accepted = sink->OnPm4Marker(event);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatching_ = false;
    dispatch_thread_ = {};
    if (!accepted) {
      sink_failed_ = true;
    }
  }
  dispatch_condition_.notify_all();
}

void Pm4MarkerDispatcher::Shutdown() noexcept {
  std::shared_ptr<Pm4MarkerSink> sink;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    dispatch_condition_.wait(lock, [this]() { return !dispatching_; });
    if (shut_down_) {
      return;
    }
    shut_down_ = true;
    sink = std::move(sink_);
  }
  if (sink) {
    sink->OnPm4MarkerSourceShutdown();
  }
}

}  // namespace gpu
}  // namespace xe

#endif
