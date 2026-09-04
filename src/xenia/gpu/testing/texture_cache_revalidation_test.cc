/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/memory.h"

DECLARE_bool(texture_cache_revalidate_unchanged);
DECLARE_bool(texture_cache_revalidate_census);
DECLARE_uint32(texture_cache_revalidate_size_limit);

namespace xe {
namespace gpu {
namespace test {

namespace {

constexpr uint32_t kGuestPhysicalAddress = 0x01000000;
constexpr uint32_t kAllocationSize = 0x10000;

class ScopedTextureRevalidationConfig {
 public:
  ScopedTextureRevalidationConfig()
      : old_revalidate_unchanged_(cvars::texture_cache_revalidate_unchanged),
        old_revalidate_census_(cvars::texture_cache_revalidate_census),
        old_size_limit_(cvars::texture_cache_revalidate_size_limit) {
    cvars::texture_cache_revalidate_unchanged = true;
    cvars::texture_cache_revalidate_census = false;
    cvars::texture_cache_revalidate_size_limit = kAllocationSize;
  }

  ~ScopedTextureRevalidationConfig() {
    cvars::texture_cache_revalidate_unchanged = old_revalidate_unchanged_;
    cvars::texture_cache_revalidate_census = old_revalidate_census_;
    cvars::texture_cache_revalidate_size_limit = old_size_limit_;
  }

 private:
  bool old_revalidate_unchanged_;
  bool old_revalidate_census_;
  uint32_t old_size_limit_;
};

class TestSharedMemory final : public SharedMemory {
 public:
  explicit TestSharedMemory(Memory& memory) : SharedMemory(memory) {}

  bool Initialize() { return InitializeCommon(); }

  void MakeCpuRangeValid(uint32_t start, uint32_t length) {
    MakeRangeValid(start, length, false);
  }

 protected:
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_upload_ranges) override {
    for (uint32_t i = 0; i < num_upload_ranges; ++i) {
      MakeRangeValid(upload_page_ranges[i].first << page_size_log2(),
                     upload_page_ranges[i].second << page_size_log2(), false);
    }
    return true;
  }
};

class TestTextureCache final : public TextureCache {
 public:
  using TestTextureKey = TextureKey;
  using TestContentUploadStatus = Texture::ContentUploadStatus;
  using TestContentUploadCompletion = Texture::ContentUploadCompletion;

  class TestTexture final : public Texture {
   public:
    TestTexture(TestTextureCache& texture_cache, const TextureKey& key)
        : Texture(texture_cache, key, false) {}
  };

  TestTextureCache(const RegisterFile& register_file,
                   SharedMemory& shared_memory,
                   bool supports_content_revalidation = true)
      : TextureCache(register_file, shared_memory, nullptr, 1, 1),
        supports_content_revalidation_(supports_content_revalidation) {
    InitializeTextureContentRevalidation();
  }

  bool SupportsTextureContentRevalidation() const override {
    return supports_content_revalidation_;
  }

  static TestTextureKey MakeLinear8BitTextureKey(
      uint32_t height, uint32_t guest_address = kGuestPhysicalAddress) {
    TestTextureKey key;
    key.base_page = guest_address >> 12;
    key.dimension = xenos::DataDimension::k2DOrStacked;
    key.width_minus_1 = 0;
    key.height_minus_1 = height - 1;
    key.depth_or_array_size_minus_1 = 0;
    key.pitch = 1;
    key.format = xenos::TextureFormat::k_8;
    key.endianness = xenos::Endian::kNone;
    key.is_valid = 1;
    return key;
  }

  static TestTextureKey MakeLinear8BitTextureKeyWithMips(
      uint32_t guest_address = kGuestPhysicalAddress) {
    TestTextureKey key = MakeLinear8BitTextureKey(8, guest_address);
    key.width_minus_1 = 7;
    key.mip_page = (guest_address + 0x1000) >> 12;
    key.mip_max_level = 1;
    return key;
  }

  static bool TryCompleteContentUpload(
      const TestContentUploadCompletion& completion,
      TestContentUploadStatus terminal_status) {
    return Texture::TryCompleteContentUpload(completion, terminal_status);
  }

  static uint32_t GetGuestTextureLoadRangeLengthForTest(uint32_t guest_address,
                                                        uint32_t guest_size) {
    return GetGuestTextureLoadRange(guest_address, guest_size).length;
  }

  std::unique_ptr<TestTexture> CreateStandaloneTexture(
      const TestTextureKey& key) {
    return std::make_unique<TestTexture>(*this, key);
  }

  bool Load(TestTexture& texture) { return LoadTextureData(texture); }

  bool TryRevalidate(TestTexture& texture) {
    auto global_lock = AcquireGlobalLock();
    return texture.TryRevalidateCpuInvalidation(global_lock);
  }

  void SetContentUploadCompletion(
      TestTexture& texture,
      const TestContentUploadCompletion& upload_completion,
      bool loaded_base = true, bool loaded_mips = false) {
    auto global_lock = AcquireGlobalLock();
    texture.SetContentUploadCompletion(global_lock, loaded_base, loaded_mips,
                                       upload_completion);
  }

  bool CanCaptureCpuLoadSource(TestTexture& texture, bool is_mip = false) {
    auto global_lock = AcquireGlobalLock();
    return texture.CaptureCpuLoadSource(global_lock, is_mip, true).valid;
  }

  void SetLoadHook(std::function<void()> load_hook) {
    load_hook_ = std::move(load_hook);
  }

  void SetSnapshotHashXor(uint64_t snapshot_hash_xor) {
    snapshot_hash_xor_ = snapshot_hash_xor;
  }

  void FailNextLoad() { next_load_succeeds_ = false; }

  void Request(uint32_t used_texture_mask) {
    RequestTextures(used_texture_mask);
  }

  uint32_t load_count() const { return load_count_; }
  bool last_load_base() const { return last_load_base_; }
  bool last_load_mips() const { return last_load_mips_; }

  uint32_t GetHostFormatSwizzle(TextureKey key) const override {
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
  }

  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override {
    return 8192;
  }

  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override {
    return 2048;
  }

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }

  bool LoadTextureDataFromResidentMemoryImpl(
      Texture& texture, bool load_base, bool load_mips,
      Texture::CpuLoadSource* base_cpu_source,
      Texture::CpuLoadSource* mips_cpu_source) override {
    auto snapshot_source = [&](Texture::CpuLoadSource* source,
                               uint32_t guest_address) {
      if (!source || !source->valid || !source->byte_length) {
        return;
      }
      source->content_hash =
          XXH3_64bits(shared_memory().memory().TranslatePhysical(guest_address),
                      source->byte_length) ^
          snapshot_hash_xor_;
      source->content_hash_valid = true;
    };
    snapshot_source(base_cpu_source, texture.key().base_page << 12);
    snapshot_source(mips_cpu_source, texture.key().mip_page << 12);
    ++load_count_;
    last_load_base_ = load_base;
    last_load_mips_ = load_mips;
    if (load_hook_) {
      std::function<void()> load_hook = std::move(load_hook_);
      load_hook_ = nullptr;
      load_hook();
    }
    const bool load_succeeded = next_load_succeeds_;
    next_load_succeeds_ = true;
    return load_succeeded;
  }

 private:
  bool supports_content_revalidation_;
  std::function<void()> load_hook_;
  uint64_t snapshot_hash_xor_ = 0;
  bool next_load_succeeds_ = true;
  uint32_t load_count_ = 0;
  bool last_load_base_ = false;
  bool last_load_mips_ = false;
};

class TextureRevalidationFixture {
 public:
  bool Initialize(uint32_t texture_height = 1,
                  bool supports_content_revalidation = true,
                  bool with_mips = false, bool allocate_guest_memory = true,
                  uint32_t guest_physical_address = kGuestPhysicalAddress) {
    guest_physical_address_ = guest_physical_address;
    if (!memory_.Initialize()) {
      return false;
    }
    if (allocate_guest_memory && !AllocateGuestMemory()) {
      return false;
    }

    shared_memory_ = std::make_unique<TestSharedMemory>(memory_);
    if (!shared_memory_->Initialize()) {
      return false;
    }
    texture_cache_ = std::make_unique<TestTextureCache>(
        register_file_, *shared_memory_, supports_content_revalidation);
    texture_ = texture_cache_->CreateStandaloneTexture(
        with_mips ? TestTextureCache::MakeLinear8BitTextureKeyWithMips(
                        guest_physical_address_)
                  : TestTextureCache::MakeLinear8BitTextureKey(
                        texture_height, guest_physical_address_));
    return true;
  }

  bool AllocateGuestMemory() {
    const uint32_t window_address =
        UINT32_C(0xA0000000) + guest_physical_address_;
    BaseHeap* heap = memory_.LookupHeap(window_address);
    if (!heap ||
        !heap->AllocFixed(window_address, kAllocationSize, kAllocationSize,
                          kMemoryAllocationReserve | kMemoryAllocationCommit,
                          kMemoryProtectRead | kMemoryProtectWrite)) {
      return false;
    }
    std::memset(memory_.TranslatePhysical(guest_physical_address_), 0x5A,
                kAllocationSize);
    return true;
  }

  TestSharedMemory& shared_memory() { return *shared_memory_; }
  TestTextureCache& texture_cache() { return *texture_cache_; }
  TestTextureCache::TestTexture& texture() { return *texture_; }

  void WriteGuestByte(uint32_t offset, uint8_t value) {
    memory_.TranslatePhysical<uint8_t*>(guest_physical_address_)[offset] =
        value;
  }

  bool ProtectGuestMemoryReadOnly() {
    const uint32_t window_address =
        UINT32_C(0xA0000000) + guest_physical_address_;
    BaseHeap* heap = memory_.LookupHeap(window_address);
    return heap &&
           heap->Protect(window_address, kAllocationSize, kMemoryProtectRead);
  }

  void SetPhysicalAliasSkipHostProtect(bool skip) {
    memory_.SetPhysicalAliasSkipHostProtect(skip);
  }

  Memory::PhysicalMemoryWriteScope BeginRawPhysicalMemoryWrite(
      uint32_t offset, uint32_t length) {
    return memory_.BeginPhysicalMemoryWrite(guest_physical_address_ + offset,
                                            length);
  }

  bool TriggerCpuWrite(uint32_t offset) {
    return TriggerCpuWriteRange(offset, 1);
  }

  bool TriggerCpuWriteRange(uint32_t offset, uint32_t length) {
    return memory_.TriggerPhysicalMemoryCallbacks(
        global_critical_region::AcquireDirect(),
        UINT32_C(0xA0000000) + guest_physical_address_ + offset, length, true,
        true);
  }

  uint32_t guest_physical_address() const { return guest_physical_address_; }

  bool TryRevalidate() { return texture_cache_->TryRevalidate(*texture_); }

  bool CanCaptureCpuLoadSource() {
    return texture_cache_->CanCaptureCpuLoadSource(*texture_);
  }

  bool LoadTexture() { return texture_cache_->Load(*texture_); }

  void SetLinear8BitTextureFetch(uint32_t index, uint32_t guest_address) {
    xenos::xe_gpu_texture_fetch_t fetch = {};
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.pitch = 1;
    fetch.format = xenos::TextureFormat::k_8;
    fetch.base_address = guest_address >> 12;
    fetch.size_2d.width = 0;
    fetch.size_2d.height = 0;
    fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    std::memcpy(
        &register_file_.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                               (sizeof(fetch) / sizeof(uint32_t)) * index],
        &fetch, sizeof(fetch));
  }

  void RequestTextures(uint32_t used_texture_mask) {
    texture_cache_->Request(used_texture_mask);
  }

  void SetContentUploadCompletion(
      const TestTextureCache::TestContentUploadCompletion& upload_completion,
      bool loaded_base = true, bool loaded_mips = false) {
    texture_cache_->SetContentUploadCompletion(*texture_, upload_completion,
                                               loaded_base, loaded_mips);
  }

  void SetLoadHook(std::function<void()> load_hook) {
    texture_cache_->SetLoadHook(std::move(load_hook));
  }

  void FailNextLoad() { texture_cache_->FailNextLoad(); }

  void SetSnapshotHashXor(uint64_t snapshot_hash_xor) {
    texture_cache_->SetSnapshotHashXor(snapshot_hash_xor);
  }

 private:
  Memory memory_;
  RegisterFile register_file_;
  std::unique_ptr<TestSharedMemory> shared_memory_;
  std::unique_ptr<TestTextureCache> texture_cache_;
  std::unique_ptr<TestTextureCache::TestTexture> texture_;
  uint32_t guest_physical_address_ = kGuestPhysicalAddress;
};

}  // namespace

TEST_CASE("texture revalidation skips false sharing and rearms the CPU watch",
          "[texture-revalidation]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.texture().GetGuestBaseSize() == 1);

  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE(fixture.texture_cache().watch_callbacks() == 1);
  REQUIRE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().revalidated_base() == 1);
  REQUIRE_FALSE(fixture.shared_memory().IsRangeValid(kGuestPhysicalAddress,
                                                     uint32_t(16)));
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);

  // Revalidation must have rearmed both the physical-memory callback and the
  // derived texture watch even though the shared-memory page stayed invalid.
  fixture.WriteGuestByte(48, 0x7C);
  REQUIRE(fixture.TriggerCpuWrite(48));
  REQUIRE(fixture.texture_cache().watch_callbacks() == 2);
  REQUIRE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().revalidated_base() == 2);
  REQUIRE_FALSE(fixture.shared_memory().IsRangeValid(kGuestPhysicalAddress,
                                                     uint32_t(16)));
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);
}

TEST_CASE("texture revalidation requires an opted-in backend",
          "[texture-revalidation][backend-capability]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize(1, false));
  REQUIRE(fixture.LoadTexture());

  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation reloads genuinely changed guest bytes",
          "[texture-revalidation]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.LoadTexture());

  fixture.WriteGuestByte(0, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(0));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation rejects an upload pending at CPU invalidation",
          "[texture-revalidation][upload-completion]") {
  using ContentUploadStatus = TestTextureCache::TestContentUploadStatus;
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.LoadTexture());
  auto completion = std::make_shared<std::atomic<ContentUploadStatus>>(
      ContentUploadStatus::kPending);
  fixture.SetContentUploadCompletion(completion);

  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  completion->store(ContentUploadStatus::kSucceeded, std::memory_order_release);

  // Success after invalidation is too late: the upload may have consumed the
  // old bytes, so this hash must already have been made ineligible.
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation rejects a failed content upload",
          "[texture-revalidation][upload-completion]") {
  using ContentUploadStatus = TestTextureCache::TestContentUploadStatus;
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.LoadTexture());
  auto completion = std::make_shared<std::atomic<ContentUploadStatus>>(
      ContentUploadStatus::kFailed);
  fixture.SetContentUploadCompletion(completion);

  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation accepts a completed content upload",
          "[texture-revalidation][upload-completion]") {
  using ContentUploadStatus = TestTextureCache::TestContentUploadStatus;
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.LoadTexture());
  auto completion = std::make_shared<std::atomic<ContentUploadStatus>>(
      ContentUploadStatus::kSucceeded);
  fixture.SetContentUploadCompletion(completion);

  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);
}

TEST_CASE("discarded texture upload completion is terminal",
          "[texture-revalidation][upload-completion]") {
  using ContentUploadStatus = TestTextureCache::TestContentUploadStatus;
  auto completion = std::make_shared<std::atomic<ContentUploadStatus>>(
      ContentUploadStatus::kPending);

  REQUIRE(TestTextureCache::TryCompleteContentUpload(
      completion, ContentUploadStatus::kFailed));
  // A late Metal completion handler must not revive a command buffer that the
  // command processor already discarded.
  REQUIRE_FALSE(TestTextureCache::TryCompleteContentUpload(
      completion, ContentUploadStatus::kSucceeded));
  REQUIRE(completion->load(std::memory_order_acquire) ==
          ContentUploadStatus::kFailed);
}

TEST_CASE("texture revalidation remembers a watchless GPU write",
          "[texture-revalidation]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.LoadTexture());

  // False sharing removes the texture watch while retaining its CPU-content
  // hash. A subsequent GPU write must remain visible even though there is no
  // texture watch left to record its origin or clear the hash. Clear the
  // current GPU-written bit with another CPU invalidation too, proving the
  // persistent generation - rather than only current validity - closes this
  // sequence.
  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  fixture.shared_memory().RangeWrittenByGpu(kGuestPhysicalAddress, 16);
  fixture.WriteGuestByte(48, 0x7C);
  REQUIRE(fixture.TriggerCpuWrite(48));

  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation rejects false sharing during snapshot upload",
          "[texture-revalidation][snapshot]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  fixture.SetLoadHook([&fixture]() {
    fixture.WriteGuestByte(32, 0x6B);
    REQUIRE(fixture.TriggerCpuWrite(32));
    REQUIRE(fixture.shared_memory().RequestRange(kGuestPhysicalAddress, 16));
  });
  // RequestRange restored current validity after the neighboring write, but the
  // persistent invalidation epoch must still conservatively reject the content
  // hash of the in-flight snapshot. The upload itself happened, so the texture
  // is up to date and the next invalidation reloads it.
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.WriteGuestByte(32, 0x8D);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation rejects changed bytes after snapshot",
          "[texture-revalidation][snapshot]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  fixture.SetLoadHook([&fixture]() {
    fixture.WriteGuestByte(0, 0x6B);
    REQUIRE(fixture.TriggerCpuWrite(0));
    REQUIRE(fixture.shared_memory().RequestRange(
        fixture.guest_physical_address(), 16));
  });

  // The backend uploaded the pre-write snapshot. The persistent invalidation
  // epoch must withhold its content hash even though RequestRange made the
  // shared-memory page valid again before publication.
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.WriteGuestByte(0, 0x7C);
  REQUIRE(fixture.TriggerCpuWrite(0));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture upload publishes when its snapshot proof fails",
          "[texture-revalidation][snapshot]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  fixture.SetLoadHook([&fixture]() {
    fixture.WriteGuestByte(32, 0x6B);
    REQUIRE(fixture.TriggerCpuWrite(32));
    REQUIRE(fixture.shared_memory().RequestRange(kGuestPhysicalAddress, 16));
  });

  // The bytes were uploaded, so the part must leave the outdated state even
  // though its source can no longer be proven unchanged. Otherwise every draw
  // re-uploads it for as long as the neighboring writes keep coming.
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);
  REQUIRE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().watch_callbacks() == 0);

  // The watch was armed by the same transition, so this write reaches the
  // texture.
  fixture.WriteGuestByte(32, 0x8D);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE(fixture.texture_cache().watch_callbacks() == 1);

  // Only the content hash was withheld, so the invalidation reloads instead of
  // revalidating bytes no hash covers.
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().revalidated_base() == 0);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture binding stops reloading a snapshot rejected at publication",
          "[texture-revalidation][bindings][snapshot]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  // Make the bound texture genuinely outdated so RequestTextures schedules a
  // replacement upload against the unchanged fetch key.
  fixture.WriteGuestByte(0, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(0));

  // Race another write with the replacement snapshot. RequestRange restores
  // current shared-memory validity, but the advanced invalidation epoch makes
  // FinalizeLoadAndWatch withhold the content hash.
  fixture.SetLoadHook([&fixture]() {
    fixture.WriteGuestByte(0, 0x7C);
    REQUIRE(fixture.TriggerCpuWrite(0));
    REQUIRE(fixture.shared_memory().RequestRange(
        fixture.guest_physical_address(), 16));
  });
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);

  // The upload happened, so the binding is in sync - re-uploading it on every
  // subsequent draw would be pure churn.
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);

  // Without a stored hash, the next invalidation has to reload.
  fixture.WriteGuestByte(0, 0x8D);
  REQUIRE(fixture.TriggerCpuWrite(0));
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 3);
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 3);
}

TEST_CASE("new texture binding stops reloading a rejected first snapshot",
          "[texture-revalidation][bindings][snapshot]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);

  // No Texture watch exists during a first upload either. Force its snapshot
  // token stale and restore current shared-memory validity before publication.
  fixture.SetLoadHook([&fixture]() {
    fixture.WriteGuestByte(0, 0x6B);
    REQUIRE(fixture.TriggerCpuWrite(0));
    REQUIRE(fixture.shared_memory().RequestRange(
        fixture.guest_physical_address(), 16));
  });
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.WriteGuestByte(0, 0x7C);
  REQUIRE(fixture.TriggerCpuWrite(0));
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture binding retries a failed replacement upload",
          "[texture-revalidation][bindings][load-failure]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.WriteGuestByte(0, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(0));
  fixture.FailNextLoad();
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);

  // A backend failure happens before FinalizeLoadAndWatch. The binding must
  // still retry because the texture remains outdated.
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 3);
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 3);
}

TEST_CASE("new texture binding retries a failed first upload",
          "[texture-revalidation][bindings][load-failure]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);

  fixture.FailNextLoad();
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation rejects a direct GPU write during encoding",
          "[texture-revalidation][write-generation]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  fixture.SetLoadHook([&fixture]() {
    // written_to_buffer=false models GPU work that writes imported guest RAM
    // directly. Current validity alone identifies the result as CPU-readable;
    // persistent GPU generation is what proves the source changed.
    fixture.shared_memory().RangeWrittenByGpu(kGuestPhysicalAddress, 16, false);
  });
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);

  // No hash may have been stored, so the next invalidation reloads.
  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation rejects guarded raw writes during publication",
          "[texture-revalidation][snapshot][raw-write]") {
  ScopedTextureRevalidationConfig config;

  SECTION("write remains in flight") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    Memory::PhysicalMemoryWriteScope raw_write;
    fixture.SetLoadHook([&fixture, &raw_write]() {
      raw_write = fixture.BeginRawPhysicalMemoryWrite(0, 1);
      REQUIRE(static_cast<bool>(raw_write));
      fixture.WriteGuestByte(0, 0x6B);
    });
    // No hash is stored while a raw writer is active, so completing the write
    // invalidates the freshly armed watch and forces a reload.
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 1);
    raw_write.End(1);
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 2);
  }

  SECTION("completed A to B to A write advances the invalidation epoch") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    fixture.SetLoadHook([&fixture]() {
      auto raw_write = fixture.BeginRawPhysicalMemoryWrite(0, 1);
      REQUIRE(static_cast<bool>(raw_write));
      fixture.WriteGuestByte(0, 0x6B);
      fixture.WriteGuestByte(0, 0x5A);
      raw_write.End(1);
    });
    REQUIRE_FALSE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 1);
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 2);
  }
}

TEST_CASE("texture source capture rejects an already active raw writer",
          "[texture-revalidation][snapshot][raw-write]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  fixture.shared_memory().MakeCpuRangeValid(fixture.guest_physical_address(),
                                            16);
  auto raw_write = fixture.BeginRawPhysicalMemoryWrite(0, 1);
  REQUIRE(static_cast<bool>(raw_write));
  REQUIRE_FALSE(fixture.CanCaptureCpuLoadSource());
  raw_write.End(0);
}

TEST_CASE("texture source capture tracks raw writer intervals independently",
          "[texture-revalidation][snapshot][raw-write]") {
  ScopedTextureRevalidationConfig config;

  SECTION("one overlapping writer remains active after another ends") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    fixture.shared_memory().MakeCpuRangeValid(fixture.guest_physical_address(),
                                              16);
    auto first = fixture.BeginRawPhysicalMemoryWrite(0, 1);
    auto second = fixture.BeginRawPhysicalMemoryWrite(0, 1);
    REQUIRE(static_cast<bool>(first));
    REQUIRE(static_cast<bool>(second));
    REQUIRE_FALSE(fixture.CanCaptureCpuLoadSource());
    first.End(0);
    REQUIRE_FALSE(fixture.CanCaptureCpuLoadSource());
    second.End(0);
    REQUIRE(fixture.CanCaptureCpuLoadSource());
  }

  SECTION("a disjoint writer does not block source capture") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    fixture.shared_memory().MakeCpuRangeValid(fixture.guest_physical_address(),
                                              16);
    auto disjoint = fixture.BeginRawPhysicalMemoryWrite(0x1000, 1);
    REQUIRE(static_cast<bool>(disjoint));
    REQUIRE(fixture.CanCaptureCpuLoadSource());
    disjoint.End(0);
  }
}

TEST_CASE("reported raw writes invalidate read-only physical texture memory",
          "[texture-revalidation][raw-write][read-only]") {
  ScopedTextureRevalidationConfig config;

  SECTION("unchanged reported write is revalidated") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    REQUIRE(fixture.ProtectGuestMemoryReadOnly());
    REQUIRE(fixture.LoadTexture());

    auto raw_write = fixture.BeginRawPhysicalMemoryWrite(0, 1);
    REQUIRE(static_cast<bool>(raw_write));
    raw_write.End(1);

    REQUIRE(fixture.texture_cache().watch_callbacks() == 1);
    REQUIRE(fixture.TryRevalidate());
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 1);
  }

  SECTION("changed bytes in a pinned writable host view are reloaded") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    fixture.SetPhysicalAliasSkipHostProtect(true);
    REQUIRE(fixture.ProtectGuestMemoryReadOnly());
    REQUIRE(fixture.LoadTexture());

    auto raw_write = fixture.BeginRawPhysicalMemoryWrite(0, 1);
    REQUIRE(static_cast<bool>(raw_write));
    fixture.WriteGuestByte(0, 0x6B);
    raw_write.End(1);

    REQUIRE(fixture.texture_cache().watch_callbacks() == 1);
    REQUIRE_FALSE(fixture.TryRevalidate());
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 2);
  }
}

TEST_CASE("texture load publication trusts the backend snapshot hash",
          "[texture-revalidation][snapshot][publication]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());

  // Deliberately make the fake backend's immutable snapshot token differ from
  // a fresh guest-memory hash. Generic publication owns validation of the token
  // epochs and generations, but must not reread and rehash the mutable guest
  // bytes that the backend has already snapshotted.
  fixture.SetSnapshotHashXor(UINT64_C(1));
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);

  // Invalidate the containing host page without changing the 16 bytes consumed
  // by this texture. If publication had silently reread guest RAM instead of
  // storing the backend's XOR token, this would incorrectly revalidate.
  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture load clamps an aligned range crossing physical memory",
          "[texture-revalidation][range-bounds]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  constexpr uint32_t kLastAllocationAddress =
      SharedMemory::kBufferSize - kAllocationSize;

  // A linear 8-bit texture with this height consumes 64 KiB + 1 byte before
  // 128-bit tail alignment. At the final 64 KiB physical allocation, that
  // extent leaves the 512 MiB shared-memory buffer, so it is clamped to the
  // end of it rather than dropped - the texture still has to be uploaded.
  REQUIRE(fixture.Initialize(2049, true, false, true, kLastAllocationAddress));
  REQUIRE(fixture.texture().GetGuestBaseSize() == kAllocationSize + 1);
  REQUIRE(TestTextureCache::GetGuestTextureLoadRangeLengthForTest(
              kLastAllocationAddress, kAllocationSize + 1) == kAllocationSize);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);
  REQUIRE(fixture.texture_cache().last_load_base());
}

TEST_CASE("texture load accepts the exact physical-memory end and zero mips",
          "[texture-revalidation][range-bounds]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  constexpr uint32_t kLastAllocationAddress =
      SharedMemory::kBufferSize - kAllocationSize;

  REQUIRE(TestTextureCache::GetGuestTextureLoadRangeLengthForTest(
              SharedMemory::kBufferSize - 16, 16) == 16);
  REQUIRE(TestTextureCache::GetGuestTextureLoadRangeLengthForTest(
              SharedMemory::kBufferSize - 16, 0) == 0);
  REQUIRE(TestTextureCache::GetGuestTextureLoadRangeLengthForTest(
              SharedMemory::kBufferSize - 16, 17) == 16);
  REQUIRE(TestTextureCache::GetGuestTextureLoadRangeLengthForTest(
              SharedMemory::kBufferSize, 16) == 0);

  REQUIRE(fixture.Initialize(2048, true, false, true, kLastAllocationAddress));
  REQUIRE(fixture.texture().GetGuestBaseSize() == kAllocationSize - 31);
  REQUIRE(fixture.texture().GetGuestMipsSize() == 0);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 1);
  REQUIRE(fixture.texture_cache().last_load_base());
  REQUIRE_FALSE(fixture.texture_cache().last_load_mips());
}

TEST_CASE("texture revalidation excludes untrackable physical ranges",
          "[texture-revalidation][eligibility]") {
  ScopedTextureRevalidationConfig config;

  SECTION("unallocated physical pages") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize(1, true, false, false));
    REQUIRE_FALSE(fixture.CanCaptureCpuLoadSource());
  }

  SECTION("physical memory aliased by the untracked 0x7F guest window") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize(1, true, false, true, 0x00800000));
    REQUIRE(fixture.LoadTexture());
    fixture.WriteGuestByte(32, 0x6B);
    REQUIRE(fixture.TriggerCpuWrite(32));
    REQUIRE_FALSE(fixture.TryRevalidate());
  }

  SECTION("the size limit includes the aligned load tail") {
    cvars::texture_cache_revalidate_size_limit = 1;
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    fixture.shared_memory().MakeCpuRangeValid(fixture.guest_physical_address(),
                                              16);
    REQUIRE_FALSE(fixture.CanCaptureCpuLoadSource());
  }
}

TEST_CASE("texture revalidation hashes the aligned 128-bit load tail",
          "[texture-revalidation]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  REQUIRE(fixture.texture().GetGuestBaseSize() == 1);
  REQUIRE(fixture.LoadTexture());

  fixture.WriteGuestByte(15, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(15));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("unused invalidated texture binding reloads when used later",
          "[texture-revalidation][bindings]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  constexpr uint32_t kSecondBinding = UINT32_C(1) << 1;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);
  fixture.SetLinear8BitTextureFetch(1, kGuestPhysicalAddress + 0x1000);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  // Invalidate slot 0, but make the intervening request use only slot 1. The
  // aggregate callback flag is consumed by this request, so slot 0's reason
  // for being out of sync must survive until the following call.
  fixture.WriteGuestByte(0, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(0));
  fixture.RequestTextures(kSecondBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 3);
}

TEST_CASE("fetch write preserves pending texture invalidation",
          "[texture-revalidation][bindings]") {
  ScopedTextureRevalidationConfig config;
  cvars::texture_cache_revalidate_unchanged = false;
  cvars::texture_cache_revalidate_census = false;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  fixture.WriteGuestByte(0, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(0));
  fixture.texture_cache().TextureFetchConstantWritten(0);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);
  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("unused invalidated texture binding revalidates when used later",
          "[texture-revalidation][bindings]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize());
  constexpr uint32_t kFirstBinding = UINT32_C(1) << 0;
  constexpr uint32_t kSecondBinding = UINT32_C(1) << 1;
  fixture.SetLinear8BitTextureFetch(0, kGuestPhysicalAddress);
  fixture.SetLinear8BitTextureFetch(1, kGuestPhysicalAddress + 0x1000);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().load_count() == 1);

  // Fault the containing host page through neighboring bytes, leaving the
  // texture's aligned 16-byte source unchanged. Slot 0 stays pending while the
  // slot 1 request consumes the aggregate callback flag.
  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  fixture.RequestTextures(kSecondBinding);
  REQUIRE(fixture.texture_cache().load_count() == 2);

  fixture.RequestTextures(kFirstBinding);
  REQUIRE(fixture.texture_cache().revalidated_base() == 1);
  REQUIRE(fixture.texture_cache().load_count() == 2);
}

TEST_CASE("texture revalidation excludes mixed and GPU-sourced loads",
          "[texture-revalidation]") {
  ScopedTextureRevalidationConfig config;

  SECTION("mixed initially valid and invalid shared-memory pages") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize(1024));
    const uint32_t guest_size = fixture.texture().GetGuestBaseSize();
    REQUIRE(guest_size > xe::memory::page_size());
    fixture.shared_memory().MakeCpuRangeValid(
        kGuestPhysicalAddress, uint32_t(xe::memory::page_size()));
    REQUIRE(fixture.LoadTexture());

    REQUIRE(fixture.TriggerCpuWrite(0));
    REQUIRE_FALSE(fixture.TryRevalidate());
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 2);
  }

  SECTION("GPU-authoritative shared-memory page") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    fixture.shared_memory().RangeWrittenByGpu(kGuestPhysicalAddress, 16);
    REQUIRE(fixture.LoadTexture());

    REQUIRE(fixture.TriggerCpuWrite(0));
    REQUIRE_FALSE(fixture.TryRevalidate());
    REQUIRE(fixture.LoadTexture());
    REQUIRE(fixture.texture_cache().load_count() == 2);
  }

  SECTION("GPU invalidation remains excluded after the pre-request scan") {
    TextureRevalidationFixture fixture;
    REQUIRE(fixture.Initialize());
    REQUIRE(fixture.LoadTexture());
    // Keep the shared-memory bytes CPU-valid to isolate the invalidation-origin
    // check from the GPU-authoritative validity check.
    fixture.shared_memory().RangeWrittenByGpu(kGuestPhysicalAddress, 16, false);
    REQUIRE_FALSE(fixture.CanCaptureCpuLoadSource());
  }
}

TEST_CASE("texture revalidation finalizes matching base independently of mips",
          "[texture-revalidation][mips]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize(1, true, true));
  REQUIRE(fixture.texture().GetGuestBaseSize() != 0);
  REQUIRE(fixture.texture().GetGuestMipsSize() != 0);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().last_load_base());
  REQUIRE(fixture.texture_cache().last_load_mips());

  // Invalidate both watched parts, but change only mip bytes. Revalidation
  // must keep the matching base and leave only mips for the next upload.
  fixture.WriteGuestByte(0x1000, 0x6B);
  REQUIRE(fixture.TriggerCpuWriteRange(0, 0x1001));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().revalidated_base() == 1);
  REQUIRE(fixture.texture_cache().revalidated_mips() == 0);
  REQUIRE(fixture.LoadTexture());
  REQUIRE_FALSE(fixture.texture_cache().last_load_base());
  REQUIRE(fixture.texture_cache().last_load_mips());
}

TEST_CASE("texture revalidation finalizes matching mips independently of base",
          "[texture-revalidation][mips]") {
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize(1, true, true));
  REQUIRE(fixture.LoadTexture());

  // Both parts share one 16 KiB host protection page. Change only the base
  // bytes; the mip snapshot must remain reusable while base is reloaded.
  fixture.WriteGuestByte(0, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(0));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().revalidated_base() == 0);
  REQUIRE(fixture.texture_cache().revalidated_mips() == 1);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().last_load_base());
  REQUIRE_FALSE(fixture.texture_cache().last_load_mips());
}

TEST_CASE("texture upload completion is independent for base and mips",
          "[texture-revalidation][mips][upload-completion]") {
  using ContentUploadStatus = TestTextureCache::TestContentUploadStatus;
  ScopedTextureRevalidationConfig config;
  TextureRevalidationFixture fixture;
  REQUIRE(fixture.Initialize(1, true, true));
  REQUIRE(fixture.LoadTexture());
  auto base_completion = std::make_shared<std::atomic<ContentUploadStatus>>(
      ContentUploadStatus::kFailed);
  auto mips_completion = std::make_shared<std::atomic<ContentUploadStatus>>(
      ContentUploadStatus::kSucceeded);
  fixture.SetContentUploadCompletion(base_completion, true, false);
  fixture.SetContentUploadCompletion(mips_completion, false, true);

  fixture.WriteGuestByte(32, 0x6B);
  REQUIRE(fixture.TriggerCpuWrite(32));
  REQUIRE_FALSE(fixture.TryRevalidate());
  REQUIRE(fixture.texture_cache().revalidated_base() == 0);
  REQUIRE(fixture.texture_cache().revalidated_mips() == 1);
  REQUIRE(fixture.LoadTexture());
  REQUIRE(fixture.texture_cache().last_load_base());
  REQUIRE_FALSE(fixture.texture_cache().last_load_mips());
}

}  // namespace test
}  // namespace gpu
}  // namespace xe
