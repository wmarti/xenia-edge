/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shared_memory_util.h"
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace gpu {
namespace metal {
namespace test {

TEST_CASE("Metal shared memory direct writes require retired GPU readers",
          "[metal][shared-memory]") {
  REQUIRE(IsSharedMemoryDirectWriteSafe(true, true, 7, 7, 0));
  REQUIRE(IsSharedMemoryDirectWriteSafe(true, true, 0, 0, 0));

  REQUIRE_FALSE(IsSharedMemoryDirectWriteSafe(false, true, 7, 7, 0));
  REQUIRE_FALSE(IsSharedMemoryDirectWriteSafe(true, false, 7, 7, 0));
  REQUIRE_FALSE(IsSharedMemoryDirectWriteSafe(true, true, 8, 7, 0));
  REQUIRE_FALSE(IsSharedMemoryDirectWriteSafe(true, true, 7, 7, 1));
}

}  // namespace test
}  // namespace metal
}  // namespace gpu
}  // namespace xe
