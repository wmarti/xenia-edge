/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_TESTING_PPC_TEST_SELECTION_H_
#define XENIA_CPU_PPC_TESTING_PPC_TEST_SELECTION_H_

#include <cstddef>

namespace xe {
namespace cpu {
namespace test {

enum class PpcTestSelectionVerdict {
  kRunnable,
  kSuiteLoadFailed,
  kNoLoadedSuites,
  kNoRunnableCases,
};

constexpr PpcTestSelectionVerdict EvaluatePpcTestSelection(
    bool suite_load_failed, size_t loaded_suite_count,
    size_t runnable_case_count) {
  if (suite_load_failed) {
    return PpcTestSelectionVerdict::kSuiteLoadFailed;
  }
  if (loaded_suite_count == 0) {
    return PpcTestSelectionVerdict::kNoLoadedSuites;
  }
  if (runnable_case_count == 0) {
    return PpcTestSelectionVerdict::kNoRunnableCases;
  }
  return PpcTestSelectionVerdict::kRunnable;
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_TESTING_PPC_TEST_SELECTION_H_
