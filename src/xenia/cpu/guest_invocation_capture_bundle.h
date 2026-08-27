/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_CAPTURE_BUNDLE_H_
#define XENIA_CPU_GUEST_INVOCATION_CAPTURE_BUNDLE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_replay_cli.h"
#include "xenia/cpu/guest_invocation_replay_config.h"

namespace xe {
namespace cpu {

inline constexpr char kGuestInvocationCaptureBundleSchema[] =
    "xenia-guest-invocation-capture-bundle-v1";
inline constexpr char kGuestInvocationCaptureCorpusFileName[] =
    "invocation.jcorpus";
inline constexpr char kGuestInvocationCaptureArtifactFileName[] =
    "invocation.guest_invocation";
inline constexpr char kGuestInvocationCaptureManifestFileName[] =
    "manifest.json";

// Publishes one accepted real-capture input as an indivisible local bundle.
// exact_corpus_bytes must contain the complete invocation-specific translation
// closure, including successful-definition order and captured host code sizes.
// replay_config must be eligible for timed replay. The output directory and its
// sibling `<name>.part` staging directory must not exist.
//
// Corpus and invocation bytes are strict-decoded before and after writing, and
// all written files are reread and rehashed before one final same-parent,
// no-replace directory rename. A failure removes only the staging directory
// created by this call. The manifest contains hashes, addresses, extents,
// counts and the canonical effective configuration, but never captured code,
// memory or register bytes.
bool WriteGuestInvocationCaptureBundle(
    const std::filesystem::path& output_directory,
    const std::vector<uint8_t>& exact_corpus_bytes,
    const ppc::GuestFunctionInvocation& invocation,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplayConfig& replay_config,
    std::string* error = nullptr);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_CAPTURE_BUNDLE_H_
