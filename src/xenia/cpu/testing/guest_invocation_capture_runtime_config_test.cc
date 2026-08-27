/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_runtime_config.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <string>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const std::string prefix =
        "xenia-guest-invocation-capture-config-test-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint32_t suffix = 0; suffix < 100; ++suffix) {
      std::error_code error;
      std::filesystem::path candidate = std::filesystem::temp_directory_path() /
                                        (prefix + "-" + std::to_string(suffix));
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        return;
      }
    }
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

GuestInvocationCaptureRuntimeConfig MakeCompleteConfig(
    const std::filesystem::path& output_directory = "capture-output") {
  GuestInvocationCaptureRuntimeConfig config;
  config.output_directory = output_directory;
  config.root_address = "0X82040000";
  config.root_end_address = "820400FC";
  config.occurrence = 3;
  return config;
}

}  // namespace

TEST_CASE("guest invocation capture runtime configuration is strict",
          "[guest-invocation-capture]") {
  GuestInvocationCaptureRuntimeConfig config;
  REQUIRE_FALSE(config.IsRequested());

  ppc::GuestInvocationRecorderSelection selection;
  ppc::GuestInvocationRecorderLimits limits;
  std::string error;
  REQUIRE_FALSE(config.BuildRecorderConfiguration(1'000'000, &selection,
                                                  &limits, &error));
  REQUIRE_FALSE(error.empty());

  config.max_attempts += 1;
  REQUIRE(config.IsRequested());
  REQUIRE_FALSE(config.BuildRecorderConfiguration(1'000'000, &selection,
                                                  &limits, &error));

  config = MakeCompleteConfig();
  config.max_attempts = 11;
  config.max_duration_ms = 30'001;
  config.max_pages = 12;
  config.max_accesses = 13;
  config.max_call_depth = 14;
  config.max_events = 15;
  config.max_functions = 16;
  REQUIRE(config.BuildRecorderConfiguration(1'000'000, &selection, &limits,
                                            &error));
  REQUIRE(error.empty());
  REQUIRE(selection.root_address == 0x82040000u);
  REQUIRE(selection.root_end_address == 0x820400FCu);
  REQUIRE(selection.occurrence == 3);
  REQUIRE(limits.max_attempts == 11);
  REQUIRE(limits.max_duration_ticks == 30'001'000);
  REQUIRE(limits.max_page_count == 12);
  REQUIRE(limits.max_access_count == 13);
  REQUIRE(limits.max_call_depth == 14);
  REQUIRE(limits.max_event_count == 15);
  REQUIRE(limits.max_function_count == 16);
}

TEST_CASE("guest invocation capture address parsing fails closed",
          "[guest-invocation-capture]") {
  GuestInvocationCaptureRuntimeConfig config = MakeCompleteConfig();
  ppc::GuestInvocationRecorderSelection selection;
  ppc::GuestInvocationRecorderLimits limits;
  std::string error;

  for (const std::string& invalid_address :
       {std::string("8204000"), std::string("820400000"),
        std::string("0x8204000"), std::string("0x820400000"),
        std::string("8204000g"), std::string("8204 000")}) {
    config.root_end_address = invalid_address;
    selection.root_address = 1;
    limits.max_attempts = 1;
    REQUIRE_FALSE(config.BuildRecorderConfiguration(1'000'000, &selection,
                                                    &limits, &error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(selection.root_address == 0);
    REQUIRE(limits.max_attempts ==
            ppc::GuestInvocationRecorderLimits().max_attempts);
  }
}

TEST_CASE("guest invocation capture duration conversion is checked",
          "[guest-invocation-capture]") {
  GuestInvocationCaptureRuntimeConfig config = MakeCompleteConfig();
  ppc::GuestInvocationRecorderSelection selection;
  ppc::GuestInvocationRecorderLimits limits;
  std::string error;

  config.max_duration_ms = 1;
  REQUIRE(config.BuildRecorderConfiguration(1, &selection, &limits, &error));
  REQUIRE(limits.max_duration_ticks == 1);

  REQUIRE_FALSE(
      config.BuildRecorderConfiguration(0, &selection, &limits, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(selection.root_address == 0);
  REQUIRE(limits.max_duration_ticks ==
          ppc::GuestInvocationRecorderLimits().max_duration_ticks);

  config.max_duration_ms = 0;
  REQUIRE_FALSE(
      config.BuildRecorderConfiguration(1, &selection, &limits, &error));

  config.max_duration_ms = std::numeric_limits<uint64_t>::max();
  REQUIRE_FALSE(
      config.BuildRecorderConfiguration(2, &selection, &limits, &error));
}

TEST_CASE("guest invocation capture output preflight never overwrites",
          "[guest-invocation-capture]") {
  TemporaryDirectory temporary_directory;
  REQUIRE_FALSE(temporary_directory.path().empty());
  GuestInvocationCaptureRuntimeConfig config =
      MakeCompleteConfig(temporary_directory.path() / "capture");
  std::string error;

  REQUIRE(config.ValidateOutputDirectory(&error));
  REQUIRE(error.empty());

  SECTION("existing output") {
    REQUIRE(std::filesystem::create_directory(config.output_directory));
    REQUIRE_FALSE(config.ValidateOutputDirectory(&error));
    REQUIRE(error.find("already exists") != std::string::npos);
  }

  SECTION("existing staging output") {
    std::filesystem::path staging = config.output_directory;
    staging += ".part";
    REQUIRE(std::filesystem::create_directory(staging));
    REQUIRE_FALSE(config.ValidateOutputDirectory(&error));
    REQUIRE(error.find("staging") != std::string::npos);
  }

  SECTION("missing parent") {
    config.output_directory =
        temporary_directory.path() / "missing" / "capture";
    REQUIRE_FALSE(config.ValidateOutputDirectory(&error));
    REQUIRE(error.find("parent") != std::string::npos);
  }

  SECTION("unsafe output") {
    for (const std::filesystem::path& unsafe :
         {std::filesystem::path(), std::filesystem::path("."),
          std::filesystem::path(".."),
          std::filesystem::temp_directory_path().root_path()}) {
      config.output_directory = unsafe;
      REQUIRE_FALSE(config.ValidateOutputDirectory(&error));
      REQUIRE(error.find("unsafe") != std::string::npos);
    }
  }
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
