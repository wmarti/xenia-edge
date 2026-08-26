/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Tags the back-edge target of spin-shaped guest loops with a
// DELAY_EXECUTION carrying DELAY_EXECUTION_INJECTED, so the backend can count
// iterations and release the core once the loop is provably waiting rather
// than working. Gated on --spin_wait_yield_after; a zero threshold (the
// default) disables the pass entirely.
//
// A loop qualifies only when every one of these holds:
//   - its guest span is at most kMaxBodyGuestInstrs instructions;
//   - its body performs no memory store (context and local stores are fine --
//     a wait loop's only stores are into its own frame's scratch);
//   - its body performs at least one memory load (it must be reading the
//     thing it waits on);
//   - its body makes at least one call. This is deliberately narrow: the
//     motivating loop's body is `bl` + compare, and requiring the call
//     excludes short store-less scan loops (memchr shapes), which are the
//     false positive that would pay the escalation's per-trip sleep while
//     doing real work.
//
// The static filter is not the safety story -- loops of spin shape exist in
// functions covering half of executed code. Safety comes from the runtime
// trigger: the backend escalates only after --spin_wait_yield_after
// consecutive iterations averaging under --spin_wait_max_iter_ns each.
class SpinWaitInjectionPass : public CompilerPass {
 public:
  SpinWaitInjectionPass();
  ~SpinWaitInjectionPass() override;

  bool Run(hir::HIRBuilder* builder) override;

  static constexpr uint32_t kMaxBodyGuestInstrs = 8;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_SPIN_WAIT_INJECTION_PASS_H_
