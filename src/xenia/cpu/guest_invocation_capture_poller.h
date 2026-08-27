/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_CAPTURE_POLLER_H_
#define XENIA_CPU_GUEST_INVOCATION_CAPTURE_POLLER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/cpu/guest_invocation_capture.h"

namespace xe {
namespace cpu {

// Periodically advances a live recorder even when the title produces no
// relevant capture event. The runtime starts this only after installing the
// coordinator as Processor's sink, and must synchronously stop it before
// detaching that sink. No thread is detached, and terminal coordinators stop
// generating wakeups on their own.
class GuestInvocationCaptureDeadlinePoller final {
 public:
  static constexpr std::chrono::milliseconds kDefaultInterval{10};

  static std::unique_ptr<GuestInvocationCaptureDeadlinePoller> Create(
      GuestInvocationCaptureCoordinator& coordinator,
      std::chrono::milliseconds interval = kDefaultInterval,
      std::string* error = nullptr,
      std::function<void()> terminal_callback = {});

  ~GuestInvocationCaptureDeadlinePoller();
  GuestInvocationCaptureDeadlinePoller(
      const GuestInvocationCaptureDeadlinePoller&) = delete;
  GuestInvocationCaptureDeadlinePoller& operator=(
      const GuestInvocationCaptureDeadlinePoller&) = delete;

  // Idempotently wakes and joins the worker. Once this returns, Poll cannot be
  // executing and will never be called again by this object.
  void StopAndJoin();

 private:
  GuestInvocationCaptureDeadlinePoller(
      GuestInvocationCaptureCoordinator& coordinator,
      std::chrono::milliseconds interval,
      std::function<void()> terminal_callback);
  void ThreadMain();

  GuestInvocationCaptureCoordinator& coordinator_;
  const std::chrono::milliseconds interval_;
  std::function<void()> terminal_callback_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stop_requested_ = false;
  std::thread thread_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XE_ENABLE_GUEST_INVOCATION_CAPTURE

#endif  // XENIA_CPU_GUEST_INVOCATION_CAPTURE_POLLER_H_
