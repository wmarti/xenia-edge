/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_replay_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#if XE_ARCH_ARM64
#include "xenia/base/platform_arm64.h"
#elif XE_ARCH_AMD64
#include "xenia/base/platform_amd64.h"
#endif
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

namespace xe::cpu::test {

namespace {

class ReplayConfigTestCodeCache final : public backend::CodeCache {
 public:
  ReplayConfigTestCodeCache(bool has_indirection_table,
                            bool encoded_indirection, MappingMode mapping_mode)
      : has_indirection_table_(has_indirection_table),
        encoded_indirection_(encoded_indirection),
        mapping_mode_(mapping_mode) {}

  const std::filesystem::path& file_name() const override { return file_name_; }
  uintptr_t execute_base_address() const override { return 0; }
  size_t total_size() const override { return 0; }
  bool has_indirection_table() const override { return has_indirection_table_; }
  bool encoded_indirection() const override { return encoded_indirection_; }
  MappingMode mapping_mode() const override { return mapping_mode_; }
  uint64_t placement_generation() const override { return 0; }
  GuestFunction* LookupFunction(uint64_t host_pc) override { return nullptr; }
  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }
  bool PatchCode(void* execute_address, const void* data,
                 size_t size) override {
    return false;
  }

 private:
  std::filesystem::path file_name_;
  bool has_indirection_table_;
  bool encoded_indirection_;
  MappingMode mapping_mode_;
};

class NamedNullBackend final : public backend::NullBackend {
 public:
  NamedNullBackend(std::string name, backend::CodeCache* code_cache)
      : name_(std::move(name)) {
    code_cache_ = code_cache;
  }

  std::string name() const override { return name_; }

 private:
  std::string name_;
};

#if XE_ARCH_ARM64
constexpr std::string_view kCurrentBackendName = "a64";
constexpr std::string_view kWrongBackendName = "x64";
#elif XE_ARCH_AMD64
constexpr std::string_view kCurrentBackendName = "x64";
constexpr std::string_view kWrongBackendName = "a64";
#else
#error Guest invocation replay config tests require an AArch64 or AMD64 host.
#endif

#if XE_PLATFORM_WIN32
constexpr GuestInvocationReplayHostPlatform kCurrentHostPlatform =
    GuestInvocationReplayHostPlatform::kWindows;
#elif XE_PLATFORM_MAC
constexpr GuestInvocationReplayHostPlatform kCurrentHostPlatform =
    GuestInvocationReplayHostPlatform::kApple;
#elif XE_PLATFORM_ANDROID
constexpr GuestInvocationReplayHostPlatform kCurrentHostPlatform =
    GuestInvocationReplayHostPlatform::kAndroid;
#elif XE_PLATFORM_GNU_LINUX
constexpr GuestInvocationReplayHostPlatform kCurrentHostPlatform =
    GuestInvocationReplayHostPlatform::kGnuLinux;
#else
#error Guest invocation replay config tests require a supported host platform.
#endif

GuestInvocationReplayConfig MakeConfig(
    std::string_view backend_name,
    GuestInvocationReplayHostPlatform host_platform =
        GuestInvocationReplayHostPlatform::kApple) {
  GuestInvocationReplayConfig config;
  config.backend_name = backend_name;
  config.host_platform = host_platform;
  config.indirection_mode = GuestInvocationReplayIndirectionMode::kEncoded;
  config.code_mapping_mode =
      GuestInvocationReplayCodeMappingMode::kWritableExecutable;
  config.backend_codegen_features = 0;
  config.host_protection_page_size = 16384;
  config.host_feature_flags = 0x0102030405060708ull;
  config.build_features = kGuestInvocationReplayBuildProfiling;
  for (std::string_view name :
       GuestInvocationReplayConfigVariableNames(backend_name, host_platform)) {
    config.entries.push_back({std::string(name), "0"});
  }
  return config;
}

void SetValue(GuestInvocationReplayConfig* config, std::string_view name,
              std::string_view value) {
  const auto it = std::lower_bound(
      config->entries.begin(), config->entries.end(), name,
      [](const GuestInvocationReplayConfigEntry& entry,
         std::string_view candidate) { return entry.name < candidate; });
  REQUIRE(it != config->entries.end());
  REQUIRE(it->name == name);
  it->value = value;
}

GuestInvocationReplayConfig MakeBenchmarkConfig(
    std::string_view backend_name,
    GuestInvocationReplayHostPlatform host_platform =
        GuestInvocationReplayHostPlatform::kApple) {
  GuestInvocationReplayConfig config = MakeConfig(backend_name, host_platform);
  config.build_features = kGuestInvocationReplayBuildAssertionsDisabled |
                          kGuestInvocationReplayBuildRelease;
  SetValue(&config, "count_call_paths", "false");
  SetValue(&config, "count_physical_remap_hits", "false");
  SetValue(&config, "debug", "false");
  SetValue(&config, "emit_mmio_aware_stores_for_recorded_exception_addresses",
           "false");
  SetValue(&config, "enable_early_precompilation", "false");
  SetValue(&config, "fold_readonly_guest_memory_loads", "false");
  SetValue(&config, "guest_scheduler", "false");
  SetValue(&config, "inline_mmio_access", "false");
  SetValue(&config, "log_safepoint_pc", "false");
  SetValue(&config, "serialize_guest_function_definitions", "true");
  SetValue(&config, "trace_function_coverage", "false");
  if (backend_name == "x64" &&
      host_platform == GuestInvocationReplayHostPlatform::kWindows) {
    SetValue(&config, "instrument_call_times", "false");
  }
  return config;
}

}  // namespace

TEST_CASE("guest invocation replay config captures the effective environment",
          "[guest-invocation-replay-config]") {
  REQUIRE(cvar::ConfigVars != nullptr);

  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  Processor processor(memory.get(), nullptr);
  ReplayConfigTestCodeCache code_cache(
      true, true, backend::CodeCache::MappingMode::kWritableExecutable);
  NamedNullBackend backend{std::string(kCurrentBackendName), &code_cache};
  REQUIRE(backend.Initialize(&processor));
  GuestInvocationReplayConfig config;
  config.backend_name = "prefilled";
  std::string error = "prefilled";
  REQUIRE(CaptureCurrentGuestInvocationReplayConfig(backend, &config, &error));
  REQUIRE(error.empty());
  REQUIRE(config.backend_name == kCurrentBackendName);
  REQUIRE(config.host_platform == kCurrentHostPlatform);
  REQUIRE(config.indirection_mode ==
          GuestInvocationReplayIndirectionMode::kEncoded);
  REQUIRE(config.code_mapping_mode ==
          GuestInvocationReplayCodeMappingMode::kWritableExecutable);
  uint32_t expected_backend_codegen_features = 0;
  if (kCurrentBackendName == "x64" &&
      static_cast<uint32_t>(
          reinterpret_cast<uintptr_t>(memory->virtual_membase())) == 0) {
    expected_backend_codegen_features |=
        kGuestInvocationReplayX64MembaseLow32Zero;
  }
  REQUIRE(config.backend_codegen_features == expected_backend_codegen_features);
  REQUIRE(config.host_protection_page_size == xe::memory::page_size());
#if XE_ARCH_ARM64
  REQUIRE(config.host_feature_flags == xe::arm64::GetFeatureFlags());
#elif XE_ARCH_AMD64
  REQUIRE(config.host_feature_flags == xe::amd64::GetFeatureFlags());
#endif
  uint32_t expected_build_features = 0;
#if defined(XE_OPTION_PROFILING) && XE_OPTION_PROFILING
  expected_build_features |= kGuestInvocationReplayBuildProfiling;
#endif
#if defined(NDEBUG)
  expected_build_features |= kGuestInvocationReplayBuildAssertionsDisabled;
#endif
#if defined(XE_BUILD_RELEASE) && XE_BUILD_RELEASE
  expected_build_features |= kGuestInvocationReplayBuildRelease;
#endif
#if defined(XE_BUILD_LTO) && XE_BUILD_LTO
  expected_build_features |= kGuestInvocationReplayBuildLTO;
#endif
  REQUIRE(config.build_features == expected_build_features);

  ReplayConfigTestCodeCache split_code_cache(
      true, true, backend::CodeCache::MappingMode::kSplitView);
  NamedNullBackend split_backend{std::string(kCurrentBackendName),
                                 &split_code_cache};
  REQUIRE(split_backend.Initialize(&processor));
  GuestInvocationReplayConfig split_config;
  REQUIRE(CaptureCurrentGuestInvocationReplayConfig(split_backend,
                                                    &split_config, &error));
  REQUIRE(split_config.code_mapping_mode ==
          GuestInvocationReplayCodeMappingMode::kSplitView);

  ReplayConfigTestCodeCache unmapped_code_cache(
      true, true, backend::CodeCache::MappingMode::kUninitialized);
  NamedNullBackend unmapped_backend{std::string(kCurrentBackendName),
                                    &unmapped_code_cache};
  REQUIRE(unmapped_backend.Initialize(&processor));
  split_config.backend_name = "prefilled";
  REQUIRE_FALSE(CaptureCurrentGuestInvocationReplayConfig(
      unmapped_backend, &split_config, &error));
  REQUIRE(split_config == GuestInvocationReplayConfig{});
  REQUIRE(error.find("initialized code mapping") != std::string::npos);

  const std::vector<std::string_view> names =
      GuestInvocationReplayConfigVariableNames(kCurrentBackendName,
                                               kCurrentHostPlatform);
  REQUIRE(config.entries.size() == names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    INFO("configuration variable: " << names[i]);
    REQUIRE(config.entries[i].name == names[i]);
    const auto it = cvar::ConfigVars->find(std::string(names[i]));
    REQUIRE(it != cvar::ConfigVars->cend());
    REQUIRE(it->second != nullptr);
    REQUIRE(config.entries[i].value == it->second->effective_value());
  }

  NamedNullBackend wrong_backend{std::string(kWrongBackendName), &code_cache};
  config.backend_name = "prefilled";
  error.clear();
  REQUIRE_FALSE(CaptureCurrentGuestInvocationReplayConfig(wrong_backend,
                                                          &config, &error));
  REQUIRE(config == GuestInvocationReplayConfig{});
  REQUIRE_FALSE(error.empty());

  NamedNullBackend uninitialized_backend{std::string(kCurrentBackendName),
                                         nullptr};
  config.backend_name = "prefilled";
  error.clear();
  REQUIRE_FALSE(CaptureCurrentGuestInvocationReplayConfig(uninitialized_backend,
                                                          &config, &error));
  REQUIRE(config == GuestInvocationReplayConfig{});
  REQUIRE(error.find("initialized code cache") != std::string::npos);

  NamedNullBackend uninitialized_memory_backend{
      std::string(kCurrentBackendName), &code_cache};
  config.backend_name = "prefilled";
  error.clear();
  REQUIRE_FALSE(CaptureCurrentGuestInvocationReplayConfig(
      uninitialized_memory_backend, &config, &error));
  REQUIRE(config == GuestInvocationReplayConfig{});
  REQUIRE(error.find("initialized guest memory") != std::string::npos);
}

TEST_CASE("guest invocation replay config has a strict canonical encoding",
          "[guest-invocation-replay-config]") {
  GuestInvocationReplayConfig config = MakeConfig("a64");
  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  std::string error;
  REQUIRE(EncodeGuestInvocationReplayConfig(config, &first, &error));
  REQUIRE(error.empty());
  REQUIRE(EncodeGuestInvocationReplayConfig(config, &second, &error));
  REQUIRE(first == second);
  REQUIRE(first.size() > 32);
  REQUIRE(std::string(first.cbegin(), first.cbegin() + 8) == "XEPPCRC1");
  REQUIRE(first[8] == GuestInvocationReplayConfig::kVersion);
  REQUIRE(first[9] == 0);
  REQUIRE(first[10] == 0);
  REQUIRE(first[11] == 0);

  std::array<uint8_t, 32> first_hash = {};
  std::array<uint8_t, 32> second_hash = {};
  REQUIRE(HashGuestInvocationReplayConfig(config, &first_hash, &error));
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash == second_hash);
  constexpr std::array<uint8_t, 32> kExpectedHash = {
      0xDE, 0xDD, 0xD0, 0xA1, 0x76, 0x14, 0x74, 0x40, 0x07, 0x3F, 0xE8,
      0x95, 0x2D, 0x73, 0x52, 0x2B, 0xE7, 0x8C, 0x61, 0xDC, 0x04, 0xC1,
      0x9E, 0xFE, 0x68, 0xFF, 0xBF, 0x92, 0xB5, 0x81, 0x07, 0x82,
  };
  REQUIRE(first_hash == kExpectedHash);

  SetValue(&config, "accurate_vmx_denormal_flush", "true");
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash != second_hash);

  config = MakeConfig("a64");
  config.host_platform = GuestInvocationReplayHostPlatform::kGnuLinux;
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash != second_hash);

  config = MakeConfig("a64");
  config.indirection_mode =
      GuestInvocationReplayIndirectionMode::kRawFixedAddress;
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash != second_hash);

  config = MakeConfig("a64");
  config.code_mapping_mode = GuestInvocationReplayCodeMappingMode::kSplitView;
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash != second_hash);

  config = MakeConfig("a64");
  config.build_features |= kGuestInvocationReplayBuildLTO;
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash != second_hash);

  config = MakeConfig("x64");
  REQUIRE(HashGuestInvocationReplayConfig(config, &first_hash, &error));
  config.backend_codegen_features |= kGuestInvocationReplayX64MembaseLow32Zero;
  REQUIRE(HashGuestInvocationReplayConfig(config, &second_hash, &error));
  REQUIRE(first_hash != second_hash);

  const std::vector<std::string_view> apple_x64_names =
      GuestInvocationReplayConfigVariableNames(
          "x64", GuestInvocationReplayHostPlatform::kApple);
  REQUIRE(std::find(apple_x64_names.cbegin(), apple_x64_names.cend(),
                    "instrument_call_times") == apple_x64_names.cend());
  const std::vector<std::string_view> windows_x64_names =
      GuestInvocationReplayConfigVariableNames(
          "x64", GuestInvocationReplayHostPlatform::kWindows);
  REQUIRE(std::find(windows_x64_names.cbegin(), windows_x64_names.cend(),
                    "instrument_call_times") != windows_x64_names.cend());

  const std::vector<std::string_view> a64_names =
      GuestInvocationReplayConfigVariableNames(
          "a64", GuestInvocationReplayHostPlatform::kApple);
  for (std::string_view compile_control :
       {"enable_early_precompilation", "fold_readonly_guest_memory_loads",
        "inline_mmio_access", "serialize_guest_function_definitions"}) {
    INFO("compile control: " << compile_control);
    REQUIRE(std::find(a64_names.cbegin(), a64_names.cend(), compile_control) !=
            a64_names.cend());
  }
}

TEST_CASE("guest invocation replay config rejects schema and envelope drift",
          "[guest-invocation-replay-config]") {
  GuestInvocationReplayConfig config = MakeConfig("a64");
  std::vector<uint8_t> encoded = {0xAA};
  std::string error;

  config.entries.pop_back();
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));
  REQUIRE(encoded.empty());
  REQUIRE_FALSE(error.empty());

  config = MakeConfig("a64");
  std::swap(config.entries[0], config.entries[1]);
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.entries[0].value.push_back('\0');
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.host_protection_page_size = 2048;
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.host_platform = GuestInvocationReplayHostPlatform::kUnknown;
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.indirection_mode = GuestInvocationReplayIndirectionMode::kUnknown;
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.code_mapping_mode = GuestInvocationReplayCodeMappingMode::kUnknown;
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.backend_codegen_features = kGuestInvocationReplayX64MembaseLow32Zero;
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("a64");
  config.build_features = 1u << 31;
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));

  config = MakeConfig("unknown");
  REQUIRE_FALSE(EncodeGuestInvocationReplayConfig(config, &encoded, &error));
}

TEST_CASE("timed guest invocation replay requires Apple A64 and fixed controls",
          "[guest-invocation-replay-config]") {
  std::string error;
  GuestInvocationReplayConfig config = MakeBenchmarkConfig("a64");
  REQUIRE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(error.empty());

  SetValue(&config, "guest_scheduler", "true");
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(error.find("guest_scheduler=false") != std::string::npos);

  config = MakeBenchmarkConfig("a64");
  SetValue(&config, "log_safepoint_pc", "true");
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(error.find("log_safepoint_pc=false") != std::string::npos);

  config = MakeBenchmarkConfig("a64");
  SetValue(&config, "emit_mmio_aware_stores_for_recorded_exception_addresses",
           "true");
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(
      error.find(
          "emit_mmio_aware_stores_for_recorded_exception_addresses=false") !=
      std::string::npos);

  constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
      kRejectedCompileControls = {
          {{"enable_early_precompilation", "true"},
           {"fold_readonly_guest_memory_loads", "true"},
           {"inline_mmio_access", "true"},
           {"serialize_guest_function_definitions", "false"}}};
  for (const auto& [name, rejected_value] : kRejectedCompileControls) {
    config = MakeBenchmarkConfig("a64");
    SetValue(&config, name, rejected_value);
    INFO("compile control: " << name);
    REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
    REQUIRE(error.find(std::string(name)) != std::string::npos);
  }

  config = MakeBenchmarkConfig("a64");
  config.build_features |= kGuestInvocationReplayBuildTraceData;
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));

  config = MakeBenchmarkConfig("a64");
  config.build_features |= kGuestInvocationReplayBuildProfiling;
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));

  config = MakeBenchmarkConfig("a64");
  config.build_features &= ~kGuestInvocationReplayBuildRelease;
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(error.find("Release build") != std::string::npos);

  config = MakeBenchmarkConfig("a64");
  config.build_features &= ~kGuestInvocationReplayBuildAssertionsDisabled;
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));

  config = MakeBenchmarkConfig("x64");
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(error.find("a64 backend") != std::string::npos);

  config =
      MakeBenchmarkConfig("a64", GuestInvocationReplayHostPlatform::kGnuLinux);
  REQUIRE_FALSE(ValidateGuestInvocationReplayBenchmarkConfig(config, &error));
  REQUIRE(error.find("Apple host") != std::string::npos);
}

}  // namespace xe::cpu::test
