/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xiocompletion.h"

#include "xenia/base/clock.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XIOCompletion::XIOCompletion(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {
  notification_semaphore_ = threading::Semaphore::Create(0, kMaxNotifications);
  assert_not_null(notification_semaphore_);
}

XIOCompletion::~XIOCompletion() = default;

void XIOCompletion::QueueNotification(IONotification& notification) {
  {
    std::unique_lock<std::mutex> lock(notification_lock_);
    notifications_.push(notification);
    notification_semaphore_->Release(1, nullptr);
  }
  kernel_state()->guest_scheduler()->WakeAll();
}

bool XIOCompletion::WaitForNotification(uint64_t wait_ticks,
                                        IONotification* notify) {
  auto ms = std::chrono::milliseconds(TimeoutTicksToMs(wait_ticks));

  if (GuestScheduler::enabled() && XThread::GetCurrentFiberThread()) {
    // Acquire at zero timeout and yield between polls, rather than blocking
    // the dispatch host thread.
    auto* scheduler = kernel_state()->guest_scheduler();
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
    const bool has_deadline =
        wait_ticks !=
        static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
    uint64_t deadline_ms =
        has_deadline ? Clock::QueryHostUptimeMillis() + ms.count() : 0;
#else
    uint64_t deadline_ms = Clock::QueryHostUptimeMillis() + ms.count();
#endif
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
    auto* self = XThread::GetCurrentFiberThread();
    const uint32_t wait_handle_id = handle();
    self->set_cooperative_wait_shape(
        XThread::CooperativeWaitKind::kIoCompletion, &wait_handle_id, 1);
#endif
    while (true) {
      auto poll = threading::Wait(notification_semaphore_.get(), false,
                                  std::chrono::milliseconds(0));
      if (poll == threading::WaitResult::kSuccess) {
        break;
      }
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
      if (deadline_ms && Clock::QueryHostUptimeMillis() >= deadline_ms) {
#else
      if (Clock::QueryHostUptimeMillis() >= deadline_ms) {
#endif
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
        self->clear_cooperative_wait_shape();
#endif
        return false;
      }
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
      scheduler->BlockCurrentThread(deadline_ms);
#else
      scheduler->BlockCurrentThread();
#endif
    }
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
    self->clear_cooperative_wait_shape();
#endif
  } else {
    auto res = threading::Wait(notification_semaphore_.get(), false, ms);
    if (res != threading::WaitResult::kSuccess) {
      return false;
    }
  }

  std::unique_lock<std::mutex> lock(notification_lock_);
  assert_false(notifications_.empty());

  std::memcpy(notify, &notifications_.front(), sizeof(IONotification));
  notifications_.pop();

  return true;
}

}  // namespace kernel
}  // namespace xe
