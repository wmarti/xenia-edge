/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;
class MetalSharedMemory;
class MetalHeapPool;

class MetalTextureCache : public TextureCache {
 public:
  // Upload churn diagnostics. distinct == calls means every upload is a
  // different texture (streaming); distinct << calls means the same textures
  // are being re-uploaded (an invalidation problem).
  uint64_t upload_calls() const { return upload_calls_; }
  uint64_t upload_distinct_keys() const { return upload_key_counts_.size(); }
  uint32_t upload_max_repeats() const { return upload_key_max_repeats_; }

  // Why each upload happened, and whether the GPU resolve that invalidated the
  // texture already wrote exactly the texels the upload is about to rebuild.
  uint64_t upload_origin_gpu_only() const { return upload_origin_gpu_only_; }
  uint64_t upload_origin_cpu_only() const { return upload_origin_cpu_only_; }
  uint64_t upload_origin_both() const { return upload_origin_both_; }
  uint64_t upload_origin_none() const { return upload_origin_none_; }
  uint64_t upload_gpu_no_resolve() const { return upload_gpu_no_resolve_; }
  uint64_t upload_gpu_match() const { return upload_gpu_match_; }
  uint64_t upload_gpu_mismatch_base() const {
    return upload_gpu_mismatch_base_;
  }
  uint64_t upload_gpu_mismatch_format() const {
    return upload_gpu_mismatch_format_;
  }
  uint64_t upload_gpu_mismatch_endian() const {
    return upload_gpu_mismatch_endian_;
  }
  uint64_t upload_gpu_mismatch_pitch() const {
    return upload_gpu_mismatch_pitch_;
  }
  uint64_t upload_gpu_mismatch_dims() const {
    return upload_gpu_mismatch_dims_;
  }
  uint64_t upload_batches_committed() const {
    return upload_batches_committed_;
  }
  uint64_t upload_batches_all_forwardable() const {
    return upload_batches_all_forwardable_;
  }
  uint64_t reval_match() const { return reval_match_; }
  uint64_t reval_mismatch() const { return reval_mismatch_; }
  uint64_t reval_no_hash() const { return reval_no_hash_; }
  uint64_t reval_bytes_hashed() const { return reval_bytes_hashed_; }
  uint64_t upload_batches_all_revalidatable() const {
    return upload_batches_all_revalidatable_;
  }

  // Which command buffer each texture upload landed in. Diagnostic only:
  // widening the upload batch corrupted the image twice, and the remaining
  // suspect is that holding a batch open moves uploads out of the current draw
  // command buffer -- which is only eligible when no render encoder is active
  // -- and into a separately committed one. These say whether that happens.
  uint64_t upload_branch_current_cb() const {
    return upload_branch_current_cb_;
  }
  uint64_t upload_branch_batch() const { return upload_branch_batch_; }
  uint64_t upload_branch_private() const { return upload_branch_private_; }

  MetalTextureCache(MetalCommandProcessor* command_processor,
                    const RegisterFile& register_file,
                    MetalSharedMemory& shared_memory,
                    uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y);
  ~MetalTextureCache();

  bool Initialize();
  void Shutdown();
  void ClearCache() override;
  void CompletedSubmissionUpdated(uint64_t completed_submission_index) override;
  // Called if an uncommitted current command buffer is abandoned, since Metal
  // won't invoke its completion handlers.
  void NotifyCommandBufferDiscarded(MTL::CommandBuffer* command_buffer);

  bool UploadTexture2D(const TextureInfo& texture_info);
  bool UploadTextureCube(const TextureInfo& texture_info);

  // Pixel format conversion
  MTL::PixelFormat ConvertXenosFormat(
      xenos::TextureFormat format,
      xenos::Endian endian = xenos::Endian::k8in32);

  // Null texture accessors for invalid bindings (following D3D12/Vulkan
  // pattern)
  MTL::Texture* GetNullTexture2D() const { return null_texture_2d_; }
  MTL::Texture* GetNullTexture3D() const { return null_texture_3d_; }
  MTL::Texture* GetNullTextureCube() const { return null_texture_cube_; }

  MTL::Texture* GetTextureForBinding(uint32_t fetch_constant,
                                     xenos::FetchOpDimension dimension,
                                     bool is_signed);

  MTL::Texture* RequestSwapTexture(uint32_t& width_scaled_out,
                                   uint32_t& height_scaled_out,
                                   xenos::TextureFormat& format_out);

  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      xenos::BorderColor border_color : 2;
      uint32_t mag_linear : 1;
      uint32_t min_linear : 1;
      uint32_t mip_linear : 1;
      xenos::AnisoFilter aniso_filter : 3;
      uint32_t mip_min_level : 4;
      uint32_t mip_base_map : 1;
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& other) const {
      return value == other.value;
    }
    bool operator!=(const SamplerParameters& other) const {
      return value != other.value;
    }
  };

  SamplerParameters GetSamplerParameters(
      const SpirvShader::SamplerBinding& binding) const;
  MTL::SamplerState* GetOrCreateSampler(SamplerParameters parameters);

  // TextureCache virtual method overrides
  void RequestTextures(uint32_t used_texture_mask) override;

  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool SupportsTextureContentRevalidation() const override { return true; }
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  bool EnsureScaledResolveMemoryCommitted(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) override;
  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                     uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);
  bool GetCurrentScaledResolveBuffer(MTL::Buffer*& buffer_out,
                                     size_t& buffer_offset_out,
                                     size_t& buffer_length_out) const;
  uint64_t GetCurrentScaledResolveRangeStartScaled() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  uint64_t GetCurrentScaledResolveRangeLengthScaled() const {
    return scaled_resolve_current_range_length_scaled_;
  }
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(
      Texture& texture, bool load_base, bool load_mips,
      Texture::CpuLoadSource* base_cpu_source,
      Texture::CpuLoadSource* mips_cpu_source) override;

 private:
  // GPU-based texture loading entry point. Returns true on success.
  bool TryGpuLoadTexture(
      Texture& texture, bool load_base, bool load_mips,
      Texture::CpuLoadSource* base_cpu_source,
      Texture::CpuLoadSource* mips_cpu_source,
      Texture::ContentUploadCompletion& content_upload_completion_out);
  MTL::StorageMode GetCacheTextureStorageMode() const;
  bool ShouldUploadViaBlit() const;
  Texture::ContentUploadCompletion GetContentUploadCompletion(
      MTL::CommandBuffer* command_buffer);
  void FailContentUploadCompletion(MTL::CommandBuffer* command_buffer);
  void BeginUploadCommandBufferBatch();
  void CensusUpload(Texture& texture, bool loaded_base, bool loaded_mips);
  // Creates the batch command buffer on the first upload that wants it, so a
  // request that uploads nothing costs none.
  MTL::CommandBuffer* EnsureUploadCommandBufferBatch();
  void EndUploadCommandBufferBatch();
  void AbortUploadCommandBufferBatch(bool commit_if_has_work = true);

  // Format / load shader mapping for Metal texture loading.
  bool IsDecompressionNeededForKey(TextureKey key) const;
  LoadShaderIndex GetLoadShaderIndexForKey(TextureKey key) const;
  MTL::PixelFormat GetPixelFormatForKey(TextureKey key) const;

  // Initialize GPU texture_load_* pipelines for Metal.
  bool InitializeLoadPipelines();

  struct Norm16Selection {
    bool unsigned_uses_float = false;
    bool signed_uses_float = false;
  };

  void InitializeNorm16Selection(MTL::Device* device);

  // Metal compute pipelines for texture_load_* shaders (unscaled and
  // resolution-scaled variants), indexed by TextureCache::LoadShaderIndex.
  MTL::ComputePipelineState* load_pipelines_[kLoadShaderCount] = {};
  MTL::ComputePipelineState* load_pipelines_scaled_[kLoadShaderCount] = {};

  // Metal-specific Texture implementation

  class MetalTexture : public Texture {
   public:
    MetalTexture(MetalTextureCache& texture_cache, const TextureKey& key,
                 MTL::Texture* metal_texture, bool track_usage = true);
    ~MetalTexture() override;

    MTL::Texture* metal_texture() const { return metal_texture_; }
    MTL::Texture* GetOrCreateView(uint32_t host_swizzle,
                                  xenos::FetchOpDimension dimension,
                                  bool is_signed);
    MTL::Texture* GetOrCreate3DAs2DView(uint32_t host_swizzle,
                                        xenos::FetchOpDimension dimension,
                                        bool is_signed);
    void Invalidate3DAs2DView() { texture_3d_as_2d_.reset(); }

   private:
    MetalTextureCache& texture_cache_;
    MTL::Texture* metal_texture_;
    std::unique_ptr<MetalTexture> texture_3d_as_2d_;
    std::unordered_map<uint64_t, MTL::Texture*> swizzled_view_cache_;
  };

 private:
  // Metal texture creation helpers
  MTL::Texture* CreateTexture2D(uint32_t width, uint32_t height,
                                uint32_t array_length, MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1);
  MTL::Texture* CreateTexture3D(uint32_t width, uint32_t height, uint32_t depth,
                                MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1);
  MTL::Texture* CreateTextureCube(uint32_t width, MTL::PixelFormat format,
                                  MTL::TextureSwizzleChannels swizzle,
                                  uint32_t mip_levels = 1,
                                  uint32_t cube_count = 1);
  void DumpTextureToFile(MTL::Texture* texture, const std::string& filename,
                         uint32_t width, uint32_t height);

  struct ScaledResolveBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t base_scaled = 0;
    uint64_t length_scaled = 0;
  };
  struct RetiredScaledResolveBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t submission_id = 0;
    uint64_t length_scaled = 0;
  };

  bool GetScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                             uint32_t length_scaled_alignment_log2,
                             uint64_t& start_scaled_out,
                             uint64_t& length_scaled_out) const;
  bool IsScaledResolveRangeResident(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2) const;
  bool EnsureScaledResolveBufferRange(uint64_t start_scaled,
                                      uint64_t length_scaled);
  void ClearScaledResolveBuffers();

  // Null texture factory methods (following existing CreateTexture pattern)
  MTL::Texture* CreateNullTexture2D();
  MTL::Texture* CreateNullTexture3D();
  MTL::Texture* CreateNullTextureCube();

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  MetalCommandProcessor* command_processor_;

  // Pre-created null textures for invalid bindings (following existing
  // patterns)
  MTL::Texture* null_texture_2d_ = nullptr;
  MTL::Texture* null_texture_3d_ = nullptr;
  MTL::Texture* null_texture_cube_ = nullptr;

  Norm16Selection r16_selection_;
  Norm16Selection rg16_selection_;
  Norm16Selection rgba16_selection_;

  std::unordered_map<uint32_t, MTL::SamplerState*> sampler_cache_;

  class UploadBufferPool;
  mutable std::mutex upload_buffer_pool_mutex_;
  std::shared_ptr<UploadBufferPool> upload_buffer_pool_;
  // One completion token per in-flight command buffer. Weak ownership keeps
  // discarded buffers from retaining state; textures and the completion
  // handler own a token only when an upload was successfully encoded.
  std::unordered_map<MTL::CommandBuffer*,
                     std::weak_ptr<std::atomic<Texture::ContentUploadStatus>>>
      content_upload_completions_;
  uint32_t content_upload_completion_prune_count_ = 0;
  MTL::CommandBuffer* upload_batch_command_buffer_ = nullptr;
  bool upload_batch_command_buffer_has_work_ = false;
  uint32_t upload_batch_depth_ = 0;
  uint64_t upload_calls_ = 0;
  uint32_t upload_key_max_repeats_ = 0;
  std::unordered_map<TextureKey, uint32_t, TextureKey::Hasher>
      upload_key_counts_;
  uint64_t upload_origin_gpu_only_ = 0;
  uint64_t upload_origin_cpu_only_ = 0;
  uint64_t upload_origin_both_ = 0;
  uint64_t upload_origin_none_ = 0;
  uint64_t upload_gpu_no_resolve_ = 0;
  uint64_t upload_gpu_match_ = 0;
  uint64_t upload_gpu_mismatch_base_ = 0;
  uint64_t upload_gpu_mismatch_format_ = 0;
  uint64_t upload_gpu_mismatch_endian_ = 0;
  uint64_t upload_gpu_mismatch_pitch_ = 0;
  uint64_t upload_gpu_mismatch_dims_ = 0;
  uint64_t upload_batches_committed_ = 0;
  uint64_t upload_batches_all_forwardable_ = 0;
  uint64_t reval_match_ = 0;
  uint64_t reval_mismatch_ = 0;
  uint64_t reval_no_hash_ = 0;
  uint64_t reval_bytes_hashed_ = 0;
  uint64_t upload_batches_all_revalidatable_ = 0;
  bool upload_batch_all_revalidatable_ = true;
  bool upload_batch_all_forwardable_ = true;
  uint64_t upload_branch_current_cb_ = 0;
  uint64_t upload_branch_batch_ = 0;
  uint64_t upload_branch_private_ = 0;
  MetalTexture* bindless_used_first_ = nullptr;
  MetalTexture* bindless_used_last_ = nullptr;
  std::unique_ptr<MetalHeapPool> texture_heap_pool_;
  bool supports_bc_texture_compression_ = false;

  std::vector<ScaledResolveBuffer> scaled_resolve_buffers_;
  std::vector<RetiredScaledResolveBuffer> scaled_resolve_retired_buffers_;
  uint64_t scaled_resolve_retired_bytes_ = 0;
  size_t scaled_resolve_current_buffer_index_ = size_t(-1);
  uint64_t scaled_resolve_current_range_start_scaled_ = 0;
  uint64_t scaled_resolve_current_range_length_scaled_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
