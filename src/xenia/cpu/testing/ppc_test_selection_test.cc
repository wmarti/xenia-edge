/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/testing/ppc_test_selection.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {
namespace {

TEST_CASE("PPC test selection requires a loaded runnable corpus",
          "[ppc-test-selection]") {
  CHECK(EvaluatePpcTestSelection(false, 1, 1) ==
        PpcTestSelectionVerdict::kRunnable);
  CHECK(EvaluatePpcTestSelection(true, 1, 1) ==
        PpcTestSelectionVerdict::kSuiteLoadFailed);
  CHECK(EvaluatePpcTestSelection(false, 0, 0) ==
        PpcTestSelectionVerdict::kNoLoadedSuites);
  CHECK(EvaluatePpcTestSelection(false, 1, 0) ==
        PpcTestSelectionVerdict::kNoRunnableCases);
}

TEST_CASE("PPC test selection prioritizes suite load failures",
          "[ppc-test-selection]") {
  CHECK(EvaluatePpcTestSelection(true, 0, 0) ==
        PpcTestSelectionVerdict::kSuiteLoadFailed);
  CHECK(EvaluatePpcTestSelection(true, 8, 1000) ==
        PpcTestSelectionVerdict::kSuiteLoadFailed);
}

}  // namespace
}  // namespace test
}  // namespace cpu
}  // namespace xe
