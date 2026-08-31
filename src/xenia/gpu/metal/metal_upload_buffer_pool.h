/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_UPLOAD_BUFFER_POOL_H_
#define XENIA_GPU_METAL_METAL_UPLOAD_BUFFER_POOL_H_

#include <functional>

#include "xenia/ui/graphics_upload_buffer_pool.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalUploadBufferPool : public ui::GraphicsUploadBufferPool {
 public:
  MetalUploadBufferPool(MTL::Device* device,
                        size_t page_size = kDefaultPageSize);
  ~MetalUploadBufferPool() override;

  using PageCreatedCallback = std::function<void(MTL::Buffer*)>;
  void SetPageCreatedCallback(PageCreatedCallback callback);

  uint8_t* Request(uint64_t submission_index, size_t size, size_t alignment,
                   MTL::Buffer** buffer_out, size_t& offset_out,
                   uint64_t& gpu_address_out);
  uint8_t* RequestPartial(uint64_t submission_index, size_t size,
                          size_t alignment, MTL::Buffer** buffer_out,
                          size_t& offset_out, uint64_t& gpu_address_out,
                          size_t& size_out);

 protected:
  Page* CreatePageImplementation() override;

 private:
  struct MetalPage : public Page {
    MetalPage(MTL::Buffer* buffer, void* mapping);
    ~MetalPage() override;

    MTL::Buffer* buffer_;
    void* mapping_;
    uint64_t gpu_address_;
  };

  MTL::Device* device_;
  PageCreatedCallback page_created_callback_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_UPLOAD_BUFFER_POOL_H_
