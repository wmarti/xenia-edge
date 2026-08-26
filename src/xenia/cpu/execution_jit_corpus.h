/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_EXECUTION_JIT_CORPUS_H_
#define XENIA_CPU_EXECUTION_JIT_CORPUS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/cpu/jit_corpus.h"

namespace xe {
namespace cpu {

// A strict, execution-safe view of the streaming JIT corpus format.
//
// JitCorpus::Read deliberately accepts a capture killed at a record boundary
// because a partial corpus is still useful for compile-only analysis. Guest
// execution needs a different contract: exact version and flags, no partial or
// duplicate records, and every page covering every recorded function extent.
// Keeping this decoder separate preserves the existing compile-only behavior.
class ExecutionJitCorpus {
 public:
  using FunctionRecord = JitCorpus::FunctionRecord;

  // Resource-safety limits for untrusted replay input. These are acceptance
  // ceilings, not capture targets.
  static constexpr uint64_t kMaxCorpusSize = 512ull * 1024ull * 1024ull;
  static constexpr uint32_t kMaxPageRecords = 65536;
  static constexpr uint32_t kMaxFunctionRecords = 1024 * 1024;
  static constexpr uint32_t kMaxFunctionSize = 16u * 1024u * 1024u;

  ExecutionJitCorpus() = default;

  // On failure, output is reset and error (when non-null) describes the first
  // rejected invariant. The streaming format has no footer or record count,
  // so a prefix ending after a complete valid record is indistinguishable from
  // an intentionally shorter corpus. Execution callers must additionally
  // verify the external whole-corpus hash named by the invocation artifact.
  static bool Decode(const uint8_t* data, size_t data_size,
                     ExecutionJitCorpus* output, std::string* error = nullptr);
  static bool Decode(const std::vector<uint8_t>& data,
                     ExecutionJitCorpus* output, std::string* error = nullptr) {
    return Decode(data.data(), data.size(), output, error);
  }
  static bool Read(const std::filesystem::path& path,
                   ExecutionJitCorpus* output, std::string* error = nullptr);

  uint32_t version() const { return version_; }
  uint32_t config_flags() const { return config_flags_; }

  // Both collections are canonicalized into strictly increasing address
  // order. Page contents are kPageSize chunks indexed identically to
  // page_addresses().
  const std::vector<uint32_t>& page_addresses() const {
    return page_addresses_;
  }
  const std::vector<uint8_t>& page_data() const { return page_data_; }
  const std::vector<FunctionRecord>& functions() const { return functions_; }

  // Entry addresses in the order their successful definitions were serialized
  // by the capture. This remains separate from functions(), whose address sort
  // is required for exact lookup. A runner uses this order to reproduce which
  // callees were already translated when each caller reached the frontend.
  const std::vector<uint32_t>& function_definition_order() const {
    return function_definition_order_;
  }

  const uint8_t* FindPageData(uint32_t page_address) const;
  const FunctionRecord* FindFunction(uint32_t entry_address) const;

 private:
  std::vector<uint32_t> page_addresses_;
  std::vector<uint8_t> page_data_;
  std::vector<FunctionRecord> functions_;
  std::vector<uint32_t> function_definition_order_;
  uint32_t version_ = 0;
  uint32_t config_flags_ = 0;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_EXECUTION_JIT_CORPUS_H_
