/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_
#define XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_

// Metal shared memory attempts bytes-no-copy aliasing on unified-memory
// devices and falls back to staged uploads when unsupported.

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/gpu/metal/metal_upload_buffer_pool.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;
class MetalSharedMemory : public SharedMemory {
 public:
  MetalSharedMemory(MetalCommandProcessor& command_processor, Memory& memory,
                    TraceWriter& trace_writer);
  ~MetalSharedMemory() override;
  bool Initialize();
  void Shutdown();
  void ClearCache() override;

  MTL::Buffer* GetBuffer() const { return buffer_; }
  const uint8_t* GetXboxRamBase() const {
    return static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  }
  // Records accesses encoded into the command processor's submission command
  // buffer. CPU updates to a page stay staged until this submission retires.
  void MarkGpuAccess(uint32_t start, uint32_t length,
                     uint64_t submission_index);
  // Standalone command buffers aren't represented by the main submission
  // counter. Keep a page refcount until their completion handler runs.
  void TrackStandaloneGpuAccess(MTL::CommandBuffer* command_buffer,
                                const std::pair<uint32_t, uint32_t>* ranges,
                                uint32_t range_count);
  // Balances TrackStandaloneGpuAccess when a command buffer is guaranteed not
  // to be committed, and thus will never run its completion handler.
  void CancelStandaloneGpuAccess(MTL::CommandBuffer* command_buffer);

  // For trace dump, simplified - just make buffer available for reading
  void UseForReading() {
    // No state transitions needed in Metal
  }
  // Override pure virtual function from SharedMemory
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_upload_ranges) override;

  // Returns true if there is GPU-written data for the trace. The buffer is
  // CPU-visible, so nothing is submitted - the caller must await the GPU.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();

 private:
  MetalCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  std::unique_ptr<MetalUploadBufferPool> upload_buffer_pool_;
  std::vector<uint64_t> page_last_main_gpu_access_submission_;
  // Fast path for consecutive draws repeatedly touching the same range.
  uint32_t last_gpu_access_page_first_ = 1;
  uint32_t last_gpu_access_page_last_ = 0;
  uint64_t last_gpu_access_submission_ = 0;
  void ReleaseStandaloneGpuAccess(MTL::CommandBuffer* command_buffer);
  std::mutex standalone_gpu_access_mutex_;
  std::vector<uint32_t> page_standalone_gpu_access_counts_;
  std::unordered_map<MTL::CommandBuffer*,
                     std::vector<std::pair<uint32_t, uint32_t>>>
      standalone_gpu_access_page_ranges_;
  MTL::Buffer* buffer_ = nullptr;
  bool use_zero_copy_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif
