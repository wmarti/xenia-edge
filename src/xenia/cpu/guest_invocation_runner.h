/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_RUNNER_H_
#define XENIA_CPU_GUEST_INVOCATION_RUNNER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_invocation_artifact.h"

namespace xe {
class Memory;

namespace cpu {

#if XE_PLATFORM_MAC
// User plus system time of one Mach thread port from THREAD_BASIC_INFO. This
// is the only CPU accounting accepted by timed replay; wall time is diagnostic.
bool ReadGuestInvocationReplayThreadCpuNanoseconds(uint32_t thread_port,
                                                   uint64_t* cpu_nanoseconds);
#endif  // XE_PLATFORM_MAC

class ExactJitCorpusModule;
class Function;
class GuestFunction;
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
class GuestInvocationRunnerTestAccess;
#endif
class Processor;
class ThreadState;

namespace backend {
class Backend;
}  // namespace backend

// Replay resolves every function in the paired corpus before timing. Keep that
// eager work well below the backend's 256 MiB generated-code cache. These are
// safety ceilings for one invocation-specific compile closure, not capture
// targets.
constexpr uint64_t kGuestInvocationReplayMaxEagerFunctionCount = 262'144;
constexpr uint64_t kGuestInvocationReplayMaxEagerGuestCodeBytes =
    16ull * 1024ull * 1024ull;
constexpr uint64_t kGuestInvocationReplayMaxCapturedHostCodeBytes =
    128ull * 1024ull * 1024ull;

// One host-protection granule that replay may access after closing the full
// guest virtual and physical views. Guest code is read-only unless its granule
// also contains captured data.
struct GuestInvocationReplayProtectionGranule {
  uint32_t guest_address = 0;
  uint32_t size = 0;
  bool writable = false;

  bool operator==(const GuestInvocationReplayProtectionGranule&) const =
      default;
};

// Pure, allocation-bounded planning result. Keeping validation separate from
// mapping lets callers reject unsafe artifacts without creating guest memory
// and gives focused tests a way to exercise the protection-closure contract.
struct GuestInvocationReplayPlan {
  uint32_t host_page_size = 0;
  uint64_t eager_function_count = 0;
  uint64_t eager_guest_code_bytes = 0;
  uint64_t captured_host_code_bytes = 0;
  std::vector<uint32_t> supplied_page_addresses;
  std::vector<uint32_t> reset_page_addresses;
  std::vector<GuestInvocationReplayProtectionGranule> protection_granules;
};

// Validates one already-decoded invocation against one already-decoded exact
// corpus. The host page size must be the actual protection granularity used by
// the process. On failure, output is reset and error describes the first
// rejected invariant.
bool BuildGuestInvocationReplayPlan(
    const ppc::GuestFunctionInvocation& invocation,
    const ExecutionJitCorpus& corpus, uint32_t host_page_size,
    GuestInvocationReplayPlan* output, std::string* error = nullptr);

// One actual warmed function body used to compute an ASLR-normalized A64 code
// shape. Addresses identify guest functions only; native addresses are never
// hashed or reported.
struct GuestInvocationReplayCodeShapeFunction {
  uint32_t guest_address = 0;
  uint32_t guest_end_address = 0;
  const uint8_t* machine_code = nullptr;
  size_t machine_code_length = 0;
};

// This is a structural identity, not an exact native-code digest. It masks the
// immediate values in >32-bit MOVZ/MOVN-plus-MOVK materialization chains and
// displacement bits in PC-relative instructions. The emitted opcode, register,
// lane and chain structure, every other warmed A64 instruction word, guest
// extents and function boundaries remain in the hash.
struct GuestInvocationReplayCodeShape {
  std::array<uint8_t, 32> sha256 = {};
  uint64_t function_count = 0;
  uint64_t host_instruction_count = 0;
  uint64_t wide_materialization_site_count = 0;
  uint64_t pc_relative_site_count = 0;

  bool operator==(const GuestInvocationReplayCodeShape&) const = default;
};

// Pure, bounded helper used by the Apple A64 runner after warmup. Input must be
// in captured successful-definition order with unique guest addresses, and
// every native body must be a nonempty, four-byte-aligned A64 stream.
bool HashGuestInvocationReplayA64CodeShape(
    const std::vector<GuestInvocationReplayCodeShapeFunction>& functions,
    GuestInvocationReplayCodeShape* output, std::string* error = nullptr);

struct GuestInvocationReplayMetrics {
  uint64_t timed_invocation_count = 0;
  uint64_t thread_cpu_nanoseconds = 0;
  uint64_t uptime_raw_nanoseconds = 0;
  uint64_t reset_only_thread_cpu_nanoseconds = 0;
  uint64_t reset_only_uptime_raw_nanoseconds = 0;
  uint64_t placement_generation_before = 0;
  uint64_t placement_generation_after = 0;
  uint64_t reset_page_count_per_invocation = 0;
  uint64_t reset_bytes_per_invocation = 0;
  GuestInvocationReplayCodeShape code_shape;
};

// Owns a bare Memory, Processor, backend and ThreadState for one selected
// invocation on Apple A64. Capture builds publish the worker only after this
// runner has finished its ThreadState setup. Future persistent replay workers
// must likewise publish after each worker's complete construction or restore
// and before the first dispatch. The invocation and corpus are borrowed and
// must outlive the runner. File decoding, provenance hashes, command-line
// policy, subprocess fault containment and marker output intentionally remain
// the caller's responsibility.
class GuestInvocationRunner {
 public:
  static constexpr uint64_t kMaxTimedInvocationCount = 10'000'000;

  static std::unique_ptr<GuestInvocationRunner> Create(
      const ppc::GuestFunctionInvocation& invocation,
      const ExecutionJitCorpus& corpus,
      std::unique_ptr<backend::Backend> backend, std::string* error = nullptr);

  ~GuestInvocationRunner();

  GuestInvocationRunner(const GuestInvocationRunner&) = delete;
  GuestInvocationRunner& operator=(const GuestInvocationRunner&) = delete;

  // Defines every corpus function in captured successful-definition order,
  // invokes the selected function once, and verifies its complete exit state.
  bool WarmAndVerify(std::string* error = nullptr);

  // Measures exactly invocation_count reset-plus-call iterations on macOS.
  // The primary current-thread CPU interval excludes the diagnostic wall-clock
  // reads; the wall interval encloses the CPU clock reads. A separate
  // reset-only diagnostic is measured after the primary interval and is never
  // subtracted. Both intervals are followed by untimed invocation verification
  // before metrics are accepted.
  bool RunTimed(uint64_t invocation_count,
                GuestInvocationReplayMetrics* metrics,
                std::string* error = nullptr);

  const GuestInvocationReplayPlan& plan() const { return plan_; }
  const backend::Backend& backend() const;

  // Returns the exact warmed root allocation size reported by the backend, or
  // zero before successful warm verification. Synthetic fixture generation
  // records this in the ordinary corpus provenance field instead of inventing
  // a placeholder size.
  uint32_t warmed_root_host_code_size() const;

 private:
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  friend class GuestInvocationRunnerTestAccess;
#endif

  GuestInvocationRunner(const ppc::GuestFunctionInvocation& invocation,
                        const ExecutionJitCorpus& corpus,
                        GuestInvocationReplayPlan plan);

  bool Initialize(std::unique_ptr<backend::Backend> backend,
                  std::string* error);
  bool CommitAndLoadPages(std::string* error);
  bool CloseAndReopenGuestViews(std::string* error);
  bool PrepareResetPageCopies(std::string* error);
  bool ResolveFunctionsInCaptureOrder(std::string* error);
  bool CaptureWarmedCodeShape(GuestInvocationReplayCodeShape* output,
                              std::string* error) const;
  bool ResetInvocation(std::string* error);
  bool Invoke(std::string* error);
  bool VerifyCurrentState(std::string* error) const;
  const uint8_t* InitialPageData(uint32_t guest_address) const;

  const ppc::GuestFunctionInvocation* invocation_ = nullptr;
  const ExecutionJitCorpus* corpus_ = nullptr;
  GuestInvocationReplayPlan plan_;

  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  std::unique_ptr<ThreadState> thread_state_;
  ExactJitCorpusModule* module_ = nullptr;
  Function* root_function_ = nullptr;
  std::vector<GuestFunction*> resolved_functions_;
  GuestInvocationReplayCodeShape warmed_code_shape_;
  struct ResetPageCopy {
    uint8_t* destination = nullptr;
    const uint8_t* source = nullptr;
  };
  std::vector<ResetPageCopy> reset_page_copies_;
  bool warmed_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_RUNNER_H_
