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

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
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

  // A save/restore helper the XEX loader synthesized: an entry the backend
  // inlines from its metadata rather than calling, so it carries an extent and
  // flags but never a body. It is deliberately not a FunctionRecord: it has no
  // host code size, requires no code pages, and takes no definition order.
  struct SaverestRecord {
    uint32_t address;
    uint32_t end_address;
    uint32_t flags;
  };

  // Resource-safety limits for untrusted replay input. These are acceptance
  // ceilings, not capture targets.
  static constexpr uint64_t kMaxCorpusSize = 512ull * 1024ull * 1024ull;
  static constexpr uint32_t kMaxPageRecords = 65536;
  static constexpr uint32_t kMaxFunctionRecords = 1024 * 1024;
  static constexpr uint32_t kMaxFunctionSize = 16u * 1024u * 1024u;
  // The XEX chains are 18 GPR, 18 FPR and 2 x 18 VMX entries in each
  // direction; this is a resource ceiling well above any real loader set.
  static constexpr uint32_t kMaxSaverestRecords = 1024;

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

  const std::vector<SaverestRecord>& saverest_records() const {
    return saverest_records_;
  }

  const uint8_t* FindPageData(uint32_t page_address) const;
  const FunctionRecord* FindFunction(uint32_t entry_address) const;
  const SaverestRecord* FindSaverest(uint32_t entry_address) const;

 private:
  std::vector<uint32_t> page_addresses_;
  std::vector<uint8_t> page_data_;
  std::vector<FunctionRecord> functions_;
  std::vector<SaverestRecord> saverest_records_;
  std::vector<uint32_t> function_definition_order_;
  uint32_t version_ = 0;
  uint32_t config_flags_ = 0;
};

// Deterministically encodes an execution-safe exact corpus assembled by a
// bounded recorder. Code pages are emitted in address order regardless of the
// order in which they are supplied. Function records are emitted in the order
// of their successful definitions because frontend translation can observe
// which static callees and save/restore helpers have already been declared.
//
// The builder intentionally has no concept of an "entered function". The
// caller must add the full translation/declaration closure in captured
// definition order, including definitions that the selected invocation did not
// enter. Any failed add poisons the builder so an ignored error cannot produce
// a silently incomplete corpus.
class ExecutionJitCorpusBuilder {
 public:
  using FunctionRecord = ExecutionJitCorpus::FunctionRecord;
  using CodePage = std::array<uint8_t, JitCorpus::kPageSize>;

  explicit ExecutionJitCorpusBuilder(uint32_t config_flags)
      : config_flags_(config_flags) {}

  ExecutionJitCorpusBuilder(const ExecutionJitCorpusBuilder&) = delete;
  ExecutionJitCorpusBuilder& operator=(const ExecutionJitCorpusBuilder&) =
      delete;

  bool AddCodePage(uint32_t page_address, const uint8_t* page_data,
                   size_t page_data_size, std::string* error = nullptr);
  bool AddFunction(const FunctionRecord& function,
                   std::string* error = nullptr);
  bool AddSaverest(const ExecutionJitCorpus::SaverestRecord& saverest,
                   std::string* error = nullptr);

  // On success, output is a canonical little-endian JitCorpus v3 stream that
  // the strict ExecutionJitCorpus decoder accepts. On failure, output is
  // cleared. Final decoding is part of encoding so missing extent pages and
  // any encoder/decoder contract drift fail before bytes reach disk.
  bool Encode(std::vector<uint8_t>* output, std::string* error = nullptr) const;

  size_t code_page_count() const { return pages_.size(); }
  size_t function_count() const { return functions_.size(); }
  size_t saverest_count() const { return saverest_records_.size(); }

 private:
  bool Fail(std::string_view message, std::string* error);
  bool CheckUsable(std::string* error) const;

  std::map<uint32_t, CodePage> pages_;
  std::vector<FunctionRecord> functions_;
  std::unordered_set<uint32_t> function_addresses_;
  std::vector<ExecutionJitCorpus::SaverestRecord> saverest_records_;
  std::unordered_set<uint32_t> saverest_addresses_;
  uint32_t config_flags_ = 0;
  bool failed_ = false;
  std::string failure_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_EXECUTION_JIT_CORPUS_H_
