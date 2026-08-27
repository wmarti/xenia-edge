/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_STATE_H_
#define XENIA_CPU_THREAD_STATE_H_

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/guest_execution_capture.h"
#endif
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class Processor;

// Per-guest-thread FTRACE bookkeeping. Owned here rather than thread_local so
// the frame stack follows the thread across dispatch CPUs under the guest
// scheduler.
struct FunctionTraceFrame {
  uint32_t address;
  uint64_t token;  // MicroProfileToken
  uint64_t tick;
  bool profiled;
};
struct FunctionTraceState {
  std::vector<FunctionTraceFrame> stack;
  uint32_t open_count = 0;
};

class ThreadState {
 public:
  ThreadState(Processor* processor, uint32_t thread_id, uint32_t stack_base = 0,
              uint32_t pcr_address = 0);
  ~ThreadState();
  ThreadState(const ThreadState&) = delete;
  ThreadState& operator=(const ThreadState&) = delete;
  ThreadState(ThreadState&&) = delete;
  ThreadState& operator=(ThreadState&&) = delete;

  Processor* processor() const { return processor_; }
  Memory* memory() const { return memory_; }
  void* backend_data() const { return backend_data_; }
  ppc::PPCContext* context() const { return context_; }
  uint32_t thread_id() const { return thread_id_; }
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  // Process-local identity for one ThreadState lifetime. Unlike a pointer or
  // guest thread ID, this is not reused after destruction and recreation.
  uint64_t guest_execution_capture_instance_id() const {
    return guest_execution_capture_instance_id_;
  }

  // Every direct ThreadState owner must call this exactly once after all PPC
  // context and owner metadata is initialized, and before exposing or running
  // the thread. Destruction while still pending is valid for failed setup.
  GuestExecutionCaptureThreadStateLifecycleDisposition
  PublishGuestExecutionCaptureReady() noexcept;
#endif

  static void Bind(ThreadState* thread_state);
  static ThreadState* Get();
  static uint32_t GetThreadID();

  FunctionTraceState& function_trace_state() { return function_trace_state_; }

 private:
  friend class Processor;

  Processor* processor_;
  Memory* memory_;
  void* backend_data_;

  uint32_t pcr_address_ = 0;
  uint32_t thread_id_ = 0;
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  uint64_t guest_execution_capture_instance_id_ = 0;
  GuestExecutionCaptureThreadStateLifecycleState
      guest_execution_capture_lifecycle_state_ =
          GuestExecutionCaptureThreadStateLifecycleState::kPending;
  ThreadState* guest_execution_capture_next_ = nullptr;
#endif

  FunctionTraceState function_trace_state_;

  // NOTE: must be 64b aligned for SSE ops.
  ppc::PPCContext* context_;
};

static_assert(!std::is_copy_constructible_v<ThreadState>);
static_assert(!std::is_copy_assignable_v<ThreadState>);
static_assert(!std::is_move_constructible_v<ThreadState>);
static_assert(!std::is_move_assignable_v<ThreadState>);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_STATE_H_
