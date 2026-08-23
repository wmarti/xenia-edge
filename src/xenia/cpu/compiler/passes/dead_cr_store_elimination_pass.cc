/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/dead_cr_store_elimination_pass.h"

#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_bool(
    eliminate_dead_cr_stores, true,
    "Drop condition-register writes that no later instruction can observe. "
    "Every PPC compare materializes all three bits of a CR field and stores "
    "each to the context, while the branch that follows consumes only the SSA "
    "value.",
    "CPU");

DECLARE_bool(debug);
DECLARE_bool(store_all_context_values);
DECLARE_bool(full_optimization_even_with_debug);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;

namespace {

// cr0..cr7 are the first 32 bytes of the context, one byte per condition bit.
// One uint32_t bit per byte, so the whole analysis fits in a register.
constexpr uint32_t kCRBase =
    static_cast<uint32_t>(offsetof(ppc::PPCContext, cr0));
constexpr uint32_t kCRBytes = 8 * 4;
constexpr uint32_t kAllCR = ~uint32_t(0);

// PowerPC calling convention: cr2, cr3 and cr4 are preserved across a call,
// the rest are the callee's to destroy. A value stored into a volatile field
// before a call therefore cannot be observed after it -- which is what makes
// the redundant writes inside a loop removable at all.
constexpr uint32_t kVolatileCR = 0x000000FFu     // cr0, cr1
                                 | 0xFFF00000u;  // cr5, cr6, cr7

uint32_t MaskForRange(uint32_t offset, uint32_t size) {
  uint32_t mask = 0;
  for (uint32_t byte = offset; byte < offset + size; ++byte) {
    if (byte >= kCRBase && byte < kCRBase + kCRBytes) {
      mask |= uint32_t(1) << (byte - kCRBase);
    }
  }
  return mask;
}

// The block a branch transfers to, or null if this is not a branch.
Block* BranchTarget(const Instr* i) {
  switch (i->GetOpcodeNum()) {
    case OPCODE_BRANCH:
      return i->src1.label ? i->src1.label->block : nullptr;
    case OPCODE_BRANCH_TRUE:
    case OPCODE_BRANCH_FALSE:
      return i->src2.label ? i->src2.label->block : nullptr;
    default:
      return nullptr;
  }
}

enum class CallKind {
  kNotACall,
  // Always executed and always returns here: the callee is free to destroy the
  // volatile fields, so a write before it cannot be observed after it.
  kUnconditional,
  // Returns here too, but only sometimes taken. Liveness is a union over
  // paths, and on the not-taken path nothing is clobbered, so this may not
  // kill anything. It reads no condition bits either, so it is transparent.
  kConditional,
};

CallKind ClassifyCall(const Instr* i) {
  switch (i->GetOpcodeNum()) {
    case OPCODE_CALL:
    case OPCODE_CALL_INDIRECT:
    case OPCODE_CALL_EXTERN:
      // A tail call leaves through this function's own return, so it is an
      // exit rather than a clobber -- let it fall through to the volatile
      // case below.
      return (i->flags & CALL_TAIL) ? CallKind::kNotACall
                                    : CallKind::kUnconditional;
    case OPCODE_CALL_TRUE:
    case OPCODE_CALL_INDIRECT_TRUE:
      return (i->flags & CALL_TAIL) ? CallKind::kNotACall
                                    : CallKind::kConditional;
    default:
      return CallKind::kNotACall;
  }
}

// Walks one block backwards, transforming the live set. Deletes stores when
// `apply` is set; otherwise only computes.
uint32_t TransferBlock(Block* block, uint32_t live, bool apply,
                       const std::unordered_map<Block*, size_t>& index,
                       const std::vector<uint32_t>& live_in) {
  auto live_in_of = [&](Block* dest) -> uint32_t {
    if (!dest) {
      return kAllCR;
    }
    auto it = index.find(dest);
    return it == index.end() ? kAllCR : live_in[it->second];
  };

  Instr* i = block->instr_tail;
  while (i) {
    Instr* prev = i->prev;
    const Opcode num = i->GetOpcodeNum();
    if (Block* target = BranchTarget(i)) {
      // A branch is VOLATILE but reads no context. What follows it in this
      // block is the not-taken path, already accounted for; the taken path is
      // the target's own live set. Anything else would either lose the target
      // or, by falling through to the blanket VOLATILE case below, relight
      // every bit and eliminate nothing.
      if (num == OPCODE_BRANCH) {
        live = live_in_of(target);
      } else {
        live |= live_in_of(target);
      }
    } else if (num == OPCODE_LOAD_CONTEXT) {
      live |= MaskForRange(static_cast<uint32_t>(i->src1.offset),
                           static_cast<uint32_t>(GetTypeSize(i->dest->type)));
    } else if (num == OPCODE_STORE_CONTEXT) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const uint32_t size =
          static_cast<uint32_t>(GetTypeSize(i->src2.value->type));
      const uint32_t mask = MaskForRange(offset, size);
      // Only a store that lies wholly inside the condition register is ours to
      // reason about.
      if (mask && offset >= kCRBase && offset + size <= kCRBase + kCRBytes) {
        if (!(mask & live)) {
          if (apply) {
            i->UnlinkAndNOP();
          }
        } else {
          live &= ~mask;
        }
      }
    } else if (const CallKind kind = ClassifyCall(i);
               kind != CallKind::kNotACall) {
      if (kind == CallKind::kUnconditional) {
        live &= ~kVolatileCR;
      }
    } else if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
      // Returns, barriers, anything that can expose the context: assume it is
      // all read.
      live = kAllCR;
    }
    i = prev;
  }
  return live;
}

}  // namespace

DeadCRStoreEliminationPass::DeadCRStoreEliminationPass() : CompilerPass() {}

DeadCRStoreEliminationPass::~DeadCRStoreEliminationPass() = default;

bool DeadCRStoreEliminationPass::Run(HIRBuilder* builder) {
  SCOPE_profile_cpu_f("cpu");

  // Same gate the cross-block dead-store elimination uses: stripping stores
  // costs the debugger the register values it recovers from the context.
  if (!cvars::eliminate_dead_cr_stores ||
      !(cvars::full_optimization_even_with_debug ||
        (!cvars::debug && !cvars::store_all_context_values))) {
    return true;
  }

  std::vector<Block*> blocks;
  std::unordered_map<Block*, size_t> index;
  for (auto block = builder->first_block(); block; block = block->next) {
    index.emplace(block, blocks.size());
    blocks.push_back(block);
  }
  if (blocks.empty()) {
    return true;
  }

  // Backward liveness over the condition register. A block with no successors
  // returns, and the caller sees whatever the context holds, so everything is
  // live out of it.
  std::vector<uint32_t> live_in(blocks.size(), 0);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t n = blocks.size(); n-- > 0;) {
      Block* block = blocks[n];
      uint32_t live_out = 0;
      if (!block->outgoing_edge_head) {
        live_out = kAllCR;
      } else {
        for (auto edge = block->outgoing_edge_head; edge;
             edge = edge->outgoing_next) {
          auto it = index.find(edge->dest);
          if (it == index.end()) {
            live_out = kAllCR;
            break;
          }
          live_out |= live_in[it->second];
        }
      }
      const uint32_t result =
          TransferBlock(block, live_out, false, index, live_in);
      if (result != live_in[n]) {
        live_in[n] = result;
        changed = true;
      }
    }
  }

  // Dropping the store is the whole change. The comparison feeding it loses
  // its only use, and DeadCodeEliminationPass removes any non-VOLATILE
  // instruction whose dest has no uses, so the compare goes with it.
  for (size_t n = 0; n < blocks.size(); ++n) {
    Block* block = blocks[n];
    uint32_t live_out = 0;
    if (!block->outgoing_edge_head) {
      live_out = kAllCR;
    } else {
      for (auto edge = block->outgoing_edge_head; edge;
           edge = edge->outgoing_next) {
        auto it = index.find(edge->dest);
        if (it == index.end()) {
          live_out = kAllCR;
          break;
        }
        live_out |= live_in[it->second];
      }
    }
    TransferBlock(block, live_out, true, index, live_in);
  }

  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
