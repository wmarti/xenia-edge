/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_replay_cli.h"

#include <array>
#include <string>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {
namespace {

GuestInvocationReplaySha256 RepeatedHash(uint8_t value) {
  GuestInvocationReplaySha256 hash = {};
  hash.fill(value);
  return hash;
}

TEST_CASE("Guest invocation benchmark marker is canonical",
          "[guest-invocation-replay-cli]") {
  GuestInvocationReplayBenchmarkProvenance provenance;
  provenance.artifact_sha256 = RepeatedHash(0x11);
  provenance.corpus_sha256 = RepeatedHash(0x22);
  provenance.capture_build_sha256 = RepeatedHash(0x33);
  provenance.candidate_build_sha256 = RepeatedHash(0x44);
  provenance.config_sha256 = RepeatedHash(0x55);

  GuestInvocationReplayMetrics metrics;
  metrics.timed_invocation_count = 17;
  metrics.reset_page_count_per_invocation = 2;
  metrics.reset_bytes_per_invocation = 8192;
  metrics.thread_cpu_nanoseconds = 123456;
  metrics.uptime_raw_nanoseconds = 234567;
  metrics.reset_only_thread_cpu_nanoseconds = 34567;
  metrics.reset_only_uptime_raw_nanoseconds = 45678;
  metrics.placement_generation_before = 8;
  metrics.placement_generation_after = 8;

  const std::string marker =
      FormatGuestInvocationReplayBenchmarkMarker(provenance, metrics);
  REQUIRE(marker ==
          "XENIA_GUEST_INVOCATION_BENCHMARK_V2"
          "\tartifact_sha256=" +
              std::string(64, '1') + "\tcorpus_sha256=" + std::string(64, '2') +
              "\tcapture_build_sha256=" + std::string(64, '3') +
              "\tcandidate_build_sha256=" + std::string(64, '4') +
              "\tconfig_sha256=" + std::string(64, '5') +
              "\titerations=17"
              "\treset_pages=2"
              "\treset_bytes_per_iteration=8192"
              "\tthread_cpu_ns=123456"
              "\tuptime_raw_ns=234567"
              "\treset_only_thread_cpu_ns=34567"
              "\treset_only_uptime_raw_ns=45678"
              "\tplacement_generation_before=8"
              "\tplacement_generation_after=8"
              "\twarm_verified=1"
              "\ttimed_exit_verified=1"
              "\tfinal_verified=1");
}

}  // namespace
}  // namespace test
}  // namespace cpu
}  // namespace xe
