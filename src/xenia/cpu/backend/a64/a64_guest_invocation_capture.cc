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

#include <atomic>
#include <limits>

#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

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

bool IsCaptureEventEnabled(ppc::PPCContext* context, uint8_t event_mask) {
  return context &&
         (std::atomic_ref<uint8_t>(context->guest_invocation_capture_event_mask)
              .load(std::memory_order_acquire) &
          event_mask) != 0;
}

ppc::GuestInvocationRecorderIdentity GetCaptureIdentity(
    const ppc::PPCContext& context) {
  return {reinterpret_cast<uintptr_t>(&context), context.thread_id};
}

void RefreshCaptureEventMask(
    ppc::PPCContext& context, GuestInvocationCaptureEventSink& sink,
    const ppc::GuestInvocationRecorderIdentity& identity) {
  std::atomic_ref<uint8_t>(context.guest_invocation_capture_event_mask)
      .store(sink.event_mask(identity), std::memory_order_release);
}

bool IsMmioAccess(const ppc::PPCContext& context, uint32_t address,
                  uint32_t size) {
  if (!size) {
    return false;
  }
  const uint64_t last_byte = uint64_t(address) + size - 1;
  if (last_byte > std::numeric_limits<uint32_t>::max()) {
    // Let OnMemoryAccess reject the wrapped range as an invalid event.
    return false;
  }

  // This is the range used by the A64 inline MMIO checks. Test overlap rather
  // than just the first byte so an ordinary access crossing into MMIO is
  // rejected before the host load or store.
  constexpr uint32_t kInlineMmioStart = 0x7FC00000u;
  constexpr uint32_t kInlineMmioEnd = 0x7FFFFFFFu;
  if (address <= kInlineMmioEnd && last_byte >= kInlineMmioStart) {
    return true;
  }

  auto* memory = context.processor ? context.processor->memory() : nullptr;
  if (!memory) {
    return false;
  }

  // Ordinary accesses are at most 16 bytes and the only emitted bulk memset
  // forms are dcbz/dcbz128. Checking every byte also handles a range that
  // begins immediately before an explicitly mapped MMIO byte.
  for (uint64_t byte = address; byte <= last_byte; ++byte) {
    if (memory->LookupVirtualMappedRange(static_cast<uint32_t>(byte))) {
      return true;
    }
  }
  return false;
}

}  // namespace

uint64_t CaptureGuestInvocationFunctionEntry(void* raw_context,
                                             uint64_t function_address,
                                             uint64_t function_end_address) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  const uint32_t root_address =
      context && context->processor
          ? context->processor->guest_invocation_capture_root_address()
          : 0;
  const uint8_t event_mask = !root_address || function_address == root_address
                                 ? kGuestInvocationCaptureRootEvent
                                 : kGuestInvocationCaptureOwnerEvent;
  if (!IsCaptureEventEnabled(context, event_mask)) {
    return 0;
  }
  auto* sink = GetCaptureSink(context);
  if (sink) {
    const auto identity = GetCaptureIdentity(*context);
    sink->OnFunctionEntry(identity, static_cast<uint32_t>(function_address),
                          static_cast<uint32_t>(function_end_address),
                          ppc::CaptureGuestPPCRegisterState(*context));
    RefreshCaptureEventMask(*context, *sink, identity);
  }
  return 0;
}

uint64_t CaptureGuestInvocationFunctionExit(void* raw_context,
                                            uint64_t function_address,
                                            uint64_t return_address) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!IsCaptureEventEnabled(context, kGuestInvocationCaptureOwnerEvent)) {
    return 0;
  }
  auto* sink = GetCaptureSink(context);
  if (sink) {
    const auto identity = GetCaptureIdentity(*context);
    sink->OnFunctionExit(identity, static_cast<uint32_t>(function_address),
                         static_cast<uint32_t>(return_address),
                         ppc::CaptureGuestPPCRegisterState(*context));
    RefreshCaptureEventMask(*context, *sink, identity);
  }
  return 0;
}

uint64_t CaptureGuestInvocationTailCall(void* raw_context,
                                        uint64_t from_address,
                                        uint64_t target_address) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!IsCaptureEventEnabled(context, kGuestInvocationCaptureOwnerEvent)) {
    return static_cast<uint32_t>(target_address);
  }
  auto* sink = GetCaptureSink(context);
  if (sink) {
    const auto identity = GetCaptureIdentity(*context);
    sink->OnTailCall(identity, static_cast<uint32_t>(from_address),
                     static_cast<uint32_t>(target_address));
    RefreshCaptureEventMask(*context, *sink, identity);
  }
  // An indirect target may arrive in a caller-saved register. Returning it
  // through x0 lets the emitter restore the exact target after the callback.
  return static_cast<uint32_t>(target_address);
}

uint64_t CaptureGuestInvocationMemoryAccess(void* raw_context,
                                            uint64_t logical_address,
                                            uint64_t size, uint64_t access) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  const auto memory_access =
      static_cast<ppc::GuestInvocationRecorderMemoryAccess>(access);
  const uint8_t event_mask =
      memory_access == ppc::GuestInvocationRecorderMemoryAccess::kRead
          ? kGuestInvocationCaptureOwnerEvent
          : kGuestInvocationCaptureWriteEvent;
  if (!IsCaptureEventEnabled(context, event_mask)) {
    return 0;
  }
  auto* sink = GetCaptureSink(context);
  if (!sink) {
    return 0;
  }
  const auto identity = GetCaptureIdentity(*context);
  const uint64_t maximum_access = static_cast<uint64_t>(
      ppc::GuestInvocationRecorderMemoryAccess::kReadWrite);
  if (logical_address > std::numeric_limits<uint32_t>::max() ||
      size > kMaximumGuestInvocationCaptureMemoryAccessSize ||
      access > maximum_access) {
    sink->OnUnsupportedDependency(
        identity,
        ppc::kGuestInvocationDependencyUnsupportedMappingOrProtection);
    RefreshCaptureEventMask(*context, *sink, identity);
    return 0;
  }
  const uint32_t address32 = static_cast<uint32_t>(logical_address);
  const uint32_t size32 = static_cast<uint32_t>(size);
  if (IsMmioAccess(*context, address32, size32)) {
    sink->OnUnsupportedDependency(identity,
                                  ppc::kGuestInvocationDependencyMmio);
  } else {
    sink->OnMemoryAccess(identity, address32, size32, memory_access);
  }
  RefreshCaptureEventMask(*context, *sink, identity);
  return 0;
}

uint64_t CaptureGuestInvocationUnsupportedDependency(
    void* raw_context, uint64_t dependency_flags) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!IsCaptureEventEnabled(context, kGuestInvocationCaptureOwnerEvent)) {
    return 0;
  }
  auto* sink = GetCaptureSink(context);
  if (sink) {
    const auto identity = GetCaptureIdentity(*context);
    sink->OnUnsupportedDependency(identity,
                                  static_cast<uint32_t>(dependency_flags));
    RefreshCaptureEventMask(*context, *sink, identity);
  }
  return 0;
}

uint64_t CaptureGuestInvocationUnwindOrLongjmp(void* raw_context) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!IsCaptureEventEnabled(context, kGuestInvocationCaptureOwnerEvent)) {
    return 0;
  }
  auto* sink = GetCaptureSink(context);
  if (sink) {
    const auto identity = GetCaptureIdentity(*context);
    sink->OnUnwindOrLongjmp(identity);
    RefreshCaptureEventMask(*context, *sink, identity);
  }
  return 0;
}

uint64_t CaptureGuestInvocationAsyncReentry(void* raw_context) {
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!IsCaptureEventEnabled(context, kGuestInvocationCaptureOwnerEvent)) {
    return 0;
  }
  auto* sink = GetCaptureSink(context);
  if (sink) {
    const auto identity = GetCaptureIdentity(*context);
    sink->OnAsyncReentry(identity);
    RefreshCaptureEventMask(*context, *sink, identity);
  }
  return 0;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif
