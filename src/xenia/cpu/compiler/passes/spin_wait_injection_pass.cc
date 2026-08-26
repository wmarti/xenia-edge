/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/spin_wait_injection_pass.h"

#include <algorithm>
#include <unordered_set>

#include "xenia/base/cvar.h"
#include "xenia/cpu/hir/hir_builder.h"

DECLARE_uint32(spin_wait_yield_after);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

SpinWaitInjectionPass::SpinWaitInjectionPass() : CompilerPass() {}

SpinWaitInjectionPass::~SpinWaitInjectionPass() {}

namespace {

// The loop body is [header .. latch] in layout order; blocks are laid out in
// guest address order, so the extent walk is a ->next chain.
struct LoopShape {
  uint32_t guest_lo = UINT32_MAX;
  uint32_t guest_hi = 0;
  bool has_memory_store = false;
  bool has_memory_load = false;
  bool has_call = false;
  bool has_indirect_branch = false;
  bool has_induction_variable = false;
};

// A loop that advances its own state each iteration is scanning, not
// waiting. The shape in HIR is a guest register self-update through its
// context slot: store_context(O, add(load_context(O), constant)). The first
// live false positive -- a byte-wise parse loop, `addi r19,r19,1; lbz; bl;
// cmpwi; bne` -- has exactly this chain for r19, while a true wait loop
// recomputes its addresses from loop-invariant registers and never advances
// anything.
bool IsInductionStore(Instr* instr) {
  if (instr->GetOpcodeNum() != OPCODE_STORE_CONTEXT) {
    return false;
  }
  Value* stored = instr->src2.value;
  if (!stored || !stored->def) {
    return false;
  }
  Instr* def = stored->def;
  if (def->GetOpcodeNum() != OPCODE_ADD && def->GetOpcodeNum() != OPCODE_SUB) {
    return false;
  }
  // One operand constant, the other loaded from the same context offset.
  Value* counted = nullptr;
  if (def->src2.value && def->src2.value->IsConstant()) {
    counted = def->src1.value;
  } else if (def->src1.value && def->src1.value->IsConstant()) {
    counted = def->src2.value;
  }
  if (!counted || !counted->def) {
    return false;
  }
  Instr* load = counted->def;
  return load->GetOpcodeNum() == OPCODE_LOAD_CONTEXT &&
         load->src1.offset == instr->src1.offset;
}

LoopShape ScanLoop(Block* header, Block* latch) {
  LoopShape shape;
  for (Block* b = header; b != nullptr; b = b->next) {
    for (auto instr = b->instr_head; instr != nullptr; instr = instr->next) {
      const Opcode num = instr->GetOpcodeNum();
      switch (num) {
        case OPCODE_SOURCE_OFFSET: {
          const uint32_t addr = uint32_t(instr->src1.offset);
          shape.guest_lo = std::min(shape.guest_lo, addr);
          shape.guest_hi = std::max(shape.guest_hi, addr);
          break;
        }
        case OPCODE_STORE:
        case OPCODE_STORE_OFFSET:
        case OPCODE_STORE_MMIO:
        case OPCODE_MEMSET:
        case OPCODE_ATOMIC_COMPARE_EXCHANGE:
          shape.has_memory_store = true;
          break;
        case OPCODE_LOAD:
        case OPCODE_LOAD_OFFSET:
        case OPCODE_LOAD_MMIO:
          shape.has_memory_load = true;
          break;
        case OPCODE_CALL:
        case OPCODE_CALL_TRUE:
          shape.has_call = true;
          break;
        case OPCODE_STORE_CONTEXT:
          if (IsInductionStore(instr)) {
            shape.has_induction_variable = true;
          }
          break;
        case OPCODE_CALL_INDIRECT:
        case OPCODE_CALL_INDIRECT_TRUE:
        case OPCODE_CALL_EXTERN:
          // An indirect or extern call could reach anything, including code
          // that stores. Treat it as disqualifying rather than as the call
          // the filter wants.
          shape.has_indirect_branch = true;
          break;
        default:
          break;
      }
    }
    if (b == latch) {
      break;
    }
  }
  return shape;
}

}  // namespace

bool SpinWaitInjectionPass::Run(HIRBuilder* builder) {
  // Pass success, not changed; Compile aborts the function on false.
  if (!cvars::spin_wait_yield_after || !builder->first_block()) {
    return true;
  }
  // Same walk as PreemptCheckInjectionPass: blocks are in guest address
  // order, so every intra-function cycle branches to an already-seen block.
  std::unordered_set<Block*> seen;
  std::unordered_set<Block*> tagged;
  for (auto block = builder->first_block(); block != nullptr;
       block = block->next) {
    seen.insert(block);
    for (auto instr = block->instr_head; instr != nullptr;
         instr = instr->next) {
      Label* label = nullptr;
      if (instr->opcode == &OPCODE_BRANCH_info) {
        label = instr->src1.label;
      } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
                 instr->opcode == &OPCODE_BRANCH_FALSE_info) {
        label = instr->src2.label;
      }
      if (!label || !label->block || !seen.count(label->block)) {
        continue;
      }
      Block* header = label->block;
      if (tagged.count(header)) {
        continue;
      }
      LoopShape shape = ScanLoop(header, block);
      if (shape.guest_lo > shape.guest_hi) {
        continue;  // no source offsets to size the loop with
      }
      const uint32_t span = (shape.guest_hi - shape.guest_lo) / 4 + 1;
      if (span > kMaxBodyGuestInstrs || shape.has_memory_store ||
          !shape.has_memory_load || !shape.has_call ||
          shape.has_indirect_branch || shape.has_induction_variable) {
        continue;
      }
      // The header's guest address identifies this site at runtime: the trip
      // helper keys its state to it, so a sleep can only be served to the
      // loop that earned it. Taken from the header block's first
      // SOURCE_OFFSET, the same way the preempt pass names its safepoints.
      uint32_t site = 0;
      for (auto s = header->instr_head; s; s = s->next) {
        if (s->opcode == &OPCODE_SOURCE_OFFSET_info) {
          site = uint32_t(s->src1.offset);
          break;
        }
      }
      // Tag the header: first non-fake instruction, falling through an
      // all-fake block exactly the way the preempt pass does.
      for (Block* b = header; b != nullptr; b = b->next) {
        auto first = b->instr_head;
        for (; first && first->IsFake(); first = first->next) {
        }
        if (first) {
          if (first->GetOpcodeNum() != OPCODE_DELAY_EXECUTION) {
            Instr* delay = builder->DelayExecution(DELAY_EXECUTION_INJECTED);
            delay->src1.offset = site;
            delay->MoveBefore(first);
          }
          tagged.insert(header);
          break;
        }
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
