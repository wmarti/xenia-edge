/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_upload_buffer_pool.h"

#include <utility>

#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace metal {

MetalUploadBufferPool::MetalUploadBufferPool(MTL::Device* device,
                                             size_t page_size)
    : GraphicsUploadBufferPool(page_size), device_(device) {}

MetalUploadBufferPool::~MetalUploadBufferPool() = default;

void MetalUploadBufferPool::SetPageCreatedCallback(
    PageCreatedCallback callback) {
  page_created_callback_ = std::move(callback);
}

MetalUploadBufferPool::MetalPage::MetalPage(MTL::Buffer* buffer, void* mapping)
    : buffer_(buffer), mapping_(mapping), gpu_address_(buffer->gpuAddress()) {}

MetalUploadBufferPool::MetalPage::~MetalPage() {
  if (buffer_) {
    buffer_->release();
  }
}

ui::GraphicsUploadBufferPool::Page*
MetalUploadBufferPool::CreatePageImplementation() {
  MTL::Buffer* buffer = device_->newBuffer(
      page_size_,
      MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
  if (!buffer) {
    XELOGE("MetalUploadBufferPool: Failed to allocate {} byte page",
           page_size_);
    return nullptr;
  }
  void* mapping = buffer->contents();
  if (page_created_callback_) {
    page_created_callback_(buffer);
  }
  return new MetalPage(buffer, mapping);
}

uint8_t* MetalUploadBufferPool::Request(uint64_t submission_index, size_t size,
                                        size_t alignment,
                                        MTL::Buffer** buffer_out,
                                        size_t& offset_out,
                                        uint64_t& gpu_address_out) {
  size_t offset;
  const MetalPage* page =
      static_cast<const MetalPage*>(GraphicsUploadBufferPool::Request(
          submission_index, size, alignment, offset));
  if (!page) {
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->buffer_;
  }
  offset_out = offset;
  gpu_address_out = page->gpu_address_ + offset;
  return reinterpret_cast<uint8_t*>(page->mapping_) + offset;
}

uint8_t* MetalUploadBufferPool::RequestPartial(uint64_t submission_index,
                                               size_t size, size_t alignment,
                                               MTL::Buffer** buffer_out,
                                               size_t& offset_out,
                                               uint64_t& gpu_address_out,
                                               size_t& size_out) {
  size_t offset;
  size_t size_obtained;
  const MetalPage* page =
      static_cast<const MetalPage*>(GraphicsUploadBufferPool::RequestPartial(
          submission_index, size, alignment, offset, size_obtained));
  if (!page) {
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->buffer_;
  }
  offset_out = offset;
  size_out = size_obtained;
  gpu_address_out = page->gpu_address_ + offset;
  return reinterpret_cast<uint8_t*>(page->mapping_) + offset;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
