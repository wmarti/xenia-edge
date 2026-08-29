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
#include "xenia/base/threading.h"
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
            "Maximum supplied guest data pages plus distinct cross-thread "
            "write pages.",
            "Guest Invocation Capture", true, uint32_t);
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

bool ValidateGuestInvocationCaptureCodePageClosure(
    const std::vector<ppc::GuestInvocationRecorderFunction>& functions,
    const std::vector<ppc::GuestInvocationPage>& code_pages,
    uint32_t host_protection_page_size, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!host_protection_page_size ||
      (host_protection_page_size & (host_protection_page_size - 1)) ||
      host_protection_page_size <
          ppc::GuestInvocationRecorderLimits::kMinimumHostProtectionPageSize ||
      host_protection_page_size >
          ppc::GuestInvocationRecorderLimits::kMaximumHostProtectionPageSize) {
    return Fail(error, "capture host protection page size is unsupported");
  }
  if (functions.empty()) {
    return Fail(error, "capture translation closure is empty");
  }

  std::unordered_set<uint32_t> supplied_pages;
  for (const ppc::GuestInvocationPage& page : code_pages) {
    if (page.guest_address & (JitCorpus::kPageSize - 1)) {
      return Fail(error, "capture code page is not aligned");
    }
    if (!supplied_pages.insert(page.guest_address).second) {
      return Fail(error, "capture result contains a duplicate code page");
    }
  }

  std::unordered_set<uint32_t> required_pages;
  for (const ppc::GuestInvocationRecorderFunction& function : functions) {
    if (!function.address || (function.address & 3) ||
        (function.end_address & 3) || function.end_address < function.address ||
        uint64_t(function.end_address) - function.address + 4 >
            ppc::GuestInvocationArtifactCodec::kMaxFunctionSize) {
      return Fail(error, "capture code closure has an invalid function extent");
    }
    const uint32_t first_page = function.address & ~(JitCorpus::kPageSize - 1);
    const uint32_t last_page =
        function.end_address & ~(JitCorpus::kPageSize - 1);
    for (uint64_t function_page = first_page; function_page <= last_page;
         function_page += JitCorpus::kPageSize) {
      const uint32_t granule_address = static_cast<uint32_t>(function_page) &
                                       ~(host_protection_page_size - 1);
      const uint64_t granule_end =
          uint64_t(granule_address) + host_protection_page_size;
      if (granule_end > uint64_t(std::numeric_limits<uint32_t>::max()) + 1) {
        return Fail(error,
                    "capture code granule wraps the guest address space");
      }
      for (uint64_t closure_page = granule_address; closure_page < granule_end;
           closure_page += JitCorpus::kPageSize) {
        required_pages.insert(static_cast<uint32_t>(closure_page));
      }
    }
  }
  if (required_pages != supplied_pages) {
    return Fail(error,
                "capture result does not contain the exact code protection "
                "granule closure");
  }
  return true;
}

bool GuestInvocationCapturePageReader::ReadPage(
    uint32_t page_address, std::array<uint8_t, 4096>* output) {
  last_read_was_retryable_ = false;
  last_read_failure_site_ = 0;
  static_assert(JitCorpus::kPageSize == 4096);
  if (!output || (page_address & (JitCorpus::kPageSize - 1)) ||
      page_address >
          std::numeric_limits<uint32_t>::max() - (JitCorpus::kPageSize - 1) ||
      !memory_.virtual_membase()) {
    last_read_failure_site_ = 1;
    return false;
  }

  // Recorder callbacks already hold their coordinator lock. Never wait for
  // the global critical region here: its contract requires it to be acquired
  // before all narrower locks, so blocking would permit a lock-order cycle
  // with a guest thread entering a capture callback from the global region.
  // A contended snapshot fails closed and rejects only the capture.
  auto global_lock = xe::global_critical_region::TryAcquire();
  if (!global_lock.owns_lock()) {
    last_read_was_retryable_ = true;
    return false;
  }

  BaseHeap* const first_heap = memory_.LookupHeap(page_address);
  BaseHeap* const last_heap =
      memory_.LookupHeap(page_address + JitCorpus::kPageSize - 1);
  if (!first_heap || first_heap != last_heap) {
    last_read_failure_site_ = 2;
    return false;
  }

  // Guest memory protection, decommit and release operations use the global
  // region too, so none can invalidate the checked host range between the
  // query and memcpy.
  HeapAllocationInfo allocation_info = {};
  if (!first_heap->QueryRegionInfo(page_address, &allocation_info)) {
    last_read_failure_site_ = 3;
    return false;
  }

  const void* const translated_page = memory_.TranslateVirtual(page_address);
  if (allocation_info.state || allocation_info.protect) {
    if (!(allocation_info.state & kMemoryAllocationCommit) ||
        !(allocation_info.protect & kMemoryProtectRead)) {
      last_read_failure_site_ = 4;
      return false;
    }
  } else {
    constexpr uint32_t kFirstReplayCodePage = 0x80040000u;
    constexpr uint32_t kLastReplayCodePage = 0x9FFFE000u;
    if (page_address < kFirstReplayCodePage ||
        page_address > kLastReplayCodePage) {
      last_read_failure_site_ = 5;
      return false;
    }
    // Neither state nor protection is not a statement that nothing is mapped
    // here: loading a module resets the whole page table of the heap it lands
    // in, so a title that loads several into one heap leaves earlier images
    // executing behind zeroed entries. The host mapping is what the guest
    // reads, so it is what decides.
    size_t readable_length = 0;
    xe::memory::PageAccess host_access = xe::memory::PageAccess::kNoAccess;
    if (!xe::memory::QueryProtect(const_cast<void*>(translated_page),
                                  readable_length, host_access) ||
        readable_length < JitCorpus::kPageSize ||
        host_access == xe::memory::PageAccess::kNoAccess) {
      last_read_failure_site_ = 7;
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
    if (!ValidateGuestInvocationCaptureCodePageClosure(
            result.translation_dependencies, result.code_pages,
            replay_config.host_protection_page_size, error)) {
      return false;
    }

    ExecutionJitCorpusBuilder corpus_builder(jit_corpus_config_flags);
    for (const ppc::GuestInvocationPage& code_page : result.code_pages) {
      if (!corpus_builder.AddCodePage(code_page.guest_address,
                                      code_page.data.data(),
                                      code_page.data.size(), error)) {
        return false;
      }
    }
    uint32_t root_definition_count = 0;
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
      // A host-backed call target is reached through a handler, so replay
      // needs its declaration but never its bytes. Refusing one here would
      // refuse every function with a lock or a kernel call on any path,
      // whether or not the recorded invocation took that path.
      if (function->behavior() == Function::Behavior::kExtern ||
          function->behavior() == Function::Behavior::kBuiltin) {
        const ExecutionJitCorpus::DeclarationRecord declaration = {
            function->address(), function->end_address(),
            JitCorpus::EncodeFunctionFlags(*function)};
        if (!corpus_builder.AddDeclaration(declaration, error)) {
          return false;
        }
        continue;
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

    // The backend reproduces a save/restore helper by inlining its metadata,
    // so replay needs the loader's declaration but never the helper's bytes.
    // Admission is keyed on that metadata rather than on the target lacking a
    // definition: every extern and builtin the registry declared is also
    // undefined, and only this predicate keeps them out.
    for (uint32_t address : result.declared_only_dependencies) {
      Function* function = processor.LookupFunction(address);
      if (!function || !function->is_guest() ||
          (!function->IsSaverest() &&
           function->behavior() != Function::Behavior::kExtern &&
           function->behavior() != Function::Behavior::kBuiltin)) {
        continue;
      }
      const ExecutionJitCorpus::DeclarationRecord declaration_record = {
          function->address(), function->end_address(),
          JitCorpus::EncodeFunctionFlags(*function)};
      if (!corpus_builder.AddDeclaration(declaration_record, error)) {
        return false;
      }
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
  if (!config.ValidateOutputDirectory(error)) {
    return nullptr;
  }

  GuestInvocationReplayConfig replay_config;
  if (!CaptureCurrentGuestInvocationReplayConfig(*processor.backend(),
                                                 &replay_config, error) ||
      !ValidateGuestInvocationReplayBenchmarkConfig(replay_config, error)) {
    return nullptr;
  }
  ppc::GuestInvocationRecorderSelection selection;
  ppc::GuestInvocationRecorderLimits limits;
  if (!config.BuildRecorderConfiguration(
          Clock::QueryHostTickFrequency(),
          replay_config.host_protection_page_size, &selection, &limits,
          error)) {
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
  // Before the sink goes live, so a function already running cannot report an
  // entry against a closure that has not been told the function exists.
  processor.ReplayGuestInvocationCaptureTranslationHistory(
      runtime->impl_->coordinator.get(), selection.root_address);
  // Replaying a whole run's history at once leaves the code-page snapshots
  // that were contended outstanding, and a function entry rejects while any
  // remain. Nothing can enter until the sink is attached below, so settle them
  // here rather than leaving the first entry to lose the race.
  for (int attempt = 0;
       attempt < 200 &&
       runtime->impl_->coordinator->HasPendingDefinitionSnapshots();
       ++attempt) {
    if (!runtime->impl_->coordinator->Poll()) {
      break;
    }
    xe::threading::MaybeYield();
  }
  if (!processor.TrySetGuestInvocationCaptureSink(
          nullptr, runtime->impl_->coordinator.get())) {
    Fail(error, "capture event sink was registered during startup");
    return nullptr;
  }
  runtime->impl_->attached = true;
  runtime->impl_->deadline_poller =
      GuestInvocationCaptureDeadlinePoller::Create(
          *runtime->impl_->coordinator,
          GuestInvocationCaptureDeadlinePoller::kDefaultInterval, error,
          [impl_pointer] {
            impl_pointer->processor.DisableGuestInvocationCaptureEventsForSink(
                impl_pointer->coordinator.get());
          });
  if (!runtime->impl_->deadline_poller) {
    processor.TrySetGuestInvocationCaptureSink(
        runtime->impl_->coordinator.get(), nullptr);
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
    if (!impl_->processor.TrySetGuestInvocationCaptureSink(
            impl_->coordinator.get(), nullptr)) {
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
