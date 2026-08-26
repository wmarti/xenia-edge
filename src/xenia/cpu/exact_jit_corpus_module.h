/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_EXACT_JIT_CORPUS_MODULE_H_
#define XENIA_CPU_EXACT_JIT_CORPUS_MODULE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/module.h"

namespace xe {
namespace cpu {

// Replay-only module exposing exactly the validated corpus entries. It does
// not create a broad RawModule address range: interior addresses and gaps are
// never accepted as function entries, and only contiguous recorded code-page
// runs are committed to the backend as executable ranges.
class ExactJitCorpusModule final : public Module {
 public:
  static std::unique_ptr<ExactJitCorpusModule> Create(
      Processor* processor, const ExecutionJitCorpus& corpus,
      std::string_view name = "execution_jit_corpus",
      std::string* error = nullptr);

  ~ExactJitCorpusModule() override;

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  bool ContainsAddress(uint32_t address) override;
  Symbol::Status DeclareFunction(uint32_t address,
                                 Function** out_function) override;

  // The PPC scanner must reproduce the captured inclusive end address. A
  // runner calls this after translation before accepting or timing a replay.
  bool HasExactExtent(const Function& function) const;

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override;

 private:
  ExactJitCorpusModule(Processor* processor, const ExecutionJitCorpus& corpus,
                       std::string_view name);

  const ExecutionJitCorpus::FunctionRecord* FindFunction(
      uint32_t entry_address) const;

  std::string name_;
  std::vector<ExecutionJitCorpus::FunctionRecord> functions_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_EXACT_JIT_CORPUS_MODULE_H_
