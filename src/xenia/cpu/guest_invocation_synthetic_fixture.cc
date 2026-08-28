/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_synthetic_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/cpu/guest_invocation_runner.h"

namespace xe {
namespace cpu {

namespace {

constexpr uint32_t kGuestPageSize = JitCorpus::kPageSize;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool IsPowerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }

void StoreGuestInstruction(std::array<uint8_t, kGuestPageSize>* page,
                           uint32_t offset, uint32_t instruction) {
  (*page)[offset + 0] = static_cast<uint8_t>(instruction >> 24);
  (*page)[offset + 1] = static_cast<uint8_t>(instruction >> 16);
  (*page)[offset + 2] = static_cast<uint8_t>(instruction >> 8);
  (*page)[offset + 3] = static_cast<uint8_t>(instruction);
}

ppc::GuestInvocationPage MakeDataPage(uint32_t address, uint8_t seed) {
  ppc::GuestInvocationPage page;
  page.guest_address = address;
  for (size_t i = 0; i < page.data.size(); ++i) {
    page.data[i] = static_cast<uint8_t>(seed + i * 13);
  }
  return page;
}

ppc::GuestFunctionInvocation MakeInvocation(uint32_t data_address) {
  ppc::GuestFunctionInvocation invocation;
  invocation.function_address = SyntheticGuestInvocationFixture::kCodeAddress;
  invocation.function_end_address =
      SyntheticGuestInvocationFixture::kCodeAddress + 12;
  invocation.entry_address = SyntheticGuestInvocationFixture::kCodeAddress;
  invocation.expected_return_address =
      SyntheticGuestInvocationFixture::kReturnAddress;
  invocation.input.link_register =
      SyntheticGuestInvocationFixture::kReturnAddress;
  invocation.input.gpr[3] = data_address;
  invocation.expected_output = invocation.input;
  invocation.expected_output.gpr[5] = 42;
  return invocation;
}

bool WriteBytes(const std::filesystem::path& path, const uint8_t* data,
                size_t size, std::string* error) {
  FILE* file = filesystem::OpenFile(path, "wb");
  if (!file) {
    return Fail(error, "failed to create a synthetic fixture file");
  }
  const bool write_succeeded =
      !size || std::fwrite(data, 1, size, file) == size;
  const bool flush_succeeded = std::fflush(file) == 0;
  const bool close_succeeded = std::fclose(file) == 0;
  if (!write_succeeded || !flush_succeeded || !close_succeeded) {
    return Fail(error, "failed to write a complete synthetic fixture file");
  }
  return true;
}

bool WriteBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& data, std::string* error) {
  return WriteBytes(path, data.data(), data.size(), error);
}

bool WriteText(const std::filesystem::path& path, const std::string& text,
               std::string* error) {
  return WriteBytes(path, reinterpret_cast<const uint8_t*>(text.data()),
                    text.size(), error);
}

class StagingDirectory {
 public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~StagingDirectory() {
    if (active_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  void Release() { active_ = false; }

 private:
  std::filesystem::path path_;
  bool active_ = true;
};

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

void AppendArtifactManifestEntry(std::string* manifest, std::string_view name,
                                 std::string_view file_name,
                                 const GuestInvocationReplaySha256& sha256,
                                 std::string_view expected_process_result,
                                 std::string_view fault_address,
                                 const GuestInvocationReplayPlan& plan,
                                 bool trailing_comma) {
  manifest->append("    {\n      \"name\": ");
  AppendJsonString(manifest, name);
  manifest->append(",\n      \"file\": ");
  AppendJsonString(manifest, file_name);
  manifest->append(",\n      \"sha256\": ");
  AppendJsonString(manifest, GuestInvocationReplaySha256Hex(sha256));
  manifest->append(",\n      \"expected_process_result\": ");
  AppendJsonString(manifest, expected_process_result);
  manifest->append(",\n      \"fault_guest_address\": ");
  if (fault_address.empty()) {
    manifest->append("null");
  } else {
    AppendJsonString(manifest, fault_address);
  }
  manifest->append(",\n      \"expected_reset_pages_per_invocation\": ");
  manifest->append(std::to_string(plan.reset_page_addresses.size()));
  manifest->append(",\n      \"expected_reset_bytes_per_invocation\": ");
  manifest->append(std::to_string(plan.reset_page_addresses.size() *
                                  uint64_t(kGuestPageSize)));
  manifest->append("\n    }");
  if (trailing_comma) {
    manifest->push_back(',');
  }
  manifest->push_back('\n');
}

std::string BuildManifest(
    const SyntheticGuestInvocationFixture& fixture,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplaySha256& replay_config_sha256,
    const GuestInvocationReplayConfig& replay_config,
    const GuestInvocationReplaySha256& corpus_sha256,
    const GuestInvocationReplaySha256& valid_artifact_sha256,
    const GuestInvocationReplaySha256& omitted_page_artifact_sha256,
    const GuestInvocationReplaySha256& address_7f_artifact_sha256,
    const GuestInvocationReplayPlan& valid_plan,
    const GuestInvocationReplayPlan& omitted_page_plan,
    const GuestInvocationReplayPlan& address_7f_plan) {
  std::string manifest;
  manifest.reserve(4096);
  manifest.append("{\n  \"schema\": ");
  AppendJsonString(&manifest, "xenia-guest-invocation-synthetic-fixture-v1");
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
  AppendJsonString(&manifest, kSyntheticGuestInvocationCorpusFileName);
  manifest.append(",\n    \"sha256\": ");
  AppendJsonString(&manifest, GuestInvocationReplaySha256Hex(corpus_sha256));
  manifest.append(",\n    \"page_count\": ");
  manifest.append(std::to_string(fixture.corpus.page_addresses().size()));
  manifest.append(",\n    \"function_count\": ");
  manifest.append(std::to_string(fixture.corpus.functions().size()));
  manifest.append(",\n    \"captured_host_code_size\": ");
  manifest.append(std::to_string(fixture.captured_host_code_size));
  manifest.append("\n  },\n  \"artifacts\": [\n");
  AppendArtifactManifestEntry(
      &manifest, "valid", kSyntheticGuestInvocationValidArtifactFileName,
      valid_artifact_sha256, "canonical_success_marker", "", valid_plan, true);
  AppendArtifactManifestEntry(
      &manifest, "omitted_page_fault",
      kSyntheticGuestInvocationOmittedPageArtifactFileName,
      omitted_page_artifact_sha256, "subprocess_failure_without_marker",
      "0x10000000", omitted_page_plan, true);
  AppendArtifactManifestEntry(
      &manifest, "address_7f_fault",
      kSyntheticGuestInvocationAddress7FArtifactFileName,
      address_7f_artifact_sha256, "subprocess_failure_without_marker",
      "0x7f000000", address_7f_plan, false);
  manifest.append("  ]\n}\n");
  return manifest;
}

bool WriteAndVerifyArtifact(
    const std::filesystem::path& path,
    const ppc::GuestFunctionInvocation& invocation,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplaySha256& corpus_sha256,
    const GuestInvocationReplaySha256& replay_config_sha256,
    GuestInvocationReplaySha256* artifact_sha256, std::string* error) {
  ppc::GuestInvocationArtifact artifact;
  artifact.capture_build_sha256 = capture_build_sha256;
  artifact.code_corpus_sha256 = corpus_sha256;
  artifact.replay_config_sha256 = replay_config_sha256;
  artifact.invocations.push_back(invocation);

  std::vector<uint8_t> bytes;
  if (!ppc::GuestInvocationArtifactCodec::Encode(artifact, &bytes, error)) {
    return false;
  }
  ppc::GuestInvocationArtifact decoded_before_write;
  if (!ppc::GuestInvocationArtifactCodec::Decode(bytes, &decoded_before_write,
                                                 error) ||
      decoded_before_write != artifact) {
    return Fail(error,
                "synthetic artifact failed its pre-write codec round trip");
  }
  if (!WriteBytes(path, bytes, error)) {
    return false;
  }

  GuestInvocationReplayFile written_file;
  if (!ReadGuestInvocationReplayFile(
          path, ppc::GuestInvocationArtifactCodec::kMaxArtifactSize,
          &written_file, error)) {
    return false;
  }
  if (written_file.bytes != bytes) {
    return Fail(error, "synthetic artifact changed while being written");
  }
  ppc::GuestInvocationArtifact decoded_after_write;
  if (!ppc::GuestInvocationArtifactCodec::Decode(written_file.bytes,
                                                 &decoded_after_write, error) ||
      decoded_after_write != artifact) {
    return Fail(error,
                "synthetic artifact failed its post-write codec round trip");
  }
  *artifact_sha256 = written_file.sha256;
  return true;
}

}  // namespace

bool BuildSyntheticGuestInvocationFixture(
    uint32_t host_page_size, uint32_t captured_host_code_size,
    uint32_t corpus_config_flags, SyntheticGuestInvocationFixture* output,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "synthetic fixture output is null");
  }
  *output = {};
  if (!IsPowerOfTwo(host_page_size) || host_page_size < kGuestPageSize ||
      host_page_size >
          GuestInvocationReplayConfig::kMaxHostProtectionPageSize) {
    return Fail(error, "synthetic fixture host page size is unsupported");
  }
  if (!captured_host_code_size) {
    return Fail(error,
                "synthetic fixture is missing the captured host code size");
  }

  SyntheticGuestInvocationFixture fixture;
  fixture.host_page_size = host_page_size;
  fixture.captured_host_code_size = captured_host_code_size;
  const uint32_t closure_page_count = host_page_size / kGuestPageSize;

  ExecutionJitCorpusBuilder corpus_builder(corpus_config_flags);
  for (uint32_t i = 0; i < closure_page_count; ++i) {
    std::array<uint8_t, kGuestPageSize> page = {};
    if (!i) {
      StoreGuestInstruction(&page, 0, 0x80A30000u);   // lwz r5, 0(r3)
      StoreGuestInstruction(&page, 4, 0x38A50001u);   // addi r5, r5, 1
      StoreGuestInstruction(&page, 8, 0x90A30000u);   // stw r5, 0(r3)
      StoreGuestInstruction(&page, 12, 0x4E800020u);  // blr
    }
    if (!corpus_builder.AddCodePage(
            SyntheticGuestInvocationFixture::kCodeAddress + i * kGuestPageSize,
            page.data(), page.size(), error)) {
      return false;
    }
  }
  const ExecutionJitCorpus::FunctionRecord function = {
      SyntheticGuestInvocationFixture::kCodeAddress,
      SyntheticGuestInvocationFixture::kCodeAddress + 12,
      captured_host_code_size, 0};
  if (!corpus_builder.AddFunction(function, error) ||
      !corpus_builder.Encode(&fixture.corpus_bytes, error)) {
    return false;
  }

  if (!ExecutionJitCorpus::Decode(fixture.corpus_bytes, &fixture.corpus,
                                  error)) {
    return false;
  }

  fixture.valid_invocation =
      MakeInvocation(SyntheticGuestInvocationFixture::kDataAddress);
  for (uint32_t i = 0; i < closure_page_count; ++i) {
    fixture.valid_invocation.input_data_pages.push_back(MakeDataPage(
        SyntheticGuestInvocationFixture::kDataAddress + i * kGuestPageSize,
        static_cast<uint8_t>(0x20 + i)));
  }
  ppc::GuestInvocationPage& input_page =
      fixture.valid_invocation.input_data_pages.front();
  input_page.data[0] = 0;
  input_page.data[1] = 0;
  input_page.data[2] = 0;
  input_page.data[3] = 41;
  fixture.valid_invocation.expected_dirty_pages.push_back(input_page);
  fixture.valid_invocation.expected_dirty_pages.front().data[3] = 42;

  fixture.omitted_page_fault_invocation =
      MakeInvocation(SyntheticGuestInvocationFixture::kDataAddress);
  fixture.address_7f_fault_invocation =
      MakeInvocation(SyntheticGuestInvocationFixture::kAddress7F);

  GuestInvocationReplayPlan plan;
  if (!BuildGuestInvocationReplayPlan(fixture.valid_invocation, fixture.corpus,
                                      host_page_size, &plan, error) ||
      plan.reset_page_addresses.size() !=
          fixture.valid_invocation.expected_dirty_pages.size()) {
    return Fail(error, "synthetic valid invocation has an invalid reset plan");
  }
  if (!BuildGuestInvocationReplayPlan(fixture.omitted_page_fault_invocation,
                                      fixture.corpus, host_page_size, &plan,
                                      error) ||
      !plan.reset_page_addresses.empty()) {
    return Fail(error, "synthetic omitted-page invocation has an invalid plan");
  }
  if (!BuildGuestInvocationReplayPlan(fixture.address_7f_fault_invocation,
                                      fixture.corpus, host_page_size, &plan,
                                      error) ||
      !plan.reset_page_addresses.empty()) {
    return Fail(error, "synthetic 0x7F invocation has an invalid plan");
  }

  *output = std::move(fixture);
  return true;
}

bool WriteSyntheticGuestInvocationFixture(
    const std::filesystem::path& output_directory,
    const SyntheticGuestInvocationFixture& fixture,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplayConfig& replay_config, std::string* error) {
  if (error) {
    error->clear();
  }
  if (output_directory.empty() || output_directory == "." ||
      output_directory == ".." ||
      output_directory == output_directory.root_path() ||
      output_directory.filename().empty()) {
    return Fail(error, "synthetic fixture output directory is unsafe");
  }
  if (!fixture.captured_host_code_size) {
    return Fail(error,
                "synthetic fixture is missing the captured host code size");
  }
  if (fixture.host_page_size != replay_config.host_protection_page_size) {
    return Fail(error,
                "synthetic fixture and replay config host page sizes differ");
  }
  if (!ValidateGuestInvocationReplayBenchmarkConfig(replay_config, error)) {
    return false;
  }

  GuestInvocationReplaySha256 replay_config_sha256 = {};
  if (!HashGuestInvocationReplayConfig(replay_config, &replay_config_sha256,
                                       error)) {
    return false;
  }

  ExecutionJitCorpus decoded_corpus;
  if (!ExecutionJitCorpus::Decode(fixture.corpus_bytes, &decoded_corpus,
                                  error)) {
    return false;
  }
  if (decoded_corpus.functions().size() != 1 ||
      decoded_corpus.functions().front().address !=
          SyntheticGuestInvocationFixture::kCodeAddress ||
      decoded_corpus.functions().front().host_code_size !=
          fixture.captured_host_code_size ||
      decoded_corpus.page_addresses().size() !=
          fixture.host_page_size / kGuestPageSize) {
    return Fail(error, "synthetic fixture corpus shape is inconsistent");
  }

  GuestInvocationReplayPlan valid_plan;
  GuestInvocationReplayPlan omitted_page_plan;
  GuestInvocationReplayPlan address_7f_plan;
  if (!BuildGuestInvocationReplayPlan(fixture.valid_invocation, decoded_corpus,
                                      fixture.host_page_size, &valid_plan,
                                      error) ||
      !BuildGuestInvocationReplayPlan(fixture.omitted_page_fault_invocation,
                                      decoded_corpus, fixture.host_page_size,
                                      &omitted_page_plan, error) ||
      !BuildGuestInvocationReplayPlan(fixture.address_7f_fault_invocation,
                                      decoded_corpus, fixture.host_page_size,
                                      &address_7f_plan, error)) {
    return false;
  }

  std::error_code filesystem_error;
  if (std::filesystem::exists(output_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "synthetic fixture output directory already exists");
  }
  const std::filesystem::path parent_directory =
      output_directory.has_parent_path() ? output_directory.parent_path() : ".";
  if (!std::filesystem::is_directory(parent_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(
        error, "synthetic fixture output parent is missing or not a directory");
  }

  std::filesystem::path staging_directory = output_directory;
  staging_directory += ".part";
  if (std::filesystem::exists(staging_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "synthetic fixture staging directory already exists");
  }
  if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "failed to create synthetic fixture staging directory");
  }
  StagingDirectory staging_guard(staging_directory);

  const std::filesystem::path corpus_path =
      staging_directory / kSyntheticGuestInvocationCorpusFileName;
  if (!WriteBytes(corpus_path, fixture.corpus_bytes, error)) {
    return false;
  }
  GuestInvocationReplayFile written_corpus;
  if (!ReadGuestInvocationReplayFile(corpus_path,
                                     ExecutionJitCorpus::kMaxCorpusSize,
                                     &written_corpus, error) ||
      written_corpus.bytes != fixture.corpus_bytes) {
    return Fail(error, "synthetic corpus changed while being written");
  }
  ExecutionJitCorpus verified_corpus;
  if (!ExecutionJitCorpus::Decode(written_corpus.bytes, &verified_corpus,
                                  error)) {
    return false;
  }

  GuestInvocationReplaySha256 valid_artifact_sha256 = {};
  GuestInvocationReplaySha256 omitted_page_artifact_sha256 = {};
  GuestInvocationReplaySha256 address_7f_artifact_sha256 = {};
  if (!WriteAndVerifyArtifact(
          staging_directory / kSyntheticGuestInvocationValidArtifactFileName,
          fixture.valid_invocation, capture_build_sha256, written_corpus.sha256,
          replay_config_sha256, &valid_artifact_sha256, error) ||
      !WriteAndVerifyArtifact(
          staging_directory /
              kSyntheticGuestInvocationOmittedPageArtifactFileName,
          fixture.omitted_page_fault_invocation, capture_build_sha256,
          written_corpus.sha256, replay_config_sha256,
          &omitted_page_artifact_sha256, error) ||
      !WriteAndVerifyArtifact(
          staging_directory /
              kSyntheticGuestInvocationAddress7FArtifactFileName,
          fixture.address_7f_fault_invocation, capture_build_sha256,
          written_corpus.sha256, replay_config_sha256,
          &address_7f_artifact_sha256, error)) {
    return false;
  }

  const std::string manifest =
      BuildManifest(fixture, capture_build_sha256, replay_config_sha256,
                    replay_config, written_corpus.sha256, valid_artifact_sha256,
                    omitted_page_artifact_sha256, address_7f_artifact_sha256,
                    valid_plan, omitted_page_plan, address_7f_plan);
  if (!WriteText(staging_directory / kSyntheticGuestInvocationManifestFileName,
                 manifest, error)) {
    return false;
  }

  std::filesystem::rename(staging_directory, output_directory,
                          filesystem_error);
  if (filesystem_error) {
    return Fail(error,
                "failed to atomically publish synthetic fixture directory");
  }
  staging_guard.Release();
  return true;
}

}  // namespace cpu
}  // namespace xe
