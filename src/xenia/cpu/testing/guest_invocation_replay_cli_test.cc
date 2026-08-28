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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

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

class ScopedTestDirectory {
 public:
  ScopedTestDirectory() {
    std::error_code filesystem_error;
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error) {
      throw std::runtime_error("temporary directory is unavailable");
    }
    const uint64_t nonce = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint32_t attempt = 0; attempt < 100; ++attempt) {
      path_ = temporary_root /
              ("xenia-continuous-cli-test-" + std::to_string(nonce) + "-" +
               std::to_string(attempt));
      filesystem_error.clear();
      if (std::filesystem::create_directory(path_, filesystem_error)) {
        return;
      }
      if (filesystem_error) {
        throw std::runtime_error("temporary directory could not be created");
      }
    }
    throw std::runtime_error("unique temporary directory could not be created");
  }

  ~ScopedTestDirectory() {
    std::error_code filesystem_error;
    std::filesystem::remove_all(path_, filesystem_error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

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
  metrics.code_shape.sha256 = RepeatedHash(0x66);
  metrics.code_shape.function_count = 3;
  metrics.code_shape.host_instruction_count = 400;
  metrics.code_shape.wide_materialization_site_count = 7;
  metrics.code_shape.pc_relative_site_count = 11;

  const std::string marker =
      FormatGuestInvocationReplayBenchmarkMarker(provenance, metrics);
  REQUIRE(marker ==
          "XENIA_GUEST_INVOCATION_BENCHMARK_V3"
          "\tartifact_sha256=" +
              std::string(64, '1') + "\tcorpus_sha256=" + std::string(64, '2') +
              "\tcapture_build_sha256=" + std::string(64, '3') +
              "\tcandidate_build_sha256=" + std::string(64, '4') +
              "\tconfig_sha256=" + std::string(64, '5') +
              "\tcode_shape_sha256=" + std::string(64, '6') +
              "\tcode_shape_functions=3"
              "\thost_instructions=400"
              "\twide_materialization_sites=7"
              "\tpc_relative_sites=11"
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

TEST_CASE("Continuous session plan rejection is one greppable record",
          "[guest-invocation-replay-cli][continuous]") {
  REQUIRE(FormatGuestSessionContinuousPlanRejection(
              "continuous replay requires a zero-segment session") ==
          "XENIA_GUEST_SESSION_CONTINUOUS_PLAN_V1 status=rejected "
          "reason=\"continuous replay requires a zero-segment session\"");

  SECTION("a quote or backslash cannot end the record early") {
    REQUIRE(FormatGuestSessionContinuousPlanRejection("say \"a\\b\"") ==
            "XENIA_GUEST_SESSION_CONTINUOUS_PLAN_V1 status=rejected "
            "reason=\"say \\\"a\\\\b\\\"\"");
  }

  SECTION("a newline cannot split the record") {
    REQUIRE(FormatGuestSessionContinuousPlanRejection("first\nsecond") ==
            "XENIA_GUEST_SESSION_CONTINUOUS_PLAN_V1 status=rejected "
            "reason=\"first second\"");
  }

  SECTION("an empty reason still names the gap") {
    REQUIRE(FormatGuestSessionContinuousPlanRejection("") ==
            "XENIA_GUEST_SESSION_CONTINUOUS_PLAN_V1 status=rejected "
            "reason=\"continuous replay rejected without a reason\"");
  }
}

TEST_CASE("Continuous session replay rejects input that is not a bundle",
          "[guest-invocation-replay-cli][continuous]") {
  const GuestInvocationReplaySha256 config_sha256 = RepeatedHash(0x20);
  GuestSessionContinuousReplayVerdict verdict;

  SECTION("an empty path rejects without touching the filesystem") {
    REQUIRE_FALSE(AttemptGuestSessionContinuousReplay(
        std::filesystem::path(), 16 * 1024, false, config_sha256, &verdict));
    REQUIRE(verdict.plan_line ==
            FormatGuestSessionContinuousPlanRejection(
                "continuous session bundle path is empty"));
    REQUIRE(verdict.exec_line.empty());
    REQUIRE_FALSE(verdict.planned);
  }

  SECTION("a corrupt bundle directory rejects with the reader's reason") {
    const ScopedTestDirectory directory;
    {
      std::ofstream manifest(
          directory.path() / kGuestExecutionSessionBundleManifestFileName,
          std::ios::binary);
      REQUIRE(manifest.is_open());
      const std::string garbage(64, '\x7F');
      manifest.write(garbage.data(),
                     static_cast<std::streamsize>(garbage.size()));
    }
    REQUIRE_FALSE(AttemptGuestSessionContinuousReplay(
        directory.path(), 16 * 1024, false, config_sha256, &verdict));
    REQUIRE(
        verdict.plan_line.rfind(std::string(kGuestSessionContinuousPlanMarker) +
                                    " status=rejected reason=\"",
                                0) == 0);
    REQUIRE(verdict.plan_line.back() == '"');
    REQUIRE(verdict.exec_line.empty());
    REQUIRE_FALSE(verdict.planned);
  }
}

}  // namespace
}  // namespace test
}  // namespace cpu
}  // namespace xe
