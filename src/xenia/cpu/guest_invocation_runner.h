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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_invocation_artifact.h"

namespace xe {
class Memory;

namespace cpu {

class ExactJitCorpusModule;
class Function;
class Processor;
class ThreadState;

namespace backend {
class Backend;
}  // namespace backend

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

struct GuestInvocationReplayMetrics {
  uint64_t timed_invocation_count = 0;
  uint64_t thread_cpu_nanoseconds = 0;
  uint64_t uptime_raw_nanoseconds = 0;
  uint64_t placement_generation_before = 0;
  uint64_t placement_generation_after = 0;
  uint64_t reset_page_count_per_invocation = 0;
  uint64_t reset_bytes_per_invocation = 0;
};

// Owns a bare Memory, Processor, backend and ThreadState for one selected
// invocation. The invocation and corpus are borrowed and must outlive the
// runner. File decoding, provenance hashes, command-line policy and marker
// output intentionally remain the caller's responsibility.
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
  // Verification and clock reads themselves are outside the timed interval.
  // A separate reset/call/verification is required after the interval before
  // metrics are accepted.
  bool RunTimed(uint64_t invocation_count,
                GuestInvocationReplayMetrics* metrics,
                std::string* error = nullptr);

  const GuestInvocationReplayPlan& plan() const { return plan_; }
  const backend::Backend& backend() const;

 private:
  GuestInvocationRunner(const ppc::GuestFunctionInvocation& invocation,
                        const ExecutionJitCorpus& corpus,
                        GuestInvocationReplayPlan plan);

  bool Initialize(std::unique_ptr<backend::Backend> backend,
                  std::string* error);
  bool CommitAndLoadPages(std::string* error);
  bool CloseAndReopenGuestViews(std::string* error);
  bool ResolveFunctionsInCaptureOrder(std::string* error);
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
  bool warmed_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_RUNNER_H_
