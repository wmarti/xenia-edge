/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/texture_cache.h"

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shared_memory.h"

DEFINE_bool(texture_cache_revalidate_unchanged, true,
            "Skip reloading a texture that a guest write to the same host page "
            "marked outdated when its guest bytes are unchanged. The host page "
            "is larger than the guest page, so most such invalidations are "
            "false sharing. Set false as an emergency compatibility opt-out.",
            "GPU");
DEFINE_bool(texture_cache_revalidate_census, false,
            "Hash the guest bytes behind a texture after a CPU-sourced load "
            "and report how many reloads the contents show to be unnecessary. "
            "Diagnostic only - no reload is actually skipped.",
            "GPU");
DEFINE_uint32(texture_cache_revalidate_size_limit, 1024 * 1024,
              "Largest guest texture range, in bytes, to hash for "
              "texture_cache_revalidate_census or "
              "texture_cache_revalidate_unchanged.",
              "GPU");

namespace {
bool IsTextureContentRevalidationTrackingEnabled() {
  return cvars::texture_cache_revalidate_unchanged ||
         cvars::texture_cache_revalidate_census;
}

}  // namespace

DEFINE_int32(
    draw_resolution_scale_x, 1,
    "Integer pixel width scale used for scaling the rendering resolution "
    "opaquely to the game.\n"
    "Values from 1 to 7 may be supported, depending on device capabilities. "
    "Requires sparse binding (Vulkan) or tiled resources (D3D12) for scales "
    "above 1x1. The emulator will automatically clamp to the maximum supported "
    "scale if the requested value exceeds device limits.\n"
    "Various effects and parts of game rendering pipelines may work "
    "incorrectly as pixels become ambiguous from the game's perspective and "
    "because half-pixel offset (which normally doesn't affect coverage when "
    "MSAA isn't used) becomes full-pixel.",
    "GPU");
DEFINE_int32(
    draw_resolution_scale_y, 1,
    "Integer pixel width scale used for scaling the rendering resolution "
    "opaquely to the game.\n"
    "See draw_resolution_scale_x for more information.",
    "GPU");
DEFINE_uint32(
    texture_cache_memory_limit_soft, 384,
    "Maximum host texture memory usage (in megabytes) above which old textures "
    "will be destroyed.",
    "GPU.Debug");
DEFINE_uint32(
    texture_cache_memory_limit_soft_lifetime, 30,
    "Seconds a texture should be unused to be considered old enough to be "
    "deleted if texture memory usage exceeds texture_cache_memory_limit_soft.",
    "GPU.Debug");
DEFINE_uint32(
    texture_cache_memory_limit_hard, 768,
    "Maximum host texture memory usage (in megabytes) above which textures "
    "will be destroyed as soon as possible.",
    "GPU.Debug");
DEFINE_uint32(
    texture_cache_memory_limit_render_to_texture, 24,
    "Part of the host texture memory budget (in megabytes) that will be scaled "
    "by the current drawing resolution scale.\n"
    "If texture_cache_memory_limit_soft, for instance, is 384, and this is 24, "
    "it will be assumed that the game will be using roughly 24 MB of "
    "render-to-texture (resolve) targets and 384 - 24 = 360 MB of regular "
    "textures - so with 2x2 resolution scaling, the soft limit will be 360 + "
    "96 MB, and with 3x3, it will be 360 + 216 MB.",
    "GPU.Debug");
DEFINE_bool(tiled_shared_memory, true,
            "Enable tiled/sparse resources for efficient large address space "
            "support. Disable for graphics debugger compatibility.",
            "GPU.Debug");

namespace xe {
namespace gpu {

TextureCache::GuestTextureLoadRange TextureCache::GetGuestTextureLoadRange(
    uint32_t guest_address, uint32_t guest_size) {
  if (!guest_size || guest_address >= SharedMemory::kBufferSize) {
    return {};
  }
  const uint64_t aligned_size =
      (uint64_t(guest_size) + UINT64_C(15)) & ~UINT64_C(15);
  if (aligned_size > SharedMemory::kBufferSize - guest_address) {
    return {};
  }
  return {guest_address, uint32_t(aligned_size)};
}

const TextureCache::LoadShaderInfo
    TextureCache::load_shader_info_[kLoadShaderCount] = {
        {1, 4},   // k8bpb
        {2, 4},   // k16bpb
        {4, 3},   // k32bpb
        {8, 2},   // k64bpb
        {16, 1},  // k128bpb
        {2, 4},   // kR5G5B5A1ToB5G5R5A1
        {2, 4},   // kR5G6B5ToB5G6R5
        {2, 4},   // kR5G5B6ToB5G6R5WithRBGASwizzle
        {2, 4},   // kRGBA4ToBGRA4
        {2, 4},   // kRGBA4ToARGB4
        {4, 3},   // kGBGR8ToGRGB8
        {8, 3},   // kGBGR8ToRGB8
        {4, 3},   // kBGRG8ToRGBG8
        {8, 3},   // kBGRG8ToRGB8
        {8, 3},   // kR10G11B11ToRGBA16
        {8, 3},   // kR10G11B11ToRGBA16SNorm
        {8, 3},   // kR11G11B10ToRGBA16
        {8, 3},   // kR11G11B10ToRGBA16SNorm
        {2, 4},   // kR16UNormToFloat
        {2, 4},   // kR16SNormToFloat
        {4, 3},   // kRG16UNormToFloat
        {4, 3},   // kRG16SNormToFloat
        {8, 2},   // kRGBA16UNormToFloat
        {8, 2},   // kRGBA16SNormToFloat
        {4, 2},   // kDXT1ToRGBA8
        {4, 1},   // kDXT3ToRGBA8
        {4, 1},   // kDXT5ToRGBA8
        {2, 1},   // kDXNToRG8
        {1, 2},   // kDXT3A
        {2, 2},   // kDXT3AAs1111ToBGRA4
        {2, 2},   // kDXT3AAs1111ToARGB4
        {1, 2},   // kDXT5AToR8
        {2, 2},   // kCTX1
        {4, 3},   // kDepthUnorm
        {4, 3},   // kDepthFloat
};

TextureCache::TextureCache(const RegisterFile& register_file,
                           SharedMemory& shared_memory,
                           TraceWriter* trace_writer,
                           uint32_t draw_resolution_scale_x,
                           uint32_t draw_resolution_scale_y)
    : register_file_(register_file),
      shared_memory_(shared_memory),
      trace_writer_(trace_writer),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y),
      draw_resolution_scale_x_divisor_(draw_resolution_scale_x),
      draw_resolution_scale_y_divisor_(draw_resolution_scale_y) {
  assert_true(draw_resolution_scale_x >= 1);
  assert_true(draw_resolution_scale_x <= kMaxDrawResolutionScaleAlongAxis);
  assert_true(draw_resolution_scale_y >= 1);
  assert_true(draw_resolution_scale_y <= kMaxDrawResolutionScaleAlongAxis);

  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    constexpr uint32_t kScaledResolvePageDwordCount =
        SharedMemory::kBufferSize / 4096 / 32;
    scaled_resolve_pages_ =
        std::unique_ptr<uint32_t[]>(new uint32_t[kScaledResolvePageDwordCount]);
    std::memset(scaled_resolve_pages_.get(), 0,
                kScaledResolvePageDwordCount * sizeof(uint32_t));
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    scaled_resolve_global_watch_handle_ = shared_memory.RegisterGlobalWatch(
        ScaledResolveGlobalWatchCallbackThunk, this);
  }
}

TextureCache::~TextureCache() {
  DestroyAllTextures(true);

  if (scaled_resolve_global_watch_handle_) {
    shared_memory().UnregisterGlobalWatch(scaled_resolve_global_watch_handle_);
  }
  if (physical_memory_write_tracking_enabled_) {
    shared_memory().memory().DisablePhysicalMemoryWriteTracking();
  }
}

void TextureCache::InitializeTextureContentRevalidation() {
  if (!SupportsTextureContentRevalidation() ||
      !IsTextureContentRevalidationTrackingEnabled()) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  if (!shared_memory().InitializeWriteGenerationTracking(global_lock)) {
    return;
  }
  shared_memory().memory().EnablePhysicalMemoryWriteTracking();
  physical_memory_write_tracking_enabled_ = true;
}

bool TextureCache::GetConfigDrawResolutionScale(uint32_t& x_out,
                                                uint32_t& y_out) {
  uint32_t config_x =
      uint32_t(std::max(INT32_C(1), cvars::draw_resolution_scale_x));
  uint32_t config_y =
      uint32_t(std::max(INT32_C(1), cvars::draw_resolution_scale_y));

  uint32_t clamped_x = std::min(kMaxDrawResolutionScaleAlongAxis, config_x);
  uint32_t clamped_y = std::min(kMaxDrawResolutionScaleAlongAxis, config_y);
  x_out = clamped_x;
  y_out = clamped_y;
  return clamped_x == config_x && clamped_y == config_y;
}

bool TextureCache::ClampDrawResolutionScaleToMaxSupported(
    uint32_t& scale_x, uint32_t& scale_y, bool sparse_bind_supported,
    uint32_t virtual_address_bits_per_resource) {
  // Without sparse/tiled resource support, resolution scaling is not possible
  // because the scaled address space exceeds what simple buffers can handle.
  if (!sparse_bind_supported) {
    bool was_clamped = scale_x > 1 || scale_y > 1;
    scale_x = 1;
    scale_y = 1;
    return !was_clamped;
  }

  // With sparse binding, limit based on virtual address space if specified.
  bool was_clamped = false;
  if (virtual_address_bits_per_resource > 0) {
    while (scale_x > 1 || scale_y > 1) {
      uint64_t highest_scaled_address =
          uint64_t(SharedMemory::kBufferSize) * (scale_x * scale_y) - 1;
      if (uint32_t(64) - xe::lzcnt(highest_scaled_address) <=
          virtual_address_bits_per_resource) {
        break;
      }
      // When reducing from a square size, prefer decreasing the horizontal
      // resolution as vertical resolution difference is visible more clearly in
      // perspective.
      was_clamped = true;
      if (scale_x >= scale_y) {
        --scale_x;
      } else {
        --scale_y;
      }
    }
  }

  return !was_clamped;
}

void TextureCache::ClearCache() { DestroyAllTextures(); }

void TextureCache::CompletedSubmissionUpdated(
    uint64_t completed_submission_index) {
  // If memory usage is too high, destroy unused textures.
  uint64_t current_time = xe::Clock::QueryHostUptimeMillis();
  // texture_cache_memory_limit_render_to_texture is assumed to be included in
  // texture_cache_memory_limit_soft and texture_cache_memory_limit_hard, at 1x,
  // so subtracting 1 from the scale.
  uint32_t limit_scaled_resolve_add_mb =
      cvars::texture_cache_memory_limit_render_to_texture *
      (draw_resolution_scale_x() * draw_resolution_scale_y() - 1);
  uint32_t limit_soft_mb =
      cvars::texture_cache_memory_limit_soft + limit_scaled_resolve_add_mb;
  uint32_t limit_hard_mb =
      cvars::texture_cache_memory_limit_hard + limit_scaled_resolve_add_mb;
  uint32_t limit_soft_lifetime =
      cvars::texture_cache_memory_limit_soft_lifetime * 1000;
  bool destroyed_any = false;
  while (texture_used_first_ != nullptr) {
    uint64_t total_host_memory_usage_mb =
        (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20;
    bool limit_hard_exceeded = total_host_memory_usage_mb > limit_hard_mb;
    if (total_host_memory_usage_mb <= limit_soft_mb && !limit_hard_exceeded) {
      break;
    }
    Texture* texture = texture_used_first_;
    if (texture->last_usage_submission_index() > completed_submission_index) {
      break;
    }
    if (!limit_hard_exceeded &&
        (texture->last_usage_time() + limit_soft_lifetime) > current_time) {
      break;
    }
    if (!destroyed_any) {
      destroyed_any = true;
      // The texture being destroyed might have been bound in the previous
      // submissions, and nothing has overwritten the binding yet, so completion
      // of the submission where the texture was last actually used on the GPU
      // doesn't imply that it's not bound currently. Reset bindings if
      // any texture has been destroyed.
      ResetTextureBindings();
    }
    // Remove the texture from the map and destroy it via its unique_ptr.
    auto found_texture_it = textures_.find(texture->key());
    assert_true(found_texture_it != textures_.end());
    if (found_texture_it != textures_.end()) {
      assert_true(found_texture_it->second.get() == texture);
      textures_.erase(found_texture_it);
      // `texture` is invalid now.
    }
  }
  if (destroyed_any) {
    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  }
}

void TextureCache::BeginSubmission(uint64_t new_submission_index) {
  assert_true(new_submission_index > current_submission_index_);
  current_submission_index_ = new_submission_index;
  current_submission_time_ = xe::Clock::QueryHostUptimeMillis();
}

void TextureCache::BeginFrame() {
  // In case there was a failure to create something in the previous frame, make
  // sure bindings are reset so a new attempt will surely be made if the texture
  // is requested again.
  ResetTextureBindings();
}

void TextureCache::MarkRangeAsResolved(uint32_t start_unscaled,
                                       uint32_t length_unscaled,
                                       bool resolution_scaled) {
  if (length_unscaled == 0) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);

  if (IsDrawResolutionScaled()) {
    uint32_t page_first = start_unscaled >> 12;
    uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
    uint32_t block_first = page_first >> 5;
    uint32_t block_last = page_last >> 5;
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = block_first; i <= block_last; ++i) {
      uint32_t add_bits = UINT32_MAX;
      if (i == block_first) {
        add_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (i == block_last && (page_last & 31) != 31) {
        add_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (resolution_scaled) {
        scaled_resolve_pages_[i] |= add_bits;
        scaled_resolve_pages_l2_[i >> 6] |= UINT64_C(1) << (i & 63);
      } else {
        // Native resolve data is in shared memory.
        // Clear the same way the CPU write watch does.
        scaled_resolve_pages_[i] &= ~add_bits;
        if (!scaled_resolve_pages_[i]) {
          scaled_resolve_pages_l2_[i >> 6] &= ~(UINT64_C(1) << (i & 63));
        }
      }
    }
  }

  // Invalidate textures. Toggling individual textures between scaled and
  // unscaled also relies on invalidation through shared memory.
  shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);
}

uint32_t TextureCache::GuestToHostSwizzle(uint32_t guest_swizzle,
                                          uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest_swizzle_component = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t host_swizzle_component;
    if (guest_swizzle_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      // Get rid of 6 and 7 values (to prevent host GPU errors if the game has
      // something broken) the simple way - by changing them to 4 (0) and 5 (1).
      host_swizzle_component = guest_swizzle_component & 0b101;
    } else {
      host_swizzle_component =
          (host_format_swizzle >> (3 * guest_swizzle_component)) & 0b111;
    }
    host_swizzle |= host_swizzle_component << (3 * i);
  }
  return host_swizzle;
}

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
  const auto& regs = register_file();

  // Preserve invalidations of bindings that aren't used by this draw. They
  // must remain distinguishable from fetch-constant changes until a later draw
  // actually processes the binding. Otherwise an unchanged fetch key could be
  // marked in sync again without reloading its stale texture data.
  const bool textures_became_outdated =
      texture_became_outdated_.load(std::memory_order_acquire) &&
      texture_became_outdated_.exchange(false, std::memory_order_acquire);
  const uint32_t pending_used_bindings =
      texture_bindings_outdated_ & used_texture_mask;
  if (textures_became_outdated || pending_used_bindings) {
    SCOPE_profile_cpu_i("gpu", "TextureCache::InvalidateOutdatedBindings");
    auto global_lock = global_critical_region_.Acquire();
    TryRevalidateUsedOutdatedTextures(global_lock, used_texture_mask);
    if (textures_became_outdated) {
      texture_bindings_outdated_ |= InvalidateOutdatedBindings(global_lock);
    }

    // A binding invalidated while unused may be revalidated only when it is
    // used later. Keep it out of sync so its fetch key is still refreshed, but
    // don't force an upload if its guest bytes proved unchanged.
    uint32_t pending_revalidated = pending_used_bindings;
    uint32_t index = 0;
    while (xe::bit_scan_forward(pending_revalidated, &index)) {
      const uint32_t index_bit = UINT32_C(1) << index;
      pending_revalidated = xe::clear_lowest_bit(pending_revalidated);
      if (!IsBindingOutdatedForUse(global_lock, texture_bindings_[index])) {
        texture_bindings_outdated_ &= ~index_bit;
      }
    }
  }
  const uint32_t bindings_outdated =
      texture_bindings_outdated_ & used_texture_mask;
  texture_bindings_outdated_ &= ~bindings_outdated;

  // Update the texture keys and the textures.
  uint32_t bindings_changed = 0;
  uint32_t textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;
  uint32_t index = 0;

  Texture* textures_to_load[64];  // max bits = 32, can be unsigned + signed
                                  // means max array size = 64
  uint32_t num_textures_to_load = 0;
  {
    SCOPE_profile_cpu_i("gpu", "TextureCache::UpdateBindingKeys");
    while (xe::bit_scan_forward(textures_remaining, &index)) {
      uint32_t index_bit = UINT32_C(1) << index;
      textures_remaining = xe::clear_lowest_bit(textures_remaining);
      TextureBinding& binding = texture_bindings_[index];
      xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
      TextureKey old_key = binding.key;
      uint32_t old_integer_scale_bits = binding.integer_scale_bits;
      uint8_t old_swizzled_signs = binding.swizzled_signs;
      const bool binding_was_outdated =
          old_key.is_valid && (bindings_outdated & index_bit);
      BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);
      texture_bindings_in_sync_ |= index_bit;
      if (!binding.key.is_valid) {
        if (old_key.is_valid) {
          bindings_changed |= index_bit;
        }
        binding.Reset();
        continue;
      }
      uint32_t old_host_swizzle = binding.host_swizzle;
      binding.host_swizzle =
          GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));
      binding.integer_scale_bits =
          GetIntegerScaleBits(fetch.format, fetch.num_format, fetch.swizzle,
                              binding.swizzled_signs);

      // Check if need to load the unsigned and the signed versions of the
      // texture (if the format is emulated with different host bit
      // representations for signed and unsigned - otherwise only the unsigned
      // one is loaded).
      bool key_changed = binding.key != old_key;
      bool any_sign_was_not_signed =
          texture_util::IsAnySignNotSigned(old_swizzled_signs);
      bool any_sign_was_signed =
          texture_util::IsAnySignSigned(old_swizzled_signs);
      bool any_sign_is_not_signed =
          texture_util::IsAnySignNotSigned(binding.swizzled_signs);
      bool any_sign_is_signed =
          texture_util::IsAnySignSigned(binding.swizzled_signs);
      if (key_changed || binding.integer_scale_bits != old_integer_scale_bits ||
          binding.host_swizzle != old_host_swizzle ||
          any_sign_is_not_signed != any_sign_was_not_signed ||
          any_sign_is_signed != any_sign_was_signed) {
        bindings_changed |= index_bit;
      }
      bool load_unsigned_data = false, load_signed_data = false;
      if (IsSignedVersionSeparateForFormat(binding.key)) {
        // Can reuse previously loaded unsigned/signed versions if the key is
        // the same and the texture was previously bound as unsigned/signed
        // respectively (checking the previous values of signedness rather than
        // binding.texture != nullptr and binding.texture_signed != nullptr also
        // prevents repeated attempts to load the texture if it has failed to
        // load).
        if (any_sign_is_not_signed) {
          if (key_changed || !any_sign_was_not_signed) {
            binding.texture = FindOrCreateTexture(binding.key);
            load_unsigned_data = true;
          } else if (binding_was_outdated && binding.texture != nullptr) {
            // Fetch constants unchanged but watched guest memory changed -
            // force a data upload against the same Texture*.
            load_unsigned_data = true;
          }
        } else {
          binding.texture = nullptr;
        }
        if (any_sign_is_signed) {
          if (key_changed || !any_sign_was_signed) {
            TextureKey signed_key = binding.key;
            signed_key.signed_separate = 1;
            binding.texture_signed = FindOrCreateTexture(signed_key);
            load_signed_data = true;
          } else if (binding_was_outdated &&
                     binding.texture_signed != nullptr) {
            load_signed_data = true;
          }
        } else {
          binding.texture_signed = nullptr;
        }
      } else {
        // Same resource for both unsigned and signed, but descriptor formats
        // may be different.
        if (key_changed) {
          binding.texture = FindOrCreateTexture(binding.key);
          load_unsigned_data = true;
        } else if (binding_was_outdated && binding.texture != nullptr) {
          load_unsigned_data = true;
        }
        binding.texture_signed = nullptr;
      }
      if (load_unsigned_data && binding.texture != nullptr) {
        textures_to_load[num_textures_to_load++] = binding.texture;
      }
      if (load_signed_data && binding.texture_signed != nullptr) {
        textures_to_load[num_textures_to_load++] = binding.texture_signed;
      }
    }
  }

  LoadTexturesData(textures_to_load, num_textures_to_load);

  if (num_textures_to_load) {
    // A load can fail before FinalizeLoadAndWatch (for example while making
    // guest memory resident or allocating backend resources). The binding was
    // already marked in sync above, so restore its pending state while its
    // backing texture is still outdated or the failed upload would never be
    // retried with unchanged fetch constants.
    auto global_lock = global_critical_region_.Acquire();
    texture_bindings_outdated_ |= InvalidateOutdatedBindings(global_lock);
  }

  if (bindings_changed) {
    SCOPE_profile_cpu_i("gpu", "TextureCache::UpdateTextureBindings");
    UpdateTextureBindingsImpl(bindings_changed);
  }

  RecordUsedTexturesInTrace(used_texture_mask);
}

void TextureCache::RecordUsedTexturesInTrace(uint32_t used_texture_mask) {
  SCOPE_profile_cpu_f("gpu");
  if (!trace_writer_ || !trace_writer_->is_open()) {
    return;
  }
  // Only ranges the shared memory actually uploads are recorded, so a texture
  // still resident from an earlier frame would never reach the trace. The
  // writer drops ranges it has already written.
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index = 0;
  while (xe::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining = xe::clear_lowest_bit(textures_remaining);
    const TextureBinding& binding = texture_bindings_[index];
    if (!binding.key.is_valid) {
      continue;
    }
    const Texture* texture =
        binding.texture ? binding.texture : binding.texture_signed;
    if (!texture) {
      continue;
    }
    if (binding.key.base_page) {
      const GuestTextureLoadRange range = GetGuestTextureLoadRange(
          binding.key.base_page << 12, texture->GetGuestBaseSize());
      if (range) {
        trace_writer_->WriteMemoryReadCached(range.address, range.length);
      }
    }
    if (binding.key.mip_page) {
      const GuestTextureLoadRange range = GetGuestTextureLoadRange(
          binding.key.mip_page << 12, texture->GetGuestMipsSize());
      if (range) {
        trace_writer_->WriteMemoryReadCached(range.address, range.length);
      }
    }
  }
}

bool TextureCache::AnyUsedTextureRequestWorkPending(
    uint32_t used_texture_mask) {
  if (!used_texture_mask) {
    return false;
  }
  // Any used slot that is out of sync needs work.
  if (used_texture_mask & ~texture_bindings_in_sync_) {
    return true;
  }
  // The aggregate flag is the only state read without the global lock. Watch
  // callbacks publish it after marking a texture outdated. If it is clear,
  // all in-sync bindings are known clean without racing their per-part flags.
  if (!texture_became_outdated_.load(std::memory_order_acquire)) {
    return false;
  }
  // Any in-sync slot whose backing texture data is outdated also needs work.
  auto global_lock = global_critical_region_.Acquire();
  uint32_t used_in_sync = used_texture_mask & texture_bindings_in_sync_;
  uint32_t index = 0;
  while (xe::bit_scan_forward(used_in_sync, &index)) {
    used_in_sync = xe::clear_lowest_bit(used_in_sync);
    const TextureBinding& binding = texture_bindings_[index];
    if (binding.key.is_valid && IsBindingOutdatedForUse(global_lock, binding)) {
      return true;
    }
  }
  return false;
}

bool TextureCache::IsBindingOutdatedForUse(
    const global_unique_lock_type& global_lock,
    const TextureBinding& binding) const {
  assert_true(global_lock.owns_lock());
  auto is_texture_outdated = [&global_lock](const Texture* texture) {
    return texture && (texture->base_outdated(global_lock) ||
                       texture->mips_outdated(global_lock));
  };
  return is_texture_outdated(binding.texture) ||
         is_texture_outdated(binding.texture_signed);
}

// Clears the in-sync bit for every slot whose backing texture data is stale so
// clearing texture_became_outdated_ can't hide an unused stale binding. The
// main RequestTextures loop still reprocesses only slots used by this draw.
uint32_t TextureCache::InvalidateOutdatedBindings(
    const global_unique_lock_type& global_lock) {
  assert_true(global_lock.owns_lock());
  uint32_t invalidated_bindings = 0;
  for (uint32_t index = 0; index < texture_bindings_.size(); ++index) {
    const uint32_t index_bit = UINT32_C(1) << index;
    const TextureBinding& binding = texture_bindings_[index];
    if (binding.key.is_valid && IsBindingOutdatedForUse(global_lock, binding)) {
      texture_bindings_in_sync_ &= ~index_bit;
      invalidated_bindings |= index_bit;
    }
  }
  return invalidated_bindings;
}

const char* TextureCache::TextureKey::GetLogDimensionName(
    xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return "1D";
    case xenos::DataDimension::k2DOrStacked:
      return "2D";
    case xenos::DataDimension::k3D:
      return "3D";
    case xenos::DataDimension::kCube:
      return "cube";
    default:
      assert_unhandled_case(dimension);
      return "unknown";
  }
}

void TextureCache::TextureKey::LogAction(const char* action) const {
  XELOGGPU(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}), mips at 0x{:08X}",
      action, tiled ? "tiled" : "linear", scaled_resolve ? "scaled " : "",
      GetWidth(), GetHeight(), GetDepthOrArraySize(), GetLogDimensionName(),
      FormatInfo::GetName(format), mip_max_level + 1, packed_mips ? "" : "un",
      mip_max_level != 0 ? "s" : "", base_page << 12, pitch << 5,
      mip_page << 12);
}

void TextureCache::Texture::LogAction(const char* action) const {
  XELOGGPU(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}, size 0x{:08X}), mips at 0x{:08X} (size "
      "0x{:08X})",
      action, key_.tiled ? "tiled" : "linear",
      key_.scaled_resolve ? "scaled " : "", key_.GetWidth(), key_.GetHeight(),
      key_.GetDepthOrArraySize(), key_.GetLogDimensionName(),
      FormatInfo::GetName(key_.format), key_.mip_max_level + 1,
      key_.packed_mips ? "" : "un", key_.mip_max_level != 0 ? "s" : "",
      key_.base_page << 12, key_.pitch << 5, GetGuestBaseSize(),
      key_.mip_page << 12, GetGuestMipsSize());
}

// The texture must be in the recent usage list. Place it in front now because
// after creation, the texture will likely be used immediately, and it should
// not be destroyed immediately after creation if dropping of old textures is
// performed somehow. The list is maintained by the Texture, not the
// TextureCache itself (unlike the `textures_` container).
TextureCache::Texture::Texture(TextureCache& texture_cache,
                               const TextureKey& key, bool track_usage)
    : texture_cache_(texture_cache),
      key_(key),
      guest_layout_(key.GetGuestLayout()),
      last_usage_submission_index_(texture_cache.current_submission_index_),
      last_usage_time_(texture_cache.current_submission_time_),
      used_previous_(track_usage ? texture_cache.texture_used_last_ : nullptr),
      used_next_(nullptr),
      in_usage_list_(track_usage) {
  if (track_usage) {
    if (texture_cache.texture_used_last_) {
      texture_cache.texture_used_last_->used_next_ = this;
    } else {
      texture_cache.texture_used_first_ = this;
    }
    texture_cache.texture_used_last_ = this;
  }

  // Never try to upload data that doesn't exist.
  base_outdated_ = guest_layout().base.level_data_extent_bytes != 0;
  mips_outdated_ = guest_layout().mips_total_extent_bytes != 0;
}

TextureCache::Texture::~Texture() {
  if (mips_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(mips_watch_handle_);
  }
  if (base_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(base_watch_handle_);
  }

  // Only remove from usage list if we were added to it (track_usage=true).
  if (in_usage_list_) {
    if (used_previous_) {
      used_previous_->used_next_ = used_next_;
    } else {
      texture_cache_.texture_used_first_ = used_next_;
    }
    if (used_next_) {
      used_next_->used_previous_ = used_previous_;
    } else {
      texture_cache_.texture_used_last_ = used_previous_;
    }
  }

  texture_cache_.UpdateTexturesTotalHostMemoryUsage(0, host_memory_usage_);
}

bool TextureCache::Texture::MakeUpToDateAndWatch(
    const global_unique_lock_type& global_lock, bool loaded_base,
    bool loaded_mips) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  const bool watch_base = loaded_base && base_outdated_;
  const bool watch_mips = loaded_mips && mips_outdated_;
  assert_true(global_lock.owns_lock());
  ++texture_cache().make_up_to_date_calls_;
  const GuestTextureLoadRange base_range =
      GetGuestTextureLoadRange(key().base_page << 12, GetGuestBaseSize());
  const GuestTextureLoadRange mips_range =
      GetGuestTextureLoadRange(key().mip_page << 12, GetGuestMipsSize());
  // Leave a part outdated if its shared-memory source can't be watched yet.
  // FinalizeLoadAndWatch republishes the aggregate outdated signal so a bound
  // texture retries after the source becomes valid.
  if (watch_base &&
      (!base_range ||
       !shared_memory.IsRangeValid(base_range.address, base_range.length))) {
    ++texture_cache().make_up_to_date_base_invalid_;
    return false;
  }
  if (watch_mips &&
      (!mips_range ||
       !shared_memory.IsRangeValid(mips_range.address, mips_range.length))) {
    ++texture_cache().make_up_to_date_mips_invalid_;
    return false;
  }

  MakeLoadedDataUpToDateAndWatch(global_lock, watch_base, watch_mips);
  return true;
}

void TextureCache::Texture::MakeLoadedDataUpToDateAndWatch(
    const global_unique_lock_type& global_lock, bool loaded_base,
    bool loaded_mips) {
  assert_true(global_lock.owns_lock());
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (loaded_base && base_outdated_) {
    assert_not_zero(GetGuestBaseSize());
    const GuestTextureLoadRange range =
        GetGuestTextureLoadRange(key().base_page << 12, GetGuestBaseSize());
    assert_true(bool(range));
    base_watch_handle_ = shared_memory.WatchMemoryRange(
        range.address, GetGuestBaseSize(), TextureCache::WatchCallback, this,
        nullptr, 0);
    base_outdated_ = false;
    base_invalidation_origin_ = 0;
  }
  if (loaded_mips && mips_outdated_) {
    assert_not_zero(GetGuestMipsSize());
    const GuestTextureLoadRange range =
        GetGuestTextureLoadRange(key().mip_page << 12, GetGuestMipsSize());
    assert_true(bool(range));
    mips_watch_handle_ = shared_memory.WatchMemoryRange(
        range.address, GetGuestMipsSize(), TextureCache::WatchCallback, this,
        nullptr, 1);
    mips_outdated_ = false;
    mips_invalidation_origin_ = 0;
  }
}

void TextureCache::Texture::MarkAsUsed() {
  // Textures not in usage tracking (track_usage=false) should not be linked.
  if (!in_usage_list_) {
    return;
  }
  assert_true(last_usage_submission_index_ <=
              texture_cache_.current_submission_index_);
  // This is called very frequently, don't relink unless needed for caching.
  if (last_usage_submission_index_ >=
      texture_cache_.current_submission_index_) {
    return;
  }
  last_usage_submission_index_ = texture_cache_.current_submission_index_;
  last_usage_time_ = texture_cache_.current_submission_time_;
  if (used_next_ == nullptr) {
    // Already the most recently used.
    return;
  }
  if (used_previous_ != nullptr) {
    used_previous_->used_next_ = used_next_;
  } else {
    texture_cache_.texture_used_first_ = used_next_;
  }
  used_next_->used_previous_ = used_previous_;
  used_previous_ = texture_cache_.texture_used_last_;
  used_next_ = nullptr;
  texture_cache_.texture_used_last_->used_next_ = this;
  texture_cache_.texture_used_last_ = this;
}

TextureCache::Texture::CpuLoadSource
TextureCache::Texture::CaptureCpuLoadSource(
    const global_unique_lock_type& global_lock, bool is_mip,
    bool was_fully_invalid_before_request) const {
  assert_true(global_lock.owns_lock());
  CpuLoadSource source;
  if (!texture_cache().SupportsTextureContentRevalidation() ||
      !IsTextureContentRevalidationTrackingEnabled() ||
      !was_fully_invalid_before_request || key().scaled_resolve) {
    return source;
  }
  // The pre-RequestRange scan checks this too, but a GPU write may occur before
  // this later capture lock is acquired. Never identify a mixed/GPU-origin
  // part with a snapshot of guest RAM.
  if ((is_mip ? mips_invalidation_origin_ : base_invalidation_origin_) &
      kInvalidationOriginGpu) {
    return source;
  }
  const uint32_t guest_size = is_mip ? GetGuestMipsSize() : GetGuestBaseSize();
  const uint32_t guest_address = (is_mip ? key().mip_page : key().base_page)
                                 << 12;
  const GuestTextureLoadRange range =
      GetGuestTextureLoadRange(guest_address, guest_size);
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (!range || range.length > cvars::texture_cache_revalidate_size_limit) {
    return source;
  }
  if (shared_memory.memory().IsPhysicalMemoryWriteInProgress(
          global_lock, range.address, range.length)) {
    return source;
  }
  // Guest 0x7F000000-0x7FFFFFFF aliases the first 16 MiB of physical RAM, but
  // isn't one of the physical views on which Memory arms access callbacks.
  // Until that alias is tracked, no content hash from the overlapping physical
  // range can remain valid across arbitrary guest stores.
  constexpr uint32_t kUntrackedPhysicalAliasSize = 0x01000000;
  if (guest_address < kUntrackedPhysicalAliasSize) {
    return source;
  }
  // Access callbacks aren't armed on unallocated physical pages. Refuse to
  // publish a hash there so a later allocation can't make the range writable
  // without invalidating a watch that was never installed.
  BaseHeap* physical_heap = shared_memory.memory().GetPhysicalHeap();
  if (!physical_heap ||
      physical_heap->QueryRangeAccess(global_lock, range.address,
                                      range.address + range.length - 1) ==
          xe::memory::PageAccess::kNoAccess) {
    return source;
  }
  if (!shared_memory.IsRangeValidFromCpu(range.address, range.length)) {
    return source;
  }
  source.invalidation_epoch = shared_memory.GetRangeInvalidationEpoch(
      global_lock, range.address, range.length);
  source.gpu_write_generation = shared_memory.GetRangeGpuWriteGeneration(
      global_lock, range.address, range.length);
  source.byte_length = range.length;
  // Once a counter saturates, another write would no longer change it. This is
  // practically unreachable, but rejecting it keeps the proof valid for
  // arbitrarily long sessions.
  source.valid = source.invalidation_epoch != UINT64_MAX &&
                 source.gpu_write_generation != UINT64_MAX;
  return source;
}

void TextureCache::Texture::SetContentUploadCompletion(
    const global_unique_lock_type& global_lock, bool loaded_base,
    bool loaded_mips, const ContentUploadCompletion& completion) {
  assert_true(global_lock.owns_lock());
  if (loaded_base) {
    base_content_upload_completion_ = completion;
  }
  if (loaded_mips) {
    mips_content_upload_completion_ = completion;
  }
}

bool TextureCache::Texture::TryCompleteContentUpload(
    const ContentUploadCompletion& completion,
    ContentUploadStatus terminal_status) {
  assert_not_null(completion);
  assert_true(terminal_status != ContentUploadStatus::kPending);
  if (!completion || terminal_status == ContentUploadStatus::kPending) {
    return false;
  }
  ContentUploadStatus expected = ContentUploadStatus::kPending;
  return completion->compare_exchange_strong(expected, terminal_status,
                                             std::memory_order_release,
                                             std::memory_order_relaxed);
}

void TextureCache::Texture::StoreCpuContentHashes(
    const global_unique_lock_type& global_lock, bool loaded_base,
    bool loaded_mips, const CpuLoadSource& base_source,
    const CpuLoadSource& mips_source, bool base_source_current,
    bool mips_source_current) {
  assert_true(global_lock.owns_lock());
  const bool tracking_enabled =
      texture_cache().SupportsTextureContentRevalidation() &&
      IsTextureContentRevalidationTrackingEnabled();
  if (!tracking_enabled) {
    if (loaded_base) {
      ClearCpuContentHash(false);
    }
    if (loaded_mips) {
      ClearCpuContentHash(true);
    }
    return;
  }
  if (loaded_base) {
    // IsCpuLoadSourceCurrent already proved the complete source predicate
    // under this continuously-held lock. Publishing must not repeat the same
    // bitmap and generation walks.
    base_content_hash_valid_ = base_source_current;
    if (base_content_hash_valid_) {
      base_content_hash_ = base_source.content_hash;
      base_content_gpu_write_generation_ = base_source.gpu_write_generation;
    } else {
      base_content_upload_completion_.reset();
    }
  }
  if (loaded_mips) {
    mips_content_hash_valid_ = mips_source_current;
    if (mips_content_hash_valid_) {
      mips_content_hash_ = mips_source.content_hash;
      mips_content_gpu_write_generation_ = mips_source.gpu_write_generation;
    } else {
      mips_content_upload_completion_.reset();
    }
  }
}

bool TextureCache::Texture::IsCpuLoadSourceCurrent(
    const global_unique_lock_type& global_lock, bool is_mip,
    const CpuLoadSource& source) const {
  assert_true(global_lock.owns_lock());
  if (!source.valid || !source.content_hash_valid || key().scaled_resolve) {
    return false;
  }
  const uint32_t guest_size = is_mip ? GetGuestMipsSize() : GetGuestBaseSize();
  const uint32_t guest_address = (is_mip ? key().mip_page : key().base_page)
                                 << 12;
  const GuestTextureLoadRange range =
      GetGuestTextureLoadRange(guest_address, guest_size);
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (!range || source.byte_length != range.length ||
      range.length > cvars::texture_cache_revalidate_size_limit ||
      shared_memory.memory().IsPhysicalMemoryWriteInProgress(
          global_lock, range.address, range.length) ||
      !shared_memory.IsRangeValidFromCpu(range.address, range.length) ||
      shared_memory.GetRangeInvalidationEpoch(global_lock, range.address,
                                              range.length) !=
          source.invalidation_epoch ||
      shared_memory.GetRangeGpuWriteGeneration(global_lock, range.address,
                                               range.length) !=
          source.gpu_write_generation) {
    return false;
  }
  return true;
}

bool TextureCache::Texture::FinalizeLoadAndWatch(
    const global_unique_lock_type& global_lock, bool loaded_base,
    bool loaded_mips, const CpuLoadSource& base_source,
    const CpuLoadSource& mips_source) {
  assert_true(global_lock.owns_lock());
  const bool base_snapshotted = loaded_base && base_source.content_hash_valid;
  const bool mips_snapshotted = loaded_mips && mips_source.content_hash_valid;
  const bool base_source_current =
      base_snapshotted &&
      IsCpuLoadSourceCurrent(global_lock, false, base_source);
  const bool mips_source_current =
      mips_snapshotted &&
      IsCpuLoadSourceCurrent(global_lock, true, mips_source);
  const bool finalize_base =
      loaded_base && (!base_snapshotted || base_source_current);
  const bool finalize_mips =
      loaded_mips && (!mips_snapshotted || mips_source_current);
  if (!MakeUpToDateAndWatch(global_lock, finalize_base, finalize_mips)) {
    StoreCpuContentHashes(global_lock, loaded_base, loaded_mips, {}, {}, false,
                          false);
    // RequestTextures marks bindings in sync before loading. Publish the
    // remaining outdated state so a failed publication is retried even when no
    // Texture watch existed during the upload (including on first use).
    texture_cache().texture_became_outdated_.store(true,
                                                   std::memory_order_release);
    return false;
  }
  StoreCpuContentHashes(global_lock, loaded_base, loaded_mips, base_source,
                        mips_source, base_source_current, mips_source_current);
  const bool source_current = (!base_snapshotted || base_source_current) &&
                              (!mips_snapshotted || mips_source_current);
  if (!source_current) {
    texture_cache().texture_became_outdated_.store(true,
                                                   std::memory_order_release);
  }
  return source_current;
}

void TextureCache::Texture::ClearCpuContentHash(bool is_mip) {
  if (is_mip) {
    mips_content_hash_valid_ = false;
    mips_content_upload_completion_.reset();
  } else {
    base_content_hash_valid_ = false;
    base_content_upload_completion_.reset();
  }
}

uint32_t TextureCache::Texture::GetInvalidationOrigin(
    const global_unique_lock_type& global_lock, bool loaded_base,
    bool loaded_mips) const {
  assert_true(global_lock.owns_lock());
  uint32_t origin = 0;
  if (loaded_base) {
    origin |= base_invalidation_origin_;
  }
  if (loaded_mips) {
    origin |= mips_invalidation_origin_;
  }
  return origin;
}

bool TextureCache::Texture::CpuContentUnchanged(
    const global_unique_lock_type& global_lock, bool check_base,
    bool check_mips, uint64_t& bytes_hashed_out, bool& eligible_out) const {
  assert_true(global_lock.owns_lock());
  eligible_out = false;
  if (!texture_cache().SupportsTextureContentRevalidation()) {
    return false;
  }
  SharedMemory& shared_memory = texture_cache().shared_memory();
  Memory& memory = shared_memory.memory();
  const uint32_t size_limit = cvars::texture_cache_revalidate_size_limit;
  const uint32_t base_address = key().base_page << 12;
  const uint32_t mips_address = key().mip_page << 12;
  GuestTextureLoadRange base_range;
  GuestTextureLoadRange mips_range;

  // Preflight every requested part before hashing any of them, so a missing
  // hash or intervening GPU write is reported as ineligible rather than as a
  // content mismatch after another part happened to be hashed.
  if (check_base && base_outdated_) {
    const uint32_t base_size = GetGuestBaseSize();
    if (!base_content_hash_valid_ ||
        (base_content_upload_completion_ &&
         base_content_upload_completion_->load(std::memory_order_acquire) !=
             ContentUploadStatus::kSucceeded)) {
      return false;
    }
    base_range = GetGuestTextureLoadRange(base_address, base_size);
    if (!base_range || base_range.length > size_limit ||
        shared_memory.GetRangeGpuWriteGeneration(
            global_lock, base_range.address, base_range.length) !=
            base_content_gpu_write_generation_) {
      return false;
    }
  }
  if (check_mips && mips_outdated_) {
    const uint32_t mips_size = GetGuestMipsSize();
    if (!mips_content_hash_valid_ ||
        (mips_content_upload_completion_ &&
         mips_content_upload_completion_->load(std::memory_order_acquire) !=
             ContentUploadStatus::kSucceeded)) {
      return false;
    }
    mips_range = GetGuestTextureLoadRange(mips_address, mips_size);
    if (!mips_range || mips_range.length > size_limit ||
        shared_memory.GetRangeGpuWriteGeneration(
            global_lock, mips_range.address, mips_range.length) !=
            mips_content_gpu_write_generation_) {
      return false;
    }
  }
  if (!base_range && !mips_range) {
    return false;
  }
  if ((base_range && memory.IsPhysicalMemoryWriteInProgress(
                         global_lock, base_range.address, base_range.length)) ||
      (mips_range && memory.IsPhysicalMemoryWriteInProgress(
                         global_lock, mips_range.address, mips_range.length))) {
    return false;
  }
  eligible_out = true;

  // The CPU watch was removed by the invalidation being measured. Protect
  // again before reading so the observe-only census can't hash mutable RAM.
  if (base_range) {
    shared_memory.WatchRangeForCpuWrites(base_range.address, base_range.length);
  }
  if (mips_range) {
    shared_memory.WatchRangeForCpuWrites(mips_range.address, mips_range.length);
  }
  if (base_range) {
    bytes_hashed_out += base_range.length;
    if (XXH3_64bits(memory.TranslatePhysical(base_range.address),
                    base_range.length) != base_content_hash_) {
      return false;
    }
  }
  if (mips_range) {
    bytes_hashed_out += mips_range.length;
    if (XXH3_64bits(memory.TranslatePhysical(mips_range.address),
                    mips_range.length) != mips_content_hash_) {
      return false;
    }
  }
  return true;
}

bool TextureCache::Texture::TryRevalidateCpuInvalidation(
    const global_unique_lock_type& global_lock) {
  assert_true(global_lock.owns_lock());
  if (!texture_cache().SupportsTextureContentRevalidation() ||
      !cvars::texture_cache_revalidate_unchanged || key().scaled_resolve) {
    return !base_outdated_ && !mips_outdated_;
  }
  const uint32_t size_limit = cvars::texture_cache_revalidate_size_limit;
  bool check_base = base_outdated_ && base_content_hash_valid_;
  bool check_mips = mips_outdated_ && mips_content_hash_valid_;
  if (check_base && base_content_upload_completion_ &&
      base_content_upload_completion_->load(std::memory_order_acquire) !=
          ContentUploadStatus::kSucceeded) {
    ClearCpuContentHash(false);
    check_base = false;
  }
  if (check_mips && mips_content_upload_completion_ &&
      mips_content_upload_completion_->load(std::memory_order_acquire) !=
          ContentUploadStatus::kSucceeded) {
    ClearCpuContentHash(true);
    check_mips = false;
  }
  const uint32_t base_address = key().base_page << 12;
  const uint32_t mips_address = key().mip_page << 12;
  const GuestTextureLoadRange base_range =
      GetGuestTextureLoadRange(base_address, GetGuestBaseSize());
  const GuestTextureLoadRange mips_range =
      GetGuestTextureLoadRange(mips_address, GetGuestMipsSize());
  if (check_base && (!base_range || base_range.length > size_limit)) {
    base_content_hash_valid_ = false;
    check_base = false;
  }
  if (check_mips && (!mips_range || mips_range.length > size_limit)) {
    mips_content_hash_valid_ = false;
    check_mips = false;
  }
  if (!check_base && !check_mips) {
    return !base_outdated_ && !mips_outdated_;
  }
  SharedMemory& shared_memory = texture_cache().shared_memory();
  Memory& memory = shared_memory.memory();
  if (check_base) {
    if (shared_memory.GetRangeGpuWriteGeneration(
            global_lock, base_range.address, base_range.length) !=
        base_content_gpu_write_generation_) {
      base_content_hash_valid_ = false;
      check_base = false;
    }
  }
  if (check_mips) {
    if (shared_memory.GetRangeGpuWriteGeneration(
            global_lock, mips_range.address, mips_range.length) !=
        mips_content_gpu_write_generation_) {
      mips_content_hash_valid_ = false;
      check_mips = false;
    }
  }
  // Raw host writers don't fault on the protected physical aliases. Their
  // write scopes make the interval visible under this lock; leave the stored
  // hash intact so the texture can be retried after the writer publishes its
  // normal physical-memory invalidation.
  if (check_base && memory.IsPhysicalMemoryWriteInProgress(
                        global_lock, base_range.address, base_range.length)) {
    check_base = false;
  }
  if (check_mips && memory.IsPhysicalMemoryWriteInProgress(
                        global_lock, mips_range.address, mips_range.length)) {
    check_mips = false;
  }
  if (!check_base && !check_mips) {
    return !base_outdated_ && !mips_outdated_;
  }
  // Protect before hashing, not after. A write that lands while the hash is
  // being taken then traps and blocks on this lock, and fires the watch again
  // once it is released, instead of being lost between the two.
  if (check_base) {
    shared_memory.WatchRangeForCpuWrites(base_range.address, base_range.length);
  }
  if (check_mips) {
    shared_memory.WatchRangeForCpuWrites(mips_range.address, mips_range.length);
  }
  bool base_match = false;
  if (check_base) {
    texture_cache().revalidation_bytes_hashed_ += base_range.length;
    base_match = XXH3_64bits(memory.TranslatePhysical(base_range.address),
                             base_range.length) == base_content_hash_;
    if (!base_match) {
      // Genuinely modified - don't rehash it on later attempts; the next
      // CPU-sourced load stores a fresh hash.
      base_content_hash_valid_ = false;
    }
  }
  bool mips_match = false;
  if (check_mips) {
    texture_cache().revalidation_bytes_hashed_ += mips_range.length;
    mips_match = XXH3_64bits(memory.TranslatePhysical(mips_range.address),
                             mips_range.length) == mips_content_hash_;
    if (!mips_match) {
      mips_content_hash_valid_ = false;
    }
  }
  if (!base_match && !mips_match) {
    return !base_outdated_ && !mips_outdated_;
  }
  MakeLoadedDataUpToDateAndWatch(global_lock, base_match, mips_match);
  if (base_match) {
    ++texture_cache().revalidated_base_;
  }
  if (mips_match) {
    ++texture_cache().revalidated_mips_;
  }
  return !base_outdated_ && !mips_outdated_;
}

void TextureCache::TryRevalidateUsedOutdatedTextures(
    const global_unique_lock_type& global_lock, uint32_t used_texture_mask) {
  assert_true(global_lock.owns_lock());
  if (!SupportsTextureContentRevalidation() ||
      !cvars::texture_cache_revalidate_unchanged) {
    return;
  }
  uint32_t remaining_bits = used_texture_mask;
  uint32_t index = 0;
  while (xe::bit_scan_forward(remaining_bits, &index)) {
    remaining_bits = xe::clear_lowest_bit(remaining_bits);
    const TextureBinding& binding = texture_bindings_[index];
    if (!binding.key.is_valid) {
      continue;
    }
    for (Texture* texture : {binding.texture, binding.texture_signed}) {
      if (texture) {
        // A texture bound twice is fine - the second attempt sees the flags
        // already cleared or the hash already dropped.
        texture->TryRevalidateCpuInvalidation(global_lock);
      }
    }
  }
}

void TextureCache::Texture::WatchCallback(
    const global_unique_lock_type& global_lock, bool is_mip,
    bool invalidated_by_gpu) {
  assert_true(global_lock.owns_lock());
  const uint32_t origin =
      invalidated_by_gpu ? kInvalidationOriginGpu : kInvalidationOriginCpu;
  if (is_mip) {
    assert_not_zero(GetGuestMipsSize());
    mips_invalidation_origin_ |= origin;
    if (invalidated_by_gpu ||
        (mips_content_upload_completion_ &&
         mips_content_upload_completion_->load(std::memory_order_acquire) !=
             ContentUploadStatus::kSucceeded)) {
      ClearCpuContentHash(true);
    }
    mips_outdated_ = true;
    mips_watch_handle_ = nullptr;
  } else {
    assert_not_zero(GetGuestBaseSize());
    base_invalidation_origin_ |= origin;
    if (invalidated_by_gpu ||
        (base_content_upload_completion_ &&
         base_content_upload_completion_->load(std::memory_order_acquire) !=
             ContentUploadStatus::kSucceeded)) {
      ClearCpuContentHash(false);
    }
    base_outdated_ = true;
    base_watch_handle_ = nullptr;
  }
}

void TextureCache::WatchCallback(const global_unique_lock_type& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  Texture& texture = *static_cast<Texture*>(context);
  texture.texture_cache().watch_callbacks_.fetch_add(1,
                                                     std::memory_order_relaxed);
  texture.WatchCallback(global_lock, argument != 0, invalidated_by_gpu);
  texture.texture_cache().texture_became_outdated_.store(
      true, std::memory_order_release);
}

void TextureCache::DestroyAllTextures(bool from_destructor) {
  ResetTextureBindings(from_destructor);
  textures_.clear();
  COUNT_profile_set("gpu/texture_cache/textures", 0);
}

TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {
  SCOPE_profile_cpu_f("gpu");
  // Check if the texture is a scaled resolve texture.
  if (IsDrawResolutionScaled() && key.tiled &&
      IsScaledResolveSupportedForFormat(key)) {
    texture_util::TextureGuestLayout scaled_resolve_guest_layout =
        key.GetGuestLayout();
    if ((scaled_resolve_guest_layout.base.level_data_extent_bytes &&
         IsRangeScaledResolved(
             key.base_page << 12,
             scaled_resolve_guest_layout.base.level_data_extent_bytes)) ||
        (scaled_resolve_guest_layout.mips_total_extent_bytes &&
         IsRangeScaledResolved(
             key.mip_page << 12,
             scaled_resolve_guest_layout.mips_total_extent_bytes))) {
      key.scaled_resolve = 1;
    }
  }

  uint32_t host_width = key.GetWidth();
  uint32_t host_height = key.GetHeight();
  if (key.scaled_resolve) {
    host_width *= draw_resolution_scale_x();
    host_height *= draw_resolution_scale_y();
  }
  // With 3x resolution scaling, a 2D texture may become bigger than the
  // Direct3D 11 limit, and with 2x, a 3D one as well.
  // TODO(Triang3l): Skip mips on Vulkan in this case - the minimum requirement
  // there is 4096, which is below the Xenos maximum texture size of 8192.
  uint32_t max_host_width_height = GetMaxHostTextureWidthHeight(key.dimension);
  uint32_t max_host_depth_or_array_size =
      GetMaxHostTextureDepthOrArraySize(key.dimension);
  if (host_width > max_host_width_height ||
      host_height > max_host_width_height ||
      key.GetDepthOrArraySize() > max_host_depth_or_array_size) {
    return nullptr;
  }

  // Try to find an existing texture.
  // TODO(Triang3l): Reuse a texture with mip_page unchanged, but base_page
  // previously 0, now not 0, to save memory - common case in streaming.
  auto found_texture_it = textures_.find(key);
  if (found_texture_it != textures_.end()) {
    return found_texture_it->second.get();
  }

  // Create the texture and add it to the map.
  Texture* texture;
  {
    std::unique_ptr<Texture> new_texture = CreateTexture(key);
    if (!new_texture) {
      key.LogAction("Failed to create");
      return nullptr;
    }
    assert_true(new_texture->key() == key);
    texture =
        textures_.emplace(key, std::move(new_texture)).first->second.get();
  }
  COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  texture->LogAction("Created");
  return texture;
}

// Packs the integer scale the fetch shader reads from the system constant to
// undo the host sampler's normalization - the guest wants e.g. [0, 255], not
// [0, 1]. 6 bits per output component: bits 0:3 = width - 1, bit 4 = signed,
// bit 5 = unsigned-biased. The scale lands after swizzling, so each output lane
// walks the guest swizzle back to its source component's width. component_bits
// only describes the stored width, so the source component is clamped to the
// last stored channel the same way the host swizzle expands the formats.
// (k_16 has a 16 bit width in all four components, k_5_6_5 gives blue in W.)
// Constant (0/1) lanes, gamma, and non-fixed formats have nothing to rescale
// and stay 0.
uint32_t TextureCache::GetIntegerScaleBits(xenos::TextureFormat guest_format,
                                           uint32_t num_format,
                                           uint32_t guest_swizzle,
                                           uint8_t swizzled_signs) {
  // num_format 0 is the normalized/fractional fetch - nothing to rescale.
  const FormatInfo& format_info = *FormatInfo::Get(guest_format);
  uint32_t scale_bits = 0;

  if (!num_format || !format_info.fixed) {
    return 0;
  }

  uint32_t last_stored_component = 0;
  for (uint32_t i = 1; i < 4; ++i) {
    if (format_info.component_bits[i]) {
      last_stored_component = i;
    }
  }

  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t source_component = (guest_swizzle >> (i * 3)) & 0b111;
    if (source_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      continue;
    }
    source_component = std::min(source_component, last_stored_component);

    xenos::TextureSign sign =
        xenos::TextureSign((swizzled_signs >> (i * 2)) & 0b11);

    uint8_t width = format_info.component_bits[source_component];
    if (!width || width > 16 || sign == xenos::TextureSign::kGamma) {
      continue;
    }

    uint32_t component_scale = uint32_t(width - 1);
    if (sign == xenos::TextureSign::kSigned) {
      component_scale |= UINT32_C(1) << 4;
      // Unsigned-biased: halve the scaled value and apply an extra offset.
    } else if (sign == xenos::TextureSign::kUnsignedBiased) {
      component_scale |= UINT32_C(1) << 5;
    }

    scale_bits |= component_scale << (i * 6);
  }

  return scale_bits;
}

void TextureCache::LoadTexturesData(Texture** textures, uint32_t n_textures) {
  SCOPE_profile_cpu_f("gpu");
  assert_true(n_textures <= 64);
  if (n_textures < 2) {
    if (!n_textures) {
      return;
    } else {
      LoadTextureData(*textures[0]);
      return;
    }
  }

  const bool track_content = SupportsTextureContentRevalidation() &&
                             IsTextureContentRevalidationTrackingEnabled();
  uint64_t index_base_outdated = 0;
  uint64_t index_mips_outdated = 0;
  uint64_t index_base_cpu_source = 0;
  uint64_t index_mips_cpu_source = 0;
  GuestTextureLoadRange base_ranges[64] = {};
  GuestTextureLoadRange mips_ranges[64] = {};
  Texture::CpuLoadSource sources[128] = {};
  Texture::CpuLoadSource* base_sources = nullptr;
  Texture::CpuLoadSource* mips_sources = nullptr;
  if (track_content) {
    base_sources = sources;
    mips_sources = base_sources + n_textures;
  }
  uint32_t nkept = 0;
  {
    SCOPE_profile_cpu_i("gpu", "TextureCache::OutdatedScanUnderLock");
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = 0; i < n_textures; ++i) {
      Texture* current = textures[i];

      // RequestTextures may have found an already-cached texture after the
      // binding-level prepass. Give that texture the same revalidation chance
      // as the single-texture path before deciding what to upload.
      if (track_content) {
        current->TryRevalidateCpuInvalidation(global_lock);
      }

      auto base_outdated = current->base_outdated(global_lock);
      auto mips_outdated = current->mips_outdated(global_lock);

      index_base_outdated |= static_cast<uint64_t>(base_outdated) << i;
      index_mips_outdated |= static_cast<uint64_t>(mips_outdated) << i;
      const TextureKey& key = current->key();
      base_ranges[i] = GetGuestTextureLoadRange(key.base_page << 12,
                                                current->GetGuestBaseSize());
      mips_ranges[i] = GetGuestTextureLoadRange(key.mip_page << 12,
                                                current->GetGuestMipsSize());
      if (track_content && !key.scaled_resolve) {
        if (base_outdated && base_ranges[i] &&
            !current->HasGpuInvalidationOrigin(global_lock, false) &&
            shared_memory().IsRangeInvalid(base_ranges[i].address,
                                           base_ranges[i].length)) {
          index_base_cpu_source |= UINT64_C(1) << i;
        }
        if (mips_outdated && mips_ranges[i] &&
            !current->HasGpuInvalidationOrigin(global_lock, true) &&
            shared_memory().IsRangeInvalid(mips_ranges[i].address,
                                           mips_ranges[i].length)) {
          index_mips_cpu_source |= UINT64_C(1) << i;
        }
      }
      if (!base_outdated && !mips_outdated) {
        textures[i] = nullptr;

      } else {
        nkept++;
      }
    }
  }

  if (nkept == 0) {
    return;
  }

  for (uint32_t i = 0; i < n_textures; ++i) {
    Texture* p_texture = textures[i];
    if (!p_texture) {
      continue;
    }
    textures[i] = nullptr;
    Texture& texture = *p_texture;

    TextureKey texture_key = texture.key();
    // Implementation may load multiple blocks at once via accesses of up to 128
    // bits (R32G32B32A32_UINT), so aligning the size to this value to make sure
    // if the texture is small (especially if it's linear), the last blocks
    // won't be cut off (hosts may return 0, 0, 0, 0 for the whole
    // R32G32B32A32_UINT access for the non-16-aligned tail even if 1...15 bytes
    // are actually provided for it).

    // Request uploading of the texture data to the shared memory.
    // This is also necessary when resolution scaling is used - the texture
    // cache relies on shared memory for invalidation of both unscaled and
    // scaled textures. Plus a texture may be unscaled partially, when only a
    // portion of its pages is invalidated, in this case we'll need the texture
    // from the shared memory to load the unscaled parts.
    // TODO(Triang3l): Load unscaled parts.
    {
      SCOPE_profile_cpu_i("gpu", "TextureCache::RequestGuestRanges");
      if (index_base_outdated & (1ULL << i)) {
        if (!base_ranges[i] ||
            !shared_memory().RequestRange(base_ranges[i].address,
                                          base_ranges[i].length)) {
          continue;
        }
      }
      if (index_mips_outdated & (1ULL << i)) {
        if (!mips_ranges[i] ||
            !shared_memory().RequestRange(mips_ranges[i].address,
                                          mips_ranges[i].length)) {
          continue;
        }
      }
    }
    if ((index_base_cpu_source | index_mips_cpu_source) & (UINT64_C(1) << i)) {
      auto global_lock = global_critical_region_.Acquire();
      base_sources[i] = texture.CaptureCpuLoadSource(
          global_lock, false,
          (index_base_cpu_source & (UINT64_C(1) << i)) != 0);
      mips_sources[i] = texture.CaptureCpuLoadSource(
          global_lock, true, (index_mips_cpu_source & (UINT64_C(1) << i)) != 0);
    }
    if (texture_key.scaled_resolve) {
      // Make sure all the scaled resolve memory is resident and accessible from
      // the shader, including any possible padding that hasn't yet been touched
      // by an actual resolve, but is still included in the texture size, so the
      // GPU won't be trying to access unmapped memory.
      if (!EnsureScaledResolveMemoryCommitted(texture_key.base_page << 12,
                                              texture.GetGuestBaseSize(), 4)) {
        continue;
      }
      if (!EnsureScaledResolveMemoryCommitted(texture_key.mip_page << 12,
                                              texture.GetGuestMipsSize(), 4)) {
        continue;
      }
    }

    // Actually load the texture data.
    if (!LoadTextureDataFromResidentMemoryImpl(
            texture, (index_base_outdated & (1ULL << i)) != 0,
            (index_mips_outdated & (1ULL << i)) != 0,
            (index_base_cpu_source & (UINT64_C(1) << i)) &&
                    base_sources[i].valid
                ? &base_sources[i]
                : nullptr,
            (index_mips_cpu_source & (UINT64_C(1) << i)) &&
                    mips_sources[i].valid
                ? &mips_sources[i]
                : nullptr)) {
      continue;
    }

    // Requeue for MakeUpToDateAndWatch.
    textures[i] = &texture;
  }
  {
    auto crit = global_critical_region_.Acquire();

    for (uint32_t i = 0; i < n_textures; ++i) {
      auto texture = textures[i];
      if (!texture) {
        continue;
      }
      // Mark the ranges as uploaded and watch them. This is needed for scaled
      // resolves as well to detect when the CPU wants to reuse the memory for a
      // regular texture or a vertex buffer, and thus the scaled resolve version
      // is not up to date anymore.
      const bool loaded_base = (index_base_outdated & (UINT64_C(1) << i)) != 0;
      const bool loaded_mips = (index_mips_outdated & (UINT64_C(1) << i)) != 0;
      const Texture::CpuLoadSource& base_source =
          index_base_cpu_source & (UINT64_C(1) << i) ? base_sources[i]
                                                     : Texture::CpuLoadSource{};
      const Texture::CpuLoadSource& mips_source =
          index_mips_cpu_source & (UINT64_C(1) << i) ? mips_sources[i]
                                                     : Texture::CpuLoadSource{};
      if (!texture->FinalizeLoadAndWatch(crit, loaded_base, loaded_mips,
                                         base_source, mips_source)) {
        continue;
      }

      texture->LogAction("Loaded");
    }
  }
}
bool TextureCache::LoadTextureData(Texture& texture) {
  SCOPE_profile_cpu_f("gpu");
  // Check what needs to be uploaded.
  bool base_outdated, mips_outdated;
  bool base_cpu_source = false;
  bool mips_cpu_source = false;
  Texture::CpuLoadSource base_source;
  Texture::CpuLoadSource mips_source;
  GuestTextureLoadRange base_range;
  GuestTextureLoadRange mips_range;
  const bool track_content = SupportsTextureContentRevalidation() &&
                             IsTextureContentRevalidationTrackingEnabled();
  {
    auto global_lock = global_critical_region_.Acquire();
    if (track_content) {
      texture.TryRevalidateCpuInvalidation(global_lock);
    }
    base_outdated = texture.base_outdated(global_lock);
    mips_outdated = texture.mips_outdated(global_lock);
    const TextureKey& key = texture.key();
    base_range = GetGuestTextureLoadRange(key.base_page << 12,
                                          texture.GetGuestBaseSize());
    mips_range = GetGuestTextureLoadRange(key.mip_page << 12,
                                          texture.GetGuestMipsSize());
    if (track_content && !key.scaled_resolve) {
      base_cpu_source =
          base_outdated && base_range &&
          !texture.HasGpuInvalidationOrigin(global_lock, false) &&
          shared_memory().IsRangeInvalid(base_range.address, base_range.length);
      mips_cpu_source =
          mips_outdated && mips_range &&
          !texture.HasGpuInvalidationOrigin(global_lock, true) &&
          shared_memory().IsRangeInvalid(mips_range.address, mips_range.length);
    }
  }
  if (!base_outdated && !mips_outdated) {
    return true;
  }

  TextureKey texture_key = texture.key();

  // Implementation may load multiple blocks at once via accesses of up to 128
  // bits (R32G32B32A32_UINT), so aligning the size to this value to make sure
  // if the texture is small (especially if it's linear), the last blocks won't
  // be cut off (hosts may return 0, 0, 0, 0 for the whole R32G32B32A32_UINT
  // access for the non-16-aligned tail even if 1...15 bytes are actually
  // provided for it).

  // Request uploading of the texture data to the shared memory.
  // This is also necessary when resolution scaling is used - the texture cache
  // relies on shared memory for invalidation of both unscaled and scaled
  // textures. Plus a texture may be unscaled partially, when only a portion of
  // its pages is invalidated, in this case we'll need the texture from the
  // shared memory to load the unscaled parts.
  // TODO(Triang3l): Load unscaled parts.
  if (base_outdated) {
    if (!base_range ||
        !shared_memory().RequestRange(base_range.address, base_range.length)) {
      return false;
    }
  }
  if (mips_outdated) {
    if (!mips_range ||
        !shared_memory().RequestRange(mips_range.address, mips_range.length)) {
      return false;
    }
  }
  if (base_cpu_source || mips_cpu_source) {
    auto global_lock = global_critical_region_.Acquire();
    base_source =
        texture.CaptureCpuLoadSource(global_lock, false, base_cpu_source);
    mips_source =
        texture.CaptureCpuLoadSource(global_lock, true, mips_cpu_source);
  }
  if (texture_key.scaled_resolve) {
    // Make sure all the scaled resolve memory is resident and accessible from
    // the shader, including any possible padding that hasn't yet been touched
    // by an actual resolve, but is still included in the texture size, so the
    // GPU won't be trying to access unmapped memory.
    if (!EnsureScaledResolveMemoryCommitted(texture_key.base_page << 12,
                                            texture.GetGuestBaseSize(), 4)) {
      return false;
    }
    if (!EnsureScaledResolveMemoryCommitted(texture_key.mip_page << 12,
                                            texture.GetGuestMipsSize(), 4)) {
      return false;
    }
  }

  // Actually load the texture data.
  if (!LoadTextureDataFromResidentMemoryImpl(
          texture, base_outdated, mips_outdated,
          base_source.valid ? &base_source : nullptr,
          mips_source.valid ? &mips_source : nullptr)) {
    return false;
  }

  // Mark the ranges as uploaded and watch them. This is needed for scaled
  // resolves as well to detect when the CPU wants to reuse the memory for a
  // regular texture or a vertex buffer, and thus the scaled resolve version is
  // not up to date anymore.
  {
    auto global_lock = global_critical_region_.Acquire();
    if (!texture.FinalizeLoadAndWatch(global_lock, base_outdated, mips_outdated,
                                      base_source, mips_source)) {
      return false;
    }
  }

  texture.LogAction("Loaded");

  return true;
}

void TextureCache::BindingInfoFromFetchConstant(
    const xenos::xe_gpu_texture_fetch_t& fetch, TextureKey& key_out,
    uint8_t* swizzled_signs_out) {
  // Reset the key and the signedness.
  key_out.MakeInvalid();
  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out =
        uint8_t(xenos::TextureSign::kUnsigned) * uint8_t(0b01010101);
  }

  switch (fetch.type) {
    case xenos::FetchConstantType::kTexture:
      break;
    case xenos::FetchConstantType::kInvalidTexture:
      if (cvars::gpu_allow_invalid_fetch_constants) {
        break;
      }
      XELOGW(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "has \"invalid\" type! This is incorrect behavior, but you can try "
          "bypassing this by launching Xenia with "
          "--gpu_allow_invalid_fetch_constants=true.",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
          fetch.dword_4, fetch.dword_5);
      return;
    default:
      XELOGW(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "is completely invalid!",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
          fetch.dword_4, fetch.dword_5);
      return;
  }

  uint32_t width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  uint32_t base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width_minus_1, &height_minus_1, &depth_or_array_size_minus_1,
      &base_page, &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    // No texture data at all.
    return;
  }
  uint32_t pitch = fetch.pitch;
  if (fetch.dimension == xenos::DataDimension::k1D) {
    bool is_invalid_1d = false;
    // Handle wide 1D textures (> 8192 wide) by mapping them to a 2D grid.
    // The shader will convert 1D coordinates to 2D using the original width
    // from the fetch constant.
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      uint32_t total_width = width_minus_1 + 1;
      uint32_t row_width = xenos::kTexture2DCubeMaxWidthHeight;
      uint32_t num_rows = (total_width + row_width - 1) / row_width;
      // Cap the materialized rows - huge widths are index-space declarations
      // with little real data behind them, and the full extent may run past
      // the 512 MB physical space (making the load fail entirely). Kept in
      // sync with the shader-side row cap.
      num_rows = std::min(num_rows, xenos::kTexture1DWideMaxRows);
      width_minus_1 = row_width - 1;
      height_minus_1 = num_rows - 1;
      // Disable mipmaps for wide 1D textures. The shader's coordinate remapping
      // assumes base-level dimensions (num_rows), but at mip level N, the 2D
      // texture becomes (8192 >> N) x (num_rows >> N), which breaks the mapping
      // when num_rows >> N becomes 1 while the shader still expects multiple
      // rows. Mipmaps are rarely used with 1D lookup textures anyway.
      mip_max_level = 0;
      // The guest pitch is meaningless for a texture the guest believes is 1D
      // (the 9 bit field couldn't even express the line width).
      pitch = xenos::kTexture2DCubeMaxWidthHeight >> 5;
    }
    assert_false(fetch.tiled);
    if (fetch.tiled) {
      XELOGE(
          "1D texture has tiling enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    assert_false(fetch.packed_mips);
    if (fetch.packed_mips) {
      XELOGE(
          "1D texture has packed mips enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    if (is_invalid_1d) {
      return;
    }
  }

  xenos::TextureFormat format = GetBaseFormat(fetch.format);

  key_out.base_page = base_page;
  key_out.mip_page = mip_page;
  key_out.dimension = fetch.dimension;
  key_out.width_minus_1 = width_minus_1;
  key_out.height_minus_1 = height_minus_1;
  key_out.depth_or_array_size_minus_1 = depth_or_array_size_minus_1;
  key_out.pitch = pitch;
  key_out.mip_max_level = mip_max_level;
  key_out.tiled = fetch.tiled;
  key_out.packed_mips = fetch.packed_mips;
  key_out.format = format;
  key_out.endianness = fetch.endianness;

  key_out.is_valid = 1;

  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = texture_util::SwizzleSigns(fetch);
  }
}

void TextureCache::ResetTextureBindings(bool from_destructor) {
  uint32_t bindings_reset = 0;
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    TextureBinding& binding = texture_bindings_[i];
    if (!binding.key.is_valid) {
      continue;
    }
    binding.Reset();
    bindings_reset |= UINT32_C(1) << i;
  }
  texture_bindings_in_sync_ &= ~bindings_reset;
  texture_bindings_outdated_ &= ~bindings_reset;
  if (!from_destructor && bindings_reset) {
    UpdateTextureBindingsImpl(bindings_reset);
  }
}

void TextureCache::UpdateTexturesTotalHostMemoryUsage(uint64_t add,
                                                      uint64_t subtract) {
  textures_total_host_memory_usage_ =
      textures_total_host_memory_usage_ - subtract + add;
  COUNT_profile_set("gpu/texture_cache/total_host_memory_usage_mb",
                    uint32_t((textures_total_host_memory_usage_ +
                              ((UINT32_C(1) << 20) - 1)) >>
                             20));
}

bool TextureCache::IsRangeScaledResolved(uint32_t start_unscaled,
                                         uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  start_unscaled = std::min(start_unscaled, SharedMemory::kBufferSize);
  length_unscaled =
      std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  if (!length_unscaled) {
    return false;
  }

  // Two-level check for faster rejection since resolve targets are usually
  // placed in relatively small and localized memory portions (confirmed by
  // testing - pretty much all times the deeper level was entered, the texture
  // was a resolve target).
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  uint32_t block_first = page_first >> 5;
  uint32_t block_last = page_last >> 5;
  uint32_t l2_block_first = block_first >> 6;
  uint32_t l2_block_last = block_last >> 6;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = l2_block_first; i <= l2_block_last; ++i) {
    uint64_t l2_block = scaled_resolve_pages_l2_[i];
    if (i == l2_block_first) {
      l2_block &= ~((UINT64_C(1) << (block_first & 63)) - 1);
    }
    if (i == l2_block_last && (block_last & 63) != 63) {
      l2_block &= (UINT64_C(1) << ((block_last & 63) + 1)) - 1;
    }
    uint32_t block_relative_index;
    while (xe::bit_scan_forward(l2_block, &block_relative_index)) {
      l2_block &= ~(UINT64_C(1) << block_relative_index);
      uint32_t block_index = (i << 6) + block_relative_index;
      uint32_t check_bits = UINT32_MAX;
      if (block_index == block_first) {
        check_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (block_index == block_last && (page_last & 31) != 31) {
        check_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (scaled_resolve_pages_[block_index] & check_bits) {
        return true;
      }
    }
  }
  return false;
}

void TextureCache::ScaledResolveGlobalWatchCallbackThunk(
    const global_unique_lock_type& global_lock, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  TextureCache* texture_cache = reinterpret_cast<TextureCache*>(context);
  texture_cache->ScaledResolveGlobalWatchCallback(
      global_lock, address_first, address_last, invalidated_by_gpu);
}

void TextureCache::ScaledResolveGlobalWatchCallback(
    const global_unique_lock_type& global_lock, uint32_t address_first,
    uint32_t address_last, bool invalidated_by_gpu) {
  assert_true(IsDrawResolutionScaled());
  if (invalidated_by_gpu) {
    // Resolves themselves do exactly the opposite of what this should do.
    return;
  }
  // Mark scaled resolve ranges as non-scaled. Textures themselves will be
  // invalidated by their shared memory watches.
  uint32_t resolve_page_first = address_first >> 12;
  uint32_t resolve_page_last = address_last >> 12;
  uint32_t resolve_block_first = resolve_page_first >> 5;
  uint32_t resolve_block_last = resolve_page_last >> 5;
  uint32_t resolve_l2_block_first = resolve_block_first >> 6;
  uint32_t resolve_l2_block_last = resolve_block_last >> 6;
  for (uint32_t i = resolve_l2_block_first; i <= resolve_l2_block_last; ++i) {
    uint64_t resolve_l2_block = scaled_resolve_pages_l2_[i];
    // Pre-mask to only process blocks within the write range.
    if (i == resolve_l2_block_first) {
      resolve_l2_block &= ~((UINT64_C(1) << (resolve_block_first & 63)) - 1);
    }
    if (i == resolve_l2_block_last && (resolve_block_last & 63) != 63) {
      resolve_l2_block &= (UINT64_C(1) << ((resolve_block_last & 63) + 1)) - 1;
    }
    uint32_t resolve_block_relative_index;
    while (
        xe::bit_scan_forward(resolve_l2_block, &resolve_block_relative_index)) {
      resolve_l2_block &= ~(UINT64_C(1) << resolve_block_relative_index);
      uint32_t resolve_block_index = (i << 6) + resolve_block_relative_index;
      uint32_t resolve_keep_bits = 0;
      if (resolve_block_index == resolve_block_first) {
        resolve_keep_bits |= (UINT32_C(1) << (resolve_page_first & 31)) - 1;
      }
      if (resolve_block_index == resolve_block_last &&
          (resolve_page_last & 31) != 31) {
        resolve_keep_bits |=
            ~((UINT32_C(1) << ((resolve_page_last & 31) + 1)) - 1);
      }
      scaled_resolve_pages_[resolve_block_index] &= resolve_keep_bits;
      if (scaled_resolve_pages_[resolve_block_index] == 0) {
        scaled_resolve_pages_l2_[i] &=
            ~(UINT64_C(1) << resolve_block_relative_index);
      }
    }
  }
}

}  // namespace gpu
}  // namespace xe
