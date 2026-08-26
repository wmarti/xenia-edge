/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/exact_jit_corpus_module.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenia/cpu/backend/assembler.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
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
                    uint32_t end_address) {
  AppendU32(data, JitCorpus::kTagFunction);
  AppendU32(data, address);
  AppendU32(data, end_address);
  AppendU32(data, 64);
  AppendU32(data, 0);
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
  AppendFunction(&encoded, 0x83000010, 0x8300001C);
  AppendFunction(&encoded, 0x82000000, 0x82000004);

  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus));

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
  REQUIRE(module_ptr->HasExactExtent(*function));

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

}  // namespace xe::cpu::test
