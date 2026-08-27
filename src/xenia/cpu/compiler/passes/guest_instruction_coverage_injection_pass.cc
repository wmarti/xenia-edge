/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/guest_instruction_coverage_injection_pass.h"

#include <limits>

#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

GuestInstructionCoverageInjectionPass::GuestInstructionCoverageInjectionPass()
    : CompilerPass() {}

GuestInstructionCoverageInjectionPass::
    ~GuestInstructionCoverageInjectionPass() = default;

bool GuestInstructionCoverageInjectionPass::Run(HIRBuilder* builder) {
  for (Block* block = builder->first_block(); block; block = block->next) {
    uint64_t guest_instruction_count = 0;
    for (Instr* instruction = block->instr_head; instruction;
         instruction = instruction->next) {
      if (instruction->GetOpcodeNum() == OPCODE_SOURCE_OFFSET) {
        ++guest_instruction_count;
      }
    }
    if (!guest_instruction_count) {
      continue;
    }
    if (guest_instruction_count > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    Instr* coverage = builder->GuestInstructionCoverage(
        static_cast<uint32_t>(guest_instruction_count));
    coverage->MoveBefore(block->instr_head);
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
