/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_PREEMPT_CHECK_INJECTION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_PREEMPT_CHECK_INJECTION_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Prepends a safepoint to the entry block and every back-edge target. Normal
// builds enable it with the guest scheduler; capture builds always enable it
// for the independent capture rendezvous as well. Both backends lower it.
class PreemptCheckInjectionPass : public CompilerPass {
 public:
  PreemptCheckInjectionPass();
  ~PreemptCheckInjectionPass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_PREEMPT_CHECK_INJECTION_PASS_H_
