/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/draw_util.h"
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace gpu {
namespace test {

TEST_CASE("D3D10 integer polygon offset snaps away from zero", "[gpu]") {
  constexpr float unorm24_factor = draw_util::kD3D10PolygonOffsetFactorUnorm24;
  constexpr float float24_layer_factor =
      draw_util::kD3D10PolygonOffsetFactorFloat24 * (1.0f / 8.0f);

  CHECK(draw_util::GetD3D10IntegerPolygonOffset(
            xenos::DepthRenderTargetFormat::kD24S8, 0.0f) == 0);
  CHECK(draw_util::GetD3D10IntegerPolygonOffset(
            xenos::DepthRenderTargetFormat::kD24S8, 0.25f / unorm24_factor) ==
        1);
  CHECK(draw_util::GetD3D10IntegerPolygonOffset(
            xenos::DepthRenderTargetFormat::kD24S8, -0.25f / unorm24_factor) ==
        -1);

  // One float24 layer is eight float32 ULPs. The helper rounds in layer units
  // first, so even a fractional positive or negative layer becomes +/-8.
  CHECK(draw_util::GetD3D10IntegerPolygonOffset(
            xenos::DepthRenderTargetFormat::kD24FS8,
            0.25f / float24_layer_factor) == 8);
  CHECK(draw_util::GetD3D10IntegerPolygonOffset(
            xenos::DepthRenderTargetFormat::kD24FS8,
            -0.25f / float24_layer_factor) == -8);
}

}  // namespace test
}  // namespace gpu
}  // namespace xe
