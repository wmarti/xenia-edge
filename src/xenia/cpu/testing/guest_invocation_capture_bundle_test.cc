/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_bundle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_invocation_synthetic_fixture.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {
namespace {

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
              ("xenia-capture-bundle-test-" + std::to_string(nonce) + "-" +
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

void SetConfigValue(GuestInvocationReplayConfig* config, std::string_view name,
                    std::string_view value) {
  const auto it = std::lower_bound(
      config->entries.begin(), config->entries.end(), name,
      [](const GuestInvocationReplayConfigEntry& entry,
         std::string_view candidate) { return entry.name < candidate; });
  REQUIRE(it != config->entries.end());
  REQUIRE(it->name == name);
  it->value = value;
}

GuestInvocationReplayConfig MakeBenchmarkConfig(uint32_t host_page_size) {
  GuestInvocationReplayConfig config;
  config.backend_name = "a64";
  config.host_platform = GuestInvocationReplayHostPlatform::kApple;
  config.indirection_mode = GuestInvocationReplayIndirectionMode::kEncoded;
  config.code_mapping_mode =
      GuestInvocationReplayCodeMappingMode::kWritableExecutable;
  config.host_protection_page_size = host_page_size;
  config.host_feature_flags = 0x0102030405060708ull;
  config.build_features = kGuestInvocationReplayBuildAssertionsDisabled |
                          kGuestInvocationReplayBuildRelease;
  for (std::string_view name : GuestInvocationReplayConfigVariableNames(
           config.backend_name, config.host_platform)) {
    config.entries.push_back({std::string(name), "0"});
  }
  SetConfigValue(&config, "count_call_paths", "false");
  SetConfigValue(&config, "count_physical_remap_hits", "false");
  SetConfigValue(&config, "debug", "false");
  SetConfigValue(&config,
                 "emit_mmio_aware_stores_for_recorded_exception_addresses",
                 "false");
  SetConfigValue(&config, "enable_early_precompilation", "false");
  SetConfigValue(&config, "fold_readonly_guest_memory_loads", "false");
  SetConfigValue(&config, "guest_scheduler", "false");
  SetConfigValue(&config, "inline_mmio_access", "false");
  SetConfigValue(&config, "log_safepoint_pc", "false");
  SetConfigValue(&config, "serialize_guest_function_definitions", "true");
  SetConfigValue(&config, "trace_function_coverage", "false");
  return config;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream.is_open());
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

void WriteSentinel(const std::filesystem::path& path,
                   std::string_view contents) {
  std::ofstream stream(path, std::ios::binary);
  REQUIRE(stream.is_open());
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  REQUIRE(stream.good());
}

}  // namespace

TEST_CASE("capture bundle publishes fixed round-tripped payloads",
          "[guest-invocation-capture-bundle]") {
  ScopedTestDirectory temporary_directory;
  SyntheticGuestInvocationFixture fixture;
  std::string error;
  REQUIRE(
      BuildSyntheticGuestInvocationFixture(16384, 321, 0, &fixture, &error));
  GuestInvocationReplayConfig config = MakeBenchmarkConfig(16384);
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  capture_build_sha256.fill(0xA5);
  const std::filesystem::path output = temporary_directory.path() / "capture";

  REQUIRE(WriteGuestInvocationCaptureBundle(
      output, fixture.corpus_bytes, fixture.valid_invocation,
      capture_build_sha256, config, &error));
  REQUIRE(error.empty());
  REQUIRE(std::filesystem::is_directory(output));
  REQUIRE_FALSE(std::filesystem::exists(output.string() + ".part"));

  std::vector<std::string> file_names;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(output)) {
    REQUIRE(entry.is_regular_file());
    file_names.push_back(entry.path().filename().string());
  }
  std::sort(file_names.begin(), file_names.end());
  std::vector<std::string> expected_file_names = {
      kGuestInvocationCaptureArtifactFileName,
      kGuestInvocationCaptureCorpusFileName,
      kGuestInvocationCaptureManifestFileName};
  std::sort(expected_file_names.begin(), expected_file_names.end());
  REQUIRE(file_names == expected_file_names);

  GuestInvocationReplayFile corpus_file;
  REQUIRE(ReadGuestInvocationReplayFile(
      output / kGuestInvocationCaptureCorpusFileName,
      ExecutionJitCorpus::kMaxCorpusSize, &corpus_file, &error));
  REQUIRE(corpus_file.bytes == fixture.corpus_bytes);
  ExecutionJitCorpus decoded_corpus;
  REQUIRE(
      ExecutionJitCorpus::Decode(corpus_file.bytes, &decoded_corpus, &error));
  REQUIRE(decoded_corpus.function_definition_order() ==
          fixture.corpus.function_definition_order());

  GuestInvocationReplayFile artifact_file;
  REQUIRE(ReadGuestInvocationReplayFile(
      output / kGuestInvocationCaptureArtifactFileName,
      ppc::GuestInvocationArtifactCodec::kMaxArtifactSize, &artifact_file,
      &error));
  ppc::GuestInvocationArtifact artifact;
  REQUIRE(ppc::GuestInvocationArtifactCodec::Decode(artifact_file.bytes,
                                                    &artifact, &error));
  REQUIRE(artifact.capture_build_sha256 == capture_build_sha256);
  REQUIRE(artifact.code_corpus_sha256 == corpus_file.sha256);
  GuestInvocationReplaySha256 config_sha256 = {};
  REQUIRE(HashGuestInvocationReplayConfig(config, &config_sha256, &error));
  REQUIRE(artifact.replay_config_sha256 == config_sha256);
  REQUIRE(artifact.invocations ==
          std::vector<ppc::GuestFunctionInvocation>{fixture.valid_invocation});

  const std::string manifest =
      ReadText(output / kGuestInvocationCaptureManifestFileName);
  REQUIRE(manifest.find(kGuestInvocationCaptureBundleSchema) !=
          std::string::npos);
  REQUIRE(manifest.find(GuestInvocationReplaySha256Hex(capture_build_sha256)) !=
          std::string::npos);
  REQUIRE(manifest.find(GuestInvocationReplaySha256Hex(corpus_file.sha256)) !=
          std::string::npos);
  REQUIRE(manifest.find(GuestInvocationReplaySha256Hex(artifact_file.sha256)) !=
          std::string::npos);
  REQUIRE(manifest.find("\"function_address\": \"0x82040000\"") !=
          std::string::npos);
  REQUIRE(manifest.find("\"function_end_address\": \"0x8204000c\"") !=
          std::string::npos);
  REQUIRE(manifest.find("\"captured_host_code_size\": 321") !=
          std::string::npos);
  REQUIRE(manifest.find("\"page_count\": 4") != std::string::npos);
  REQUIRE(manifest.find("\"input_data_pages\": 4") != std::string::npos);
  REQUIRE(manifest.find("\"title") == std::string::npos);
  REQUIRE(manifest.find("page_data") == std::string::npos);
  REQUIRE(manifest.find("register_state") == std::string::npos);
}

TEST_CASE("capture bundle preserves existing output and staging",
          "[guest-invocation-capture-bundle]") {
  ScopedTestDirectory temporary_directory;
  SyntheticGuestInvocationFixture fixture;
  std::string error;
  REQUIRE(BuildSyntheticGuestInvocationFixture(4096, 123, 0, &fixture, &error));
  const GuestInvocationReplayConfig config = MakeBenchmarkConfig(4096);
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  capture_build_sha256.fill(0x5A);

  const std::filesystem::path existing_output =
      temporary_directory.path() / "existing-output";
  REQUIRE(std::filesystem::create_directory(existing_output));
  const std::filesystem::path output_sentinel = existing_output / "sentinel";
  WriteSentinel(output_sentinel, "preserve-output");
  REQUIRE_FALSE(WriteGuestInvocationCaptureBundle(
      existing_output, fixture.corpus_bytes, fixture.valid_invocation,
      capture_build_sha256, config, &error));
  REQUIRE(error == "capture bundle output directory already exists");
  REQUIRE(ReadText(output_sentinel) == "preserve-output");
  REQUIRE_FALSE(std::filesystem::exists(existing_output.string() + ".part"));

  const std::filesystem::path staged_output =
      temporary_directory.path() / "existing-staging";
  const std::filesystem::path existing_staging =
      staged_output.string() + ".part";
  REQUIRE(std::filesystem::create_directory(existing_staging));
  const std::filesystem::path staging_sentinel = existing_staging / "sentinel";
  WriteSentinel(staging_sentinel, "preserve-staging");
  REQUIRE_FALSE(WriteGuestInvocationCaptureBundle(
      staged_output, fixture.corpus_bytes, fixture.valid_invocation,
      capture_build_sha256, config, &error));
  REQUIRE(error == "capture bundle staging directory already exists");
  REQUIRE_FALSE(std::filesystem::exists(staged_output));
  REQUIRE(ReadText(staging_sentinel) == "preserve-staging");
}

TEST_CASE("capture bundle rejects tampered inputs before staging",
          "[guest-invocation-capture-bundle]") {
  ScopedTestDirectory temporary_directory;
  SyntheticGuestInvocationFixture fixture;
  std::string error;
  REQUIRE(
      BuildSyntheticGuestInvocationFixture(16384, 123, 0, &fixture, &error));
  const GuestInvocationReplayConfig config = MakeBenchmarkConfig(16384);
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  capture_build_sha256.fill(0xC3);

  std::vector<uint8_t> tampered_corpus = fixture.corpus_bytes;
  REQUIRE_FALSE(tampered_corpus.empty());
  tampered_corpus.front() ^= 0xFF;
  const std::filesystem::path corpus_output =
      temporary_directory.path() / "bad-corpus";
  REQUIRE_FALSE(WriteGuestInvocationCaptureBundle(
      corpus_output, tampered_corpus, fixture.valid_invocation,
      capture_build_sha256, config, &error));
  REQUIRE_FALSE(std::filesystem::exists(corpus_output));
  REQUIRE_FALSE(std::filesystem::exists(corpus_output.string() + ".part"));

  ppc::GuestFunctionInvocation mismatched_invocation = fixture.valid_invocation;
  ++mismatched_invocation.function_end_address;
  const std::filesystem::path invocation_output =
      temporary_directory.path() / "bad-invocation";
  REQUIRE_FALSE(WriteGuestInvocationCaptureBundle(
      invocation_output, fixture.corpus_bytes, mismatched_invocation,
      capture_build_sha256, config, &error));
  REQUIRE_FALSE(std::filesystem::exists(invocation_output));
  REQUIRE_FALSE(std::filesystem::exists(invocation_output.string() + ".part"));

  const std::filesystem::path missing_parent =
      temporary_directory.path() / "missing-parent";
  const std::filesystem::path missing_parent_output = missing_parent / "output";
  REQUIRE_FALSE(WriteGuestInvocationCaptureBundle(
      missing_parent_output, fixture.corpus_bytes, fixture.valid_invocation,
      capture_build_sha256, config, &error));
  REQUIRE(error ==
          "capture bundle output parent is missing or not a directory");
  REQUIRE_FALSE(std::filesystem::exists(missing_parent));
}

TEST_CASE("capture bundle publication cannot replace a racing output",
          "[guest-invocation-capture-bundle]") {
  ScopedTestDirectory temporary_directory;
  SyntheticGuestInvocationFixture fixture;
  std::string error;
  REQUIRE(BuildSyntheticGuestInvocationFixture(4096, 123, 0, &fixture, &error));
  // Keep the writer busy after staging becomes observable so the competing
  // directory is deterministically installed after the initial existence
  // check and before publication.
  constexpr uint32_t kInputPageCount = 4096;
  fixture.valid_invocation.input_data_pages.reserve(kInputPageCount);
  for (uint32_t i = 1; i < kInputPageCount; ++i) {
    ppc::GuestInvocationPage page;
    page.guest_address = SyntheticGuestInvocationFixture::kDataAddress +
                         i * JitCorpus::kPageSize;
    page.data.front() = static_cast<uint8_t>(i);
    fixture.valid_invocation.input_data_pages.push_back(std::move(page));
  }
  const GuestInvocationReplayConfig config = MakeBenchmarkConfig(4096);
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  capture_build_sha256.fill(0x7E);
  const std::filesystem::path output =
      temporary_directory.path() / "racing-output";
  const std::filesystem::path staging = output.string() + ".part";
  const std::filesystem::path sentinel = output / "sentinel";

  std::atomic<bool> writer_finished = false;
  std::atomic<bool> competitor_created_output = false;
  std::thread competitor([&]() {
    while (!writer_finished.load(std::memory_order_acquire)) {
      std::error_code filesystem_error;
      if (std::filesystem::exists(staging, filesystem_error) &&
          !filesystem_error &&
          std::filesystem::create_directory(output, filesystem_error)) {
        std::ofstream stream(sentinel, std::ios::binary);
        stream << "preserve-racing-output";
        competitor_created_output.store(stream.good(),
                                        std::memory_order_release);
        return;
      }
      std::this_thread::yield();
    }
  });
  const bool write_succeeded = WriteGuestInvocationCaptureBundle(
      output, fixture.corpus_bytes, fixture.valid_invocation,
      capture_build_sha256, config, &error);
  writer_finished.store(true, std::memory_order_release);
  competitor.join();

  REQUIRE(competitor_created_output.load(std::memory_order_acquire));
  REQUIRE_FALSE(write_succeeded);
  REQUIRE(error ==
          "failed to atomically publish the capture bundle without "
          "replacement");
  REQUIRE(ReadText(sentinel) == "preserve-racing-output");
  REQUIRE_FALSE(std::filesystem::exists(staging));
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
