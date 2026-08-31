/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_MEMORY_UTIL_H_
#define XENIA_GPU_METAL_METAL_SHARED_MEMORY_UTIL_H_

#include <cstdint>

namespace xe {
namespace gpu {
namespace metal {

// A CPU write to the mirror buffer is safe only after every GPU user of the
// page has retired. Otherwise the new bytes must reach the mirror through an
// ordered GPU copy.
inline bool IsSharedMemoryDirectWriteSafe(
    bool direct_write_enabled, bool has_shared_buffer_mapping,
    uint64_t last_main_gpu_access_submission,
    uint64_t completed_main_submission, uint32_t standalone_gpu_access_count) {
  return direct_write_enabled && has_shared_buffer_mapping &&
         standalone_gpu_access_count == 0 &&
         last_main_gpu_access_submission <= completed_main_submission;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SHARED_MEMORY_UTIL_H_
