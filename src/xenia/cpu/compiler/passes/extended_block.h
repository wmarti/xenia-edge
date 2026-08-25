/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_EXTENDED_BLOCK_H_
#define XENIA_CPU_COMPILER_PASSES_EXTENDED_BLOCK_H_

#include "xenia/cpu/hir/block.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/opcodes.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// An extended block is a maximal chain of layout-adjacent blocks in which every
// block after the first is entered only by running the block before it all the
// way to its end.
//
// Why that shape. Entering only that way means every definition earlier in the
// chain has already executed when a later block runs, so a value -- and the
// register holding it -- can cross the boundary with no phi node and no merge.
// The HIR has no phi, so anything with two ways in has to go through memory,
// and this is exactly the part that does not.
//
// The third condition below is the one that is easy to get wrong, and getting
// it wrong is a miscompile that the 169,048-case corpus cannot see. A HIR block
// is NOT split at a conditional branch: the frontend emits `branch_true cond,
// target` and then keeps appending the fall-through code to the same block. So
// a block can branch out from its middle and carry on for another twenty
// instructions, and the branch target then has exactly one incoming edge, from
// exactly the layout-previous block, while the values defined after that branch
// never execute on the path that reaches it.
//
// Halo Reach's 827EA298 is the case that found this: `branch_true v839,
// label34` is instruction 10 of label33, `v842 = load_context +288` is
// instruction 11, and label33 runs 22 more instructions before its tail `branch
// label35`. Promotion rewrote label34's `load_context +288` into a use of v842
// and the guest faulted during module launch, deterministically, reading a
// register that had never been written on that path.
//
// Note also that hir::Edge::DOMINATES cannot be used for this. It is set purely
// from "dest has exactly one incoming edge" (control_flow_analysis_pass.cc),
// which for the shape above is not dominance at all.
//
// Returns true when `block` begins a new extended block, i.e. when per-block
// state has to be reset before processing it.
inline bool StartsExtendedBlock(const hir::Block* block) {
  const hir::Edge* in = block->incoming_edge_head;
  if (!in) {
    // No way in at all: the entry block, or unreachable.
    return true;
  }
  if (in->incoming_next) {
    // More than one predecessor: a merge, and the values arriving on the two
    // paths need not agree.
    return true;
  }
  const hir::Block* prev = block->prev;
  if (in->src != prev) {
    // Requiring the predecessor to be `prev` rather than any single
    // predecessor is what keeps this safe for the register allocator, which
    // numbers instructions in layout order and walks its usage lists by that
    // ordinal. A chain that jumped backwards in layout would break that
    // ordering.
    return true;
  }
  // The edge has to leave `prev` from its trailing run of branches, so that
  // everything else in `prev` has run by the time we do.
  for (const hir::Instr* i = prev->instr_tail;
       i && (i->opcode->flags & hir::OPCODE_FLAG_BRANCH); i = i->prev) {
    const hir::Label* target = nullptr;
    if (i->opcode == &hir::OPCODE_BRANCH_info) {
      target = i->src1.label;
    } else if (i->opcode == &hir::OPCODE_BRANCH_TRUE_info ||
               i->opcode == &hir::OPCODE_BRANCH_FALSE_info) {
      target = i->src2.label;
    }
    if (target && target->block == block) {
      // Branched to from the tail run: prev ran to completion first.
      return false;
    }
  }
  // Not branched to from the tail run. We are the fall-through, which is also
  // safe -- unless prev ends in an unconditional branch, in which case there is
  // no fall-through and the only way here was a branch from prev's middle.
  const hir::Instr* tail = prev->instr_tail;
  return tail && tail->opcode == &hir::OPCODE_BRANCH_info;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_EXTENDED_BLOCK_H_
