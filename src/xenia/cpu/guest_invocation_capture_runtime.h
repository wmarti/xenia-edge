/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_CAPTURE_RUNTIME_H_
#define XENIA_CPU_GUEST_INVOCATION_CAPTURE_RUNTIME_H_

#include <memory>

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include <filesystem>
#include <string>

#include "xenia/cpu/guest_invocation_capture.h"
#endif

namespace xe {

class Memory;

namespace cpu {

class Processor;

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

// Owns the bounded capture service registered non-owningly with Processor.
// The emulator creates this after applying per-title configuration and destroys
// it only after guest callbacks have stopped. Capture-specific configuration,
// page reads and atomic publication stay out of the application lifecycle.
class GuestInvocationCaptureRuntime final {
 public:
  static bool IsRequested();

  // Creates and attaches the capture sink. guest_scheduler_enabled is passed
  // explicitly to keep the CPU runtime independent of kernel configuration.
  // Capture fails closed unless it is false.
  static std::unique_ptr<GuestInvocationCaptureRuntime> Create(
      Memory& memory, Processor& processor, bool guest_scheduler_enabled,
      std::string* error = nullptr);

  ~GuestInvocationCaptureRuntime();
  GuestInvocationCaptureRuntime(const GuestInvocationCaptureRuntime&) = delete;
  GuestInvocationCaptureRuntime& operator=(
      const GuestInvocationCaptureRuntime&) = delete;

  // Idempotently detaches the non-owning Processor sink, then finalizes the
  // coordinator. The caller must first stop all guest callbacks.
  void Stop();
  GuestInvocationCaptureStatus status() const;

  const std::filesystem::path& output_directory() const;
  const ppc::GuestInvocationRecorderSelection& selection() const;

 private:
  struct Impl;

  explicit GuestInvocationCaptureRuntime(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

#endif

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_CAPTURE_RUNTIME_H_
