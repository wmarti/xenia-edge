/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shared_memory.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_shared_memory_util.h"

DEFINE_bool(
    metal_shared_memory_direct_write, true,
    "Directly copy safe Metal shared-memory uploads into the shared storage "
    "buffer instead of staging them through a blit.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  // Try to alias guest memory on unified-memory devices and fall back to a
  // dedicated shared buffer when not supported.
  // Initialize base class.
  if (!InitializeCommon()) {
    return false;
  }

  const ui::metal::MetalProvider& provider =
      command_processor_.GetMetalProvider();
  MTL::Device* device = provider.GetDevice();

  if (!device) {
    XELOGE("Metal device is null in MetalSharedMemory::Initialize");
    return false;
  }

  // Create Metal buffer - similar to D3D12's approach
  // On Apple Silicon, ResourceStorageModeShared gives CPU/GPU access
  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    XELOGE("Metal shared memory: Xbox RAM is null");
    return false;
  }

  if (cvars::shared_memory_zero_copy) {
    XELOGW(
        "Metal shared memory: zero-copy is experimental; guest writes are not "
        "versioned against in-flight GPU reads");
    size_t system_page_size = xe::memory::page_size();
    if (reinterpret_cast<uintptr_t>(xbox_ram) % system_page_size == 0) {
      buffer_ = device->newBuffer(xbox_ram, kBufferSize,
                                  MTL::ResourceStorageModeShared, nullptr);
      if (buffer_) {
        use_zero_copy_ = true;
        // The Metal buffer aliases physical_membase_ directly. Guest
        // protection remains tracked in the parent heap page table, but the
        // pinned host view itself must stay writable for Metal and raw device
        // writers until the buffer is released.
        memory().SetPhysicalAliasSkipHostProtect(true);
        XELOGD("Metal shared memory: using bytes-no-copy buffer");
      } else {
        XELOGW("Metal shared memory: bytes-no-copy buffer creation failed");
      }
    } else {
      XELOGW(
          "Metal shared memory: Xbox RAM not page-aligned for bytes-no-copy");
    }
  }

  if (!buffer_) {
    buffer_ = device->newBuffer(kBufferSize, MTL::ResourceStorageModeShared);
  }
  if (!buffer_) {
    XELOGE("Failed to create Metal shared memory buffer");
    return false;
  }

  upload_buffer_pool_ = std::make_unique<MetalUploadBufferPool>(
      device, xe::align(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                        size_t(1) << page_size_log2()));
  const size_t page_count = size_t(kBufferSize) >> page_size_log2();
  page_last_main_gpu_access_submission_.assign(page_count, 0);
  page_standalone_gpu_access_counts_.assign(page_count, 0);
  last_gpu_access_page_first_ = 1;
  last_gpu_access_page_last_ = 0;
  last_gpu_access_submission_ = 0;

  // For trace dump, do initial full copy; UploadRanges handles incremental
  // updates for normal runs.
  if (!use_zero_copy_) {
    if (xbox_ram) {
      memcpy(buffer_->contents(), xbox_ram, kBufferSize);
    }
  } else {
    XELOGD("Metal shared memory: skipping initial copy (zero-copy)");
  }

  return true;
}

void MetalSharedMemory::ClearCache() {
  SharedMemory::ClearCache();
  if (upload_buffer_pool_) {
    upload_buffer_pool_->ClearCache();
  }
}

void MetalSharedMemory::MarkGpuAccess(uint32_t start, uint32_t length,
                                      uint64_t submission_index) {
  if (!length || !submission_index ||
      page_last_main_gpu_access_submission_.empty() || start >= kBufferSize) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  if (end <= start) {
    return;
  }
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  page_last = std::min<uint32_t>(
      page_last,
      static_cast<uint32_t>(page_last_main_gpu_access_submission_.size() - 1));
  if (submission_index == last_gpu_access_submission_ &&
      page_first >= last_gpu_access_page_first_ &&
      page_last <= last_gpu_access_page_last_) {
    return;
  }
  for (uint32_t page = page_first; page <= page_last; ++page) {
    page_last_main_gpu_access_submission_[page] =
        std::max(page_last_main_gpu_access_submission_[page], submission_index);
  }
  last_gpu_access_submission_ = submission_index;
  last_gpu_access_page_first_ = page_first;
  last_gpu_access_page_last_ = page_last;
}

void MetalSharedMemory::TrackStandaloneGpuAccess(
    MTL::CommandBuffer* command_buffer,
    const std::pair<uint32_t, uint32_t>* ranges, uint32_t range_count) {
  if (!command_buffer || !ranges || !range_count ||
      page_standalone_gpu_access_counts_.empty()) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> tracked_page_ranges;
  tracked_page_ranges.reserve(range_count);
  for (uint32_t i = 0; i < range_count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length || start >= kBufferSize) {
      continue;
    }
    uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
    if (end <= start) {
      continue;
    }
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    page_last = std::min<uint32_t>(
        page_last,
        static_cast<uint32_t>(page_standalone_gpu_access_counts_.size() - 1));
    tracked_page_ranges.push_back({page_first, page_last});
  }
  if (tracked_page_ranges.empty()) {
    return;
  }
  bool install_completion_handler = false;
  {
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    auto [access_it, inserted] =
        standalone_gpu_access_page_ranges_.try_emplace(command_buffer);
    install_completion_handler = inserted;
    auto& command_buffer_page_ranges = access_it->second;
    command_buffer_page_ranges.reserve(command_buffer_page_ranges.size() +
                                       tracked_page_ranges.size());
    for (const auto& page_range : tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        ++page_standalone_gpu_access_counts_[page];
      }
      command_buffer_page_ranges.push_back(page_range);
    }
  }
  if (install_completion_handler) {
    command_buffer->addCompletedHandler(
        [this](MTL::CommandBuffer* completed_command_buffer) {
          ReleaseStandaloneGpuAccess(completed_command_buffer);
        });
  }
}

void MetalSharedMemory::CancelStandaloneGpuAccess(
    MTL::CommandBuffer* command_buffer) {
  ReleaseStandaloneGpuAccess(command_buffer);
}

void MetalSharedMemory::ReleaseStandaloneGpuAccess(
    MTL::CommandBuffer* command_buffer) {
  if (!command_buffer) {
    return;
  }
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  auto access_it = standalone_gpu_access_page_ranges_.find(command_buffer);
  if (access_it == standalone_gpu_access_page_ranges_.end()) {
    return;
  }
  for (const auto& page_range : access_it->second) {
    for (uint32_t page = page_range.first; page <= page_range.second; ++page) {
      if (page < page_standalone_gpu_access_counts_.size() &&
          page_standalone_gpu_access_counts_[page]) {
        --page_standalone_gpu_access_counts_[page];
      }
    }
  }
  standalone_gpu_access_page_ranges_.erase(access_it);
}

bool MetalSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  SCOPE_profile_cpu_f("gpu");
  if (!buffer_ || num_upload_ranges == 0) {
    return true;
  }

  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    XELOGE("MetalSharedMemory::UploadRanges: Xbox RAM is null");
    return false;
  }
  uint8_t* xbox_data = static_cast<uint8_t*>(xbox_ram);

  const uint32_t page_size = 1u << page_size_log2();

  if (use_zero_copy_) {
    for (uint32_t i = 0; i < num_upload_ranges; ++i) {
      uint64_t start = uint64_t(upload_page_ranges[i].first) * page_size;
      uint64_t length = uint64_t(upload_page_ranges[i].second) * page_size;
      if (start >= kBufferSize || !length) {
        continue;
      }
      length = std::min<uint64_t>(length, kBufferSize - start);
      trace_writer_.WriteMemoryRead(static_cast<uint32_t>(start),
                                    static_cast<uint32_t>(length));
      MakeRangeValid(static_cast<uint32_t>(start),
                     static_cast<uint32_t>(length), false);
    }
    return true;
  }

  if (!upload_buffer_pool_) {
    XELOGE("MetalSharedMemory::UploadRanges: upload buffer pool is null");
    return false;
  }
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());

  MTL::BlitCommandEncoder* blit_encoder = nullptr;
  auto get_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
    if (!blit_encoder) {
      blit_encoder = command_processor_.GetSharedMemoryUploadBlitEncoder();
    }
    return blit_encoder;
  };

  uint8_t* shared_buffer_contents = static_cast<uint8_t*>(buffer_->contents());
  const bool has_shared_buffer_mapping = shared_buffer_contents != nullptr;
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();

  struct UploadRun {
    uint32_t start;
    uint32_t end;
    bool direct_safe;
  };

  auto append_upload_run = [](std::vector<UploadRun>& runs, uint32_t start,
                              uint32_t end, bool direct_safe) {
    if (end <= start) {
      return;
    }
    if (!runs.empty() && runs.back().direct_safe == direct_safe &&
        runs.back().end == start) {
      runs.back().end = end;
      return;
    }
    runs.push_back({start, end, direct_safe});
  };

  auto build_upload_runs = [&](uint32_t start, uint32_t end,
                               std::vector<UploadRun>& runs) {
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = (end - 1) >> page_size_log2();
    std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint64_t page_end = page_start + page_size;
      uint32_t byte_start =
          static_cast<uint32_t>(std::max<uint64_t>(page_start, start));
      uint32_t byte_end =
          static_cast<uint32_t>(std::min<uint64_t>(page_end, end));
      uint64_t page_last_main_access =
          page < page_last_main_gpu_access_submission_.size()
              ? page_last_main_gpu_access_submission_[page]
              : UINT64_MAX;
      uint32_t standalone_access_count =
          page < page_standalone_gpu_access_counts_.size()
              ? page_standalone_gpu_access_counts_[page]
              : 1;
      append_upload_run(runs, byte_start, byte_end,
                        IsSharedMemoryDirectWriteSafe(
                            ::cvars::metal_shared_memory_direct_write,
                            has_shared_buffer_mapping, page_last_main_access,
                            completed_submission, standalone_access_count));
    }
  };

  auto direct_write_run = [&](uint32_t start, uint32_t end) {
    uint32_t length = end - start;
    MakeRangeValid(start, length, false);
    std::memcpy(shared_buffer_contents + start, xbox_data + start, length);
  };

  auto stage_upload_run = [&](uint32_t start, uint32_t end) -> bool {
    uint32_t offset = start;
    uint32_t remaining = end - start;
    while (remaining) {
      MTL::BlitCommandEncoder* encoder = get_blit_encoder();
      if (!encoder) {
        XELOGE("MetalSharedMemory::UploadRanges: failed to get blit encoder");
        return false;
      }
      MTL::Buffer* upload_buffer = nullptr;
      size_t upload_offset = 0;
      uint64_t upload_gpu_address = 0;
      size_t upload_size = 0;
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), remaining, page_size,
          &upload_buffer, upload_offset, upload_gpu_address, upload_size);
      if (!upload_mapping || !upload_buffer || !upload_size) {
        XELOGE(
            "MetalSharedMemory::UploadRanges: failed to allocate upload "
            "staging buffer");
        return false;
      }
      MakeRangeValid(offset, static_cast<uint32_t>(upload_size), false);
      std::memcpy(upload_mapping, xbox_data + offset, upload_size);
      encoder->copyFromBuffer(upload_buffer, upload_offset, buffer_, offset,
                              upload_size);
      MarkGpuAccess(offset, static_cast<uint32_t>(upload_size),
                    command_processor_.GetCurrentSubmission());
      offset += static_cast<uint32_t>(upload_size);
      remaining -= static_cast<uint32_t>(upload_size);
    }
    return true;
  };

  uint32_t merged_start = 0;
  uint32_t merged_end = 0;
  bool have_merged = false;

  auto flush_merged_range = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    std::vector<UploadRun> runs;
    build_upload_runs(start, end, runs);
    for (const UploadRun& run : runs) {
      if (run.direct_safe) {
        direct_write_run(run.start, run.end);
      } else if (!stage_upload_run(run.start, run.end)) {
        return false;
      }
    }
    return true;
  };

  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    const auto& range = upload_page_ranges[i];
    uint64_t start_64 = uint64_t(range.first) * page_size;
    uint64_t end_64 = start_64 + uint64_t(range.second) * page_size;
    if (start_64 >= kBufferSize) {
      continue;
    }
    uint32_t start = static_cast<uint32_t>(start_64);
    uint32_t end =
        static_cast<uint32_t>(std::min<uint64_t>(end_64, kBufferSize));
    if (start >= kBufferSize) {
      continue;
    }
    trace_writer_.WriteMemoryRead(start, end - start);

    if (!have_merged) {
      merged_start = start;
      merged_end = end;
      have_merged = true;
      continue;
    }

    // Merge overlapping/adjacent ranges.
    if (start <= merged_end) {
      if (end > merged_end) {
        merged_end = end;
      }
    } else {
      if (!flush_merged_range(merged_start, merged_end)) {
        command_processor_.EndSharedMemoryUploadBlitEncoder();
        return false;
      }
      merged_start = start;
      merged_end = end;
    }
  }

  if (have_merged && !flush_merged_range(merged_start, merged_end)) {
    command_processor_.EndSharedMemoryUploadBlitEncoder();
    return false;
  }

  return true;
}

bool MetalSharedMemory::InitializeTraceSubmitDownloads() {
  PrepareForTraceDownload();
  return trace_download_page_count() != 0;
}

void MetalSharedMemory::InitializeTraceCompleteDownloads() {
  if (buffer_) {
    const uint8_t* buffer_data =
        static_cast<const uint8_t*>(buffer_->contents());
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(download_range.first, download_range.second,
                                    buffer_data + download_range.first);
    }
  }
  ReleaseTraceDownloadRanges();
}

void MetalSharedMemory::Shutdown() {
  upload_buffer_pool_.reset();
  page_last_main_gpu_access_submission_.clear();
  {
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    page_standalone_gpu_access_counts_.clear();
    standalone_gpu_access_page_ranges_.clear();
  }
  if (buffer_) {
    buffer_->release();
    buffer_ = nullptr;
  }
  if (use_zero_copy_) {
    // Keep the imported host range writable until Metal has released its
    // bytes-no-copy buffer and no longer owns a pin on those pages.
    memory().SetPhysicalAliasSkipHostProtect(false);
  }
  use_zero_copy_ = false;

  ShutdownCommon();  // Base class cleanup
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
