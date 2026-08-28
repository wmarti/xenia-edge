/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_REPLAY_CLI_H_
#define XENIA_CPU_GUEST_INVOCATION_REPLAY_CLI_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/cpu/guest_execution_session_bundle.h"
#include "xenia/cpu/guest_invocation_runner.h"

namespace xe {
namespace cpu {

using GuestInvocationReplaySha256 = std::array<uint8_t, 32>;

struct GuestInvocationReplayFile {
  std::vector<uint8_t> bytes;
  GuestInvocationReplaySha256 sha256 = {};
};

// Reads and hashes the same bounded byte sequence that a replay decoder will
// consume. Growth, truncation, empty files and size-limit violations fail
// closed rather than hashing different bytes from those that are decoded.
bool ReadGuestInvocationReplayFile(const std::filesystem::path& path,
                                   uint64_t maximum_size,
                                   GuestInvocationReplayFile* output,
                                   std::string* error = nullptr);

// Streams a file into SHA-256 without retaining it. Used for the candidate
// executable, whose exact bytes are provenance rather than replay input.
bool HashGuestInvocationReplayFile(const std::filesystem::path& path,
                                   GuestInvocationReplaySha256* output,
                                   std::string* error = nullptr);

std::string GuestInvocationReplaySha256Hex(
    const GuestInvocationReplaySha256& sha256);

struct GuestInvocationReplayBenchmarkProvenance {
  GuestInvocationReplaySha256 artifact_sha256 = {};
  GuestInvocationReplaySha256 corpus_sha256 = {};
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  GuestInvocationReplaySha256 candidate_build_sha256 = {};
  GuestInvocationReplaySha256 config_sha256 = {};
};

// Returns the driver's canonical 22-field marker without a trailing newline.
// Verification fields are fixed to 1 because callers must only invoke this
// after warm, timed-exit, placement, code-shape and final verification have all
// succeeded.
std::string FormatGuestInvocationReplayBenchmarkMarker(
    const GuestInvocationReplayBenchmarkProvenance& provenance,
    const GuestInvocationReplayMetrics& metrics);

// One grep-addressable record per verdict. The plan record is emitted for
// every outcome so a rejected run is as findable as a planned one.
inline constexpr char kGuestSessionContinuousPlanMarker[] =
    "XENIA_GUEST_SESSION_CONTINUOUS_PLAN_V1";
inline constexpr char kGuestSessionContinuousExecMarker[] =
    "XENIA_GUEST_SESSION_CONTINUOUS_EXEC_V1";

struct GuestSessionContinuousReplayVerdict {
  bool planned = false;
  // Always populated. Quoting is applied to the reason so a decoder message
  // can never end the record early.
  std::string plan_line;
  // Populated only when the plan succeeded.
  std::string exec_line;
};

std::string FormatGuestSessionContinuousPlanRejection(std::string_view reason);

// Applies the continuous lane's two construct-time configuration gates in the
// order the single-invocation lane uses them, then attempts the continuous
// plan. Nothing partial is ever reported: any unexpected state is a rejection
// naming its own reason.
bool AttemptGuestSessionContinuousReplayPlan(
    const GuestExecutionSessionBundle& bundle, uint32_t host_page_size,
    bool runtime_guest_scheduler,
    const GuestInvocationReplaySha256& runtime_replay_config_sha256,
    GuestSessionContinuousReplayVerdict* output);

// Reads a published session bundle directory before applying the same gates.
// A directory that is not a complete bundle rejects with the reader's message.
bool AttemptGuestSessionContinuousReplay(
    const std::filesystem::path& bundle_directory, uint32_t host_page_size,
    bool runtime_guest_scheduler,
    const GuestInvocationReplaySha256& runtime_replay_config_sha256,
    GuestSessionContinuousReplayVerdict* output);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_REPLAY_CLI_H_
