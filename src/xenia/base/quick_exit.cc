/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>

namespace xe {
namespace {

std::mutex prepare_mutex;
std::condition_variable prepare_condition;
QuickExitPrepareCallback prepare_callback = nullptr;
void* prepare_context = nullptr;
uint32_t prepare_in_flight = 0;
thread_local bool prepare_active = false;

}  // namespace

bool RegisterQuickExitPrepareCallback(QuickExitPrepareCallback callback,
                                      void* context) noexcept {
  if (!callback || !context) {
    return false;
  }
  std::lock_guard<std::mutex> lock(prepare_mutex);
  if (prepare_callback) {
    return prepare_callback == callback && prepare_context == context;
  }
  if (prepare_in_flight) {
    return false;
  }
  prepare_callback = callback;
  prepare_context = context;
  return true;
}

bool UnregisterQuickExitPrepareCallback(QuickExitPrepareCallback callback,
                                        void* context) noexcept {
  if (!callback || !context || prepare_active) {
    return false;
  }
  std::unique_lock<std::mutex> lock(prepare_mutex);
  if (prepare_callback != callback || prepare_context != context) {
    return false;
  }
  prepare_callback = nullptr;
  prepare_context = nullptr;
  prepare_condition.wait(lock, []() { return prepare_in_flight == 0; });
  return true;
}

void PrepareForQuickExit() noexcept {
  if (prepare_active) {
    return;
  }
  QuickExitPrepareCallback callback = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(prepare_mutex);
    callback = prepare_callback;
    context = prepare_context;
    if (!callback) {
      return;
    }
    ++prepare_in_flight;
  }
  prepare_active = true;
  callback(context);
  prepare_active = false;
  {
    std::lock_guard<std::mutex> lock(prepare_mutex);
    --prepare_in_flight;
  }
  prepare_condition.notify_all();
}

[[noreturn]] void QuickExit(int exit_code) noexcept {
  PrepareForQuickExit();
  FlushLog();
  std::quick_exit(exit_code);
}

}  // namespace xe
