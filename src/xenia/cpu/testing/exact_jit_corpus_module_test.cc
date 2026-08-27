/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/exact_jit_corpus_module.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/cpu/backend/assembler.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/testing/util.h"
#include "xenia/memory.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::test {

namespace {

void AppendU32(std::vector<uint8_t>* data, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    data->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

std::vector<uint8_t> MakeHeader() {
  std::vector<uint8_t> data;
  AppendU32(&data, JitCorpus::kMagic);
  AppendU32(&data, JitCorpus::kVersion);
  AppendU32(&data, JitCorpus::kPageSize);
  AppendU32(&data, JitCorpus::kConfigGuestScheduler);
  return data;
}

void AppendPage(std::vector<uint8_t>* data, uint32_t address, uint8_t seed) {
  AppendU32(data, JitCorpus::kTagPage);
  AppendU32(data, address);
  for (uint32_t i = 0; i < JitCorpus::kPageSize; ++i) {
    data->push_back(static_cast<uint8_t>(seed + i * 17));
  }
}

void AppendFunction(std::vector<uint8_t>* data, uint32_t address,
                    uint32_t end_address, uint32_t flags = 0) {
  AppendU32(data, JitCorpus::kTagFunction);
  AppendU32(data, address);
  AppendU32(data, end_address);
  AppendU32(data, 64);
  AppendU32(data, flags);
}

uint32_t FunctionFlags(Function::Behavior behavior,
                       SaveRestoreType saverest_type, bool is_restore,
                       uint8_t saverest_index) {
  return JitCorpus::EncodeFunctionFlags(behavior, saverest_type, is_restore,
                                        saverest_index);
}

class StubGuestFunction final : public GuestFunction {
 public:
  StubGuestFunction(Module* module, uint32_t address)
      : GuestFunction(module, address) {}

  uint8_t* machine_code() const override { return nullptr; }
  size_t machine_code_length() const override { return 0; }

 protected:
  bool CallImpl(ThreadState* thread_state, uint32_t return_address) override {
    return false;
  }
};

class StubBackend final : public backend::Backend {
 public:
  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override {
    executable_ranges.emplace_back(guest_low, guest_high);
  }

  std::unique_ptr<backend::Assembler> CreateAssembler() override {
    return nullptr;
  }

  std::unique_ptr<GuestFunction> CreateGuestFunction(
      Module* module, uint32_t address) override {
    return std::make_unique<StubGuestFunction>(module, address);
  }

  uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                        uint64_t current_pc) override {
    return current_pc;
  }

  std::vector<std::pair<uint32_t, uint32_t>> executable_ranges;
};

}  // namespace

TEST_CASE("exact JIT corpus module exposes only recorded function entries",
          "[execution-jit-corpus]") {
  std::vector<uint8_t> encoded = MakeHeader();
  AppendPage(&encoded, 0x82000000, 0x11);
  AppendPage(&encoded, 0x82001000, 0x33);
  AppendPage(&encoded, 0x83000000, 0x22);
  AppendFunction(&encoded, 0x83000010, 0x8300001C,
                 FunctionFlags(Function::Behavior::kProlog,
                               SaveRestoreType::FPR, false, 31));
  AppendFunction(&encoded, 0x82000000, 0x82000004,
                 FunctionFlags(Function::Behavior::kEpilogReturn,
                               SaveRestoreType::GPR, true, 14));

  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus));
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{0x83000010, 0x82000000});

  Memory memory;
  auto backend = std::make_unique<StubBackend>();
  StubBackend* backend_ptr = backend.get();
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::move(backend)));

  std::string error;
  auto module =
      ExactJitCorpusModule::Create(&processor, corpus, "exact", &error);
  REQUIRE(module);
  REQUIRE(error.empty());
  ExactJitCorpusModule* module_ptr = module.get();
  REQUIRE(processor.AddModule(std::move(module)));

  REQUIRE(module_ptr->name() == "exact");
  REQUIRE(module_ptr->is_executable());
  REQUIRE(module_ptr->ContainsAddress(0x82000000));
  REQUIRE(module_ptr->ContainsAddress(0x83000010));
  REQUIRE_FALSE(module_ptr->ContainsAddress(0x82000004));
  REQUIRE_FALSE(module_ptr->ContainsAddress(0x82001000));
  REQUIRE_FALSE(module_ptr->ContainsAddress(0x83000000));
  REQUIRE(processor.LookupModule(0x82000000) == module_ptr);
  REQUIRE_FALSE(processor.LookupModule(0x82000004));

  REQUIRE(backend_ptr->executable_ranges ==
          std::vector<std::pair<uint32_t, uint32_t>>{{0x82000000, 0x82002000},
                                                     {0x83000000, 0x83001000}});

  Function* function = nullptr;
  REQUIRE(module_ptr->DeclareFunction(0x82000004, &function) ==
          Symbol::Status::kFailed);
  REQUIRE_FALSE(function);

  REQUIRE(module_ptr->DeclareFunction(0x82000000, &function) ==
          Symbol::Status::kNew);
  REQUIRE(function);
  REQUIRE(function->end_address() == 0x82000004);
  REQUIRE(function->behavior() == Function::Behavior::kEpilogReturn);
  REQUIRE(function->SaverestType() == SaveRestoreType::GPR);
  REQUIRE(function->IsRestore());
  REQUIRE(function->SaverestIndex() == 14);
  REQUIRE(module_ptr->HasExactExtent(*function));

  Function* save_function = nullptr;
  REQUIRE(module_ptr->DeclareFunction(0x83000010, &save_function) ==
          Symbol::Status::kNew);
  REQUIRE(save_function);
  REQUIRE(save_function->end_address() == 0x8300001C);
  REQUIRE(save_function->behavior() == Function::Behavior::kProlog);
  REQUIRE(save_function->SaverestType() == SaveRestoreType::FPR);
  REQUIRE(save_function->IsSave());
  REQUIRE(save_function->SaverestIndex() == 31);
  save_function->set_status(Symbol::Status::kDeclared);

  function->set_status(Symbol::Status::kDeclared);
  function->set_end_address(0x82000008);
  REQUIRE(module_ptr->DeclareFunction(0x82000000, &function) ==
          Symbol::Status::kFailed);
  REQUIRE(function->status() == Symbol::Status::kFailed);
  REQUIRE_FALSE(module_ptr->HasExactExtent(*function));

  auto foreign_module =
      ExactJitCorpusModule::Create(&processor, corpus, "foreign", &error);
  REQUIRE(foreign_module);
  Function* foreign_function = nullptr;
  REQUIRE(foreign_module->DeclareFunction(0x82000000, &foreign_function) ==
          Symbol::Status::kNew);
  REQUIRE(foreign_function);
  REQUIRE_FALSE(module_ptr->HasExactExtent(*foreign_function));
}

TEST_CASE("exact JIT corpus module translates explicit resume suffixes",
          "[execution-jit-corpus][resume]") {
  constexpr uint32_t kOwnerEntry = 0x82040000;
  constexpr uint32_t kOwnerEnd = kOwnerEntry + 12;
  constexpr uint32_t kInteriorResume = kOwnerEntry + 4;
  constexpr uint32_t kEndResume = kOwnerEnd;

  std::vector<uint8_t> encoded = MakeHeader();
  AppendPage(&encoded, kOwnerEntry, 0x44);
  AppendFunction(&encoded, kOwnerEntry, kOwnerEnd,
                 FunctionFlags(Function::Behavior::kProlog,
                               SaveRestoreType::FPR, false, 28));
  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus));
  const std::vector<uint32_t> captured_definition_order =
      corpus.function_definition_order();

  Memory memory;
  REQUIRE(memory.Initialize());
  store_and_swap<uint32_t>(memory.TranslateVirtual(kOwnerEntry),
                           0x38630001u);  // addi r3, r3, 1
  store_and_swap<uint32_t>(memory.TranslateVirtual(kOwnerEntry + 4),
                           0x38630002u);  // addi r3, r3, 2
  store_and_swap<uint32_t>(memory.TranslateVirtual(kOwnerEntry + 8),
                           0x38630004u);  // addi r3, r3, 4
  store_and_swap<uint32_t>(memory.TranslateVirtual(kOwnerEnd),
                           0x4E800020u);  // blr

  auto backend = testing::CreateBackend();
  REQUIRE(backend);
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::move(backend)));

  const std::array<ExactJitCorpusModule::ResumeEntry, 2> resume_entries = {{
      {kEndResume, kOwnerEntry, kOwnerEnd},
      {kInteriorResume, kOwnerEntry, kOwnerEnd},
  }};
  std::string error;
  auto module = ExactJitCorpusModule::Create(
      &processor, corpus,
      std::span<const ExactJitCorpusModule::ResumeEntry>(resume_entries),
      "resumes", &error);
  REQUIRE(module);
  REQUIRE(error.empty());
  ExactJitCorpusModule* module_ptr = module.get();
  REQUIRE(processor.AddModule(std::move(module)));

  REQUIRE(module_ptr->ContainsAddress(kOwnerEntry));
  REQUIRE(module_ptr->ContainsAddress(kInteriorResume));
  REQUIRE(module_ptr->ContainsAddress(kEndResume));
  REQUIRE_FALSE(module_ptr->ContainsAddress(kOwnerEntry + 8));
  REQUIRE_FALSE(module_ptr->ContainsAddress(kOwnerEnd + 4));
  REQUIRE(processor.LookupModule(kOwnerEntry + 8) == nullptr);

  Function* interior_function = processor.ResolveFunction(kInteriorResume);
  REQUIRE(interior_function);
  REQUIRE(interior_function->address() == kInteriorResume);
  REQUIRE(interior_function->end_address() == kOwnerEnd);
  REQUIRE(interior_function->behavior() == Function::Behavior::kDefault);
  REQUIRE(interior_function->SaverestType() == SaveRestoreType::NONE);
  REQUIRE_FALSE(interior_function->IsSaverest());
  REQUIRE(module_ptr->HasExactExtent(*interior_function));

  Function* end_function = processor.ResolveFunction(kEndResume);
  REQUIRE(end_function);
  REQUIRE(end_function->address() == kEndResume);
  REQUIRE(end_function->end_address() == kOwnerEnd);
  REQUIRE(end_function->behavior() == Function::Behavior::kDefault);
  REQUIRE(end_function->SaverestType() == SaveRestoreType::NONE);
  REQUIRE_FALSE(end_function->IsSaverest());
  REQUIRE(module_ptr->HasExactExtent(*end_function));

  Function* owner_function = nullptr;
  REQUIRE(module_ptr->DeclareFunction(kOwnerEntry, &owner_function) ==
          Symbol::Status::kNew);
  REQUIRE(owner_function);
  REQUIRE(owner_function->end_address() == kOwnerEnd);
  REQUIRE(owner_function->behavior() == Function::Behavior::kProlog);
  REQUIRE(owner_function->SaverestType() == SaveRestoreType::FPR);
  REQUIRE(owner_function->IsSave());
  REQUIRE(owner_function->SaverestIndex() == 28);
  REQUIRE(module_ptr->HasExactExtent(*owner_function));
  REQUIRE(corpus.function_definition_order() == captured_definition_order);

  const uint32_t stack_size = 64 * 1024;
  const uint32_t stack_address = memory.SystemHeapAlloc(stack_size);
  REQUIRE(stack_address != 0);
  auto thread_state = std::make_unique<ThreadState>(&processor, 0x100,
                                                    stack_address + stack_size);
  thread_state->context()->lr = 0xBCBCBCBC;
  thread_state->context()->r[3] = 10;
  REQUIRE(interior_function->Call(thread_state.get(),
                                  uint32_t(thread_state->context()->lr)));
  REQUIRE(thread_state->context()->r[3] == 16);
  thread_state->context()->r[3] = 20;
  REQUIRE(end_function->Call(thread_state.get(),
                             uint32_t(thread_state->context()->lr)));
  REQUIRE(thread_state->context()->r[3] == 20);
  thread_state.reset();
  memory.SystemHeapFree(stack_address);

  interior_function->set_end_address(kOwnerEnd - 4);
  REQUIRE_FALSE(module_ptr->HasExactExtent(*interior_function));
  REQUIRE(module_ptr->DeclareFunction(kInteriorResume, &interior_function) ==
          Symbol::Status::kFailed);
  REQUIRE(interior_function->status() == Symbol::Status::kFailed);
}

TEST_CASE("exact JIT corpus module rejects invalid resume declarations",
          "[execution-jit-corpus][resume]") {
  constexpr uint32_t kOwnerEntry = 0x82050000;
  constexpr uint32_t kOwnerEnd = kOwnerEntry + 28;
  constexpr uint32_t kNestedEntry = kOwnerEntry + 16;
  constexpr uint32_t kNestedEnd = kOwnerEnd;

  std::vector<uint8_t> encoded = MakeHeader();
  AppendPage(&encoded, kOwnerEntry, 0x55);
  AppendFunction(&encoded, kNestedEntry, kNestedEnd);
  AppendFunction(&encoded, kOwnerEntry, kOwnerEnd);
  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus));
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{kNestedEntry, kOwnerEntry});

  Memory memory;
  auto backend = std::make_unique<StubBackend>();
  StubBackend* backend_ptr = backend.get();
  Processor processor(&memory, nullptr);
  REQUIRE(processor.Setup(std::move(backend)));

  using ResumeEntry = ExactJitCorpusModule::ResumeEntry;
  std::string error;
  const auto require_rejected = [&](std::initializer_list<ResumeEntry> entries,
                                    std::string_view expected_error) {
    const std::vector<ResumeEntry> declaration(entries);
    auto module = ExactJitCorpusModule::Create(
        &processor, corpus, std::span<const ResumeEntry>(declaration),
        "rejected", &error);
    REQUIRE_FALSE(module);
    REQUIRE(error == expected_error);
  };

  require_rejected({{0, kOwnerEntry, kOwnerEnd}},
                   "exact corpus resume PC is zero or unaligned");
  require_rejected({{kOwnerEntry + 2, kOwnerEntry, kOwnerEnd}},
                   "exact corpus resume PC is zero or unaligned");
  require_rejected({{kOwnerEntry + 4, kOwnerEntry + 8, kOwnerEnd}},
                   "exact corpus resume entry has an unknown owner");
  require_rejected({{kOwnerEntry + 4, kOwnerEntry, kOwnerEnd - 4}},
                   "exact corpus resume entry owner extent is inconsistent");
  require_rejected({{kOwnerEntry + 12, kNestedEntry, kNestedEnd}},
                   "exact corpus resume PC is outside its owner extent");
  require_rejected({{kOwnerEnd + 4, kOwnerEntry, kOwnerEnd}},
                   "exact corpus resume PC is outside its owner extent");
  require_rejected({{kNestedEntry, kOwnerEntry, kOwnerEnd}},
                   "exact corpus resume PC overlaps a captured entry");
  require_rejected({{kOwnerEntry, kOwnerEntry, kOwnerEnd}},
                   "exact corpus resume PC overlaps a captured entry");
  require_rejected({{kOwnerEntry + 4, kOwnerEntry, kOwnerEnd},
                    {kOwnerEntry + 4, kOwnerEntry, kOwnerEnd}},
                   "exact corpus contains a duplicate resume entry");
  require_rejected({{kOwnerEntry + 20, kOwnerEntry, kOwnerEnd},
                    {kOwnerEntry + 20, kNestedEntry, kNestedEnd}},
                   "exact corpus contains conflicting resume entries");

  REQUIRE(backend_ptr->executable_ranges.empty());
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{kNestedEntry, kOwnerEntry});
}

}  // namespace xe::cpu::test
