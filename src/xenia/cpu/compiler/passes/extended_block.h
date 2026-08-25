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

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// An extended block is a maximal chain of layout-adjacent blocks in which every
// block after the first has exactly one way in: the fall-through from the block
// laid out immediately before it.
//
// Why that shape and not something more general. Single entry means every
// definition earlier in the chain dominates every later use, so a value -- and
// the register holding it -- can cross the boundary with no phi node and no
// merge. The HIR has no phi, so anything with two ways in has to go through
// memory, and this is exactly the part that does not.
//
// Requiring the predecessor to be `prev` rather than any single predecessor is
// what makes it safe for the register allocator, which numbers instructions in
// layout order and walks its usage lists by that ordinal. A chain that jumped
// backwards in layout would break that ordering.
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
  return in->src != block->prev;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_EXTENDED_BLOCK_H_
