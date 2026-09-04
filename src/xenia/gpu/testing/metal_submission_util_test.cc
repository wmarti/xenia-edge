/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_submission_util.h"
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace gpu {
namespace metal {
namespace test {

TEST_CASE("texture reload is ordered after earlier draw work",
          "[metal][submission]") {
  CHECK(GetDrawTextureRequestBarrier(false, true, true, true) ==
        DrawTextureRequestBarrier::kNone);
  CHECK(GetDrawTextureRequestBarrier(true, false, false, true) ==
        DrawTextureRequestBarrier::kNone);
  CHECK(GetDrawTextureRequestBarrier(true, true, true, true) ==
        DrawTextureRequestBarrier::kEndRenderEncoder);
  CHECK(GetDrawTextureRequestBarrier(true, true, false, true) ==
        DrawTextureRequestBarrier::kNone);
  CHECK(GetDrawTextureRequestBarrier(true, true, true, false) ==
        DrawTextureRequestBarrier::kEndCommandBuffer);
  CHECK(GetDrawTextureRequestBarrier(true, true, false, false) ==
        DrawTextureRequestBarrier::kEndCommandBuffer);
}

TEST_CASE("converted index buffers wait for submission completion",
          "[metal][submission]") {
  CHECK_FALSE(CanReuseConvertedIndexBuffer(true, 4, 4));
  CHECK_FALSE(CanReuseConvertedIndexBuffer(false, 4, 3));
  CHECK(CanReuseConvertedIndexBuffer(false, 4, 4));
  CHECK(CanReuseConvertedIndexBuffer(false, 4, 5));
  CHECK(CanReuseConvertedIndexBuffer(false, 0, 0));
}

}  // namespace test
}  // namespace metal
}  // namespace gpu
}  // namespace xe
