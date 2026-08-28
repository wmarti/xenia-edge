/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_capture_provider.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_capture_page_reader.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

namespace {

constexpr uint32_t kGuestPageSize = JitCorpus::kPageSize;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool IsPowerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }

bool IsSupportedPageAddress(uint32_t address) {
  return (address >= 0x00001000u && address <= 0x7EFFF000u) ||
         (address >= 0x80000000u && address <= 0x9FFFF000u);
}

uint32_t BackingPageAddress(uint32_t address) {
  return address >= 0x90000000u && address < 0xA0000000u ? address - 0x10000000u
                                                         : address;
}

bool HasWriteAccess(ppc::GuestInvocationRecorderMemoryAccess access) {
  return access == ppc::GuestInvocationRecorderMemoryAccess::kWrite ||
         access == ppc::GuestInvocationRecorderMemoryAccess::kReadWrite;
}

bool IsValidAccess(ppc::GuestInvocationRecorderMemoryAccess access) {
  return access == ppc::GuestInvocationRecorderMemoryAccess::kRead ||
         access == ppc::GuestInvocationRecorderMemoryAccess::kWrite ||
         access == ppc::GuestInvocationRecorderMemoryAccess::kReadWrite;
}

bool IsValidFunctionExtent(uint32_t address, uint32_t end_address) {
  return address && !(address & 3) && !(end_address & 3) &&
         end_address >= address &&
         uint64_t(end_address) - address + 4 <=
             ExecutionJitCorpus::kMaxFunctionSize;
}

using ProviderState = GuestExecutionSessionCaptureProviderState;
using CheckpointParticipant = kernel::GuestSchedulerCheckpointParticipant;
using CheckpointSnapshot = kernel::GuestSchedulerCheckpointBarrierSnapshot;

bool IsCatalogPhase(ProviderState state) {
  return state == ProviderState::kCataloging ||
         state == ProviderState::kRecording;
}

bool IsExecutionPhase(ProviderState state) {
  return state == ProviderState::kRecording;
}

bool IsRestorableJitParticipant(const CheckpointParticipant& participant) {
  return participant.restorable && participant.guest_pc &&
         !(participant.guest_pc & 3) &&
         participant.resume_kind ==
             kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint;
}

bool IsPassiveOutsideGuestParticipant(
    const CheckpointParticipant& participant) {
  if (participant.restorable || participant.guest_pc ||
      (participant.state !=
           kernel::GuestSchedulerCheckpointParticipantState::kReady &&
       participant.state !=
           kernel::GuestSchedulerCheckpointParticipantState::kSuspended)) {
    return false;
  }
  return participant.resume_kind ==
             kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation ||
         participant.resume_kind ==
             kernel::GuestSchedulerCheckpointResumeKind::kNotYetRun;
}

// The shape ValidateSchedulerTopology already accepts for a blocked-in-export
// route: a non-restorable participant parked at the link register of the
// branch that entered the export thunk.
bool IsBlockedExportParticipant(const CheckpointParticipant& participant) {
  return !participant.restorable && participant.guest_pc &&
         !(participant.guest_pc & 3) &&
         participant.state ==
             kernel::GuestSchedulerCheckpointParticipantState::kBlocked &&
         participant.resume_kind ==
             kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport;
}

// The wait classes whose link register is the export's single return point,
// matching the modeled export dispatch adapter's own allowlist. A delay, a
// fence or spin poll and the I/O classes reach that link register once per poll
// rather than once per export, and an alertable or APC-pending wait can run
// guest code on the waiting thread's stack before the export returns.
bool IsBlockedExportWaitInAllowlist(const CheckpointParticipant& participant) {
  const bool modeled_kind =
      participant.blocked_wait_kind ==
          kernel::GuestSchedulerCaptureWaitKind::kSingle ||
      participant.blocked_wait_kind ==
          kernel::GuestSchedulerCaptureWaitKind::kMultiAny ||
      participant.blocked_wait_kind ==
          kernel::GuestSchedulerCaptureWaitKind::kMultiAll;
  const uint8_t refused_flags =
      kernel::kGuestSchedulerCaptureWaitFlagAlertable |
      kernel::kGuestSchedulerCaptureWaitFlagUserApcPending;
  return modeled_kind && participant.blocked_wait.handle_count &&
         !(participant.blocked_wait.flags & refused_flags);
}

std::string DescribeParticipant(const CheckpointParticipant& participant) {
  return fmt::format(
      "tid={:08X} state={} resume_kind={} restorable={} pc={:08X}",
      participant.thread_id, static_cast<uint32_t>(participant.state),
      static_cast<uint32_t>(participant.resume_kind), participant.restorable,
      participant.guest_pc);
}

// kind/handles/flags/handle0/deadline, the same shape the capture runtime's
// rejection census prints, so one reader parses both diagnostics.
std::string DescribeParticipantWaitShape(
    const CheckpointParticipant& participant) {
  return fmt::format(
      "wait={}/{}/{}/{:08X}/{}",
      static_cast<uint32_t>(participant.blocked_wait_kind),
      static_cast<uint32_t>(participant.blocked_wait.handle_count),
      static_cast<uint32_t>(participant.blocked_wait.flags),
      participant.blocked_wait.handles[0],
      participant.blocked_wait.deadline_ms);
}

std::string DescribeOpenExportDispatch(
    bool log_installed, bool log_usable,
    const std::vector<GuestExecutionCaptureExternalEventActiveCall>& calls) {
  if (!log_installed) {
    return "export=nolog";
  }
  if (!log_usable) {
    return "export=rejected";
  }
  const GuestExecutionCaptureExternalEventActiveCall* outermost = nullptr;
  for (const GuestExecutionCaptureExternalEventActiveCall& call : calls) {
    if (!outermost || call.participant_depth < outermost->participant_depth) {
      outermost = &call;
    }
  }
  if (!outermost) {
    return "export=none";
  }
  return fmt::format("export={}/{:08X}/{:08X}", outermost->export_ordinal,
                     outermost->guest_address, outermost->call_site_address);
}

}  // namespace

struct GuestExecutionSessionCaptureProvider::Impl {
  struct DefinitionRecord {
    uint32_t end_address = 0;
    uint32_t definition_order = 0;
    bool defined = false;
    bool code_pages_snapshotted = false;
    std::set<uint32_t> dependencies;
    std::vector<uint32_t> code_pages;
  };

  struct CapturedThreadState {
    GuestExecutionCaptureParticipantIdentity participant;
    ppc::GuestInvocationRecorderIdentity invocation_identity;
    ppc::GuestPPCRegisterState registers;
  };

  struct PendingExportCheckpoint {
    ppc::GuestPPCThreadCheckpoint state;
    GuestExecutionCaptureExternalEventToken token;
  };

  struct InstructionCounter {
    GuestExecutionCaptureParticipantIdentity participant;
  };

  struct DataPageUse {
    uint64_t first_participant = 0;
    bool shared = false;
    bool written = false;
  };

  enum class CodeReadResult {
    kSuccess,
    kRetry,
    kFailure,
  };

  class ThreadStateVisitor final
      : public GuestExecutionCaptureThreadStateVisitor {
   public:
    explicit ThreadStateVisitor(std::vector<CapturedThreadState>* output)
        : output_(output) {}

    bool VisitThreadState(const ThreadState& thread_state) noexcept override {
      try {
        ppc::PPCContext* context = thread_state.context();
        if (!context || !thread_state.guest_execution_capture_instance_id() ||
            !thread_state.thread_id() ||
            context->thread_id != thread_state.thread_id()) {
          invalid_ = true;
          return false;
        }
        output_->push_back(
            {{thread_state.guest_execution_capture_instance_id(),
              thread_state.thread_id()},
             {reinterpret_cast<uintptr_t>(context), context->thread_id},
             ppc::CaptureGuestPPCRegisterState(*context)});
        return true;
      } catch (...) {
        allocation_failed_ = true;
        return false;
      }
    }

    bool invalid() const { return invalid_; }
    bool allocation_failed() const { return allocation_failed_; }

   private:
    std::vector<CapturedThreadState>* output_ = nullptr;
    bool invalid_ = false;
    bool allocation_failed_ = false;
  };

  class InstructionCounterArmVisitor final
      : public GuestExecutionCaptureThreadStateVisitor {
   public:
    explicit InstructionCounterArmVisitor(
        std::span<const InstructionCounter> counters,
        std::span<ppc::PPCContext*> contexts)
        : counters_(counters), contexts_(contexts) {
      std::fill(contexts_.begin(), contexts_.end(), nullptr);
    }

    bool VisitThreadState(const ThreadState& thread_state) noexcept override {
      ppc::PPCContext* context = thread_state.context();
      const GuestExecutionCaptureParticipantIdentity participant = {
          thread_state.guest_execution_capture_instance_id(),
          thread_state.thread_id(),
      };
      const auto counter = std::lower_bound(
          counters_.begin(), counters_.end(), participant.capture_instance_id,
          [](const InstructionCounter& candidate, uint64_t instance_id) {
            return candidate.participant.capture_instance_id < instance_id;
          });
      if (!context || counter == counters_.end() ||
          counter->participant != participant ||
          context->thread_id != participant.guest_thread_id) {
        return false;
      }
      const size_t index = size_t(counter - counters_.begin());
      if (contexts_[index]) {
        return false;
      }
      contexts_[index] = context;
      return true;
    }

    bool CompleteVisit() noexcept override {
      for (ppc::PPCContext* context : contexts_) {
        if (!context ||
            std::atomic_ref<uint64_t*>(
                context->guest_execution_session_instruction_counter)
                .load(std::memory_order_acquire)) {
          return false;
        }
      }
      for (ppc::PPCContext* context : contexts_) {
        std::atomic_ref<uint64_t>(
            context->guest_execution_session_instruction_count)
            .store(0, std::memory_order_relaxed);
        std::atomic_ref<uint64_t*>(
            context->guest_execution_session_instruction_counter)
            .store(&context->guest_execution_session_instruction_count,
                   std::memory_order_release);
      }
      std::fill(contexts_.begin(), contexts_.end(), nullptr);
      return true;
    }

    void CancelVisit() noexcept override {
      std::fill(contexts_.begin(), contexts_.end(), nullptr);
    }

   private:
    std::span<const InstructionCounter> counters_;
    std::span<ppc::PPCContext*> contexts_;
  };

  class InstructionCounterDrainVisitor final
      : public GuestExecutionCaptureThreadStateVisitor {
   public:
    InstructionCounterDrainVisitor(
        std::span<const InstructionCounter> counters,
        std::span<ppc::PPCContext*> contexts,
        std::vector<GuestExecutionSessionInstructionCoverageDelta>* output,
        bool disarm, bool require_exact_roster, bool require_drained)
        : counters_(counters),
          contexts_(contexts),
          output_(output),
          disarm_(disarm),
          require_exact_roster_(require_exact_roster),
          require_drained_(require_drained) {
      std::fill(contexts_.begin(), contexts_.end(), nullptr);
    }

    bool VisitThreadState(const ThreadState& thread_state) noexcept override {
      ppc::PPCContext* context = thread_state.context();
      const GuestExecutionCaptureParticipantIdentity participant = {
          thread_state.guest_execution_capture_instance_id(),
          thread_state.thread_id(),
      };
      const auto counter = std::lower_bound(
          counters_.begin(), counters_.end(), participant.capture_instance_id,
          [](const InstructionCounter& candidate, uint64_t instance_id) {
            return candidate.participant.capture_instance_id < instance_id;
          });
      if (counter == counters_.end() || counter->participant != participant) {
        return !require_exact_roster_;
      }
      const size_t index = size_t(counter - counters_.begin());
      if (!context || context->thread_id != participant.guest_thread_id ||
          contexts_[index]) {
        return false;
      }
      contexts_[index] = context;
      return true;
    }

    bool CompleteVisit() noexcept override {
      bool valid = true;
      for (ppc::PPCContext*& context : contexts_) {
        if (!context) {
          if (require_exact_roster_) {
            std::fill(contexts_.begin(), contexts_.end(), nullptr);
            return false;
          }
          continue;
        }
        uint64_t* expected =
            &context->guest_execution_session_instruction_count;
        if (std::atomic_ref<uint64_t*>(
                context->guest_execution_session_instruction_counter)
                .load(std::memory_order_acquire) != expected) {
          if (require_exact_roster_) {
            std::fill(contexts_.begin(), contexts_.end(), nullptr);
            return false;
          }
          context = nullptr;
          valid = false;
        }
      }
      bool drained = true;
      for (size_t index = 0; index < contexts_.size(); ++index) {
        ppc::PPCContext* context = contexts_[index];
        if (!context) {
          continue;
        }
        if (disarm_) {
          std::atomic_ref<uint64_t*>(
              context->guest_execution_session_instruction_counter)
              .store(nullptr, std::memory_order_release);
        }
        const uint64_t delta =
            std::atomic_ref<uint64_t>(
                context->guest_execution_session_instruction_count)
                .exchange(0, std::memory_order_acq_rel);
        drained = drained && (!require_drained_ || !delta);
        if (output_ && delta) {
          output_->push_back({counters_[index].participant, delta});
        }
      }
      std::fill(contexts_.begin(), contexts_.end(), nullptr);
      return valid && drained;
    }

    void CancelVisit() noexcept override {
      std::fill(contexts_.begin(), contexts_.end(), nullptr);
    }

   private:
    std::span<const InstructionCounter> counters_;
    std::span<ppc::PPCContext*> contexts_;
    std::vector<GuestExecutionSessionInstructionCoverageDelta>* output_;
    bool disarm_ = false;
    bool require_exact_roster_ = false;
    bool require_drained_ = false;
  };

  Impl(Memory& memory_value, Processor& processor_value,
       GuestExecutionSessionCaptureProviderConfig config_value)
      : processor(processor_value),
        config(config_value),
        page_reader(memory_value) {}

  bool RejectLocked(std::string_view message) {
    if (state.load(std::memory_order_relaxed) != ProviderState::kRejected) {
      rejection_message.assign(message);
      state.store(ProviderState::kRejected, std::memory_order_release);
    }
    return false;
  }

  bool RejectException() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      return RejectLocked(
          "capture provider allocation or internal operation "
          "threw an exception");
    } catch (...) {
      state.store(ProviderState::kRejected, std::memory_order_release);
      return false;
    }
  }

  bool RejectExternal(std::string_view message) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      return RejectLocked(message);
    } catch (...) {
      state.store(ProviderState::kRejected, std::memory_order_release);
      return false;
    }
  }

  template <typename Callback>
  bool InvokeCallback(Callback&& callback) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      return callback();
    } catch (...) {
      return RejectException();
    }
  }

  bool SetErrorFromRejection(std::string* error) const {
    if (error) {
      error->assign(rejection_message.empty()
                        ? "capture provider rejected without a diagnostic"
                        : rejection_message);
    }
    return false;
  }

  bool CollectProtectionPages(uint32_t first_page, uint32_t last_page,
                              std::vector<uint32_t>* output) {
    output->clear();
    const uint32_t mask = ~(config.host_protection_page_size - 1);
    const uint32_t first_granule = first_page & mask;
    const uint32_t last_granule = last_page & mask;
    const uint64_t end =
        uint64_t(last_granule) + config.host_protection_page_size;
    if (end > uint64_t(std::numeric_limits<uint32_t>::max()) + 1) {
      return RejectLocked(
          "capture provider host protection granule wraps guest memory");
    }
    for (uint64_t page = first_granule; page < end; page += kGuestPageSize) {
      const uint32_t address = static_cast<uint32_t>(page);
      if (!IsSupportedPageAddress(address)) {
        return RejectLocked(
            "capture provider protection granule uses unsupported memory");
      }
      output->push_back(address);
    }
    return true;
  }

  CodeReadResult ReadStableCodePage(
      uint32_t page_address, std::array<uint8_t, kGuestPageSize>* output) {
    std::array<uint8_t, kGuestPageSize> verification = {};
    const bool first = page_reader.ReadPage(page_address, output);
    if (!first && page_reader.last_read_was_retryable()) {
      return CodeReadResult::kRetry;
    }
    const bool second =
        first && page_reader.ReadPage(page_address, &verification);
    if (!second && page_reader.last_read_was_retryable()) {
      return CodeReadResult::kRetry;
    }
    if (!first || !second) {
      RejectLocked("capture provider could not read a guest code page");
      return CodeReadResult::kFailure;
    }
    if (*output != verification) {
      RejectLocked("capture provider guest code changed while sampled");
      return CodeReadResult::kFailure;
    }
    return CodeReadResult::kSuccess;
  }

  CodeReadResult SnapshotDefinitionLocked(DefinitionRecord& definition) {
    std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> snapshots;
    for (uint32_t page_address : definition.code_pages) {
      if (written_backing_pages.contains(BackingPageAddress(page_address))) {
        RejectLocked(
            "capture provider guest code was written before definition");
        return CodeReadResult::kFailure;
      }
      std::array<uint8_t, kGuestPageSize> page = {};
      const CodeReadResult result = ReadStableCodePage(page_address, &page);
      if (result != CodeReadResult::kSuccess) {
        return result;
      }
      const auto existing = immutable_code_pages.find(page_address);
      if (existing != immutable_code_pages.cend() && existing->second != page) {
        RejectLocked(
            "capture provider shared code page changed across definitions");
        return CodeReadResult::kFailure;
      }
      snapshots.emplace(page_address, std::move(page));
    }
    for (auto& [address, page] : snapshots) {
      immutable_code_pages.emplace(address, std::move(page));
    }
    definition.code_pages_snapshotted = true;
    return CodeReadResult::kSuccess;
  }

  bool RetryPendingDefinitionsLocked(bool final_attempt) {
    for (auto it = pending_definition_snapshots.begin();
         it != pending_definition_snapshots.end();) {
      DefinitionRecord& definition = definitions.at(*it);
      const CodeReadResult result = SnapshotDefinitionLocked(definition);
      if (result == CodeReadResult::kRetry) {
        if (final_attempt) {
          return RejectLocked(
              "capture provider code snapshot remained contended at a "
              "checkpoint");
        }
        return true;
      }
      if (result == CodeReadResult::kFailure) {
        return false;
      }
      it = pending_definition_snapshots.erase(it);
    }
    return true;
  }

  bool EnsureDefinitionLocked(uint32_t address, DefinitionRecord** output) {
    if (!address || (address & 3)) {
      return RejectLocked(
          "capture provider catalog has an invalid function address");
    }
    auto existing = definitions.find(address);
    if (existing != definitions.end()) {
      *output = &existing->second;
      return true;
    }
    if (definitions.size() >= config.maximum_function_count) {
      return RejectLocked(
          "capture provider function catalog exceeds its configured limit");
    }
    auto [inserted, did_insert] =
        definitions.emplace(address, DefinitionRecord{});
    if (!did_insert) {
      return RejectLocked("capture provider function catalog insertion failed");
    }
    *output = &inserted->second;
    return true;
  }

  bool RegisterDependencyLocked(uint32_t source_address,
                                uint32_t dependency_address) {
    DefinitionRecord* source = nullptr;
    DefinitionRecord* dependency = nullptr;
    if (!EnsureDefinitionLocked(source_address, &source) ||
        !EnsureDefinitionLocked(dependency_address, &dependency)) {
      return false;
    }
    if (source->defined) {
      return RejectLocked(
          "capture provider dependency arrived after source definition");
    }
    if (!source->dependencies.contains(dependency_address)) {
      if (dependency_count >= config.maximum_dependency_count) {
        return RejectLocked(
            "capture provider dependency catalog exceeds its limit");
      }
      source->dependencies.insert(dependency_address);
      ++dependency_count;
    }
    return true;
  }

  bool RegisterDefinitionLocked(uint32_t address, uint32_t end_address) {
    if (!IsValidFunctionExtent(address, end_address)) {
      return RejectLocked(
          "capture provider definition has an invalid function extent");
    }
    DefinitionRecord* definition = nullptr;
    if (!EnsureDefinitionLocked(address, &definition)) {
      return false;
    }
    if (definition->defined) {
      return RejectLocked(
          "capture provider received a duplicate successful definition");
    }
    std::vector<uint32_t> pages;
    if (!CollectProtectionPages(address & ~(kGuestPageSize - 1),
                                end_address & ~(kGuestPageSize - 1), &pages)) {
      return false;
    }
    size_t new_page_count = 0;
    for (uint32_t page : pages) {
      if (!catalog_code_pages.contains(page)) {
        ++new_page_count;
      }
    }
    if (catalog_code_pages.size() > config.maximum_code_page_count ||
        new_page_count >
            config.maximum_code_page_count - catalog_code_pages.size()) {
      return RejectLocked(
          "capture provider code-page catalog exceeds its configured limit");
    }
    catalog_code_pages.insert(pages.cbegin(), pages.cend());
    definition->defined = true;
    definition->end_address = end_address;
    definition->definition_order =
        static_cast<uint32_t>(definition_order.size());
    definition->code_pages = std::move(pages);
    definition_order.push_back(address);
    const CodeReadResult result = SnapshotDefinitionLocked(*definition);
    if (result == CodeReadResult::kRetry) {
      pending_definition_snapshots.insert(address);
      return true;
    }
    return result == CodeReadResult::kSuccess;
  }

  bool AddClosureSeedLocked(uint32_t address, uint32_t end_address) {
    const auto definition = definitions.find(address);
    if (definition == definitions.cend() || !definition->second.defined ||
        definition->second.end_address != end_address) {
      return RejectLocked(
          "capture provider execution references an uncataloged function "
          "extent");
    }
    const auto existing = closure_seeds.find(address);
    if (existing != closure_seeds.cend() && existing->second != end_address) {
      return RejectLocked(
          "capture provider execution reported conflicting function extents");
    }
    closure_seeds.emplace(address, end_address);
    return true;
  }

  const DefinitionRecord* FindOwningDefinitionLocked(uint32_t guest_pc) {
    const DefinitionRecord* owner = nullptr;
    for (const auto& [address, definition] : definitions) {
      if (!definition.defined || guest_pc < address ||
          guest_pc > definition.end_address) {
        continue;
      }
      if (owner) {
        RejectLocked(
            "capture provider checkpoint PC has overlapping catalog owners");
        return nullptr;
      }
      owner = &definition;
    }
    if (!owner) {
      RejectLocked(
          "capture provider checkpoint PC has no successful definition");
    }
    return owner;
  }

  uint32_t FindDefinitionAddressLocked(const DefinitionRecord* target) const {
    for (const auto& [address, definition] : definitions) {
      if (&definition == target) {
        return address;
      }
    }
    return 0;
  }

  const CheckpointParticipant* FindCheckpointParticipant(
      const CheckpointSnapshot& checkpoint, uint32_t guest_thread_id) const {
    const CheckpointParticipant* result = nullptr;
    for (const CheckpointParticipant& participant : checkpoint.participants) {
      if (participant.thread_id != guest_thread_id) {
        continue;
      }
      if (result) {
        return nullptr;
      }
      result = &participant;
    }
    return result;
  }

  bool FindOuterReturn(
      const GuestExecutionCaptureParticipantIdentity& participant,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      uint32_t* output) {
    uint32_t outer_return = 0;
    size_t outer_count = 0;
    for (const GuestExecutionCaptureActiveHostCall& call :
         host_calls.active_calls) {
      if (call.participant == participant && call.participant_depth == 1) {
        outer_return = call.return_address;
        ++outer_count;
      }
    }
    if (outer_count != 1 || !outer_return || (outer_return & 3)) {
      return RejectLocked(
          "capture provider checkpoint lacks one outer host-call boundary");
    }
    *output = outer_return;
    return true;
  }

  // Binds a blocked participant to the single open modeled export dispatch it
  // is parked inside. The log is the one the capture runtime installed on this
  // Processor, so the provider and the export shims always see the same open
  // calls. Every refusal is a first-failure diagnostic in the census style; the
  // participant stays rejected exactly as it is today unless all of it holds.
  bool BindBlockedExportDispatch(
      const CheckpointParticipant& participant,
      GuestExecutionCaptureExternalEventActiveCall* output,
      std::string* reason) const {
    // Queried before the first refusal so every refusal names the wait it
    // refused and the dispatch it did or did not find, which is what separates
    // an unwired wait from a wired one that failed a later check.
    const std::shared_ptr<GuestExecutionCaptureExternalEventLog> log =
        processor.guest_execution_capture_external_event_log();
    std::vector<GuestExecutionCaptureExternalEventActiveCall> calls;
    const bool log_usable =
        log &&
        log->CopyParticipantActiveCalls(
            {participant.capture_instance_id, participant.thread_id}, &calls);
    const auto refuse = [&](std::string_view text) {
      if (reason) {
        reason->assign(text);
        reason->append(": ");
        reason->append(DescribeParticipant(participant));
        reason->push_back(' ');
        reason->append(
            DescribeOpenExportDispatch(log != nullptr, log_usable, calls));
        reason->push_back(' ');
        reason->append(DescribeParticipantWaitShape(participant));
      }
      return false;
    };
    if (!IsBlockedExportWaitInAllowlist(participant)) {
      return refuse(
          "capture provider blocked participant is outside the modeled "
          "blocking-export wait allowlist");
    }
    if (!log) {
      return refuse(
          "capture provider has no installed modeled export event log");
    }
    if (!log_usable) {
      return refuse(
          "capture provider modeled export event log is not replayable");
    }
    if (calls.empty()) {
      return refuse(
          "capture provider blocked participant has no open modeled export "
          "dispatch");
    }
    if (calls.size() != 1) {
      return refuse(
          "capture provider blocked participant has more than one open "
          "modeled export dispatch");
    }
    const GuestExecutionCaptureExternalEventActiveCall& call = calls.front();
    if (call.kind != GuestExecutionSessionEventKind::kKernelExport ||
        !call.is_outermost()) {
      return refuse(
          "capture provider blocked participant is not parked in an outermost "
          "modeled kernel export");
    }
    // A modeled blocking wait always declares the guest thread fields it
    // writes, so a dispatch with no declared effect cannot be one.
    if (!call.guest_address || (call.guest_address & 3) ||
        !call.effect_byte_count) {
      return refuse(
          "capture provider modeled export dispatch has no bindable export "
          "identity");
    }
    if (call.call_site_address != participant.guest_pc) {
      return refuse(
          "capture provider blocked participant PC differs from its modeled "
          "export return point");
    }
    if (output) {
      *output = call;
    }
    return true;
  }

  bool CaptureThreadStates(size_t expected_count,
                           std::vector<CapturedThreadState>* output,
                           std::string* error) noexcept {
    try {
      output->clear();
      output->reserve(expected_count);
      ThreadStateVisitor visitor(output);
      const GuestExecutionCaptureThreadStateVisitResult result =
          processor.VisitGuestExecutionCaptureThreadStates(visitor);
      if (result != GuestExecutionCaptureThreadStateVisitResult::kCompleted ||
          visitor.invalid() || visitor.allocation_failed() ||
          output->size() != expected_count) {
        return Fail(error,
                    "capture provider could not snapshot the exact ready "
                    "ThreadState roster");
      }
      return true;
    } catch (...) {
      return Fail(error,
                  "capture provider ThreadState snapshot allocation failed");
    }
  }

  bool ArmInstructionCountersLocked(
      const std::vector<CapturedThreadState>& captured) {
    std::vector<InstructionCounter> counters;
    counters.reserve(captured.size());
    for (const CapturedThreadState& thread : captured) {
      if (!initial_states.contains(thread.participant.capture_instance_id)) {
        return RejectLocked(
            "capture provider cannot arm a participant instruction counter");
      }
      counters.push_back({thread.participant});
    }
    std::sort(counters.begin(), counters.end(),
              [](const auto& left, const auto& right) {
                return left.participant.capture_instance_id <
                       right.participant.capture_instance_id;
              });
    for (size_t index = 0; index < counters.size(); ++index) {
      if (index && counters[index - 1].participant.capture_instance_id ==
                       counters[index].participant.capture_instance_id) {
        return RejectLocked(
            "capture provider instruction counter roster is not unique");
      }
    }
    std::vector<ppc::PPCContext*> context_scratch(counters.size(), nullptr);
    InstructionCounterArmVisitor visitor(counters, context_scratch);
    if (processor.VisitGuestExecutionCaptureThreadStates(visitor) !=
        GuestExecutionCaptureThreadStateVisitResult::kCompleted) {
      return RejectLocked(
          "capture provider could not atomically arm the exact instruction "
          "counter roster");
    }
    instruction_counters = std::move(counters);
    instruction_counter_context_scratch = std::move(context_scratch);
    return true;
  }

  bool DisarmInstructionCountersLocked(bool require_drained) {
    if (instruction_counters.empty()) {
      return true;
    }
    InstructionCounterDrainVisitor visitor(
        instruction_counters, instruction_counter_context_scratch, nullptr,
        true, require_drained, require_drained);
    const GuestExecutionCaptureThreadStateVisitResult result =
        require_drained
            ? processor.VisitGuestExecutionCaptureThreadStates(visitor)
            : processor.VisitReadyGuestExecutionCaptureThreadStates(visitor);
    if (require_drained &&
        result != GuestExecutionCaptureThreadStateVisitResult::kCompleted) {
      InstructionCounterDrainVisitor cleanup_visitor(
          instruction_counters, instruction_counter_context_scratch, nullptr,
          true, false, false);
      processor.VisitReadyGuestExecutionCaptureThreadStates(cleanup_visitor);
    }
    instruction_counters.clear();
    instruction_counter_context_scratch.clear();
    if (result != GuestExecutionCaptureThreadStateVisitResult::kCompleted) {
      return RejectLocked(require_drained
                              ? "capture provider sealed before the exact "
                                "instruction counter roster was drained"
                              : "capture provider could not safely disarm all "
                                "live instruction counters");
    }
    return true;
  }

  bool BuildCheckpointStatesLocked(
      const CheckpointSnapshot& checkpoint,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          participants,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      const std::vector<CapturedThreadState>& captured,
      std::map<uint64_t, std::vector<uint8_t>>* output,
      bool establish_execution_identities) {
    if (host_calls.rejection !=
            GuestExecutionCaptureHostCallRosterRejection::kNone ||
        participants.empty() ||
        checkpoint.participants.size() > participants.size() ||
        captured.size() != participants.size()) {
      return RejectLocked(
          "capture provider checkpoint inputs have inconsistent rosters");
    }
    std::vector<GuestExecutionCaptureParticipantIdentity> ordered;
    ordered.reserve(participants.size());
    for (const auto& lifecycle : participants) {
      if (lifecycle.state !=
          GuestExecutionCaptureThreadStateLifecycleState::kReady) {
        return RejectLocked(
            "capture provider checkpoint contains a non-ready participant");
      }
      ordered.push_back(lifecycle.participant);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& left, const auto& right) {
                return left.capture_instance_id < right.capture_instance_id;
              });
    for (size_t index = 0; index < ordered.size(); ++index) {
      if (!ordered[index].capture_instance_id ||
          !ordered[index].guest_thread_id ||
          (index && (ordered[index - 1].capture_instance_id ==
                         ordered[index].capture_instance_id ||
                     ordered[index - 1].guest_thread_id ==
                         ordered[index].guest_thread_id))) {
        return RejectLocked(
            "capture provider checkpoint participant identity is invalid");
      }
    }

    std::map<uint64_t, std::vector<uint8_t>> encoded;
    std::map<uint64_t, PendingExportCheckpoint> pending_exports;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> execution_identities;
    std::map<uint64_t, uint32_t> outer_return_addresses;
    std::set<uint64_t> outside_guest_participants;
    for (size_t ordinal = 0; ordinal < ordered.size(); ++ordinal) {
      const auto& identity = ordered[ordinal];
      const auto thread = std::find_if(
          captured.cbegin(), captured.cend(), [&](const auto& candidate) {
            return candidate.participant == identity;
          });
      if (thread == captured.cend() ||
          thread->invocation_identity.thread_id != identity.guest_thread_id ||
          !thread->invocation_identity.context_id) {
        return RejectLocked(
            "capture provider scheduler and ThreadState identities differ");
      }

      ppc::GuestPPCThreadCheckpoint state_blob;
      state_blob.participant_ordinal = static_cast<uint32_t>(ordinal);
      state_blob.guest_thread_id = identity.guest_thread_id;
      state_blob.registers = thread->registers;
      const CheckpointParticipant* scheduler_participant =
          FindCheckpointParticipant(checkpoint, identity.guest_thread_id);
      if (scheduler_participant) {
        if (scheduler_participant->capture_instance_id !=
            identity.capture_instance_id) {
          return RejectLocked(
              "capture provider scheduler and ThreadState identities differ");
        }
        if (IsPassiveOutsideGuestParticipant(*scheduler_participant)) {
          if (std::any_of(host_calls.active_calls.cbegin(),
                          host_calls.active_calls.cend(),
                          [&](const auto& call) {
                            return call.participant == identity;
                          })) {
            return RejectLocked(
                "capture provider cannot encode a passive scheduler "
                "participant with an active host call");
          }
          state_blob.resume_kind = ppc::GuestPPCThreadResumeKind::kOutsideGuest;
          outside_guest_participants.insert(identity.capture_instance_id);
        } else if (IsBlockedExportParticipant(*scheduler_participant)) {
          // Only the start boundary can carry this route. A participant still
          // parked at the final boundary waits for an export that returns after
          // the interval, so no captured event can ever witness it.
          if (!establish_execution_identities) {
            return RejectLocked(
                "capture provider cannot encode a blocked modeled export at "
                "the final boundary: " +
                DescribeParticipant(*scheduler_participant));
          }
          GuestExecutionCaptureExternalEventActiveCall dispatch;
          std::string reason;
          if (!BindBlockedExportDispatch(*scheduler_participant, &dispatch,
                                         &reason)) {
            return RejectLocked(reason);
          }
          uint32_t outer_return = 0;
          if (!FindOuterReturn(identity, host_calls, &outer_return)) {
            return false;
          }
          outer_return_addresses.emplace(identity.capture_instance_id,
                                         outer_return);
          const DefinitionRecord* owner =
              FindOwningDefinitionLocked(scheduler_participant->guest_pc);
          if (!owner) {
            return false;
          }
          const uint32_t owner_address = FindDefinitionAddressLocked(owner);
          if (!owner_address || !owner->code_pages_snapshotted ||
              !AddClosureSeedLocked(owner_address, owner->end_address)) {
            return false;
          }
          state_blob.resume_kind =
              ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern;
          state_blob.resume_pc = scheduler_participant->guest_pc;
          state_blob.owning_function_address = owner_address;
          state_blob.owning_function_end_address = owner->end_address;
          state_blob.outer_guest_return_address = outer_return;
          state_blob.pending_export_guest_address = dispatch.guest_address;
          // Provisional, and never published: the dispatch is still open, so
          // its canonical event has no sequence yet. EncodeParticipantState
          // resolves the token and serves the only durable form.
          state_blob.pending_external_event_sequence = dispatch.token.value;
          pending_exports.emplace(
              identity.capture_instance_id,
              PendingExportCheckpoint{state_blob, dispatch.token});
        } else if (!IsRestorableJitParticipant(*scheduler_participant)) {
          return RejectLocked(
              "capture provider supports only exact-PC JIT, passive "
              "outside-guest or modeled blocking-export scheduler "
              "participants");
        } else {
          uint32_t outer_return = 0;
          if (!FindOuterReturn(identity, host_calls, &outer_return)) {
            return false;
          }
          outer_return_addresses.emplace(identity.capture_instance_id,
                                         outer_return);
          const DefinitionRecord* owner =
              FindOwningDefinitionLocked(scheduler_participant->guest_pc);
          if (!owner) {
            return false;
          }
          const uint32_t owner_address = FindDefinitionAddressLocked(owner);
          if (!owner_address || !owner->code_pages_snapshotted ||
              !AddClosureSeedLocked(owner_address, owner->end_address)) {
            return false;
          }
          state_blob.resume_kind =
              ppc::GuestPPCThreadResumeKind::kGuestBlockHead;
          state_blob.resume_pc = scheduler_participant->guest_pc;
          state_blob.owning_function_address = owner_address;
          state_blob.owning_function_end_address = owner->end_address;
          state_blob.outer_guest_return_address = outer_return;
        }
      } else {
        if (std::any_of(host_calls.active_calls.cbegin(),
                        host_calls.active_calls.cend(), [&](const auto& call) {
                          return call.participant == identity;
                        })) {
          return RejectLocked(
              "capture provider cannot encode an active host call as "
              "outside guest");
        }
        state_blob.resume_kind = ppc::GuestPPCThreadResumeKind::kOutsideGuest;
        outside_guest_participants.insert(identity.capture_instance_id);
      }
      std::vector<uint8_t> bytes;
      std::string error;
      if (!ppc::GuestPPCThreadCheckpointCodec::Encode(state_blob, &bytes,
                                                      &error)) {
        return RejectLocked("capture provider could not encode checkpoint: " +
                            error);
      }
      encoded.emplace(identity.capture_instance_id, std::move(bytes));
      execution_identities.emplace(
          std::pair(thread->invocation_identity.context_id,
                    thread->invocation_identity.thread_id),
          identity.capture_instance_id);
    }
    const size_t executable_scheduler_participant_count = std::count_if(
        checkpoint.participants.cbegin(), checkpoint.participants.cend(),
        [](const CheckpointParticipant& candidate) {
          return IsRestorableJitParticipant(candidate) ||
                 IsBlockedExportParticipant(candidate);
        });
    if (encoded.size() != ordered.size() ||
        execution_identities.size() != ordered.size() ||
        outer_return_addresses.size() !=
            executable_scheduler_participant_count) {
      return RejectLocked(
          "capture provider checkpoint identity mapping is not one-to-one");
    }
    if (establish_execution_identities) {
      active_invocation_identities = std::move(execution_identities);
      initial_outer_return_addresses = std::move(outer_return_addresses);
      initial_outside_guest_participants =
          std::move(outside_guest_participants);
      initial_pending_exports = std::move(pending_exports);
    } else if (execution_identities != active_invocation_identities ||
               outside_guest_participants !=
                   initial_outside_guest_participants) {
      return RejectLocked(
          "capture provider participant boundary class changed in-session");
    }
    if (!establish_execution_identities) {
      for (uint64_t participant : outside_guest_participants) {
        const auto initial = initial_states.find(participant);
        const auto final = encoded.find(participant);
        if (initial == initial_states.cend() || final == encoded.cend() ||
            initial->second != final->second) {
          return RejectLocked(
              "capture provider outside-guest participant changed at the "
              "boundary");
        }
      }
    }
    *output = std::move(encoded);
    return true;
  }

  bool IsKnownExecutionIdentityLocked(
      const ppc::GuestInvocationRecorderIdentity& identity,
      uint64_t* participant) {
    if (!identity.context_id || !identity.thread_id) {
      return RejectLocked(
          "capture provider callback has an invalid execution identity");
    }
    const auto found = active_invocation_identities.find(
        {identity.context_id, identity.thread_id});
    if (found == active_invocation_identities.cend()) {
      return RejectLocked(
          "capture provider callback came from outside the held roster");
    }
    if (initial_outside_guest_participants.contains(found->second)) {
      return RejectLocked(
          "capture provider outside-guest participant executed without a "
          "typed entry checkpoint");
    }
    *participant = found->second;
    return true;
  }

  bool RecordMemoryAccessLocked(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t size, ppc::GuestInvocationRecorderMemoryAccess access) {
    if (!IsExecutionPhase(state.load(std::memory_order_relaxed))) {
      return state.load(std::memory_order_relaxed) != ProviderState::kRejected;
    }
    uint64_t participant = 0;
    if (!IsKnownExecutionIdentityLocked(identity, &participant) || !size ||
        !IsValidAccess(access)) {
      return RejectLocked("capture provider memory callback is not canonical");
    }
    if (memory_access_count >= config.maximum_memory_access_count) {
      return RejectLocked(
          "capture provider memory access count exceeds its limit");
    }
    ++memory_access_count;
    const uint64_t last_byte = uint64_t(address) + size - 1;
    if (last_byte > std::numeric_limits<uint32_t>::max()) {
      return RejectLocked("capture provider memory access wraps guest memory");
    }
    const uint32_t first_page = address & ~(kGuestPageSize - 1);
    const uint32_t last_page =
        static_cast<uint32_t>(last_byte) & ~(kGuestPageSize - 1);
    for (uint64_t page = first_page; page <= last_page;
         page += kGuestPageSize) {
      if (!IsSupportedPageAddress(static_cast<uint32_t>(page))) {
        return RejectLocked(
            "capture provider memory access uses unsupported memory");
      }
    }

    std::vector<uint32_t> supplied_pages;
    if (!CollectProtectionPages(first_page, last_page, &supplied_pages)) {
      return false;
    }
    std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> new_pages;
    std::map<uint32_t, uint32_t> new_backing_views;
    for (uint32_t page : supplied_pages) {
      const uint32_t backing = BackingPageAddress(page);
      const auto known_view = data_backing_views.find(backing);
      const auto new_view = new_backing_views.find(backing);
      if ((known_view != data_backing_views.cend() &&
           known_view->second != page) ||
          (new_view != new_backing_views.cend() && new_view->second != page)) {
        return RejectLocked(
            "capture provider observed both physical alias views");
      }
      if (known_view == data_backing_views.cend()) {
        new_backing_views.emplace(backing, page);
      }
      if (initial_data_pages.contains(page)) {
        continue;
      }
      std::array<uint8_t, kGuestPageSize> bytes = {};
      if (!page_reader.ReadPage(page, &bytes)) {
        return RejectLocked(
            "capture provider could not snapshot a data-page preimage");
      }
      new_pages.emplace(page, std::move(bytes));
    }
    if (initial_data_pages.size() > config.maximum_data_page_count ||
        new_pages.size() >
            config.maximum_data_page_count - initial_data_pages.size()) {
      return RejectLocked(
          "capture provider data-page closure exceeds its configured limit");
    }
    for (auto& [page, bytes] : new_pages) {
      initial_data_pages.emplace(page, std::move(bytes));
    }
    for (const auto& [backing, view] : new_backing_views) {
      data_backing_views.emplace(backing, view);
    }

    const bool writes = HasWriteAccess(access);
    for (uint64_t page = first_page; page <= last_page;
         page += kGuestPageSize) {
      const uint32_t page_address = static_cast<uint32_t>(page);
      const uint32_t backing = BackingPageAddress(page_address);
      DataPageUse& use = data_page_uses[backing];
      if (use.first_participant && use.first_participant != participant) {
        use.shared = true;
      }
      if (use.shared && (use.written || writes)) {
        return RejectLocked(
            "capture provider cannot order a cross-participant shared write");
      }
      if (!use.first_participant) {
        use.first_participant = participant;
      }
      if (writes) {
        use.written = true;
        dirty_data_pages.insert(page_address);
        written_backing_pages.insert(backing);
      }
    }
    return true;
  }

  bool BuildClosureLocked(std::set<uint32_t>* closure) {
    if (closure_seeds.empty()) {
      return RejectLocked("capture provider has no exact-PC code-corpus seed");
    }
    std::vector<uint32_t> pending;
    for (const auto& [address, end_address] : closure_seeds) {
      const auto definition = definitions.find(address);
      if (definition == definitions.cend() || !definition->second.defined ||
          definition->second.end_address != end_address) {
        return RejectLocked(
            "capture provider closure seed is not an exact definition");
      }
      pending.push_back(address);
    }
    closure->clear();
    while (!pending.empty()) {
      const uint32_t address = pending.back();
      pending.pop_back();
      if (!closure->insert(address).second) {
        continue;
      }
      const auto definition = definitions.find(address);
      if (definition == definitions.cend() || !definition->second.defined ||
          !definition->second.code_pages_snapshotted) {
        return RejectLocked(
            "capture provider translation closure is incomplete");
      }
      pending.insert(pending.end(), definition->second.dependencies.cbegin(),
                     definition->second.dependencies.cend());
    }
    return true;
  }

  bool FinalizeContentAndCorpusLocked() {
    if (!RetryPendingDefinitionsLocked(true)) {
      return false;
    }
    std::set<uint32_t> closure;
    if (!BuildClosureLocked(&closure)) {
      return false;
    }
    if (!dirty_data_pages.empty()) {
      return RejectLocked(
          "capture provider cannot seal dirty guest pages without complete "
          "external-memory mutation coverage");
    }

    std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> code_pages;
    std::map<uint32_t, uint32_t> code_backing_views;
    for (uint32_t function_address : closure) {
      const DefinitionRecord& definition = definitions.at(function_address);
      for (uint32_t page_address : definition.code_pages) {
        const auto snapshot = immutable_code_pages.find(page_address);
        if (snapshot == immutable_code_pages.cend()) {
          return RejectLocked(
              "capture provider closure lacks immutable code bytes");
        }
        const uint32_t backing = BackingPageAddress(page_address);
        const auto backing_view = code_backing_views.find(backing);
        if (backing_view != code_backing_views.cend() &&
            backing_view->second != page_address) {
          return RejectLocked(
              "capture provider code closure uses both physical aliases");
        }
        if (written_backing_pages.contains(backing)) {
          return RejectLocked(
              "capture provider observed a write to the code closure");
        }
        code_backing_views.emplace(backing, page_address);
        const auto inserted =
            code_pages.emplace(page_address, snapshot->second);
        if (!inserted.second && inserted.first->second != snapshot->second) {
          return RejectLocked(
              "capture provider closure code snapshots conflict");
        }
      }
    }

    for (const auto& [backing, code_view] : code_backing_views) {
      const auto data_view = data_backing_views.find(backing);
      if (data_view == data_backing_views.cend()) {
        continue;
      }
      if (data_view->second != code_view) {
        return RejectLocked(
            "capture provider data aliases the selected code closure");
      }
      const auto initial = initial_data_pages.find(code_view);
      const auto code = code_pages.find(code_view);
      if (initial == initial_data_pages.cend() || code == code_pages.cend() ||
          initial->second != code->second) {
        return RejectLocked(
            "capture provider data preimage conflicts with selected code");
      }
    }

    for (const auto& [page_address, snapshot] : code_pages) {
      std::array<uint8_t, kGuestPageSize> current = {};
      if (ReadStableCodePage(page_address, &current) !=
              CodeReadResult::kSuccess ||
          current != snapshot) {
        if (state.load(std::memory_order_relaxed) != ProviderState::kRejected) {
          RejectLocked(
              "capture provider code changed after successful definition");
        }
        return false;
      }
    }

    ExecutionJitCorpusBuilder corpus_builder(config.jit_corpus_config_flags);
    for (const auto& [page_address, bytes] : code_pages) {
      std::string error;
      if (!corpus_builder.AddCodePage(page_address, bytes.data(), bytes.size(),
                                      &error)) {
        return RejectLocked("capture provider code corpus page failed: " +
                            error);
      }
    }
    for (uint32_t address : definition_order) {
      if (!closure.contains(address)) {
        continue;
      }
      const DefinitionRecord& definition = definitions.at(address);
      Function* function = processor.QueryFunction(address);
      if (!function || !function->is_guest() ||
          function->status() != Symbol::Status::kDefined ||
          function->address() != address ||
          function->end_address() != definition.end_address ||
          function->behavior() == Function::Behavior::kExtern ||
          function->behavior() == Function::Behavior::kBuiltin) {
        return RejectLocked(
            "capture provider corpus function is not published guest code");
      }
      auto* guest_function = static_cast<GuestFunction*>(function);
      const size_t host_code_size = guest_function->machine_code_length();
      if (!guest_function->machine_code() || !host_code_size ||
          host_code_size > std::numeric_limits<uint32_t>::max()) {
        return RejectLocked(
            "capture provider corpus function has no bounded host code");
      }
      const ExecutionJitCorpusBuilder::FunctionRecord record = {
          address, definition.end_address,
          static_cast<uint32_t>(host_code_size),
          JitCorpus::EncodeFunctionFlags(*function)};
      std::string error;
      if (!corpus_builder.AddFunction(record, &error)) {
        return RejectLocked("capture provider corpus function failed: " +
                            error);
      }
    }
    if (corpus_builder.function_count() != closure.size()) {
      return RejectLocked(
          "capture provider corpus lost definition-order entries");
    }
    std::string corpus_error;
    if (!corpus_builder.Encode(&code_corpus, &corpus_error)) {
      return RejectLocked("capture provider corpus encoding failed: " +
                          corpus_error);
    }

    initial_content.clear();
    final_content.clear();
    for (const auto& [page_address, bytes] : code_pages) {
      initial_content.push_back(
          {GuestExecutionSessionContentKind::kGuestCode, page_address,
           std::vector<uint8_t>(bytes.cbegin(), bytes.cend())});
    }
    for (const auto& [page_address, initial_bytes] : initial_data_pages) {
      const uint32_t backing = BackingPageAddress(page_address);
      if (code_backing_views.contains(backing)) {
        continue;
      }
      std::array<uint8_t, kGuestPageSize> final_bytes = {};
      if (!page_reader.ReadPage(page_address, &final_bytes)) {
        return RejectLocked(
            "capture provider could not snapshot a final guest data page");
      }
      const bool dirtied = dirty_data_pages.contains(page_address);
      if (!dirtied && final_bytes != initial_bytes) {
        return RejectLocked(
            "capture provider detected an unobserved guest memory mutation");
      }
      initial_content.push_back(
          {GuestExecutionSessionContentKind::kGuestPage, page_address,
           std::vector<uint8_t>(initial_bytes.cbegin(), initial_bytes.cend())});
      if (dirtied) {
        final_content.push_back(
            {GuestExecutionSessionContentKind::kGuestPage, page_address,
             std::vector<uint8_t>(final_bytes.cbegin(), final_bytes.cend())});
      }
    }
    corpus_function_count = static_cast<uint32_t>(closure.size());
    selected_code_page_count = static_cast<uint32_t>(code_pages.size());
    selected_data_page_count =
        static_cast<uint32_t>(initial_content.size() - code_pages.size());
    return true;
  }

  bool Detach() noexcept {
    std::lock_guard<std::mutex> attachment_lock(attachment_mutex);
    if (!attached.load(std::memory_order_acquire)) {
      return true;
    }
    if (!processor.TrySetGuestInvocationCaptureSink(owner, nullptr)) {
      return false;
    }
    attached.store(false, std::memory_order_release);
    return true;
  }

  Processor& processor;
  GuestExecutionSessionCaptureProviderConfig config;
  GuestInvocationCapturePageReader page_reader;
  GuestExecutionSessionCaptureProvider* owner = nullptr;

  mutable std::mutex mutex;
  std::mutex lifecycle_mutex;
  std::mutex attachment_mutex;
  std::atomic<ProviderState> state{ProviderState::kCataloging};
  std::atomic<bool> attached{false};
  std::atomic<void (*)(void*)> seal_detached_test_hook{nullptr};
  std::atomic<void*> seal_detached_test_context{nullptr};
  std::atomic<uint32_t> lifecycle_waiter_count{0};
  std::string rejection_message;

  std::map<uint32_t, DefinitionRecord> definitions;
  std::vector<uint32_t> definition_order;
  std::set<uint32_t> pending_definition_snapshots;
  std::set<uint32_t> catalog_code_pages;
  std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> immutable_code_pages;
  uint32_t dependency_count = 0;

  std::map<std::pair<uint64_t, uint64_t>, uint64_t>
      active_invocation_identities;
  std::map<uint64_t, uint32_t> initial_outer_return_addresses;
  std::set<uint64_t> initial_outside_guest_participants;
  std::map<uint64_t, std::vector<uint8_t>> initial_states;
  std::map<uint64_t, std::vector<uint8_t>> final_states;
  // Start-boundary participants whose encoded route names an export event that
  // was still open at the barrier. The pairing token stays here and never
  // reaches a durable byte.
  std::map<uint64_t, PendingExportCheckpoint> initial_pending_exports;
  const GuestExecutionSessionCaptureExportSequenceResolver*
      export_sequence_resolver = nullptr;
  std::vector<InstructionCounter> instruction_counters;
  std::vector<ppc::PPCContext*> instruction_counter_context_scratch;
  std::map<uint32_t, uint32_t> closure_seeds;

  std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> initial_data_pages;
  std::map<uint32_t, uint32_t> data_backing_views;
  std::map<uint32_t, DataPageUse> data_page_uses;
  std::set<uint32_t> dirty_data_pages;
  std::set<uint32_t> written_backing_pages;
  uint64_t memory_access_count = 0;

  std::vector<GuestExecutionSessionAssemblerContent> initial_content;
  std::vector<GuestExecutionSessionAssemblerContent> final_content;
  std::vector<uint8_t> code_corpus;
  uint32_t corpus_function_count = 0;
  uint32_t selected_code_page_count = 0;
  uint32_t selected_data_page_count = 0;
};

std::unique_ptr<GuestExecutionSessionCaptureProvider>
GuestExecutionSessionCaptureProvider::CreateAndAttach(
    Memory& memory, Processor& processor,
    const GuestExecutionSessionCaptureProviderConfig& config,
    std::string* error) {
  if (error) {
    error->clear();
  }
  GuestExecutionSessionCaptureProviderConfig normalized_config = config;
  if (!normalized_config.host_protection_page_size) {
    const size_t native_page_size = xe::memory::page_size();
    if (native_page_size > std::numeric_limits<uint32_t>::max()) {
      Fail(error, "capture provider native page size is unsupported");
      return nullptr;
    }
    normalized_config.host_protection_page_size =
        static_cast<uint32_t>(native_page_size);
  }
  if (normalized_config.jit_corpus_config_flags !=
          JitCorpus::kConfigGuestScheduler ||
      !IsPowerOfTwo(normalized_config.host_protection_page_size) ||
      normalized_config.host_protection_page_size < kGuestPageSize ||
      normalized_config.host_protection_page_size >
          ppc::GuestInvocationRecorderLimits::kMaximumHostProtectionPageSize ||
      !normalized_config.maximum_data_page_count ||
      !normalized_config.maximum_code_page_count ||
      normalized_config.maximum_code_page_count >
          ExecutionJitCorpus::kMaxPageRecords ||
      !normalized_config.maximum_memory_access_count ||
      !normalized_config.maximum_function_count ||
      normalized_config.maximum_function_count >
          ExecutionJitCorpus::kMaxFunctionRecords ||
      !normalized_config.maximum_dependency_count) {
    Fail(error,
         "capture provider configuration is invalid or not "
         "scheduler-on");
    return nullptr;
  }
  try {
    auto impl = std::make_unique<Impl>(memory, processor, normalized_config);
    auto provider = std::unique_ptr<GuestExecutionSessionCaptureProvider>(
        new GuestExecutionSessionCaptureProvider(std::move(impl)));
    provider->impl_->owner = provider.get();
    if (!processor.TrySetGuestInvocationCaptureSink(nullptr, provider.get())) {
      Fail(error, "capture provider could not attach before title translation");
      return nullptr;
    }
    provider->impl_->attached.store(true, std::memory_order_release);
    return provider;
  } catch (...) {
    Fail(error, "capture provider allocation failed");
    return nullptr;
  }
}

GuestExecutionSessionCaptureProvider::GuestExecutionSessionCaptureProvider(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestExecutionSessionCaptureProvider::~GuestExecutionSessionCaptureProvider() {
  if (impl_) {
    EndCapture(false);
  }
  if (impl_ && impl_->processor.guest_invocation_capture_sink() == this) {
    std::abort();
  }
}

bool GuestExecutionSessionCaptureProvider::SupportsCheckpointParticipant(
    const CheckpointParticipant& participant, std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  try {
    if (participant.thread_id &&
        (IsRestorableJitParticipant(participant) ||
         IsPassiveOutsideGuestParticipant(participant))) {
      return true;
    }
    if (participant.thread_id && IsBlockedExportParticipant(participant)) {
      std::string reason;
      if (impl_->BindBlockedExportDispatch(participant, nullptr, &reason)) {
        return true;
      }
      return Fail(error, reason);
    }
    return Fail(
        error,
        fmt::format("capture provider supports only restorable exact-PC JIT, "
                    "passive outside-guest or modeled blocking-export "
                    "scheduler participants: {}",
                    DescribeParticipant(participant)));
  } catch (...) {
    return Fail(error,
                "capture provider could not evaluate a checkpoint participant");
  }
}

bool GuestExecutionSessionCaptureProvider::BeginCapture(
    const CheckpointSnapshot& checkpoint,
    std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
        participants,
    const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  impl_->lifecycle_waiter_count.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->lifecycle_waiter_count.fetch_sub(1, std::memory_order_relaxed);
  std::vector<Impl::CapturedThreadState> captured;
  if (!impl_->CaptureThreadStates(participants.size(), &captured, error)) {
    impl_->RejectExternal(error && !error->empty()
                              ? *error
                              : "capture provider could not snapshot the "
                                "initial live thread states");
    return false;
  }
  try {
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->state.load(std::memory_order_relaxed) !=
              ProviderState::kCataloging ||
          !impl_->attached.load(std::memory_order_acquire)) {
        impl_->RejectLocked(
            "capture provider can begin only from its attached catalog state");
        return impl_->SetErrorFromRejection(error);
      }
      if (!impl_->RetryPendingDefinitionsLocked(true) ||
          !impl_->BuildCheckpointStatesLocked(checkpoint, participants,
                                              host_calls, captured,
                                              &impl_->initial_states, true) ||
          !impl_->ArmInstructionCountersLocked(captured)) {
        return impl_->SetErrorFromRejection(error);
      }
      impl_->state.store(ProviderState::kRecording, std::memory_order_release);
    }
    if (!impl_->processor.TrySetGuestInvocationCaptureSink(this, this)) {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->DisarmInstructionCountersLocked(false);
      impl_->RejectLocked(
          "capture provider could not arm the invocation hook generation");
      return impl_->SetErrorFromRejection(error);
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.load(std::memory_order_relaxed) ==
        ProviderState::kRejected) {
      impl_->DisarmInstructionCountersLocked(false);
      return impl_->SetErrorFromRejection(error);
    }
    return true;
  } catch (...) {
    try {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->DisarmInstructionCountersLocked(false);
    } catch (...) {
    }
    impl_->RejectException();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->SetErrorFromRejection(error);
  }
}

bool GuestExecutionSessionCaptureProvider::CollectInstructionCoverageDeltas(
    std::vector<GuestExecutionSessionInstructionCoverageDelta>* output,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "capture provider instruction output is null");
  }
  impl_->lifecycle_waiter_count.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->lifecycle_waiter_count.fetch_sub(1, std::memory_order_relaxed);
  try {
    output->clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.load(std::memory_order_relaxed) !=
        ProviderState::kRecording) {
      impl_->RejectLocked(
          "capture provider collected instruction coverage outside recording");
      return impl_->SetErrorFromRejection(error);
    }
    output->reserve(impl_->instruction_counters.size());
    Impl::InstructionCounterDrainVisitor visitor(
        impl_->instruction_counters, impl_->instruction_counter_context_scratch,
        output, false, true, false);
    if (impl_->processor.VisitGuestExecutionCaptureThreadStates(visitor) !=
        GuestExecutionCaptureThreadStateVisitResult::kCompleted) {
      impl_->RejectLocked(
          "capture provider could not atomically drain the exact instruction "
          "counter roster");
      return impl_->SetErrorFromRejection(error);
    }
    for (const auto& delta : *output) {
      if (impl_->initial_outside_guest_participants.contains(
              delta.participant.capture_instance_id)) {
        impl_->RejectLocked(
            "capture provider outside-guest participant accumulated guest "
            "instructions");
        output->clear();
        return impl_->SetErrorFromRejection(error);
      }
    }
    return true;
  } catch (...) {
    impl_->RejectException();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->SetErrorFromRejection(error);
  }
}

bool GuestExecutionSessionCaptureProvider::SealCapture(
    const CheckpointSnapshot& checkpoint,
    const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  impl_->lifecycle_waiter_count.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->lifecycle_waiter_count.fetch_sub(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.load(std::memory_order_relaxed) !=
        ProviderState::kRecording) {
      impl_->RejectLocked("capture provider can seal only an active recording");
      return impl_->SetErrorFromRejection(error);
    }
  }
  if (!impl_->Detach()) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RejectLocked(
        "capture provider could not detach and drain execution hooks");
    return impl_->SetErrorFromRejection(error);
  }
  std::vector<Impl::CapturedThreadState> captured;
  if (!impl_->CaptureThreadStates(impl_->initial_states.size(), &captured,
                                  error)) {
    impl_->RejectExternal(error && !error->empty()
                              ? *error
                              : "capture provider could not snapshot the "
                                "final live thread states");
    return false;
  }
  if (auto hook =
          impl_->seal_detached_test_hook.load(std::memory_order_acquire)) {
    hook(impl_->seal_detached_test_context.load(std::memory_order_acquire));
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.load(std::memory_order_relaxed) ==
        ProviderState::kRejected) {
      return impl_->SetErrorFromRejection(error);
    }
    if (!impl_->DisarmInstructionCountersLocked(true)) {
      return impl_->SetErrorFromRejection(error);
    }
    std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> participants;
    participants.reserve(captured.size());
    for (const auto& thread : captured) {
      participants.push_back(
          {thread.participant,
           GuestExecutionCaptureThreadStateLifecycleState::kReady});
    }
    if (!impl_->BuildCheckpointStatesLocked(checkpoint, participants,
                                            host_calls, captured,
                                            &impl_->final_states, false) ||
        !impl_->FinalizeContentAndCorpusLocked()) {
      return impl_->SetErrorFromRejection(error);
    }
    impl_->state.store(ProviderState::kSealed, std::memory_order_release);
    return true;
  } catch (...) {
    impl_->RejectException();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->SetErrorFromRejection(error);
  }
}

void GuestExecutionSessionCaptureProvider::EndCapture(bool accepted) noexcept {
  impl_->lifecycle_waiter_count.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->lifecycle_waiter_count.fetch_sub(1, std::memory_order_relaxed);
  const bool detached = impl_->Detach();
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->DisarmInstructionCountersLocked(false);
    if (!detached) {
      impl_->RejectLocked("capture provider could not detach at session end");
      return;
    }
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kAccepted ||
        current == ProviderState::kStopped) {
      return;
    } else if (accepted && current == ProviderState::kSealed) {
      impl_->state.store(ProviderState::kAccepted, std::memory_order_release);
    } else if (!accepted && current != ProviderState::kRejected) {
      impl_->state.store(ProviderState::kStopped, std::memory_order_release);
    } else if (accepted && current != ProviderState::kAccepted) {
      impl_->RejectLocked(
          "capture provider was accepted without a sealed result");
    }
  } catch (...) {
    impl_->RejectException();
  }
}

bool GuestExecutionSessionCaptureProvider::EncodeParticipantState(
    const GuestExecutionCaptureParticipantIdentity& participant,
    bool initial_checkpoint, std::vector<uint8_t>* output,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "capture provider state output is null");
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    const auto& states =
        initial_checkpoint ? impl_->initial_states : impl_->final_states;
    if (current != ProviderState::kRecording &&
        current != ProviderState::kSealed) {
      output->clear();
      return impl_->SetErrorFromRejection(error);
    }
    if (!initial_checkpoint && current != ProviderState::kSealed) {
      output->clear();
      return Fail(error, "capture provider has no final state before the seal");
    }
    const auto state = states.find(participant.capture_instance_id);
    if (state == states.cend() || participant.guest_thread_id == 0) {
      output->clear();
      return Fail(error, "capture provider has no state for the participant");
    }
    const auto pending =
        impl_->initial_pending_exports.find(participant.capture_instance_id);
    if (initial_checkpoint &&
        pending != impl_->initial_pending_exports.cend()) {
      // The provisional encoding in initial_states named the dispatch's
      // pairing token; only the resolved sequence may leave this method.
      uint64_t global_sequence = 0;
      if (!impl_->export_sequence_resolver ||
          !impl_->export_sequence_resolver->ResolveModeledExportSequence(
              pending->second.token, participant, &global_sequence) ||
          !global_sequence) {
        output->clear();
        return Fail(error,
                    "capture provider modeled export dispatch never reached "
                    "the session tape");
      }
      ppc::GuestPPCThreadCheckpoint resolved = pending->second.state;
      resolved.pending_external_event_sequence = global_sequence;
      std::string encode_error;
      if (!ppc::GuestPPCThreadCheckpointCodec::Encode(resolved, output,
                                                      &encode_error)) {
        output->clear();
        return Fail(error,
                    "capture provider could not encode a resolved pending "
                    "export checkpoint: " +
                        encode_error);
      }
      return true;
    }
    *output = state->second;
    return true;
  } catch (...) {
    output->clear();
    impl_->RejectException();
    return Fail(error, "capture provider state copy failed");
  }
}

bool GuestExecutionSessionCaptureProvider::DefersInitialParticipantState(
    const GuestExecutionCaptureParticipantIdentity& participant) noexcept {
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    // A participant bound to a still-open modeled export names an event the
    // tape has not recorded yet, so its route cannot be encoded until the
    // session publishes.
    return impl_->initial_pending_exports.find(
               participant.capture_instance_id) !=
           impl_->initial_pending_exports.cend();
  } catch (...) {
    impl_->RejectException();
    return false;
  }
}

void GuestExecutionSessionCaptureProvider::SetModeledExportSequenceResolver(
    const GuestExecutionSessionCaptureExportSequenceResolver*
        resolver) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->export_sequence_resolver = resolver;
}

bool GuestExecutionSessionCaptureProvider::CollectCheckpointContent(
    bool initial_checkpoint,
    std::vector<GuestExecutionSessionAssemblerContent>* output,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "capture provider content output is null");
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.load(std::memory_order_relaxed) !=
        ProviderState::kSealed) {
      output->clear();
      return impl_->SetErrorFromRejection(error);
    }
    *output =
        initial_checkpoint ? impl_->initial_content : impl_->final_content;
    return true;
  } catch (...) {
    output->clear();
    impl_->RejectException();
    return Fail(error, "capture provider content copy failed");
  }
}

bool GuestExecutionSessionCaptureProvider::CollectSessionCodeCorpus(
    std::vector<uint8_t>* output, std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "capture provider corpus output is null");
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.load(std::memory_order_relaxed) !=
            ProviderState::kSealed ||
        impl_->code_corpus.empty()) {
      output->clear();
      return impl_->SetErrorFromRejection(error);
    }
    *output = impl_->code_corpus;
    return true;
  } catch (...) {
    output->clear();
    impl_->RejectException();
    return Fail(error, "capture provider corpus copy failed");
  }
}

uint8_t GuestExecutionSessionCaptureProvider::initial_event_mask() const {
  return IsExecutionPhase(impl_->state.load(std::memory_order_acquire))
             ? kGuestInvocationCaptureAllEvents
             : 0;
}

uint8_t GuestExecutionSessionCaptureProvider::event_mask(
    const ppc::GuestInvocationRecorderIdentity&) const {
  return initial_event_mask();
}

bool GuestExecutionSessionCaptureProvider::Poll() noexcept {
  return impl_->InvokeCallback([this] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    return !IsCatalogPhase(current) ||
           impl_->RetryPendingDefinitionsLocked(false);
  });
}

bool GuestExecutionSessionCaptureProvider::OnFunctionDependency(
    uint32_t source_address, uint32_t dependency_address) noexcept {
  return impl_->InvokeCallback([this, source_address, dependency_address] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    return !IsCatalogPhase(current) ||
           impl_->RegisterDependencyLocked(source_address, dependency_address);
  });
}

bool GuestExecutionSessionCaptureProvider::OnFunctionDefined(
    uint32_t address, uint32_t end_address) noexcept {
  return impl_->InvokeCallback([this, address, end_address] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    return !IsCatalogPhase(current) ||
           impl_->RegisterDefinitionLocked(address, end_address);
  });
}

bool GuestExecutionSessionCaptureProvider::OnFunctionEntry(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t end_address, const ppc::GuestPPCRegisterState&) noexcept {
  return impl_->InvokeCallback([this, identity, address, end_address] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    if (!IsExecutionPhase(current)) {
      return true;
    }
    uint64_t participant = 0;
    if (!impl_->IsKnownExecutionIdentityLocked(identity, &participant)) {
      return false;
    }
    return impl_->RetryPendingDefinitionsLocked(true) &&
           impl_->AddClosureSeedLocked(address, end_address);
  });
}

bool GuestExecutionSessionCaptureProvider::OnFunctionExit(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t return_address, const ppc::GuestPPCRegisterState&) noexcept {
  return impl_->InvokeCallback([this, identity, address, return_address] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    if (!IsExecutionPhase(current)) {
      return true;
    }
    uint64_t participant = 0;
    const auto definition = impl_->definitions.find(address);
    if (!impl_->IsKnownExecutionIdentityLocked(identity, &participant)) {
      return false;
    }
    if (definition == impl_->definitions.cend() ||
        !definition->second.defined) {
      return impl_->RejectLocked(
          "capture provider observed a function exit outside its exact "
          "definition catalog");
    }
    const auto outer_return =
        impl_->initial_outer_return_addresses.find(participant);
    if (outer_return == impl_->initial_outer_return_addresses.cend()) {
      return impl_->RejectLocked(
          "capture provider function exit has no participant outer boundary");
    }
    if (return_address == outer_return->second) {
      return true;
    }
    const Impl::DefinitionRecord* owner =
        impl_->FindOwningDefinitionLocked(return_address);
    if (!owner) {
      return false;
    }
    const uint32_t owner_address = impl_->FindDefinitionAddressLocked(owner);
    return owner_address &&
           impl_->AddClosureSeedLocked(owner_address, owner->end_address);
  });
}

bool GuestExecutionSessionCaptureProvider::OnMemoryAccess(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t size, ppc::GuestInvocationRecorderMemoryAccess access) noexcept {
  return impl_->InvokeCallback([this, identity, address, size, access] {
    return impl_->RecordMemoryAccessLocked(identity, address, size, access);
  });
}

bool GuestExecutionSessionCaptureProvider::OnUnsupportedDependency(
    const ppc::GuestInvocationRecorderIdentity& identity,
    uint32_t dependency_flags) noexcept {
  return impl_->InvokeCallback([this, identity, dependency_flags] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    if (!IsExecutionPhase(current)) {
      return true;
    }
    uint64_t participant = 0;
    if (!impl_->IsKnownExecutionIdentityLocked(identity, &participant)) {
      return false;
    }
    return impl_->RejectLocked(fmt::format(
        "capture provider observed unsupported dependency flags 0x{:08X}",
        dependency_flags));
  });
}

bool GuestExecutionSessionCaptureProvider::OnTailCall(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t from_address,
    uint32_t target_address) noexcept {
  return impl_->InvokeCallback([this, identity, from_address, target_address] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    if (!IsExecutionPhase(current)) {
      return true;
    }
    uint64_t participant = 0;
    const auto source = impl_->definitions.find(from_address);
    const auto target = impl_->definitions.find(target_address);
    if (!impl_->IsKnownExecutionIdentityLocked(identity, &participant) ||
        source == impl_->definitions.cend() || !source->second.defined ||
        target == impl_->definitions.cend() || !target->second.defined) {
      return impl_->RejectLocked(
          "capture provider tail call is outside the exact catalog");
    }
    return impl_->AddClosureSeedLocked(target_address,
                                       target->second.end_address);
  });
}

bool GuestExecutionSessionCaptureProvider::OnUnwindOrLongjmp(
    const ppc::GuestInvocationRecorderIdentity& identity) noexcept {
  return impl_->InvokeCallback([this, identity] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    if (!IsExecutionPhase(current)) {
      return true;
    }
    uint64_t participant = 0;
    return impl_->IsKnownExecutionIdentityLocked(identity, &participant) &&
           impl_->RejectLocked(
               "capture provider cannot replay an unmodeled unwind");
  });
}

bool GuestExecutionSessionCaptureProvider::OnAsyncReentry(
    const ppc::GuestInvocationRecorderIdentity& identity) noexcept {
  return impl_->InvokeCallback([this, identity] {
    const ProviderState current = impl_->state.load(std::memory_order_relaxed);
    if (current == ProviderState::kRejected) {
      return false;
    }
    if (!IsExecutionPhase(current)) {
      return true;
    }
    uint64_t participant = 0;
    return impl_->IsKnownExecutionIdentityLocked(identity, &participant) &&
           impl_->RejectLocked(
               "capture provider cannot replay asynchronous reentry");
  });
}

void GuestExecutionSessionCaptureProvider::SetSealDetachedTestHook(
    void (*hook)(void*), void* context) noexcept {
  if (!hook) {
    impl_->seal_detached_test_hook.store(nullptr, std::memory_order_release);
    impl_->seal_detached_test_context.store(nullptr, std::memory_order_relaxed);
    return;
  }
  impl_->seal_detached_test_context.store(context, std::memory_order_relaxed);
  impl_->seal_detached_test_hook.store(hook, std::memory_order_release);
}

uint32_t GuestExecutionSessionCaptureProvider::lifecycle_waiter_count_for_test()
    const noexcept {
  return impl_->lifecycle_waiter_count.load(std::memory_order_relaxed);
}

GuestExecutionSessionCaptureProviderStatus
GuestExecutionSessionCaptureProvider::status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  GuestExecutionSessionCaptureProviderStatus result;
  result.state = impl_->state.load(std::memory_order_relaxed);
  result.memory_access_count = impl_->memory_access_count;
  result.catalog_function_count =
      static_cast<uint32_t>(impl_->definition_order.size());
  result.corpus_function_count = impl_->corpus_function_count;
  result.code_page_count = impl_->selected_code_page_count;
  result.data_page_count = impl_->selected_data_page_count;
  result.message = impl_->rejection_message;
  return result;
}

}  // namespace cpu
}  // namespace xe

#endif
