/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SUBMISSION_UTIL_H_
#define XENIA_GPU_METAL_METAL_SUBMISSION_UTIL_H_

#include <cstdint>

namespace xe {
namespace gpu {
namespace metal {

enum class DrawTextureRequestBarrier {
  kNone,
  kEndRenderEncoder,
  kEndCommandBuffer,
};

constexpr DrawTextureRequestBarrier GetDrawTextureRequestBarrier(
    bool request_work_pending, bool has_current_command_buffer,
    bool has_active_render_encoder, bool can_upload_in_current_command_buffer) {
  if (!request_work_pending || !has_current_command_buffer) {
    return DrawTextureRequestBarrier::kNone;
  }
  if (!can_upload_in_current_command_buffer) {
    return DrawTextureRequestBarrier::kEndCommandBuffer;
  }
  return has_active_render_encoder
             ? DrawTextureRequestBarrier::kEndRenderEncoder
             : DrawTextureRequestBarrier::kNone;
}

constexpr bool CanReuseConvertedIndexBuffer(bool referenced_in_current_frame,
                                            uint64_t last_submission_used,
                                            uint64_t completed_submission) {
  return !referenced_in_current_frame &&
         last_submission_used <= completed_submission;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SUBMISSION_UTIL_H_
