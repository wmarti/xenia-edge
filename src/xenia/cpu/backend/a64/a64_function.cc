/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_function.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include <atomic>

#include "xenia/cpu/backend/a64/a64_guest_invocation_capture.h"
#endif
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
namespace {

class GuestInvocationCaptureHostEntryGuard {
 public:
  explicit GuestInvocationCaptureHostEntryGuard(
      std::atomic_ref<uint32_t> host_entry_depth)
      : host_entry_depth_(host_entry_depth) {}

  ~GuestInvocationCaptureHostEntryGuard() {
    const uint32_t prior_depth =
        host_entry_depth_.fetch_sub(1, std::memory_order_acq_rel);
    assert_true(prior_depth != 0);
  }

  GuestInvocationCaptureHostEntryGuard(
      const GuestInvocationCaptureHostEntryGuard&) = delete;
  GuestInvocationCaptureHostEntryGuard& operator=(
      const GuestInvocationCaptureHostEntryGuard&) = delete;

 private:
  std::atomic_ref<uint32_t> host_entry_depth_;
};

}  // namespace
#endif

A64Function::A64Function(Module* module, uint32_t address)
    : GuestFunction(module, address) {}

A64Function::~A64Function() {
  // machine_code_ is freed by code cache.
}

void A64Function::Setup(uint8_t* machine_code, size_t machine_code_length) {
  pending_machine_code_ = machine_code;
  pending_machine_code_length_ = machine_code_length;
}

bool A64Function::Publish(A64CodeCache* code_cache) {
  if (!code_cache || !pending_machine_code_ || !pending_machine_code_length_ ||
      machine_code_.load(std::memory_order_relaxed)) {
    return false;
  }
  code_cache->AddIndirection64(
      address(), reinterpret_cast<uint64_t>(pending_machine_code_));
  machine_code_length_.store(pending_machine_code_length_,
                             std::memory_order_relaxed);
  machine_code_.store(pending_machine_code_, std::memory_order_release);
  return true;
}

bool A64Function::CallImpl(ThreadState* thread_state, uint32_t return_address) {
  auto backend =
      reinterpret_cast<A64Backend*>(thread_state->processor()->backend());
  auto thunk = backend->host_to_guest_thunk();
  auto* code = machine_code_.load(std::memory_order_acquire);
  if (!thunk || !code) {
    return false;
  }
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  auto* backend_context =
      backend->BackendContextForGuestContext(thread_state->context());
  std::atomic_ref<uint32_t> host_entry_depth(
      backend_context->guest_invocation_capture_host_entry_depth);
  const uint32_t prior_depth =
      host_entry_depth.fetch_add(1, std::memory_order_acq_rel);
  GuestInvocationCaptureHostEntryGuard host_entry_guard(host_entry_depth);
  if (prior_depth) {
    CaptureGuestInvocationAsyncReentry(thread_state->context());
  }
#endif
  thunk(code, thread_state->context(),
        reinterpret_cast<void*>(uintptr_t(return_address)));
  return true;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
