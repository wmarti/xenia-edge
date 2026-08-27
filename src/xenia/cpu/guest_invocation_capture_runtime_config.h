/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_CAPTURE_RUNTIME_CONFIG_H_
#define XENIA_CPU_GUEST_INVOCATION_CAPTURE_RUNTIME_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>

#include "xenia/cpu/guest_invocation_recorder.h"

namespace xe {
namespace cpu {

// Pointer-free launch inputs for the current one-segment capture primitive.
// The final execution-session selector will replace the root-specific fields,
// while the bounds and no-overwrite output contract remain reusable.
struct GuestInvocationCaptureRuntimeConfig {
  static constexpr uint32_t kDefaultMaxAttempts = 8;
  static constexpr uint64_t kDefaultMaxDurationMs = 30'000;
  static constexpr uint32_t kDefaultMaxPages = 8192;
  static constexpr uint64_t kDefaultMaxAccesses = 1'000'000;
  static constexpr uint32_t kDefaultMaxCallDepth = 256;
  static constexpr uint64_t kDefaultMaxEvents = 2'000'000;
  static constexpr uint32_t kDefaultMaxFunctions = 32'768;

  std::filesystem::path output_directory;
  std::string root_address;
  std::string root_end_address;
  uint32_t occurrence = 0;
  uint32_t max_attempts = kDefaultMaxAttempts;
  uint64_t max_duration_ms = kDefaultMaxDurationMs;
  uint32_t max_pages = kDefaultMaxPages;
  uint64_t max_accesses = kDefaultMaxAccesses;
  uint32_t max_call_depth = kDefaultMaxCallDepth;
  uint64_t max_events = kDefaultMaxEvents;
  uint32_t max_functions = kDefaultMaxFunctions;

  // Any non-default field is an explicit request. A partial request is later
  // rejected rather than silently disabling capture.
  bool IsRequested() const;

  // Converts strict hexadecimal selectors and milliseconds into recorder
  // inputs. Recorder::Create remains the single authority for semantic bounds.
  bool BuildRecorderConfiguration(
      uint64_t host_tick_frequency, uint32_t host_protection_page_size,
      ppc::GuestInvocationRecorderSelection* selection,
      ppc::GuestInvocationRecorderLimits* limits,
      std::string* error = nullptr) const;

  // Early no-overwrite preflight. The atomic bundle writer repeats these
  // checks at publication so a race cannot replace existing data.
  bool ValidateOutputDirectory(std::string* error = nullptr) const;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_CAPTURE_RUNTIME_CONFIG_H_
