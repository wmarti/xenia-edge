/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_GUEST_INVOCATION_CAPTURE_H_
#define XENIA_CPU_BACKEND_A64_A64_GUEST_INVOCATION_CAPTURE_H_

#include <cstdint>

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

inline constexpr uint32_t kMaximumGuestInvocationCaptureMemoryAccessSize = 128;

// Native callbacks entered through the full guest-to-host thunk. Except for
// the tail target round trip, return values are ignored by emitted code.
uint64_t CaptureGuestInvocationFunctionEntry(void* raw_context,
                                             uint64_t function_address,
                                             uint64_t function_end_address,
                                             uint64_t observed_control = 0);
uint64_t CaptureGuestInvocationFunctionExit(void* raw_context,
                                            uint64_t function_address,
                                            uint64_t return_address,
                                            uint64_t observed_control = 0);
uint64_t CaptureGuestInvocationTailCall(void* raw_context,
                                        uint64_t from_address,
                                        uint64_t target_address,
                                        uint64_t observed_control = 0);
uint64_t CaptureGuestInvocationMemoryAccess(void* raw_context,
                                            uint64_t logical_address,
                                            uint64_t size, uint64_t access,
                                            uint64_t observed_control = 0);
uint64_t CaptureGuestInvocationUnsupportedDependency(
    void* raw_context, uint64_t dependency_flags,
    uint64_t observed_control = 0);
uint64_t CaptureGuestInvocationUnwindOrLongjmp(void* raw_context,
                                               uint64_t observed_control = 0);
uint64_t CaptureGuestInvocationAsyncReentry(void* raw_context,
                                            uint64_t observed_control = 0);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_BACKEND_A64_A64_GUEST_INVOCATION_CAPTURE_H_
