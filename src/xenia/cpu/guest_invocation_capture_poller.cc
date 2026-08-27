/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_poller.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <exception>
#include <string_view>
#include <system_error>

namespace xe {
namespace cpu {

namespace {

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

}  // namespace

std::unique_ptr<GuestInvocationCaptureDeadlinePoller>
GuestInvocationCaptureDeadlinePoller::Create(
    GuestInvocationCaptureCoordinator& coordinator,
    std::chrono::milliseconds interval, std::string* error) {
  if (error) {
    error->clear();
  }
  if (interval <= std::chrono::milliseconds::zero()) {
    Fail(error, "capture deadline poll interval must be positive");
    return nullptr;
  }

  std::unique_ptr<GuestInvocationCaptureDeadlinePoller> poller(
      new GuestInvocationCaptureDeadlinePoller(coordinator, interval));
  try {
    poller->thread_ = std::thread(
        &GuestInvocationCaptureDeadlinePoller::ThreadMain, poller.get());
  } catch (const std::system_error& exception) {
    if (error) {
      error->assign("failed to start capture deadline poller: ");
      error->append(exception.what());
    }
    return nullptr;
  }
  return poller;
}

GuestInvocationCaptureDeadlinePoller::GuestInvocationCaptureDeadlinePoller(
    GuestInvocationCaptureCoordinator& coordinator,
    std::chrono::milliseconds interval)
    : coordinator_(coordinator), interval_(interval) {}

GuestInvocationCaptureDeadlinePoller::~GuestInvocationCaptureDeadlinePoller() {
  StopAndJoin();
}

void GuestInvocationCaptureDeadlinePoller::StopAndJoin() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    // Detaching would let the worker outlive the coordinator. StopAndJoin is an
    // owner-side lifecycle operation and may never be called by the worker.
    if (thread_.get_id() == std::this_thread::get_id()) {
      std::terminate();
    }
    thread_.join();
  }
}

void GuestInvocationCaptureDeadlinePoller::ThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_requested_) {
    if (condition_.wait_for(lock, interval_,
                            [this] { return stop_requested_; })) {
      break;
    }

    lock.unlock();
    coordinator_.Poll();
    const GuestInvocationCaptureState state = coordinator_.status().state;
    lock.lock();
    if (state != GuestInvocationCaptureState::kRecording) {
      break;
    }
  }
}

}  // namespace cpu
}  // namespace xe

#endif  // XE_ENABLE_GUEST_INVOCATION_CAPTURE
