/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_runtime.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_invocation_capture_bundle.h"
#include "xenia/cpu/guest_invocation_capture_page_reader.h"
#include "xenia/cpu/guest_invocation_capture_poller.h"
#include "xenia/cpu/guest_invocation_capture_runtime_config.h"
#include "xenia/cpu/guest_invocation_replay_cli.h"
#include "xenia/cpu/guest_invocation_replay_config.h"
#include "xenia/cpu/jit_corpus.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DEFINE_transient_path(
    guest_invocation_capture_output, "",
    "New local directory for one bounded guest-invocation capture bundle. "
    "The directory and its .part sibling must not already exist.",
    "Guest Invocation Capture");
DEFINE_transient_string(
    guest_invocation_capture_root_address, "",
    "Selected guest function entry as exactly eight hexadecimal digits, "
    "optionally prefixed by 0x.",
    "Guest Invocation Capture");
DEFINE_transient_string(
    guest_invocation_capture_root_end_address, "",
    "Inclusive selected guest function end as exactly eight hexadecimal "
    "digits, optionally prefixed by 0x.",
    "Guest Invocation Capture");
DEFINE_CVar(guest_invocation_capture_occurrence, 0,
            "1-based global occurrence of the selected guest function.",
            "Guest Invocation Capture", true, uint32_t);
DEFINE_CVar(guest_invocation_capture_max_attempts,
            xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxAttempts,
            "Maximum discovery and final attempts before rejection.",
            "Guest Invocation Capture", true, uint32_t);
DEFINE_CVar(
    guest_invocation_capture_max_duration_ms,
    xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxDurationMs,
    "Maximum wall-clock duration of the bounded capture in milliseconds.",
    "Guest Invocation Capture", true, uint64_t);
DEFINE_CVar(guest_invocation_capture_max_pages,
            xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxPages,
            "Maximum captured guest data pages.", "Guest Invocation Capture",
            true, uint32_t);
DEFINE_CVar(guest_invocation_capture_max_accesses,
            xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxAccesses,
            "Maximum recorded guest memory accesses.",
            "Guest Invocation Capture", true, uint64_t);
DEFINE_CVar(guest_invocation_capture_max_call_depth,
            xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxCallDepth,
            "Maximum nested guest call depth.", "Guest Invocation Capture",
            true, uint32_t);
DEFINE_CVar(guest_invocation_capture_max_events,
            xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxEvents,
            "Maximum serialized recorder events.", "Guest Invocation Capture",
            true, uint64_t);
DEFINE_CVar(guest_invocation_capture_max_functions,
            xe::cpu::GuestInvocationCaptureRuntimeConfig::kDefaultMaxFunctions,
            "Maximum functions in the exact translation closure.",
            "Guest Invocation Capture", true, uint32_t);

namespace xe {
namespace cpu {
namespace {

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

GuestInvocationCaptureRuntimeConfig CurrentConfig() {
  GuestInvocationCaptureRuntimeConfig config;
  config.output_directory = cvars::guest_invocation_capture_output;
  config.root_address = cvars::guest_invocation_capture_root_address;
  config.root_end_address = cvars::guest_invocation_capture_root_end_address;
  config.occurrence = cvars::guest_invocation_capture_occurrence;
  config.max_attempts = cvars::guest_invocation_capture_max_attempts;
  config.max_duration_ms = cvars::guest_invocation_capture_max_duration_ms;
  config.max_pages = cvars::guest_invocation_capture_max_pages;
  config.max_accesses = cvars::guest_invocation_capture_max_accesses;
  config.max_call_depth = cvars::guest_invocation_capture_max_call_depth;
  config.max_events = cvars::guest_invocation_capture_max_events;
  config.max_functions = cvars::guest_invocation_capture_max_functions;
  return config;
}

}  // namespace

bool GuestInvocationCapturePageReader::ReadPage(
    uint32_t page_address, std::array<uint8_t, 4096>* output) {
  static_assert(JitCorpus::kPageSize == 4096);
  if (!output || (page_address & (JitCorpus::kPageSize - 1)) ||
      page_address >
          std::numeric_limits<uint32_t>::max() - (JitCorpus::kPageSize - 1) ||
      !memory_.virtual_membase()) {
    return false;
  }

  BaseHeap* const first_heap = memory_.LookupHeap(page_address);
  BaseHeap* const last_heap =
      memory_.LookupHeap(page_address + JitCorpus::kPageSize - 1);
  if (!first_heap || first_heap != last_heap) {
    return false;
  }

  // Keep the allocation metadata stable through the copy. Guest memory
  // protection, decommit and release operations use this same recursive global
  // critical region, so none can invalidate the checked host range between the
  // query and memcpy.
  auto global_lock = xe::global_critical_region::AcquireDirect();
  HeapAllocationInfo allocation_info = {};
  if (!first_heap->QueryRegionInfo(page_address, &allocation_info)) {
    return false;
  }

  const void* const translated_page = memory_.TranslateVirtual(page_address);
  if (allocation_info.state || allocation_info.protect) {
    if (!(allocation_info.state & kMemoryAllocationCommit) ||
        !(allocation_info.protect & kMemoryProtectRead)) {
      return false;
    }
  } else {
    constexpr uint32_t kFirstReplayCodePage = 0x80040000u;
    constexpr uint32_t kLastReplayCodePage = 0x9FFFE000u;
    if (page_address < kFirstReplayCodePage ||
        page_address > kLastReplayCodePage) {
      return false;
    }
    const uint32_t alias_address = page_address ^ 0x10000000u;
    BaseHeap* const alias_heap = memory_.LookupHeap(alias_address);
    HeapAllocationInfo alias_info = {};
    if (!alias_heap || alias_heap == first_heap ||
        memory_.LookupHeap(alias_address + JitCorpus::kPageSize - 1) !=
            alias_heap ||
        !alias_heap->QueryRegionInfo(alias_address, &alias_info) ||
        !(alias_info.state & kMemoryAllocationCommit) ||
        !(alias_info.protect & kMemoryProtectRead)) {
      return false;
    }
    size_t readable_length = 0;
    xe::memory::PageAccess host_access = xe::memory::PageAccess::kNoAccess;
    if (!xe::memory::QueryProtect(const_cast<void*>(translated_page),
                                  readable_length, host_access) ||
        readable_length < JitCorpus::kPageSize ||
        host_access == xe::memory::PageAccess::kNoAccess) {
      return false;
    }
  }
  std::memcpy(output->data(), translated_page, output->size());
  return true;
}

struct GuestInvocationCaptureRuntime::Impl {
  class HostClock final : public ppc::GuestInvocationRecorderClock {
   public:
    uint64_t NowTicks() const override { return Clock::QueryHostTickCount(); }
  };

  Impl(Memory& memory, Processor& processor,
       std::filesystem::path output_directory,
       ppc::GuestInvocationRecorderSelection selection,
       uint32_t jit_corpus_config_flags,
       GuestInvocationReplayConfig replay_config,
       GuestInvocationReplaySha256 capture_build_sha256)
      : processor(processor),
        output_directory(std::move(output_directory)),
        selection(std::move(selection)),
        jit_corpus_config_flags(jit_corpus_config_flags),
        replay_config(std::move(replay_config)),
        capture_build_sha256(capture_build_sha256),
        page_reader(memory) {}

  bool PublishSegment(uint64_t segment_ordinal, uint64_t capture_start_tick,
                      uint64_t capture_end_tick,
                      const ppc::GuestInvocationRecorderResult& result,
                      std::string* error) {
    if (published) {
      return Fail(error,
                  "capture segment publication was requested more than once");
    }
    if (segment_ordinal != 0 || capture_end_tick < capture_start_tick) {
      return Fail(error, "capture segment has invalid ordering metadata");
    }
    if (result.invocation.function_address != selection.root_address ||
        result.invocation.function_end_address != selection.root_end_address ||
        result.invocation.entry_address != selection.root_address) {
      return Fail(error,
                  "capture result does not match the selected root extent");
    }
    if (result.translation_dependencies.empty()) {
      return Fail(error, "capture result has an empty translation closure");
    }

    ExecutionJitCorpusBuilder corpus_builder(jit_corpus_config_flags);
    std::unordered_set<uint32_t> copied_code_pages;
    for (const ppc::GuestInvocationPage& code_page : result.code_pages) {
      if (!copied_code_pages.insert(code_page.guest_address).second) {
        return Fail(error, "capture result contains a duplicate code page");
      }
      if (!corpus_builder.AddCodePage(code_page.guest_address,
                                      code_page.data.data(),
                                      code_page.data.size(), error)) {
        return false;
      }
    }
    uint32_t root_definition_count = 0;
    std::unordered_set<uint32_t> required_code_pages;
    for (const ppc::GuestInvocationRecorderFunction& dependency :
         result.translation_dependencies) {
      Function* function = processor.LookupFunction(dependency.address);
      if (!function || !function->is_guest() ||
          function->status() != Symbol::Status::kDefined) {
        return Fail(
            error,
            fmt::format(
                "capture closure function {:08X} is not defined guest code",
                dependency.address));
      }
      if (function->address() != dependency.address ||
          function->end_address() != dependency.end_address) {
        return Fail(
            error,
            fmt::format("capture closure extent changed for function {:08X}",
                        dependency.address));
      }
      if (function->behavior() == Function::Behavior::kExtern ||
          function->behavior() == Function::Behavior::kBuiltin) {
        return Fail(
            error, fmt::format("capture closure function {:08X} is host-backed",
                               dependency.address));
      }
      auto* guest_function = static_cast<GuestFunction*>(function);
      const size_t host_code_size = guest_function->machine_code_length();
      if (!guest_function->machine_code() || !host_code_size ||
          host_code_size > std::numeric_limits<uint32_t>::max()) {
        return Fail(
            error,
            fmt::format(
                "capture closure function {:08X} has no bounded host code",
                dependency.address));
      }
      if (!dependency.address || dependency.end_address < dependency.address ||
          (dependency.address & 3) || (dependency.end_address & 3) ||
          dependency.end_address > std::numeric_limits<uint32_t>::max() - 4) {
        return Fail(
            error,
            fmt::format("capture closure function {:08X} has an invalid extent",
                        dependency.address));
      }

      const uint64_t end_exclusive = uint64_t(dependency.end_address) + 4;
      for (uint64_t page =
               dependency.address & ~(uint64_t(JitCorpus::kPageSize) - 1);
           page < end_exclusive; page += JitCorpus::kPageSize) {
        const uint32_t page_address = static_cast<uint32_t>(page);
        required_code_pages.insert(page_address);
        if (!copied_code_pages.contains(page_address)) {
          return Fail(error,
                      fmt::format("capture result is missing immutable code "
                                  "page {:08X}",
                                  page_address));
        }
      }

      const ExecutionJitCorpusBuilder::FunctionRecord function_record = {
          dependency.address, dependency.end_address,
          static_cast<uint32_t>(host_code_size),
          JitCorpus::EncodeFunctionFlags(*function)};
      if (!corpus_builder.AddFunction(function_record, error)) {
        return false;
      }
      if (dependency.address == selection.root_address) {
        ++root_definition_count;
      }
    }
    if (root_definition_count != 1) {
      return Fail(error,
                  "capture translation closure does not contain one root");
    }
    if (required_code_pages.size() != copied_code_pages.size()) {
      return Fail(error,
                  "capture result contains code outside the translation "
                  "closure");
    }

    std::vector<uint8_t> exact_corpus_bytes;
    if (!corpus_builder.Encode(&exact_corpus_bytes, error) ||
        !WriteGuestInvocationCaptureBundle(
            output_directory, exact_corpus_bytes, result.invocation,
            capture_build_sha256, replay_config, error)) {
      return false;
    }
    published = true;
    XELOGI(
        "Guest invocation capture published: output={} segment={} attempts={} "
        "functions={} code_pages={} capture_ticks={}",
        path_to_utf8(output_directory), segment_ordinal, result.attempt_count,
        corpus_builder.function_count(), corpus_builder.code_page_count(),
        capture_end_tick - capture_start_tick);
    return true;
  }

  Processor& processor;
  std::filesystem::path output_directory;
  ppc::GuestInvocationRecorderSelection selection;
  const uint32_t jit_corpus_config_flags;
  GuestInvocationReplayConfig replay_config;
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  GuestInvocationCapturePageReader page_reader;
  HostClock clock;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator;
  std::unique_ptr<GuestInvocationCaptureDeadlinePoller> deadline_poller;
  bool attached = false;
  bool published = false;
};

bool GuestInvocationCaptureRuntime::IsRequested() {
  return CurrentConfig().IsRequested();
}

std::unique_ptr<GuestInvocationCaptureRuntime>
GuestInvocationCaptureRuntime::Create(Memory& memory, Processor& processor,
                                      bool guest_scheduler_enabled,
                                      std::string* error) {
  if (error) {
    error->clear();
  }
  if (!processor.backend()) {
    Fail(error, "capture requires an initialized CPU backend");
    return nullptr;
  }
  if (processor.guest_invocation_capture_sink()) {
    Fail(error, "capture event sink is already registered");
    return nullptr;
  }

  const GuestInvocationCaptureRuntimeConfig config = CurrentConfig();
  ppc::GuestInvocationRecorderSelection selection;
  ppc::GuestInvocationRecorderLimits limits;
  if (!config.BuildRecorderConfiguration(Clock::QueryHostTickFrequency(),
                                         &selection, &limits, error)) {
    return nullptr;
  }
  if (!config.ValidateOutputDirectory(error)) {
    return nullptr;
  }

  GuestInvocationReplayConfig replay_config;
  if (!CaptureCurrentGuestInvocationReplayConfig(*processor.backend(),
                                                 &replay_config, error) ||
      !ValidateGuestInvocationReplayBenchmarkConfig(replay_config, error)) {
    return nullptr;
  }
  GuestInvocationReplaySha256 capture_build_sha256 = {};
  if (!HashGuestInvocationReplayFile(filesystem::GetExecutablePath(),
                                     &capture_build_sha256, error)) {
    return nullptr;
  }

  const uint32_t jit_corpus_config_flags =
      guest_scheduler_enabled ? JitCorpus::kConfigGuestScheduler : 0;
  auto impl = std::make_unique<Impl>(
      memory, processor, config.output_directory, selection,
      jit_corpus_config_flags, std::move(replay_config), capture_build_sha256);
  Impl* impl_pointer = impl.get();
  impl->coordinator = GuestInvocationCaptureCoordinator::Create(
      0, impl->selection, limits, impl->page_reader, impl->clock,
      [impl_pointer](uint64_t segment_ordinal, uint64_t capture_start_tick,
                     uint64_t capture_end_tick,
                     const ppc::GuestInvocationRecorderResult& result,
                     std::string* publication_error) {
        return impl_pointer->PublishSegment(segment_ordinal, capture_start_tick,
                                            capture_end_tick, result,
                                            publication_error);
      },
      error);
  if (!impl->coordinator) {
    return nullptr;
  }

  auto runtime = std::unique_ptr<GuestInvocationCaptureRuntime>(
      new GuestInvocationCaptureRuntime(std::move(impl)));
  processor.set_guest_invocation_capture_sink(
      runtime->impl_->coordinator.get());
  runtime->impl_->attached = true;
  runtime->impl_->deadline_poller =
      GuestInvocationCaptureDeadlinePoller::Create(
          *runtime->impl_->coordinator,
          GuestInvocationCaptureDeadlinePoller::kDefaultInterval, error);
  if (!runtime->impl_->deadline_poller) {
    processor.set_guest_invocation_capture_sink(nullptr);
    runtime->impl_->attached = false;
    return nullptr;
  }
  return runtime;
}

GuestInvocationCaptureRuntime::GuestInvocationCaptureRuntime(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestInvocationCaptureRuntime::~GuestInvocationCaptureRuntime() { Stop(); }

void GuestInvocationCaptureRuntime::Stop() {
  if (!impl_) {
    return;
  }
  // Drain the owned worker while the coordinator is still Processor's valid
  // sink. Once this returns it cannot race detachment or coordinator teardown.
  impl_->deadline_poller.reset();
  if (impl_->attached) {
    GuestInvocationCaptureEventSink* const registered_sink =
        impl_->processor.guest_invocation_capture_sink();
    if (registered_sink == impl_->coordinator.get()) {
      impl_->processor.set_guest_invocation_capture_sink(nullptr);
    } else if (registered_sink) {
      XELOGE(
          "Guest invocation capture sink changed before runtime shutdown; "
          "leaving the replacement registered");
    }
    impl_->attached = false;
  }
  impl_->coordinator->Stop();
}

GuestInvocationCaptureStatus GuestInvocationCaptureRuntime::status() const {
  return impl_->coordinator->status();
}

const std::filesystem::path& GuestInvocationCaptureRuntime::output_directory()
    const {
  return impl_->output_directory;
}

const ppc::GuestInvocationRecorderSelection&
GuestInvocationCaptureRuntime::selection() const {
  return impl_->selection;
}

}  // namespace cpu
}  // namespace xe

#endif
