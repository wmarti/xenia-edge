/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_runtime_config.h"

#include <charconv>
#include <limits>
#include <string_view>

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace cpu {
namespace {

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool ParseAddress(std::string_view text, std::string_view option_name,
                  uint32_t* output, std::string* error) {
  if (!output) {
    return Fail(error, "capture address output is missing");
  }
  if (text.size() == 10 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if (text.size() != 8) {
    return Fail(
        error, fmt::format("--{} must contain exactly eight hexadecimal digits",
                           option_name));
  }
  uint32_t value = 0;
  const std::from_chars_result result =
      std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    return Fail(error, fmt::format("--{} contains a non-hexadecimal character",
                                   option_name));
  }
  *output = value;
  return true;
}

}  // namespace

bool GuestInvocationCaptureRuntimeConfig::IsRequested() const {
  return !output_directory.empty() || !root_address.empty() ||
         !root_end_address.empty() || occurrence != 0 ||
         max_attempts != kDefaultMaxAttempts ||
         max_duration_ms != kDefaultMaxDurationMs ||
         max_pages != kDefaultMaxPages || max_accesses != kDefaultMaxAccesses ||
         max_call_depth != kDefaultMaxCallDepth ||
         max_events != kDefaultMaxEvents ||
         max_functions != kDefaultMaxFunctions;
}

bool GuestInvocationCaptureRuntimeConfig::BuildRecorderConfiguration(
    uint64_t host_tick_frequency,
    ppc::GuestInvocationRecorderSelection* selection,
    ppc::GuestInvocationRecorderLimits* limits, std::string* error) const {
  if (error) {
    error->clear();
  }
  if (!selection || !limits) {
    return Fail(error, "capture recorder configuration output is missing");
  }
  *selection = {};
  *limits = {};
  if (output_directory.empty() || root_address.empty() ||
      root_end_address.empty() || !occurrence) {
    return Fail(error,
                "capture requires output, root address, root end address and "
                "a 1-based occurrence");
  }
  ppc::GuestInvocationRecorderSelection parsed_selection;
  ppc::GuestInvocationRecorderLimits parsed_limits;
  if (!ParseAddress(root_address, "guest_invocation_capture_root_address",
                    &parsed_selection.root_address, error) ||
      !ParseAddress(root_end_address,
                    "guest_invocation_capture_root_end_address",
                    &parsed_selection.root_end_address, error)) {
    return false;
  }
  parsed_selection.occurrence = occurrence;

  if (!host_tick_frequency || !max_duration_ms ||
      max_duration_ms >
          std::numeric_limits<uint64_t>::max() / host_tick_frequency) {
    return Fail(error,
                "capture maximum duration cannot be represented in host ticks");
  }
  const uint64_t duration_product = max_duration_ms * host_tick_frequency;
  parsed_limits.max_duration_ticks =
      duration_product / 1000 + (duration_product % 1000 != 0);
  if (!parsed_limits.max_duration_ticks) {
    return Fail(error, "capture maximum duration rounded to zero host ticks");
  }
  parsed_limits.max_attempts = max_attempts;
  parsed_limits.max_page_count = max_pages;
  parsed_limits.max_access_count = max_accesses;
  parsed_limits.max_call_depth = max_call_depth;
  parsed_limits.max_event_count = max_events;
  parsed_limits.max_function_count = max_functions;
  *selection = parsed_selection;
  *limits = parsed_limits;
  return true;
}

bool GuestInvocationCaptureRuntimeConfig::ValidateOutputDirectory(
    std::string* error) const {
  if (error) {
    error->clear();
  }
  if (output_directory.empty() || output_directory == "." ||
      output_directory == ".." ||
      output_directory == output_directory.root_path() ||
      output_directory.filename().empty()) {
    return Fail(error, "capture bundle output directory is unsafe");
  }

  std::error_code filesystem_error;
  if (std::filesystem::exists(output_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "capture bundle output directory already exists");
  }
  const std::filesystem::path parent_directory =
      output_directory.has_parent_path() ? output_directory.parent_path() : ".";
  if (!std::filesystem::is_directory(parent_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error,
                "capture bundle output parent is missing or not a directory");
  }
  std::filesystem::path staging_directory = output_directory;
  staging_directory += ".part";
  if (std::filesystem::exists(staging_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "capture bundle staging directory already exists");
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
