/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_REPLAY_CONFIG_H_
#define XENIA_CPU_GUEST_INVOCATION_REPLAY_CONFIG_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace cpu {

namespace backend {
class Backend;
}  // namespace backend

// Build properties that alter emitted guest code or timed call overhead. The
// dedicated recorder hooks are capture provenance, not replay compatibility:
// their executable identity is recorded separately and they are absent from a
// timed runner.
enum GuestInvocationReplayBuildFeature : uint32_t {
  kGuestInvocationReplayBuildTraceInstructions = 1u << 0,
  kGuestInvocationReplayBuildTraceData = 1u << 1,
  kGuestInvocationReplayBuildTraceFunctions = 1u << 2,
  kGuestInvocationReplayBuildProfiling = 1u << 3,
  kGuestInvocationReplayBuildAssertionsDisabled = 1u << 4,
  kGuestInvocationReplayBuildRelease = 1u << 5,
};

constexpr uint32_t kGuestInvocationReplayKnownBuildFeatureMask =
    kGuestInvocationReplayBuildTraceInstructions |
    kGuestInvocationReplayBuildTraceData |
    kGuestInvocationReplayBuildTraceFunctions |
    kGuestInvocationReplayBuildProfiling |
    kGuestInvocationReplayBuildAssertionsDisabled |
    kGuestInvocationReplayBuildRelease;

enum class GuestInvocationReplayHostPlatform : uint32_t {
  kUnknown = 0,
  kWindows = 1,
  kApple = 2,
  kAndroid = 3,
  kGnuLinux = 4,
};

enum class GuestInvocationReplayIndirectionMode : uint32_t {
  kUnknown = 0,
  kRawFixedAddress = 1,
  kEncoded = 2,
};

enum GuestInvocationReplayBackendCodegenFeature : uint32_t {
  kGuestInvocationReplayX64MembaseLow32Zero = 1u << 0,
};

struct GuestInvocationReplayConfigEntry {
  std::string name;
  std::string value;

  bool operator==(const GuestInvocationReplayConfigEntry&) const = default;
};

// A pointer-free snapshot of every admitted JIT-semantic setting and host
// capability. Values use IConfigVar's locale-neutral effective serialization,
// after command-line, per-title and global-config precedence has been applied.
struct GuestInvocationReplayConfig {
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kGuestPageSize = 4096;
  static constexpr uint32_t kMaxHostProtectionPageSize = 65536;
  static constexpr uint32_t kMaxEntries = 128;
  static constexpr uint32_t kMaxNameSize = 96;
  static constexpr uint32_t kMaxValueSize = 4096;

  std::string backend_name;
  GuestInvocationReplayHostPlatform host_platform =
      GuestInvocationReplayHostPlatform::kUnknown;
  GuestInvocationReplayIndirectionMode indirection_mode =
      GuestInvocationReplayIndirectionMode::kUnknown;
  uint32_t backend_codegen_features = 0;
  uint32_t host_protection_page_size = 0;
  uint64_t host_feature_flags = 0;
  uint32_t build_features = 0;
  std::vector<GuestInvocationReplayConfigEntry> entries;

  bool operator==(const GuestInvocationReplayConfig&) const = default;
};

// The exact, versioned schema for a backend, in canonical name order. An empty
// result means the backend is unsupported. Adding or removing an entry requires
// a replay-config version bump and review of capture compatibility.
std::vector<std::string_view> GuestInvocationReplayConfigVariableNames(
    std::string_view backend_name,
    GuestInvocationReplayHostPlatform host_platform);

// Captures the current process's effective values. Fails closed if the backend
// does not match the host architecture or any schema variable is unregistered.
bool CaptureCurrentGuestInvocationReplayConfig(
    const backend::Backend& backend, GuestInvocationReplayConfig* output,
    std::string* error = nullptr);

// Canonical little-endian bytes used only as SHA-256 input and for audit logs.
// Encoding requires the exact schema, canonical entry order and bounded values.
bool EncodeGuestInvocationReplayConfig(
    const GuestInvocationReplayConfig& config, std::vector<uint8_t>* output,
    std::string* error = nullptr);
bool HashGuestInvocationReplayConfig(const GuestInvocationReplayConfig& config,
                                     std::array<uint8_t, 32>* output,
                                     std::string* error = nullptr);

// Enforces the noise- and scheduler-free subset required for a timed replay.
// Capture may still serialize a non-benchmark configuration for diagnostics,
// but the runner must pass this check before warming or executing guest code.
bool ValidateGuestInvocationReplayBenchmarkConfig(
    const GuestInvocationReplayConfig& config, std::string* error = nullptr);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_REPLAY_CONFIG_H_
