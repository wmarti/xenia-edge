/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_BUNDLE_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_BUNDLE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/cpu/guest_execution_session.h"

namespace xe {
namespace cpu {

inline constexpr char kGuestExecutionSessionBundleManifestFileName[] =
    "manifest.xegs";

struct GuestExecutionSessionContentBlob {
  GuestExecutionSessionSha256 sha256 = {};
  std::vector<uint8_t> bytes;

  bool operator==(const GuestExecutionSessionContentBlob&) const = default;
};

// Chunks are in manifest ordinal order. Content blobs may be supplied in any
// order; the writer rejects duplicates and the reader returns digest order.
struct GuestExecutionSessionBundle {
  GuestExecutionSessionManifest manifest;
  std::vector<std::vector<uint8_t>> chunks;
  std::vector<GuestExecutionSessionContentBlob> content_blobs;

  bool operator==(const GuestExecutionSessionBundle&) const = default;
};

struct GuestExecutionSessionBundleLimits {
  GuestExecutionSessionLimits session = {};
  uint32_t maximum_content_blobs = 1u << 20;
  uint64_t maximum_total_content_bytes = 16ull * 1024ull * 1024ull * 1024ull;
  uint64_t maximum_bundle_bytes = 32ull * 1024ull * 1024ull * 1024ull;
};

// Applies the same complete codec, reference, digest, size and closure checks
// used before publication, without touching the filesystem.
bool ValidateGuestExecutionSessionBundle(
    const GuestExecutionSessionBundle& bundle, std::string* error = nullptr,
    GuestExecutionSessionBundleLimits limits = {});

// Validates every codec, session invariant, reference, byte count and digest
// before creating anything. The output and sibling `<name>.part` must not
// exist. Fixed, generated names are written exclusively into the same-parent
// staging directory, durably flushed and reread before one atomic no-replace
// directory rename. A pre-publication failure removes only staging created by
// this call.
bool WriteGuestExecutionSessionBundle(
    const std::filesystem::path& output_directory,
    const GuestExecutionSessionBundle& bundle, std::string* error = nullptr,
    GuestExecutionSessionBundleLimits limits = {});

// Reads only fixed/generated leaf names, rejects a symlink root and every
// symlink, non-regular, extra, missing or substituted entry, then verifies all
// codecs, hashes and content references before returning. On failure output is
// reset and no caller-controlled bundle entry name is followed.
bool ReadGuestExecutionSessionBundle(
    const std::filesystem::path& bundle_directory,
    GuestExecutionSessionBundle* output, std::string* error = nullptr,
    GuestExecutionSessionBundleLimits limits = {});

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_BUNDLE_H_
