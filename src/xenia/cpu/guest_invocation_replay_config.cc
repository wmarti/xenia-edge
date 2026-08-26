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
#include <cstring>
#include <limits>
#include <string_view>

#include "third_party/crypto/sha256.h"
#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#if XE_ARCH_ARM64
#include "xenia/base/platform_arm64.h"
#elif XE_ARCH_AMD64
#include "xenia/base/platform_amd64.h"
#endif
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

namespace {

constexpr std::array<uint8_t, 8> kEncodingMagic = {'X', 'E', 'P', 'P',
                                                   'C', 'R', 'C', '1'};

// This is an intentionally explicit allowlist, not a category scan. Paths,
// logging destinations and compile-only diagnostics must not invalidate an
// otherwise identical execution replay. Every value below is read by PPC HIR,
// an optimization pass, backend emission or generated-code runtime support.
constexpr const char* kCommonVariables[] = {
    "accurate_vmx_denormal_flush",
    "break_condition_gpr",
    "break_condition_op",
    "break_condition_truncate",
    "break_condition_value",
    "break_on_debugbreak",
    "break_on_instruction",
    "break_on_unimplemented_instructions",
    "clock_no_scaling",
    "clock_source_raw",
    "context_promote_vec128",
    "count_call_paths",
    "count_physical_remap_hits",
    "cpu_trace_mask",
    "db16cyc_consecutive_gap_ns",
    "db16cyc_sleep_ns",
    "db16cyc_yield_after",
    "debug",
    "disable_context_promotion",
    "disable_prefetch_and_cachecontrol",
    "eliminate_dead_cr_stores",
    "emit_inline_mmio_checks",
    "emit_mmio_aware_stores_for_recorded_exception_addresses",
    "enable_early_precompilation",
    "fold_readonly_guest_memory_loads",
    "full_optimization_even_with_debug",
    "guest_scheduler",
    "ignore_trap_instructions",
    "inline_loadclock",
    "inline_mmio_access",
    "log_safepoint_pc",
    "no_reserved_ops",
    "no_round_to_single",
    "permit_float_constant_evaluation",
    "pvr",
    "record_mmio_access_exceptions",
    "serialize_guest_function_definitions",
    "spin_wait_max_iter_ns",
    "spin_wait_yield_after",
    "store_all_context_values",
    "trace_function_coverage",
    "validate_hir",
};

constexpr const char* kA64Variables[] = {
    "a64_enable_host_guest_stack_synchronization",
    "a64_extension_mask",
};

constexpr const char* kX64Variables[] = {
    "align_all_basic_blocks",
    "debugprint_trap_log",
    "delay_via_maybeyield",
    "elide_e0_check",
    "emit_source_annotations",
    "enable_host_guest_stack_synchronization",
    "enable_incorrect_roundingmode_behavior",
    "enable_rmw_context_merging",
    "ignore_undefined_externs",
    "max_stackpoints",
    "use_fast_dot_product",
    "x64_extension_mask",
    "xop_arithmetic_right_shifts",
    "xop_compares",
    "xop_left_shifts",
    "xop_right_shifts",
    "xop_rotates",
};

// The legacy guest-call profiler and its CVar exist only in the Win32 x64
// backend because they read KUSER_SHARED_DATA directly.
constexpr const char* kX64WindowsVariables[] = {
    "instrument_call_times",
};

bool Fail(GuestInvocationReplayConfig* config, std::vector<uint8_t>* bytes,
          std::array<uint8_t, 32>* hash, std::string* error,
          std::string_view message) {
  if (config) {
    *config = {};
  }
  if (bytes) {
    bytes->clear();
  }
  if (hash) {
    hash->fill(0);
  }
  if (error) {
    error->assign(message);
  }
  return false;
}

bool IsPowerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }

bool IsCanonicalName(std::string_view name) {
  if (name.empty() || name.size() > GuestInvocationReplayConfig::kMaxNameSize) {
    return false;
  }
  return std::all_of(name.cbegin(), name.cend(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

void AppendU32(std::vector<uint8_t>* output, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    output->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void AppendU64(std::vector<uint8_t>* output, uint64_t value) {
  for (uint32_t i = 0; i < 8; ++i) {
    output->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void AppendString(std::vector<uint8_t>* output, std::string_view value) {
  AppendU32(output, static_cast<uint32_t>(value.size()));
  output->insert(output->end(), value.cbegin(), value.cend());
}

bool ValidateConfig(const GuestInvocationReplayConfig& config,
                    std::string* error) {
  const std::vector<std::string_view> expected_names =
      GuestInvocationReplayConfigVariableNames(config.backend_name,
                                               config.host_platform);
  if (expected_names.empty()) {
    if (error) {
      error->assign("replay configuration backend is unsupported");
    }
    return false;
  }
  switch (config.host_platform) {
    case GuestInvocationReplayHostPlatform::kWindows:
    case GuestInvocationReplayHostPlatform::kApple:
    case GuestInvocationReplayHostPlatform::kAndroid:
    case GuestInvocationReplayHostPlatform::kGnuLinux:
      break;
    default:
      if (error) {
        error->assign("replay configuration host platform is unsupported");
      }
      return false;
  }
  switch (config.indirection_mode) {
    case GuestInvocationReplayIndirectionMode::kRawFixedAddress:
    case GuestInvocationReplayIndirectionMode::kEncoded:
      break;
    default:
      if (error) {
        error->assign("replay configuration indirection mode is unsupported");
      }
      return false;
  }
  const uint32_t known_backend_codegen_features =
      config.backend_name == "x64" ? kGuestInvocationReplayX64MembaseLow32Zero
                                   : 0;
  if (config.backend_codegen_features & ~known_backend_codegen_features) {
    if (error) {
      error->assign(
          "replay configuration contains unknown backend codegen features");
    }
    return false;
  }
  if (!IsPowerOfTwo(config.host_protection_page_size) ||
      config.host_protection_page_size <
          GuestInvocationReplayConfig::kGuestPageSize ||
      config.host_protection_page_size >
          GuestInvocationReplayConfig::kMaxHostProtectionPageSize) {
    if (error) {
      error->assign("replay configuration host page size is unsupported");
    }
    return false;
  }
  if (config.build_features & ~kGuestInvocationReplayKnownBuildFeatureMask) {
    if (error) {
      error->assign("replay configuration contains unknown build features");
    }
    return false;
  }
  if (config.entries.size() != expected_names.size() ||
      config.entries.size() > GuestInvocationReplayConfig::kMaxEntries) {
    if (error) {
      error->assign("replay configuration does not contain the exact schema");
    }
    return false;
  }
  for (size_t i = 0; i < config.entries.size(); ++i) {
    const GuestInvocationReplayConfigEntry& entry = config.entries[i];
    if (entry.name != expected_names[i]) {
      if (error) {
        error->assign(
            "replay configuration variables are missing or non-canonical");
      }
      return false;
    }
    if (!IsCanonicalName(entry.name) ||
        entry.value.size() > GuestInvocationReplayConfig::kMaxValueSize ||
        entry.value.find('\0') != std::string::npos) {
      if (error) {
        error->assign("replay configuration variable is malformed");
      }
      return false;
    }
  }
  return true;
}

const GuestInvocationReplayConfigEntry* FindEntry(
    const GuestInvocationReplayConfig& config, std::string_view name) {
  const auto it = std::lower_bound(
      config.entries.cbegin(), config.entries.cend(), name,
      [](const GuestInvocationReplayConfigEntry& entry,
         std::string_view candidate) { return entry.name < candidate; });
  return it != config.entries.cend() && it->name == name ? &*it : nullptr;
}

bool RequireValue(const GuestInvocationReplayConfig& config,
                  std::string_view name, std::string_view expected,
                  std::string* error) {
  const GuestInvocationReplayConfigEntry* entry = FindEntry(config, name);
  if (!entry || entry->value != expected) {
    if (error) {
      *error = "timed replay requires " + std::string(name) + "=" +
               std::string(expected);
    }
    return false;
  }
  return true;
}

}  // namespace

std::vector<std::string_view> GuestInvocationReplayConfigVariableNames(
    std::string_view backend_name,
    GuestInvocationReplayHostPlatform host_platform) {
  std::vector<std::string_view> names(std::begin(kCommonVariables),
                                      std::end(kCommonVariables));
  if (backend_name == "a64") {
    names.insert(names.end(), std::begin(kA64Variables),
                 std::end(kA64Variables));
  } else if (backend_name == "x64") {
    names.insert(names.end(), std::begin(kX64Variables),
                 std::end(kX64Variables));
    if (host_platform == GuestInvocationReplayHostPlatform::kWindows) {
      names.insert(names.end(), std::begin(kX64WindowsVariables),
                   std::end(kX64WindowsVariables));
    }
  } else {
    return {};
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool CaptureCurrentGuestInvocationReplayConfig(
    const backend::Backend& backend, GuestInvocationReplayConfig* output,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, nullptr, nullptr, error,
                "replay configuration output is null");
  }
  *output = {};

  GuestInvocationReplayConfig config;
  config.backend_name = backend.name();
#if XE_PLATFORM_WIN32
  config.host_platform = GuestInvocationReplayHostPlatform::kWindows;
#elif XE_PLATFORM_MAC
  config.host_platform = GuestInvocationReplayHostPlatform::kApple;
#elif XE_PLATFORM_ANDROID
  config.host_platform = GuestInvocationReplayHostPlatform::kAndroid;
#elif XE_PLATFORM_GNU_LINUX
  config.host_platform = GuestInvocationReplayHostPlatform::kGnuLinux;
#else
  return Fail(output, nullptr, nullptr, error,
              "guest invocation replay has no supported host platform");
#endif
  config.host_protection_page_size =
      static_cast<uint32_t>(xe::memory::page_size());

#if XE_ARCH_ARM64
  if (config.backend_name != "a64") {
    return Fail(output, nullptr, nullptr, error,
                "replay backend does not match the ARM64 host");
  }
  config.host_feature_flags = xe::arm64::GetFeatureFlags();
#elif XE_ARCH_AMD64
  if (config.backend_name != "x64") {
    return Fail(output, nullptr, nullptr, error,
                "replay backend does not match the AMD64 host");
  }
  config.host_feature_flags = xe::amd64::GetFeatureFlags();
#else
  return Fail(output, nullptr, nullptr, error,
              "guest invocation replay has no backend for this host");
#endif

  const backend::CodeCache* code_cache = backend.code_cache();
  if (!code_cache || !code_cache->has_indirection_table()) {
    return Fail(output, nullptr, nullptr, error,
                "replay capture requires an initialized code cache");
  }
  config.indirection_mode =
      code_cache->encoded_indirection()
          ? GuestInvocationReplayIndirectionMode::kEncoded
          : GuestInvocationReplayIndirectionMode::kRawFixedAddress;

  const Processor* processor = backend.processor();
  if (!processor || !processor->memory() ||
      !processor->memory()->virtual_membase()) {
    return Fail(output, nullptr, nullptr, error,
                "replay capture requires initialized guest memory");
  }
  if (config.backend_name == "x64" &&
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
          processor->memory()->virtual_membase())) == 0) {
    config.backend_codegen_features |=
        kGuestInvocationReplayX64MembaseLow32Zero;
  }

  if (backend.trace_instr_available()) {
    config.build_features |= kGuestInvocationReplayBuildTraceInstructions;
  }
  if (backend.trace_data_available()) {
    config.build_features |= kGuestInvocationReplayBuildTraceData;
  }
  if (backend.trace_func_available()) {
    config.build_features |= kGuestInvocationReplayBuildTraceFunctions;
  }
#if defined(XE_OPTION_PROFILING) && XE_OPTION_PROFILING
  config.build_features |= kGuestInvocationReplayBuildProfiling;
#endif
#if defined(NDEBUG)
  config.build_features |= kGuestInvocationReplayBuildAssertionsDisabled;
#endif
#if defined(XE_BUILD_RELEASE) && XE_BUILD_RELEASE
  config.build_features |= kGuestInvocationReplayBuildRelease;
#endif

  if (!cvar::ConfigVars) {
    return Fail(output, nullptr, nullptr, error,
                "configuration variable registry is unavailable");
  }
  const std::vector<std::string_view> names =
      GuestInvocationReplayConfigVariableNames(config.backend_name,
                                               config.host_platform);
  if (names.empty()) {
    return Fail(output, nullptr, nullptr, error,
                "replay configuration backend is unsupported");
  }
  config.entries.reserve(names.size());
  for (std::string_view name : names) {
    const auto it = cvar::ConfigVars->find(std::string(name));
    if (it == cvar::ConfigVars->cend() || !it->second) {
      return Fail(output, nullptr, nullptr, error,
                  "required replay configuration variable is unregistered: " +
                      std::string(name));
    }
    config.entries.push_back(
        {std::string(name), it->second->effective_value()});
  }

  if (!ValidateConfig(config, error)) {
    return false;
  }
  *output = std::move(config);
  return true;
}

bool EncodeGuestInvocationReplayConfig(
    const GuestInvocationReplayConfig& config, std::vector<uint8_t>* output,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, nullptr, nullptr, error,
                "replay configuration byte output is null");
  }
  output->clear();
  if (!ValidateConfig(config, error)) {
    return false;
  }

  std::vector<uint8_t> encoded;
  encoded.reserve(64 + config.entries.size() * 32);
  encoded.insert(encoded.end(), kEncodingMagic.cbegin(), kEncodingMagic.cend());
  AppendU32(&encoded, GuestInvocationReplayConfig::kVersion);
  AppendU32(&encoded, static_cast<uint32_t>(config.host_platform));
  AppendU32(&encoded, static_cast<uint32_t>(config.indirection_mode));
  AppendU32(&encoded, config.backend_codegen_features);
  AppendU32(&encoded, config.host_protection_page_size);
  AppendU64(&encoded, config.host_feature_flags);
  AppendU32(&encoded, config.build_features);
  AppendString(&encoded, config.backend_name);
  AppendU32(&encoded, static_cast<uint32_t>(config.entries.size()));
  for (const GuestInvocationReplayConfigEntry& entry : config.entries) {
    AppendString(&encoded, entry.name);
    AppendString(&encoded, entry.value);
  }
  *output = std::move(encoded);
  return true;
}

bool HashGuestInvocationReplayConfig(const GuestInvocationReplayConfig& config,
                                     std::array<uint8_t, 32>* output,
                                     std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, nullptr, nullptr, error,
                "replay configuration hash output is null");
  }
  output->fill(0);
  std::vector<uint8_t> encoded;
  if (!EncodeGuestInvocationReplayConfig(config, &encoded, error)) {
    return false;
  }
  static_assert(sizeof(unsigned char) == sizeof(uint8_t));
  sha256::SHA256 sha;
  sha.add(encoded.data(), encoded.size());
  sha.getHash(reinterpret_cast<unsigned char*>(output->data()));
  return true;
}

bool ValidateGuestInvocationReplayBenchmarkConfig(
    const GuestInvocationReplayConfig& config, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!ValidateConfig(config, error)) {
    return false;
  }
  constexpr uint32_t kInstrumentedBuildFeatures =
      kGuestInvocationReplayBuildTraceInstructions |
      kGuestInvocationReplayBuildTraceData |
      kGuestInvocationReplayBuildTraceFunctions |
      kGuestInvocationReplayBuildProfiling;
  if (config.build_features & kInstrumentedBuildFeatures) {
    if (error) {
      error->assign(
          "timed replay requires a build without JIT tracing or profiling");
    }
    return false;
  }
  if (!(config.build_features & kGuestInvocationReplayBuildRelease)) {
    if (error) {
      error->assign("timed replay requires a Release build");
    }
    return false;
  }
  if (!(config.build_features &
        kGuestInvocationReplayBuildAssertionsDisabled)) {
    if (error) {
      error->assign("timed replay requires an assertions-disabled build");
    }
    return false;
  }

  constexpr std::array<std::pair<std::string_view, std::string_view>, 12>
      kRequiredValues = {
          {{"break_on_instruction", "0"},
           {"count_call_paths", "false"},
           {"count_physical_remap_hits", "false"},
           {"cpu_trace_mask", "0"},
           {"debug", "false"},
           {"emit_mmio_aware_stores_for_recorded_exception_addresses", "false"},
           {"enable_early_precompilation", "false"},
           {"fold_readonly_guest_memory_loads", "false"},
           {"guest_scheduler", "false"},
           {"inline_mmio_access", "false"},
           {"serialize_guest_function_definitions", "true"},
           {"trace_function_coverage", "false"}}};
  for (const auto& [name, expected] : kRequiredValues) {
    if (!RequireValue(config, name, expected, error)) {
      return false;
    }
  }
  if (config.backend_name == "x64" &&
      config.host_platform == GuestInvocationReplayHostPlatform::kWindows &&
      !RequireValue(config, "instrument_call_times", "false", error)) {
    return false;
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
