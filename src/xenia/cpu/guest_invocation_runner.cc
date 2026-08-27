/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_runner.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/exact_jit_corpus_module.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <time.h>
#endif  // XE_PLATFORM_MAC

namespace xe {
namespace cpu {

namespace {

constexpr uint32_t kGuestPageSize =
    ppc::GuestInvocationArtifactCodec::kPageSize;
constexpr uint32_t kMaximumSupportedHostPageSize = 64 * 1024;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool FailPlan(GuestInvocationReplayPlan* output, std::string* error,
              std::string_view message) {
  if (output) {
    *output = {};
  }
  return Fail(error, message);
}

bool IsPowerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }

bool IsSupportedDataPageAddress(uint32_t address) {
  return (address >= 0x00001000u && address <= 0x7EFFF000u) ||
         (address >= 0x80000000u && address <= 0x9FFFF000u);
}

bool IsSupportedCodePageAddress(uint32_t address) {
  return address >= 0x80040000u && address <= 0x9FFFE000u;
}

uint32_t XexBackingPageAddress(uint32_t address) {
  return address >= 0x90000000u && address < 0xA0000000u ? address - 0x10000000u
                                                         : address;
}

const ppc::GuestInvocationPage* FindInvocationPage(
    const std::vector<ppc::GuestInvocationPage>& pages, uint32_t address) {
  const auto it = std::lower_bound(
      pages.cbegin(), pages.cend(), address,
      [](const ppc::GuestInvocationPage& page, uint32_t candidate_address) {
        return page.guest_address < candidate_address;
      });
  return it != pages.cend() && it->guest_address == address ? &*it : nullptr;
}

bool ValidateInvocationPageList(
    const std::vector<ppc::GuestInvocationPage>& pages, bool allow_empty,
    std::string* error) {
  if (!allow_empty && pages.empty()) {
    return Fail(error, "invocation has no input data pages");
  }
  if (pages.size() >
      ppc::GuestInvocationArtifactCodec::kMaxDataPagesPerInvocation) {
    return Fail(error, "invocation page list exceeds the resource limit");
  }

  uint32_t previous_address = 0;
  bool has_previous_address = false;
  for (const ppc::GuestInvocationPage& page : pages) {
    if (page.guest_address & (kGuestPageSize - 1)) {
      return Fail(error, "invocation page list contains an unaligned page");
    }
    if (!IsSupportedDataPageAddress(page.guest_address)) {
      return Fail(error,
                  "invocation page list contains an unsupported address");
    }
    if (has_previous_address && page.guest_address <= previous_address) {
      return Fail(error,
                  "invocation page list is not strictly address ordered");
    }
    previous_address = page.guest_address;
    has_previous_address = true;
  }
  return true;
}

bool ValidateCorpusShape(const ExecutionJitCorpus& corpus, std::string* error) {
  const std::vector<uint32_t>& page_addresses = corpus.page_addresses();
  if (page_addresses.empty() || corpus.functions().empty() ||
      corpus.function_definition_order().empty()) {
    return Fail(error, "replay corpus is empty");
  }
  if (page_addresses.size() > ExecutionJitCorpus::kMaxPageRecords ||
      corpus.functions().size() > ExecutionJitCorpus::kMaxFunctionRecords) {
    return Fail(error, "replay corpus exceeds a resource limit");
  }
  if (page_addresses.size() >
          std::numeric_limits<size_t>::max() / JitCorpus::kPageSize ||
      corpus.page_data().size() !=
          page_addresses.size() * JitCorpus::kPageSize) {
    return Fail(error, "replay corpus page data has an invalid size");
  }
  if (corpus.function_definition_order().size() != corpus.functions().size()) {
    return Fail(error,
                "replay corpus definition order is not a full permutation");
  }

  uint32_t previous_page = 0;
  bool has_previous_page = false;
  for (uint32_t page_address : page_addresses) {
    if ((page_address & (JitCorpus::kPageSize - 1)) ||
        !IsSupportedCodePageAddress(page_address)) {
      return Fail(error, "replay corpus contains an invalid code page");
    }
    if (has_previous_page && page_address <= previous_page) {
      return Fail(error, "replay corpus code pages are not strictly ordered");
    }
    previous_page = page_address;
    has_previous_page = true;
  }

  std::set<uint32_t> function_addresses;
  uint32_t previous_function = 0;
  bool has_previous_function = false;
  for (const ExecutionJitCorpus::FunctionRecord& function :
       corpus.functions()) {
    if (!function.address || (function.address & 3) ||
        (function.end_address & 3) || function.end_address < function.address ||
        uint64_t(function.end_address) - function.address + 4 >
            ExecutionJitCorpus::kMaxFunctionSize) {
      return Fail(error, "replay corpus contains an invalid function extent");
    }
    JitCorpus::FunctionMetadata metadata;
    if (!JitCorpus::DecodeFunctionFlags(function.flags, &metadata)) {
      return Fail(error, "replay corpus contains invalid function metadata");
    }
    if (has_previous_function && function.address <= previous_function) {
      return Fail(error, "replay corpus functions are not strictly ordered");
    }
    previous_function = function.address;
    has_previous_function = true;
    function_addresses.insert(function.address);
  }

  std::set<uint32_t> ordered_addresses;
  for (uint32_t address : corpus.function_definition_order()) {
    if (!function_addresses.contains(address) ||
        !ordered_addresses.insert(address).second) {
      return Fail(error,
                  "replay corpus definition order is not a full permutation");
    }
  }
  return true;
}

struct SuppliedPageFlags {
  bool code = false;
  bool data = false;
  bool dirty = false;
};

struct GranuleFlags {
  bool writable = false;
  bool dirty = false;
};

#if XE_PLATFORM_MAC
bool ReadCurrentThreadCpuNanoseconds(thread_t thread,
                                     uint64_t* cpu_nanoseconds) {
  if (!cpu_nanoseconds) {
    return false;
  }
  thread_basic_info_data_t info = {};
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  const kern_return_t result =
      thread_info(thread, THREAD_BASIC_INFO,
                  reinterpret_cast<thread_info_t>(&info), &count);
  if (result != KERN_SUCCESS || count < THREAD_BASIC_INFO_COUNT ||
      info.user_time.seconds < 0 || info.user_time.microseconds < 0 ||
      info.system_time.seconds < 0 || info.system_time.microseconds < 0) {
    return false;
  }
  constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ull;
  constexpr uint64_t kNanosecondsPerMicrosecond = 1000ull;
  *cpu_nanoseconds =
      uint64_t(info.user_time.seconds) * kNanosecondsPerSecond +
      uint64_t(info.user_time.microseconds) * kNanosecondsPerMicrosecond +
      uint64_t(info.system_time.seconds) * kNanosecondsPerSecond +
      uint64_t(info.system_time.microseconds) * kNanosecondsPerMicrosecond;
  return true;
}
#endif  // XE_PLATFORM_MAC

}  // namespace

bool BuildGuestInvocationReplayPlan(
    const ppc::GuestFunctionInvocation& invocation,
    const ExecutionJitCorpus& corpus, uint32_t host_page_size,
    GuestInvocationReplayPlan* output, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return FailPlan(nullptr, error, "replay plan output is null");
  }
  *output = {};

  if (!IsPowerOfTwo(host_page_size) || host_page_size < kGuestPageSize ||
      host_page_size > kMaximumSupportedHostPageSize) {
    return FailPlan(output, error,
                    "host page size is unsupported for invocation replay");
  }
  if (!ValidateCorpusShape(corpus, error)) {
    return false;
  }
  if (invocation.dependency_flags & ~ppc::kGuestInvocationKnownDependencyMask) {
    return FailPlan(output, error,
                    "invocation contains unknown dependency flags");
  }
  if (invocation.dependency_flags) {
    return FailPlan(output, error,
                    "invocation has unsupported replay dependencies");
  }
  if (!invocation.function_address || (invocation.function_address & 3) ||
      (invocation.function_end_address & 3) ||
      invocation.function_end_address < invocation.function_address ||
      uint64_t(invocation.function_end_address) - invocation.function_address +
              4 >
          ppc::GuestInvocationArtifactCodec::kMaxFunctionSize) {
    return FailPlan(output, error,
                    "invocation contains an invalid function extent");
  }
  if (invocation.entry_address != invocation.function_address) {
    return FailPlan(output, error,
                    "invocation entry does not equal its function start");
  }
  if (!invocation.expected_return_address ||
      (invocation.expected_return_address & 3) ||
      (invocation.expected_return_address >= invocation.function_address &&
       invocation.expected_return_address <= invocation.function_end_address) ||
      invocation.input.link_register != invocation.expected_return_address) {
    return FailPlan(output, error,
                    "invocation contains an invalid return boundary");
  }

  const ExecutionJitCorpus::FunctionRecord* root_record =
      corpus.FindFunction(invocation.function_address);
  if (!root_record ||
      root_record->end_address != invocation.function_end_address) {
    return FailPlan(output, error,
                    "invocation function is absent or has a different extent");
  }

  if (!ValidateInvocationPageList(invocation.input_data_pages, true, error) ||
      !ValidateInvocationPageList(invocation.expected_dirty_pages, true,
                                  error)) {
    *output = {};
    return false;
  }
  for (const ppc::GuestInvocationPage& dirty_page :
       invocation.expected_dirty_pages) {
    const ppc::GuestInvocationPage* input_page = FindInvocationPage(
        invocation.input_data_pages, dirty_page.guest_address);
    if (!input_page) {
      return FailPlan(output, error,
                      "dirty page is absent from invocation input pages");
    }
    if (input_page->data == dirty_page.data) {
      return FailPlan(output, error,
                      "dirty page is unchanged from its invocation input");
    }
  }

  std::map<uint32_t, SuppliedPageFlags> supplied_pages;
  for (uint32_t address : corpus.page_addresses()) {
    supplied_pages[address].code = true;
  }
  for (const ppc::GuestInvocationPage& page : invocation.input_data_pages) {
    SuppliedPageFlags& flags = supplied_pages[page.guest_address];
    if (flags.code) {
      return FailPlan(output, error,
                      "invocation data overlaps an exact corpus code page");
    }
    flags.data = true;
  }
  for (const ppc::GuestInvocationPage& page : invocation.expected_dirty_pages) {
    supplied_pages[page.guest_address].dirty = true;
  }

  std::map<uint32_t, uint32_t> xex_backing_owners;
  for (const auto& supplied_page : supplied_pages) {
    const uint32_t address = supplied_page.first;
    const uint32_t backing_address = XexBackingPageAddress(address);
    const auto [it, inserted] =
        xex_backing_owners.emplace(backing_address, address);
    if (!inserted && it->second != address) {
      return FailPlan(output, error,
                      "supplied 0x8 and 0x9 pages alias the same backing page");
    }
  }

  std::map<uint32_t, GranuleFlags> granules;
  for (const auto& [address, flags] : supplied_pages) {
    const uint32_t granule_address = address & ~(host_page_size - 1);
    GranuleFlags& granule = granules[granule_address];
    granule.writable |= flags.data;
    granule.dirty |= flags.dirty;
  }

  for (const auto& granule_entry : granules) {
    const uint32_t granule_address = granule_entry.first;
    const uint64_t granule_end = uint64_t(granule_address) + host_page_size;
    if (granule_end > uint64_t(std::numeric_limits<uint32_t>::max()) + 1) {
      return FailPlan(output, error,
                      "host protection granule wraps the guest address space");
    }
    for (uint64_t page_address = granule_address; page_address < granule_end;
         page_address += kGuestPageSize) {
      if (!supplied_pages.contains(static_cast<uint32_t>(page_address))) {
        return FailPlan(
            output, error,
            "supplied pages do not close a host protection granule");
      }
    }
  }

  GuestInvocationReplayPlan plan;
  plan.host_page_size = host_page_size;
  plan.supplied_page_addresses.reserve(supplied_pages.size());
  for (const auto& supplied_page : supplied_pages) {
    plan.supplied_page_addresses.push_back(supplied_page.first);
  }
  plan.protection_granules.reserve(granules.size());
  for (const auto& [address, flags] : granules) {
    plan.protection_granules.push_back(GuestInvocationReplayProtectionGranule{
        address, host_page_size, flags.writable});
    if (flags.dirty) {
      const uint64_t granule_end = uint64_t(address) + host_page_size;
      for (uint64_t page_address = address; page_address < granule_end;
           page_address += kGuestPageSize) {
        plan.reset_page_addresses.push_back(
            static_cast<uint32_t>(page_address));
      }
    }
  }
  *output = std::move(plan);
  return true;
}

std::unique_ptr<GuestInvocationRunner> GuestInvocationRunner::Create(
    const ppc::GuestFunctionInvocation& invocation,
    const ExecutionJitCorpus& corpus, std::unique_ptr<backend::Backend> backend,
    std::string* error) {
  if (error) {
    error->clear();
  }
#if !XE_PLATFORM_MAC || !XE_ARCH_ARM64
  Fail(error, "guest invocation replay runner requires Apple A64");
  return nullptr;
#endif  // !XE_PLATFORM_MAC || !XE_ARCH_ARM64
  if (!backend) {
    Fail(error, "invocation replay requires a backend");
    return nullptr;
  }

  const size_t native_page_size = xe::memory::page_size();
  if (native_page_size > std::numeric_limits<uint32_t>::max()) {
    Fail(error, "native host page size does not fit the replay format");
    return nullptr;
  }
  GuestInvocationReplayPlan plan;
  if (!BuildGuestInvocationReplayPlan(invocation, corpus,
                                      static_cast<uint32_t>(native_page_size),
                                      &plan, error)) {
    return nullptr;
  }

  std::unique_ptr<GuestInvocationRunner> runner(
      new GuestInvocationRunner(invocation, corpus, std::move(plan)));
  if (!runner->Initialize(std::move(backend), error)) {
    return nullptr;
  }
  return runner;
}

GuestInvocationRunner::GuestInvocationRunner(
    const ppc::GuestFunctionInvocation& invocation,
    const ExecutionJitCorpus& corpus, GuestInvocationReplayPlan plan)
    : invocation_(&invocation), corpus_(&corpus), plan_(std::move(plan)) {}

GuestInvocationRunner::~GuestInvocationRunner() = default;

const backend::Backend& GuestInvocationRunner::backend() const {
  return *processor_->backend();
}

bool GuestInvocationRunner::Initialize(
    std::unique_ptr<backend::Backend> backend, std::string* error) {
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    return Fail(error, "guest memory initialization failed");
  }

  processor_ = std::make_unique<Processor>(memory_.get(), nullptr);
  if (!processor_->Setup(std::move(backend))) {
    return Fail(error, "guest processor initialization failed");
  }
  if (!CommitAndLoadPages(error) || !CloseAndReopenGuestViews(error) ||
      !PrepareResetPageCopies(error)) {
    return false;
  }

  std::string module_error;
  std::unique_ptr<ExactJitCorpusModule> module = ExactJitCorpusModule::Create(
      processor_.get(), *corpus_, "guest_invocation_replay", &module_error);
  if (!module) {
    if (error) {
      error->assign(module_error.empty() ? "exact corpus module creation failed"
                                         : module_error);
    }
    return false;
  }
  module_ = module.get();
  if (!processor_->AddModule(std::move(module))) {
    module_ = nullptr;
    return Fail(error, "exact corpus module registration failed");
  }

  thread_state_ = std::make_unique<ThreadState>(processor_.get(), 0x100u, 0, 0);
  return true;
}

bool GuestInvocationRunner::CommitAndLoadPages(std::string* error) {
  struct NativeHeapPage {
    BaseHeap* heap = nullptr;
    uint32_t address = 0;
    uint32_t size = 0;
  };
  std::map<std::pair<uint32_t, uint32_t>, NativeHeapPage> native_pages;

  for (uint32_t address : plan_.supplied_page_addresses) {
    BaseHeap* heap = memory_->LookupHeap(address);
    if (!heap || address < heap->heap_base()) {
      return Fail(error, "supplied page has no ordinary virtual/XEX heap");
    }
    const uint32_t heap_page_size = heap->page_size();
    const uint32_t heap_page_address =
        heap->heap_base() +
        ((address - heap->heap_base()) / heap_page_size) * heap_page_size;
    native_pages.emplace(
        std::make_pair(heap->heap_base(), heap_page_address),
        NativeHeapPage{heap, heap_page_address, heap_page_size});
  }

  for (const auto& native_page_entry : native_pages) {
    const NativeHeapPage& native_page = native_page_entry.second;
    if (!native_page.heap->AllocFixed(
            native_page.address, native_page.size, 0,
            kMemoryAllocationReserve | kMemoryAllocationCommit,
            kMemoryProtectRead | kMemoryProtectWrite)) {
      return Fail(error, "heap-native replay page commit failed");
    }
  }

  for (uint32_t address : plan_.supplied_page_addresses) {
    const uint8_t* initial_data = InitialPageData(address);
    if (!initial_data) {
      return Fail(error, "planned replay page has no initial contents");
    }
    std::memcpy(memory_->TranslateVirtual(address), initial_data,
                kGuestPageSize);
  }
  return true;
}

bool GuestInvocationRunner::CloseAndReopenGuestViews(std::string* error) {
  if (!xe::memory::Protect(memory_->virtual_membase(), 0x100000000ull,
                           xe::memory::PageAccess::kNoAccess)) {
    return Fail(error, "failed to close the full guest virtual view");
  }
  if (!xe::memory::Protect(memory_->physical_membase(), 0x20000000ull,
                           xe::memory::PageAccess::kNoAccess)) {
    return Fail(error, "failed to close the full raw physical view");
  }

  for (const GuestInvocationReplayProtectionGranule& granule :
       plan_.protection_granules) {
    const xe::memory::PageAccess access =
        granule.writable ? xe::memory::PageAccess::kReadWrite
                         : xe::memory::PageAccess::kReadOnly;
    if (!xe::memory::Protect(memory_->TranslateVirtual(granule.guest_address),
                             granule.size, access)) {
      return Fail(error, "failed to reopen a supplied protection granule");
    }
  }
  return true;
}

bool GuestInvocationRunner::PrepareResetPageCopies(std::string* error) {
  reset_page_copies_.clear();
  reset_page_copies_.reserve(plan_.reset_page_addresses.size());
  for (uint32_t address : plan_.reset_page_addresses) {
    const uint8_t* initial_data = InitialPageData(address);
    if (!initial_data) {
      reset_page_copies_.clear();
      return Fail(error, "reset page has no initial contents");
    }
    reset_page_copies_.push_back(
        ResetPageCopy{memory_->TranslateVirtual(address), initial_data});
  }
  return true;
}

bool GuestInvocationRunner::ResolveFunctionsInCaptureOrder(std::string* error) {
  root_function_ = nullptr;
  for (uint32_t address : corpus_->function_definition_order()) {
    Function* function = processor_->ResolveFunction(address);
    if (!function) {
      return Fail(error,
                  "captured function failed to resolve during replay warmup");
    }
    if (!module_->HasExactExtent(*function)) {
      return Fail(error,
                  "replayed function does not have its captured exact extent");
    }
    if (address == invocation_->function_address) {
      root_function_ = function;
    }
  }
  if (!root_function_) {
    return Fail(error, "selected invocation root was not defined in order");
  }
  return true;
}

bool GuestInvocationRunner::WarmAndVerify(std::string* error) {
  if (error) {
    error->clear();
  }
  if (warmed_) {
    return Fail(error, "invocation replay has already been warmed");
  }
  if (!ResolveFunctionsInCaptureOrder(error) || !ResetInvocation(error) ||
      !Invoke(error) || !VerifyCurrentState(error)) {
    return false;
  }
  warmed_ = true;
  return true;
}

bool GuestInvocationRunner::ResetInvocation(std::string* error) {
  for (const ResetPageCopy& copy : reset_page_copies_) {
    std::memcpy(copy.destination, copy.source, kGuestPageSize);
  }

  ppc::RestoreGuestPPCRegisterState(invocation_->input,
                                    thread_state_->context());
  if (!processor_->backend()->ResetGuestInvocationReplayState(
          thread_state_->context())) {
    return Fail(error, "backend rejected deterministic invocation reset");
  }
  return true;
}

bool GuestInvocationRunner::Invoke(std::string* error) {
  if (!root_function_) {
    return Fail(error, "invocation root is unresolved");
  }
  if (!root_function_->Call(thread_state_.get(),
                            invocation_->expected_return_address)) {
    return Fail(error, "guest invocation did not return successfully");
  }
  return true;
}

bool GuestInvocationRunner::VerifyCurrentState(std::string* error) const {
  if (ppc::CaptureGuestPPCRegisterState(*thread_state_->context()) !=
      invocation_->expected_output) {
    return Fail(error,
                "guest invocation architectural output does not match capture");
  }

  for (const ppc::GuestInvocationPage& input_page :
       invocation_->input_data_pages) {
    const ppc::GuestInvocationPage* dirty_page = FindInvocationPage(
        invocation_->expected_dirty_pages, input_page.guest_address);
    const auto& expected_data = dirty_page ? dirty_page->data : input_page.data;
    if (std::memcmp(memory_->TranslateVirtual(input_page.guest_address),
                    expected_data.data(), expected_data.size())) {
      return Fail(error, "guest invocation data output does not match capture");
    }
  }

  for (uint32_t address : corpus_->page_addresses()) {
    const uint8_t* expected_data = corpus_->FindPageData(address);
    if (!expected_data || std::memcmp(memory_->TranslateVirtual(address),
                                      expected_data, JitCorpus::kPageSize)) {
      return Fail(error, "guest invocation modified exact corpus code bytes");
    }
  }
  if (!root_function_ || !module_->HasExactExtent(*root_function_)) {
    return Fail(error, "invocation root lost its captured exact extent");
  }
  return true;
}

const uint8_t* GuestInvocationRunner::InitialPageData(
    uint32_t guest_address) const {
  const ppc::GuestInvocationPage* data_page =
      FindInvocationPage(invocation_->input_data_pages, guest_address);
  if (data_page) {
    return data_page->data.data();
  }
  return corpus_->FindPageData(guest_address);
}

bool GuestInvocationRunner::RunTimed(uint64_t invocation_count,
                                     GuestInvocationReplayMetrics* metrics,
                                     std::string* error) {
  if (error) {
    error->clear();
  }
  if (!metrics) {
    return Fail(error, "invocation replay metrics output is null");
  }
  *metrics = {};
  if (!warmed_) {
    return Fail(error, "invocation replay must be warmed before timing");
  }
  if (!invocation_count || invocation_count > kMaxTimedInvocationCount) {
    return Fail(error, "timed invocation count is outside the bounded range");
  }

#if !XE_PLATFORM_MAC
  return Fail(error, "timed guest invocation replay is macOS-only");
#else
  backend::CodeCache* code_cache = processor_->backend()->code_cache();
  if (!code_cache) {
    return Fail(error, "replay backend has no code cache");
  }
  const uint64_t placement_generation_before =
      code_cache->placement_generation();

  const thread_t current_thread = mach_thread_self();
  if (current_thread == MACH_PORT_NULL) {
    return Fail(error, "failed to read current-thread CPU time before replay");
  }

  // Nest the primary CPU interval inside the diagnostic wall interval so the
  // primary metric does not include either wall-clock query. The wall metric
  // intentionally includes both THREAD_BASIC_INFO queries and is diagnostic.
  bool timed_calls_succeeded = true;
  const uint64_t wall_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  uint64_t thread_cpu_start = 0;
  if (!ReadCurrentThreadCpuNanoseconds(current_thread, &thread_cpu_start)) {
    mach_port_deallocate(mach_task_self(), current_thread);
    return Fail(error, "failed to read current-thread CPU time before replay");
  }
  for (uint64_t i = 0; i < invocation_count; ++i) {
    if (!ResetInvocation(error) || !Invoke(error)) {
      timed_calls_succeeded = false;
      break;
    }
  }
  uint64_t thread_cpu_end = 0;
  const bool cpu_read_succeeded =
      ReadCurrentThreadCpuNanoseconds(current_thread, &thread_cpu_end);
  const uint64_t wall_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  mach_port_deallocate(mach_task_self(), current_thread);
  const uint64_t placement_generation_after =
      code_cache->placement_generation();

  if (!timed_calls_succeeded) {
    return false;
  }
  if (!VerifyCurrentState(error)) {
    return false;
  }
  if (!cpu_read_succeeded || thread_cpu_end <= thread_cpu_start) {
    return Fail(error, "current-thread CPU interval is missing or zero");
  }
  if (wall_end <= wall_start) {
    return Fail(error, "uptime raw interval is missing or zero");
  }
  if (placement_generation_after != placement_generation_before) {
    return Fail(error, "code placement changed inside the timed interval");
  }

  if (!ResetInvocation(error) || !Invoke(error) || !VerifyCurrentState(error)) {
    return false;
  }
  if (code_cache->placement_generation() != placement_generation_after) {
    return Fail(error, "code placement changed during final verification");
  }

  // Measure reset in a separate post-primary batch. This is a raw diagnostic,
  // not a value that may be subtracted from the primary reset-plus-call metric:
  // the two intervals have different cache and execution histories.
  const thread_t reset_thread = mach_thread_self();
  if (reset_thread == MACH_PORT_NULL) {
    return Fail(error,
                "failed to read current-thread CPU time before reset replay");
  }
  bool reset_calls_succeeded = true;
  const uint64_t reset_wall_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  uint64_t reset_thread_cpu_start = 0;
  if (!ReadCurrentThreadCpuNanoseconds(reset_thread, &reset_thread_cpu_start)) {
    mach_port_deallocate(mach_task_self(), reset_thread);
    return Fail(error,
                "failed to read current-thread CPU time before reset replay");
  }
  for (uint64_t i = 0; i < invocation_count; ++i) {
    if (!ResetInvocation(error)) {
      reset_calls_succeeded = false;
      break;
    }
  }
  uint64_t reset_thread_cpu_end = 0;
  const bool reset_cpu_read_succeeded =
      ReadCurrentThreadCpuNanoseconds(reset_thread, &reset_thread_cpu_end);
  const uint64_t reset_wall_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  mach_port_deallocate(mach_task_self(), reset_thread);

  if (!reset_calls_succeeded) {
    return false;
  }
  if (!reset_cpu_read_succeeded ||
      reset_thread_cpu_end < reset_thread_cpu_start) {
    return Fail(error, "current-thread reset CPU interval is invalid");
  }
  if (reset_wall_end <= reset_wall_start) {
    return Fail(error, "reset uptime raw interval is missing or zero");
  }
  if (!Invoke(error) || !VerifyCurrentState(error)) {
    return false;
  }
  if (code_cache->placement_generation() != placement_generation_after) {
    return Fail(error,
                "code placement changed during reset diagnostic verification");
  }

  GuestInvocationReplayMetrics accepted_metrics;
  accepted_metrics.timed_invocation_count = invocation_count;
  accepted_metrics.thread_cpu_nanoseconds = thread_cpu_end - thread_cpu_start;
  accepted_metrics.uptime_raw_nanoseconds = wall_end - wall_start;
  accepted_metrics.reset_only_thread_cpu_nanoseconds =
      reset_thread_cpu_end - reset_thread_cpu_start;
  accepted_metrics.reset_only_uptime_raw_nanoseconds =
      reset_wall_end - reset_wall_start;
  accepted_metrics.placement_generation_before = placement_generation_before;
  accepted_metrics.placement_generation_after = placement_generation_after;
  accepted_metrics.reset_page_count_per_invocation =
      plan_.reset_page_addresses.size();
  accepted_metrics.reset_bytes_per_invocation =
      uint64_t(plan_.reset_page_addresses.size()) * kGuestPageSize;
  *metrics = accepted_metrics;
  return true;
#endif  // XE_PLATFORM_MAC
}

}  // namespace cpu
}  // namespace xe
