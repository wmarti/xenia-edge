/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/exact_jit_corpus_module.h"

#include <algorithm>

#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {

std::unique_ptr<ExactJitCorpusModule> ExactJitCorpusModule::Create(
    Processor* processor, const ExecutionJitCorpus& corpus,
    std::string_view name, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!processor || !processor->backend()) {
    if (error) {
      error->assign("exact corpus module requires an initialized processor");
    }
    return nullptr;
  }
  if (corpus.page_addresses().empty() || corpus.functions().empty()) {
    if (error) {
      error->assign("exact corpus module requires a decoded execution corpus");
    }
    return nullptr;
  }
  return std::unique_ptr<ExactJitCorpusModule>(
      new ExactJitCorpusModule(processor, corpus, name));
}

ExactJitCorpusModule::ExactJitCorpusModule(Processor* processor,
                                           const ExecutionJitCorpus& corpus,
                                           std::string_view name)
    : Module(processor), name_(name), functions_(corpus.functions()) {
  const std::vector<uint32_t>& page_addresses = corpus.page_addresses();
  for (size_t i = 0; i < page_addresses.size();) {
    size_t end = i + 1;
    while (end < page_addresses.size() &&
           page_addresses[end] ==
               page_addresses[end - 1] + JitCorpus::kPageSize) {
      ++end;
    }
    processor_->backend()->CommitExecutableRange(
        page_addresses[i], page_addresses[end - 1] + JitCorpus::kPageSize);
    i = end;
  }
}

ExactJitCorpusModule::~ExactJitCorpusModule() = default;

const ExecutionJitCorpus::FunctionRecord* ExactJitCorpusModule::FindFunction(
    uint32_t entry_address) const {
  const auto it = std::lower_bound(
      functions_.cbegin(), functions_.cend(), entry_address,
      [](const ExecutionJitCorpus::FunctionRecord& function, uint32_t address) {
        return function.address < address;
      });
  return it != functions_.cend() && it->address == entry_address ? &*it
                                                                 : nullptr;
}

bool ExactJitCorpusModule::ContainsAddress(uint32_t address) {
  return FindFunction(address) != nullptr;
}

Symbol::Status ExactJitCorpusModule::DeclareFunction(uint32_t address,
                                                     Function** out_function) {
  if (!out_function) {
    return Symbol::Status::kFailed;
  }
  *out_function = nullptr;
  const ExecutionJitCorpus::FunctionRecord* record = FindFunction(address);
  if (!record) {
    return Symbol::Status::kFailed;
  }

  const Symbol::Status status = Module::DeclareFunction(address, out_function);
  if (!*out_function) {
    return Symbol::Status::kFailed;
  }
  if (status == Symbol::Status::kNew) {
    // PPCFrontend declaration and translation happen after this returns, so
    // the scanner sees the exact captured inclusive upper bound immediately.
    (*out_function)->set_end_address(record->end_address);
    return status;
  }
  if ((*out_function)->end_address() != record->end_address) {
    (*out_function)->set_status(Symbol::Status::kFailed);
    return Symbol::Status::kFailed;
  }
  return status;
}

bool ExactJitCorpusModule::HasExactExtent(const Function& function) const {
  const ExecutionJitCorpus::FunctionRecord* record =
      FindFunction(function.address());
  return function.module() == this && record &&
         function.end_address() == record->end_address;
}

std::unique_ptr<Function> ExactJitCorpusModule::CreateFunction(
    uint32_t address) {
  return processor_->backend()->CreateGuestFunction(this, address);
}

}  // namespace cpu
}  // namespace xe
