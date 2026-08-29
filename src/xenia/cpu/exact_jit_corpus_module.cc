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

namespace {

bool RestoreFunctionMetadata(const ExecutionJitCorpus::FunctionRecord& record,
                             Function* function) {
  JitCorpus::FunctionMetadata metadata;
  if (!function || !JitCorpus::DecodeFunctionFlags(record.flags, &metadata)) {
    return false;
  }
  function->set_behavior(metadata.behavior);
  function->SetSaverest(metadata.saverest_type, metadata.is_restore,
                        metadata.saverest_index);
  return true;
}

bool FunctionMetadataMatches(const ExecutionJitCorpus::FunctionRecord& record,
                             const Function& function) {
  JitCorpus::FunctionMetadata metadata;
  return JitCorpus::DecodeFunctionFlags(record.flags, &metadata) &&
         function.behavior() == metadata.behavior &&
         function.SaverestType() == metadata.saverest_type &&
         function.IsRestore() == metadata.is_restore &&
         function.SaverestIndex() == metadata.saverest_index;
}

bool HasDefaultFunctionMetadata(const Function& function) {
  return function.behavior() == Function::Behavior::kDefault &&
         function.SaverestType() == SaveRestoreType::NONE &&
         !function.IsRestore() && function.SaverestIndex() == 0;
}

bool ValidateResumeEntries(
    const ExecutionJitCorpus& corpus,
    std::span<const ExactJitCorpusModule::ResumeEntry> resume_entries,
    std::vector<ExactJitCorpusModule::ResumeEntry>* validated_entries,
    std::string* error) {
  validated_entries->assign(resume_entries.begin(), resume_entries.end());
  for (const ExactJitCorpusModule::ResumeEntry& entry : *validated_entries) {
    if (!entry.resume_pc || (entry.resume_pc & 3)) {
      if (error) {
        error->assign("exact corpus resume PC is zero or unaligned");
      }
      return false;
    }
    const ExecutionJitCorpus::FunctionRecord* owner =
        corpus.FindFunction(entry.owning_function_entry);
    if (!owner) {
      if (error) {
        error->assign("exact corpus resume entry has an unknown owner");
      }
      return false;
    }
    if (owner->end_address != entry.owning_function_end) {
      if (error) {
        error->assign("exact corpus resume entry owner extent is inconsistent");
      }
      return false;
    }
    if (entry.resume_pc < owner->address ||
        entry.resume_pc > owner->end_address) {
      if (error) {
        error->assign("exact corpus resume PC is outside its owner extent");
      }
      return false;
    }
    if (corpus.FindFunction(entry.resume_pc)) {
      if (error) {
        error->assign("exact corpus resume PC overlaps a captured entry");
      }
      return false;
    }
  }
  std::sort(validated_entries->begin(), validated_entries->end(),
            [](const ExactJitCorpusModule::ResumeEntry& left,
               const ExactJitCorpusModule::ResumeEntry& right) {
              return left.resume_pc < right.resume_pc;
            });
  for (size_t i = 1; i < validated_entries->size(); ++i) {
    const ExactJitCorpusModule::ResumeEntry& previous =
        (*validated_entries)[i - 1];
    const ExactJitCorpusModule::ResumeEntry& current = (*validated_entries)[i];
    if (previous.resume_pc != current.resume_pc) {
      continue;
    }
    if (error) {
      if (previous.owning_function_entry == current.owning_function_entry &&
          previous.owning_function_end == current.owning_function_end) {
        error->assign("exact corpus contains a duplicate resume entry");
      } else {
        error->assign("exact corpus contains conflicting resume entries");
      }
    }
    return false;
  }
  return true;
}

}  // namespace

std::unique_ptr<ExactJitCorpusModule> ExactJitCorpusModule::Create(
    Processor* processor, const ExecutionJitCorpus& corpus,
    std::string_view name, std::string* error) {
  return Create(processor, corpus, std::span<const ResumeEntry>(), name, error);
}

std::unique_ptr<ExactJitCorpusModule> ExactJitCorpusModule::Create(
    Processor* processor, const ExecutionJitCorpus& corpus,
    std::span<const ResumeEntry> resume_entries, std::string_view name,
    std::string* error) {
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
  std::vector<ResumeEntry> validated_entries;
  if (!ValidateResumeEntries(corpus, resume_entries, &validated_entries,
                             error)) {
    return nullptr;
  }
  return std::unique_ptr<ExactJitCorpusModule>(new ExactJitCorpusModule(
      processor, corpus, std::move(validated_entries), name));
}

ExactJitCorpusModule::ExactJitCorpusModule(
    Processor* processor, const ExecutionJitCorpus& corpus,
    std::vector<ResumeEntry> resume_entries, std::string_view name)
    : Module(processor),
      name_(name),
      functions_(corpus.functions()),
      declaration_records_(corpus.declaration_records()),
      resume_entries_(std::move(resume_entries)) {
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

const ExecutionJitCorpus::DeclarationRecord*
ExactJitCorpusModule::FindDeclaration(uint32_t entry_address) const {
  const auto it = std::lower_bound(
      declaration_records_.cbegin(), declaration_records_.cend(), entry_address,
      [](const ExecutionJitCorpus::DeclarationRecord& declaration,
         uint32_t address) { return declaration.address < address; });
  return it != declaration_records_.cend() && it->address == entry_address
             ? &*it
             : nullptr;
}

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

const ExactJitCorpusModule::ResumeEntry* ExactJitCorpusModule::FindResumeEntry(
    uint32_t resume_pc) const {
  const auto it = std::lower_bound(
      resume_entries_.cbegin(), resume_entries_.cend(), resume_pc,
      [](const ResumeEntry& entry, uint32_t address) {
        return entry.resume_pc < address;
      });
  return it != resume_entries_.cend() && it->resume_pc == resume_pc ? &*it
                                                                    : nullptr;
}

bool ExactJitCorpusModule::ContainsAddress(uint32_t address) {
  return FindFunction(address) != nullptr ||
         FindResumeEntry(address) != nullptr ||
         FindDeclaration(address) != nullptr;
}

Symbol::Status ExactJitCorpusModule::DeclareFunction(uint32_t address,
                                                     Function** out_function) {
  if (!out_function) {
    return Symbol::Status::kFailed;
  }
  *out_function = nullptr;
  const ExecutionJitCorpus::FunctionRecord* record = FindFunction(address);
  const ResumeEntry* resume_entry = FindResumeEntry(address);
  const ExecutionJitCorpus::DeclarationRecord* declaration =
      FindDeclaration(address);
  if (!record && !resume_entry && !declaration) {
    return Symbol::Status::kFailed;
  }
  // A declaration has an extent and metadata but no body and no pages, so it
  // reuses the function-record metadata path with a zero host size rather
  // than a second decoder.
  const ExecutionJitCorpus::FunctionRecord declaration_record =
      declaration ? ExecutionJitCorpus::FunctionRecord{declaration->address,
                                                       declaration->end_address,
                                                       0, declaration->flags}
                  : ExecutionJitCorpus::FunctionRecord{};
  if (declaration) {
    record = &declaration_record;
  }

  const Symbol::Status status = Module::DeclareFunction(address, out_function);
  if (!*out_function) {
    return Symbol::Status::kFailed;
  }
  if (status == Symbol::Status::kNew) {
    if (resume_entry) {
      (*out_function)->set_end_address(resume_entry->owning_function_end);
      if (!HasDefaultFunctionMetadata(**out_function)) {
        (*out_function)->set_status(Symbol::Status::kFailed);
        return Symbol::Status::kFailed;
      }
      return status;
    }
    // PPCFrontend declaration and translation happen after this returns. Set
    // both the exact captured extent and the XEX save/restore metadata now, so
    // a caller translated later sees the same already-defined helper state as
    // capture when replay follows function_definition_order().
    (*out_function)->set_end_address(record->end_address);
    if (!RestoreFunctionMetadata(*record, *out_function)) {
      (*out_function)->set_status(Symbol::Status::kFailed);
      return Symbol::Status::kFailed;
    }
    (*out_function)->set_declaration_only(declaration != nullptr);
    return status;
  }
  const uint32_t expected_end_address =
      resume_entry ? resume_entry->owning_function_end : record->end_address;
  const bool metadata_matches =
      resume_entry ? HasDefaultFunctionMetadata(**out_function)
                   : FunctionMetadataMatches(*record, **out_function);
  if ((*out_function)->end_address() != expected_end_address ||
      !metadata_matches ||
      (*out_function)->is_declaration_only() != (declaration != nullptr)) {
    (*out_function)->set_status(Symbol::Status::kFailed);
    return Symbol::Status::kFailed;
  }
  return status;
}

bool ExactJitCorpusModule::HasExactExtent(const Function& function) const {
  if (function.module() != this) {
    return false;
  }
  const ExecutionJitCorpus::FunctionRecord* record =
      FindFunction(function.address());
  if (record) {
    return function.end_address() == record->end_address;
  }
  const ResumeEntry* resume_entry = FindResumeEntry(function.address());
  return resume_entry &&
         function.end_address() == resume_entry->owning_function_end;
}

std::unique_ptr<Function> ExactJitCorpusModule::CreateFunction(
    uint32_t address) {
  return processor_->backend()->CreateGuestFunction(this, address);
}

}  // namespace cpu
}  // namespace xe
