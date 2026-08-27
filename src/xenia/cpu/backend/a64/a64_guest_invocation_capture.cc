/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_guest_invocation_capture.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {
namespace {

GuestInvocationCaptureEventSink* GetCaptureSink(
    const ppc::PPCContext* context) {
  if (!context || !context->processor) {
    return nullptr;
  }
  return context->processor->guest_invocation_capture_sink();
}

ppc::GuestInvocationRecorderIdentity GetCaptureIdentity(
    const ppc::PPCContext& context) {
  return {reinterpret_cast<uintptr_t>(&context), context.thread_id};
}

}  // namespace

uint64_t CaptureGuestInvocationFunctionEntry(void* raw_context,
                                             uint64_t function_address,
                                             uint64_t function_end_address) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* sink = GetCaptureSink(context);
  if (sink) {
    sink->OnFunctionEntry(GetCaptureIdentity(*context),
                          static_cast<uint32_t>(function_address),
                          static_cast<uint32_t>(function_end_address),
                          ppc::CaptureGuestPPCRegisterState(*context));
  }
  return 0;
}

uint64_t CaptureGuestInvocationFunctionExit(void* raw_context,
                                            uint64_t function_address,
                                            uint64_t return_address) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* sink = GetCaptureSink(context);
  if (sink) {
    sink->OnFunctionExit(GetCaptureIdentity(*context),
                         static_cast<uint32_t>(function_address),
                         static_cast<uint32_t>(return_address),
                         ppc::CaptureGuestPPCRegisterState(*context));
  }
  return 0;
}

uint64_t CaptureGuestInvocationTailCall(void* raw_context,
                                        uint64_t from_address,
                                        uint64_t target_address) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* sink = GetCaptureSink(context);
  if (sink) {
    sink->OnTailCall(GetCaptureIdentity(*context),
                     static_cast<uint32_t>(from_address),
                     static_cast<uint32_t>(target_address));
  }
  // An indirect target may arrive in a caller-saved register. Returning it
  // through x0 lets the emitter restore the exact target after the callback.
  return static_cast<uint32_t>(target_address);
}

uint64_t CaptureGuestInvocationUnsupportedDependency(
    void* raw_context, uint64_t dependency_flags) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* sink = GetCaptureSink(context);
  if (sink) {
    sink->OnUnsupportedDependency(GetCaptureIdentity(*context),
                                  static_cast<uint32_t>(dependency_flags));
  }
  return 0;
}

uint64_t CaptureGuestInvocationUnwindOrLongjmp(void* raw_context) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* sink = GetCaptureSink(context);
  if (sink) {
    sink->OnUnwindOrLongjmp(GetCaptureIdentity(*context));
  }
  return 0;
}

uint64_t CaptureGuestInvocationAsyncReentry(void* raw_context) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto* sink = GetCaptureSink(context);
  if (sink) {
    sink->OnAsyncReentry(GetCaptureIdentity(*context));
  }
  return 0;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif
