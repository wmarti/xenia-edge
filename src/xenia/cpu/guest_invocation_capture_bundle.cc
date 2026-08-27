/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_bundle.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

#include "third_party/crypto/sha256.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_invocation_runner.h"

#if XE_PLATFORM_MAC
#include <sys/stdio.h>
#elif XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#elif XE_PLATFORM_LINUX
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace xe {
namespace cpu {
namespace {

constexpr uint64_t kMaximumManifestSize = 1024 * 1024;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

GuestInvocationReplaySha256 HashBytes(const uint8_t* data, size_t size) {
  sha256::SHA256 hasher;
  hasher.add(data, size);
  GuestInvocationReplaySha256 digest = {};
  static_assert(sizeof(unsigned char) == sizeof(uint8_t));
  hasher.getHash(reinterpret_cast<unsigned char*>(digest.data()));
  return digest;
}

GuestInvocationReplaySha256 HashBytes(const std::vector<uint8_t>& bytes) {
  return HashBytes(bytes.data(), bytes.size());
}

bool WriteBytesExclusive(const std::filesystem::path& path, const uint8_t* data,
                         size_t size, std::string* error) {
  // `x` is the C11 exclusive-create mode. Even if another process creates a
  // fixed bundle file after staging-directory creation, it is never replaced.
  FILE* file = filesystem::OpenFile(path, "wbx");
  if (!file) {
    return Fail(error, "failed to exclusively create a capture bundle file");
  }
  const bool write_succeeded =
      !size || std::fwrite(data, 1, size, file) == size;
  const bool flush_succeeded = std::fflush(file) == 0;
  const bool close_succeeded = std::fclose(file) == 0;
  if (!write_succeeded || !flush_succeeded || !close_succeeded) {
    return Fail(error, "failed to write a complete capture bundle file");
  }
  return true;
}

bool WriteBytesExclusive(const std::filesystem::path& path,
                         const std::vector<uint8_t>& bytes,
                         std::string* error) {
  return WriteBytesExclusive(path, bytes.data(), bytes.size(), error);
}

class StagingDirectory {
 public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~StagingDirectory() {
    if (active_) {
      std::error_code filesystem_error;
      std::filesystem::remove_all(path_, filesystem_error);
    }
  }

  void Release() { active_ = false; }

 private:
  std::filesystem::path path_;
  bool active_ = true;
};

bool PublishDirectoryNoReplace(const std::filesystem::path& staging_directory,
                               const std::filesystem::path& output_directory,
                               std::string* error) {
#if XE_PLATFORM_MAC
  if (::renamex_np(staging_directory.c_str(), output_directory.c_str(),
                   RENAME_EXCL) == 0) {
    return true;
  }
#elif XE_PLATFORM_WIN32
  // MoveFileW, unlike MoveFileExW with MOVEFILE_REPLACE_EXISTING, fails if the
  // destination already exists.
  if (::MoveFileW(staging_directory.c_str(), output_directory.c_str())) {
    return true;
  }
#elif XE_PLATFORM_LINUX && defined(SYS_renameat2)
  constexpr unsigned int kRenameNoReplace = 1;
  if (::syscall(SYS_renameat2, AT_FDCWD, staging_directory.c_str(), AT_FDCWD,
                output_directory.c_str(), kRenameNoReplace) == 0) {
    return true;
  }
#endif
  return Fail(error,
              "failed to atomically publish the capture bundle without "
              "replacement");
}

void AppendJsonString(std::string* output, std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  output->push_back('"');
  for (unsigned char character : value) {
    switch (character) {
      case '"':
        output->append("\\\"");
        break;
      case '\\':
        output->append("\\\\");
        break;
      case '\b':
        output->append("\\b");
        break;
      case '\f':
        output->append("\\f");
        break;
      case '\n':
        output->append("\\n");
        break;
      case '\r':
        output->append("\\r");
        break;
      case '\t':
        output->append("\\t");
        break;
      default:
        if (character < 0x20) {
          output->append("\\u00");
          output->push_back(kHex[character >> 4]);
          output->push_back(kHex[character & 0xFu]);
        } else {
          output->push_back(static_cast<char>(character));
        }
        break;
    }
  }
  output->push_back('"');
}

std::string HexAddress(uint32_t address) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result = "0x00000000";
  for (size_t i = 0; i < 8; ++i) {
    result[2 + i] = kHex[(address >> ((7 - i) * 4)) & 0xFu];
  }
  return result;
}

std::string BuildManifest(
    const ExecutionJitCorpus& corpus,
    const ppc::GuestFunctionInvocation& invocation,
    const GuestInvocationReplayPlan& plan,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplaySha256& replay_config_sha256,
    const GuestInvocationReplayConfig& replay_config,
    const GuestInvocationReplaySha256& corpus_sha256, size_t corpus_byte_size,
    const GuestInvocationReplaySha256& artifact_sha256,
    size_t artifact_byte_size) {
  const ExecutionJitCorpus::FunctionRecord* root =
      corpus.FindFunction(invocation.function_address);
  std::string manifest;
  manifest.reserve(8192);
  manifest.append("{\n  \"schema\": ");
  AppendJsonString(&manifest, kGuestInvocationCaptureBundleSchema);
  manifest.append(",\n  \"capture_build_sha256\": ");
  AppendJsonString(&manifest,
                   GuestInvocationReplaySha256Hex(capture_build_sha256));
  manifest.append(",\n  \"replay_config_sha256\": ");
  AppendJsonString(&manifest,
                   GuestInvocationReplaySha256Hex(replay_config_sha256));
  manifest.append(",\n  \"replay_config\": {\n    \"version\": ");
  manifest.append(std::to_string(GuestInvocationReplayConfig::kVersion));
  manifest.append(",\n    \"backend_name\": ");
  AppendJsonString(&manifest, replay_config.backend_name);
  manifest.append(",\n    \"host_platform\": ");
  manifest.append(
      std::to_string(static_cast<uint32_t>(replay_config.host_platform)));
  manifest.append(",\n    \"indirection_mode\": ");
  manifest.append(
      std::to_string(static_cast<uint32_t>(replay_config.indirection_mode)));
  manifest.append(",\n    \"code_mapping_mode\": ");
  manifest.append(
      std::to_string(static_cast<uint32_t>(replay_config.code_mapping_mode)));
  manifest.append(",\n    \"backend_codegen_features\": ");
  manifest.append(std::to_string(replay_config.backend_codegen_features));
  manifest.append(",\n    \"host_protection_page_size\": ");
  manifest.append(std::to_string(replay_config.host_protection_page_size));
  manifest.append(",\n    \"host_feature_flags_decimal\": ");
  AppendJsonString(&manifest, std::to_string(replay_config.host_feature_flags));
  manifest.append(",\n    \"build_features\": ");
  manifest.append(std::to_string(replay_config.build_features));
  manifest.append(",\n    \"entries\": [\n");
  for (size_t i = 0; i < replay_config.entries.size(); ++i) {
    manifest.append("      {\"name\": ");
    AppendJsonString(&manifest, replay_config.entries[i].name);
    manifest.append(", \"value\": ");
    AppendJsonString(&manifest, replay_config.entries[i].value);
    manifest.push_back('}');
    if (i + 1 != replay_config.entries.size()) {
      manifest.push_back(',');
    }
    manifest.push_back('\n');
  }
  manifest.append("    ]\n  },\n  \"corpus\": {\n    \"file\": ");
  AppendJsonString(&manifest, kGuestInvocationCaptureCorpusFileName);
  manifest.append(",\n    \"sha256\": ");
  AppendJsonString(&manifest, GuestInvocationReplaySha256Hex(corpus_sha256));
  manifest.append(",\n    \"byte_size\": ");
  manifest.append(std::to_string(corpus_byte_size));
  manifest.append(",\n    \"page_count\": ");
  manifest.append(std::to_string(corpus.page_addresses().size()));
  manifest.append(",\n    \"function_count\": ");
  manifest.append(std::to_string(corpus.functions().size()));
  manifest.append(",\n    \"definition_order_count\": ");
  manifest.append(std::to_string(corpus.function_definition_order().size()));
  manifest.append(",\n    \"eager_guest_code_bytes\": ");
  manifest.append(std::to_string(plan.eager_guest_code_bytes));
  manifest.append(",\n    \"captured_host_code_bytes\": ");
  manifest.append(std::to_string(plan.captured_host_code_bytes));
  manifest.append("\n  },\n  \"artifact\": {\n    \"file\": ");
  AppendJsonString(&manifest, kGuestInvocationCaptureArtifactFileName);
  manifest.append(",\n    \"sha256\": ");
  AppendJsonString(&manifest, GuestInvocationReplaySha256Hex(artifact_sha256));
  manifest.append(",\n    \"byte_size\": ");
  manifest.append(std::to_string(artifact_byte_size));
  manifest.append(
      ",\n    \"invocation_count\": 1\n  },\n  \"root\": {\n    "
      "\"function_address\": ");
  AppendJsonString(&manifest, HexAddress(invocation.function_address));
  manifest.append(",\n    \"function_end_address\": ");
  AppendJsonString(&manifest, HexAddress(invocation.function_end_address));
  manifest.append(",\n    \"entry_address\": ");
  AppendJsonString(&manifest, HexAddress(invocation.entry_address));
  manifest.append(",\n    \"expected_return_address\": ");
  AppendJsonString(&manifest, HexAddress(invocation.expected_return_address));
  manifest.append(",\n    \"guest_extent_bytes\": ");
  manifest.append(std::to_string(uint64_t(invocation.function_end_address) -
                                 invocation.function_address + 4));
  manifest.append(",\n    \"captured_host_code_size\": ");
  manifest.append(std::to_string(root->host_code_size));
  manifest.append("\n  },\n  \"counts\": {\n    \"input_data_pages\": ");
  manifest.append(std::to_string(invocation.input_data_pages.size()));
  manifest.append(",\n    \"expected_dirty_pages\": ");
  manifest.append(std::to_string(invocation.expected_dirty_pages.size()));
  manifest.append(",\n    \"supplied_pages\": ");
  manifest.append(std::to_string(plan.supplied_page_addresses.size()));
  manifest.append(",\n    \"reset_pages\": ");
  manifest.append(std::to_string(plan.reset_page_addresses.size()));
  manifest.append(",\n    \"reset_bytes\": ");
  manifest.append(std::to_string(plan.reset_page_addresses.size() *
                                 uint64_t(JitCorpus::kPageSize)));
  manifest.append(",\n    \"protection_granules\": ");
  manifest.append(std::to_string(plan.protection_granules.size()));
  manifest.append("\n  }\n}\n");
  return manifest;
}

bool VerifyWrittenFile(const std::filesystem::path& path, uint64_t maximum_size,
                       const std::vector<uint8_t>& expected_bytes,
                       const GuestInvocationReplaySha256& expected_sha256,
                       GuestInvocationReplayFile* output, std::string* error) {
  GuestInvocationReplayFile written_file;
  if (!ReadGuestInvocationReplayFile(path, maximum_size, &written_file,
                                     error)) {
    return false;
  }
  if (written_file.bytes != expected_bytes ||
      written_file.sha256 != expected_sha256) {
    return Fail(error, "a capture bundle file changed while being written");
  }
  if (output) {
    *output = std::move(written_file);
  }
  return true;
}

}  // namespace

bool WriteGuestInvocationCaptureBundle(
    const std::filesystem::path& output_directory,
    const std::vector<uint8_t>& exact_corpus_bytes,
    const ppc::GuestFunctionInvocation& invocation,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplayConfig& replay_config, std::string* error) {
  if (error) {
    error->clear();
  }
  if (output_directory.empty() || output_directory == "." ||
      output_directory == ".." ||
      output_directory == output_directory.root_path() ||
      output_directory.filename().empty()) {
    return Fail(error, "capture bundle output directory is unsafe");
  }
  if (!ValidateGuestInvocationReplayBenchmarkConfig(replay_config, error)) {
    return false;
  }

  GuestInvocationReplaySha256 replay_config_sha256 = {};
  if (!HashGuestInvocationReplayConfig(replay_config, &replay_config_sha256,
                                       error)) {
    return false;
  }

  ExecutionJitCorpus corpus;
  if (!ExecutionJitCorpus::Decode(exact_corpus_bytes, &corpus, error)) {
    return false;
  }
  GuestInvocationReplayPlan plan;
  if (!BuildGuestInvocationReplayPlan(invocation, corpus,
                                      replay_config.host_protection_page_size,
                                      &plan, error)) {
    return false;
  }

  const GuestInvocationReplaySha256 corpus_sha256 =
      HashBytes(exact_corpus_bytes);
  ppc::GuestInvocationArtifact artifact;
  artifact.capture_build_sha256 = capture_build_sha256;
  artifact.code_corpus_sha256 = corpus_sha256;
  artifact.replay_config_sha256 = replay_config_sha256;
  artifact.invocations.push_back(invocation);
  std::vector<uint8_t> artifact_bytes;
  if (!ppc::GuestInvocationArtifactCodec::Encode(artifact, &artifact_bytes,
                                                 error)) {
    return false;
  }
  ppc::GuestInvocationArtifact decoded_artifact;
  if (!ppc::GuestInvocationArtifactCodec::Decode(artifact_bytes,
                                                 &decoded_artifact, error)) {
    return false;
  }
  if (decoded_artifact != artifact) {
    return Fail(error, "capture artifact failed its pre-write round trip");
  }
  const GuestInvocationReplaySha256 artifact_sha256 = HashBytes(artifact_bytes);

  const std::string manifest = BuildManifest(
      corpus, invocation, plan, capture_build_sha256, replay_config_sha256,
      replay_config, corpus_sha256, exact_corpus_bytes.size(), artifact_sha256,
      artifact_bytes.size());
  const std::vector<uint8_t> manifest_bytes(manifest.cbegin(), manifest.cend());
  if (manifest_bytes.empty() || manifest_bytes.size() > kMaximumManifestSize) {
    return Fail(error, "capture bundle manifest exceeds its size limit");
  }
  const GuestInvocationReplaySha256 manifest_sha256 = HashBytes(manifest_bytes);

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
  if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "failed to create capture bundle staging directory");
  }
  StagingDirectory staging_guard(staging_directory);

  const std::filesystem::path corpus_path =
      staging_directory / kGuestInvocationCaptureCorpusFileName;
  if (!WriteBytesExclusive(corpus_path, exact_corpus_bytes, error)) {
    return false;
  }
  GuestInvocationReplayFile written_corpus;
  if (!VerifyWrittenFile(corpus_path, ExecutionJitCorpus::kMaxCorpusSize,
                         exact_corpus_bytes, corpus_sha256, &written_corpus,
                         error)) {
    return false;
  }
  ExecutionJitCorpus decoded_written_corpus;
  if (!ExecutionJitCorpus::Decode(written_corpus.bytes, &decoded_written_corpus,
                                  error)) {
    return false;
  }
  GuestInvocationReplayPlan written_plan;
  if (!BuildGuestInvocationReplayPlan(invocation, decoded_written_corpus,
                                      replay_config.host_protection_page_size,
                                      &written_plan, error) ||
      written_plan.host_page_size != plan.host_page_size ||
      written_plan.eager_function_count != plan.eager_function_count ||
      written_plan.eager_guest_code_bytes != plan.eager_guest_code_bytes ||
      written_plan.captured_host_code_bytes != plan.captured_host_code_bytes ||
      written_plan.supplied_page_addresses != plan.supplied_page_addresses ||
      written_plan.reset_page_addresses != plan.reset_page_addresses ||
      written_plan.protection_granules != plan.protection_granules) {
    return Fail(error,
                "written capture corpus produced a different replay plan");
  }

  const std::filesystem::path artifact_path =
      staging_directory / kGuestInvocationCaptureArtifactFileName;
  if (!WriteBytesExclusive(artifact_path, artifact_bytes, error)) {
    return false;
  }
  GuestInvocationReplayFile written_artifact;
  if (!VerifyWrittenFile(
          artifact_path, ppc::GuestInvocationArtifactCodec::kMaxArtifactSize,
          artifact_bytes, artifact_sha256, &written_artifact, error)) {
    return false;
  }
  ppc::GuestInvocationArtifact decoded_written_artifact;
  if (!ppc::GuestInvocationArtifactCodec::Decode(
          written_artifact.bytes, &decoded_written_artifact, error)) {
    return false;
  }
  if (decoded_written_artifact != artifact) {
    return Fail(error, "written capture artifact failed its codec round trip");
  }

  const std::filesystem::path manifest_path =
      staging_directory / kGuestInvocationCaptureManifestFileName;
  if (!WriteBytesExclusive(manifest_path, manifest_bytes, error) ||
      !VerifyWrittenFile(manifest_path, kMaximumManifestSize, manifest_bytes,
                         manifest_sha256, nullptr, error)) {
    return false;
  }

  if (!PublishDirectoryNoReplace(staging_directory, output_directory, error)) {
    return false;
  }
  staging_guard.Release();
  return true;
}

}  // namespace cpu
}  // namespace xe
