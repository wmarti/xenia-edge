/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_SYNTHETIC_FIXTURE_H_
#define XENIA_CPU_GUEST_INVOCATION_SYNTHETIC_FIXTURE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_replay_cli.h"
#include "xenia/cpu/guest_invocation_replay_config.h"

namespace xe {
namespace cpu {

inline constexpr char kSyntheticGuestInvocationCorpusFileName[] =
    "synthetic.jcorpus";
inline constexpr char kSyntheticGuestInvocationValidArtifactFileName[] =
    "valid.guest_invocation";
inline constexpr char kSyntheticGuestInvocationOmittedPageArtifactFileName[] =
    "omitted_page_fault.guest_invocation";
inline constexpr char kSyntheticGuestInvocationAddress7FArtifactFileName[] =
    "address_7f_fault.guest_invocation";
inline constexpr char kSyntheticGuestInvocationManifestFileName[] =
    "manifest.json";

// Copyright-free fixture inputs for the linked subprocess replay gate. The
// valid invocation executes lwz/addi/stw/blr against a supplied writable page.
// The other two invocations execute the same exact corpus with r3 pointing to
// an omitted ordinary page or the unsupported 0x7F range, respectively.
struct SyntheticGuestInvocationFixture {
  static constexpr uint32_t kCodeAddress = 0x82040000u;
  static constexpr uint32_t kDataAddress = 0x10000000u;
  static constexpr uint32_t kAddress7F = 0x7F000000u;
  static constexpr uint32_t kReturnAddress = 0x83000000u;

  uint32_t host_page_size = 0;
  uint32_t captured_host_code_size = 0;
  std::vector<uint8_t> corpus_bytes;
  ExecutionJitCorpus corpus;
  ppc::GuestFunctionInvocation valid_invocation;
  ppc::GuestFunctionInvocation omitted_page_fault_invocation;
  ppc::GuestFunctionInvocation address_7f_fault_invocation;
};

// Builds strict in-memory inputs using exactly one host-protection-granule of
// code and, for the valid invocation, exactly one granule of data. The host
// code size must be nonzero because execution replay rejects compile-only
// corpus records that lack this capture provenance. corpus_config_flags records
// the codegen configuration used to obtain that host code size.
bool BuildSyntheticGuestInvocationFixture(
    uint32_t host_page_size, uint32_t captured_host_code_size,
    uint32_t corpus_config_flags, SyntheticGuestInvocationFixture* output,
    std::string* error = nullptr);

// Writes a complete bundle through the production corpus and artifact codecs.
// The output directory and its sibling .part directory must not exist. All
// files become visible together through a final directory rename; failures
// remove only the staging directory created by this call.
bool WriteSyntheticGuestInvocationFixture(
    const std::filesystem::path& output_directory,
    const SyntheticGuestInvocationFixture& fixture,
    const GuestInvocationReplaySha256& capture_build_sha256,
    const GuestInvocationReplayConfig& replay_config,
    std::string* error = nullptr);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_SYNTHETIC_FIXTURE_H_
