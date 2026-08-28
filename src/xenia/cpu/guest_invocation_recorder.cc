/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_recorder.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace cpu {
namespace ppc {

namespace {

constexpr uint32_t kGuestPageSize = GuestInvocationArtifactCodec::kPageSize;

bool IsPowerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }

bool IsValidFunctionExtent(uint32_t address, uint32_t end_address) {
  return address && !(address & 3) && !(end_address & 3) &&
         end_address >= address &&
         uint64_t(end_address) - address + 4 <=
             GuestInvocationArtifactCodec::kMaxFunctionSize;
}

bool IsSupportedPageAddress(uint32_t address) {
  return (address >= 0x00001000u && address <= 0x7EFFF000u) ||
         (address >= 0x80000000u && address <= 0x9FFFF000u);
}

uint32_t BackingPageAddress(uint32_t address) {
  return address >= 0x90000000u && address < 0xA0000000u ? address - 0x10000000u
                                                         : address;
}

bool HasWriteAccess(GuestInvocationRecorderMemoryAccess access) {
  return access == GuestInvocationRecorderMemoryAccess::kWrite ||
         access == GuestInvocationRecorderMemoryAccess::kReadWrite;
}

bool IsValidAccess(GuestInvocationRecorderMemoryAccess access) {
  return access == GuestInvocationRecorderMemoryAccess::kRead ||
         access == GuestInvocationRecorderMemoryAccess::kWrite ||
         access == GuestInvocationRecorderMemoryAccess::kReadWrite;
}

}  // namespace

struct GuestInvocationRecorder::Impl {
  struct DefinitionRecord {
    uint32_t end_address = 0;
    uint32_t definition_order = 0;
    bool defined = false;
    bool code_pages_snapshotted = false;
    std::set<uint32_t> dependencies;
    std::vector<uint32_t> code_page_addresses;
  };

  struct CodePageSnapshot {
    std::array<uint8_t, kGuestPageSize> data = {};
    uint64_t write_generation = 0;
  };

  enum class CodePageReadResult {
    kSuccess,
    kRetry,
    kFailure,
  };

  struct CallFrame {
    uint32_t address = 0;
    uint32_t return_address = 0;
  };

  Impl(const GuestInvocationRecorderSelection& selection_value,
       const GuestInvocationRecorderLimits& limits_value,
       GuestInvocationRecorderPageReader& page_reader_value,
       const GuestInvocationRecorderClock& clock_value, uint64_t deadline_value)
      : selection(selection_value),
        limits(limits_value),
        page_reader(page_reader_value),
        clock(clock_value),
        deadline(deadline_value) {
    if (selection.owner.context_id) {
      owner = selection.owner;
    }
  }

  bool Reject(GuestInvocationRecorderRejection rejection_value,
              std::string_view message, uint32_t dependency_flags = 0) {
    if (state == GuestInvocationRecorderState::kComplete) {
      return false;
    }
    if (state != GuestInvocationRecorderState::kRejected) {
      state = GuestInvocationRecorderState::kRejected;
      rejection = rejection_value;
      rejected_dependency_flags = dependency_flags;
      rejection_message.assign(message);
      result.reset();
      call_stack.clear();
      inherited_return_address.reset();
      initial_pages.clear();
    }
    return false;
  }

  struct CallbackScope {
    explicit CallbackScope(Impl& impl_value) : impl(impl_value) {
      if (impl.callback_active) {
        impl.Reject(GuestInvocationRecorderRejection::kAsyncReentry,
                    "recorder callback reentered its serialized stream",
                    kGuestInvocationDependencyAsyncInterrupt);
        return;
      }
      impl.callback_active = true;
      owns_callback = true;
    }

    ~CallbackScope() {
      if (owns_callback) {
        impl.callback_active = false;
      }
    }

    explicit operator bool() const { return owns_callback; }

    Impl& impl;
    bool owns_callback = false;
  };

  bool CheckDeadline() {
    if (state == GuestInvocationRecorderState::kRejected) {
      return false;
    }
    if (state == GuestInvocationRecorderState::kComplete) {
      return true;
    }
    const uint64_t now = clock.NowTicks();
    if (state == GuestInvocationRecorderState::kRejected) {
      return false;
    }
    if (now >= deadline) {
      return Reject(GuestInvocationRecorderRejection::kDeadlineExceeded,
                    "invocation recording deadline exceeded");
    }
    return true;
  }

  bool BeginEvent() {
    if (!CheckDeadline()) {
      return false;
    }
    if (state == GuestInvocationRecorderState::kComplete) {
      return true;
    }
    if (event_count >= limits.max_event_count) {
      return Reject(GuestInvocationRecorderRejection::kEventLimit,
                    "invocation recording event limit exceeded");
    }
    ++event_count;
    return true;
  }

  bool BeginAccessEvent() {
    if (!BeginEvent()) {
      return false;
    }
    if (access_count >= limits.max_access_count) {
      return Reject(GuestInvocationRecorderRejection::kAccessLimit,
                    "invocation recording access limit exceeded");
    }
    ++access_count;
    return true;
  }

  bool IsOwner(const GuestInvocationRecorderIdentity& identity) const {
    return owner.has_value() && identity == *owner;
  }

  bool AliasesOwner(const GuestInvocationRecorderIdentity& identity) const {
    return owner.has_value() && (identity.context_id == owner->context_id ||
                                 identity.thread_id == owner->thread_id);
  }

  bool IsValidIdentity(const GuestInvocationRecorderIdentity& identity) const {
    return identity.context_id && identity.thread_id;
  }

  bool ClaimOwner(const GuestInvocationRecorderIdentity& identity) {
    if (!IsValidIdentity(identity)) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "selected root occurrence has an invalid owner identity");
    }
    if (owner.has_value() && identity != *owner) {
      return Reject(GuestInvocationRecorderRejection::kAsyncReentry,
                    "selected root occurrence does not match its configured "
                    "owner",
                    kGuestInvocationDependencyAsyncInterrupt);
    }
    owner = identity;
    return true;
  }

  bool RejectOwnerAlias(const GuestInvocationRecorderIdentity& identity) {
    if (IsRecordingAttempt() && AliasesOwner(identity)) {
      return Reject(GuestInvocationRecorderRejection::kAsyncReentry,
                    "owner context or thread reentered asynchronously",
                    kGuestInvocationDependencyAsyncInterrupt);
    }
    return true;
  }

  bool EnsureRegistryAddress(uint32_t address, DefinitionRecord** output) {
    if (!address || (address & 3)) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "translation registry contains an invalid function "
                    "address");
    }
    auto existing = definitions.find(address);
    if (existing != definitions.end()) {
      *output = &existing->second;
      return true;
    }
    if (definitions.size() >= limits.max_function_count) {
      return Reject(GuestInvocationRecorderRejection::kFunctionLimit,
                    "translation registry function limit exceeded");
    }
    auto [inserted, was_inserted] =
        definitions.emplace(address, DefinitionRecord{});
    if (!was_inserted) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "translation registry insertion failed");
    }
    *output = &inserted->second;
    return true;
  }

  bool RegisterDependency(uint32_t source_address,
                          uint32_t dependency_address) {
    DefinitionRecord* source = nullptr;
    DefinitionRecord* dependency = nullptr;
    if (!EnsureRegistryAddress(source_address, &source) ||
        !EnsureRegistryAddress(dependency_address, &dependency)) {
      return false;
    }
    if (source->defined) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "dependency was reported after its source definition");
    }
    source->dependencies.insert(dependency_address);
    return true;
  }

  CodePageReadResult ReadStableCodePage(
      uint32_t page_address, std::array<uint8_t, kGuestPageSize>* stable_page) {
    std::array<uint8_t, kGuestPageSize> verification = {};
    const bool first_read = page_reader.ReadPage(page_address, stable_page);
    if (state == GuestInvocationRecorderState::kRejected) {
      return CodePageReadResult::kFailure;
    }
    if (!first_read && page_reader.last_read_was_retryable()) {
      return CodePageReadResult::kRetry;
    }
    const bool second_read =
        first_read && page_reader.ReadPage(page_address, &verification);
    if (state == GuestInvocationRecorderState::kRejected) {
      return CodePageReadResult::kFailure;
    }
    if (!second_read && page_reader.last_read_was_retryable()) {
      return CodePageReadResult::kRetry;
    }
    if (!first_read || !second_read) {
      Reject(GuestInvocationRecorderRejection::kPageReadFailure,
             "unable to read a guest code page",
             kGuestInvocationDependencyUnsupportedMappingOrProtection);
      return CodePageReadResult::kFailure;
    }
    if (*stable_page != verification) {
      Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
             "guest code changed while it was sampled",
             kGuestInvocationDependencySelfModifyingCode);
      return CodePageReadResult::kFailure;
    }
    return CodePageReadResult::kSuccess;
  }

  bool RequireStableCodePage(uint32_t page_address,
                             std::array<uint8_t, kGuestPageSize>* stable_page,
                             std::string_view site) {
    const CodePageReadResult read =
        ReadStableCodePage(page_address, stable_page);
    if (read == CodePageReadResult::kSuccess) {
      return true;
    }
    if (read == CodePageReadResult::kRetry) {
      return Reject(
          GuestInvocationRecorderRejection::kPageReadFailure,
          fmt::format("global memory snapshot was contended at capture "
                      "boundary: {} page {:08X} closure {} attempt {} state {}",
                      site, page_address, closure_code_pages.size(),
                      attempt_count, static_cast<uint32_t>(state)),
          kGuestInvocationDependencyUnsupportedMappingOrProtection);
    }
    return false;
  }

  CodePageReadResult SnapshotDefinition(DefinitionRecord& definition) {
    std::map<uint32_t, CodePageSnapshot> code_pages;
    for (uint32_t page_address : definition.code_page_addresses) {
      const auto generation =
          definition_page_write_generations.find(page_address);
      if (generation == definition_page_write_generations.cend() ||
          generation->second) {
        Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
               "guest code was written before its definition snapshot",
               kGuestInvocationDependencySelfModifyingCode);
        return CodePageReadResult::kFailure;
      }
      CodePageSnapshot snapshot;
      const CodePageReadResult read =
          ReadStableCodePage(page_address, &snapshot.data);
      if (read != CodePageReadResult::kSuccess) {
        return read;
      }
      snapshot.write_generation = generation->second;
      const auto existing = definition_code_pages.find(page_address);
      if (existing != definition_code_pages.cend() &&
          (existing->second.write_generation != snapshot.write_generation ||
           existing->second.data != snapshot.data)) {
        Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
               "shared code page changed between successful definitions",
               kGuestInvocationDependencySelfModifyingCode);
        return CodePageReadResult::kFailure;
      }
      code_pages.emplace(page_address, std::move(snapshot));
    }
    for (auto& [page_address, snapshot] : code_pages) {
      definition_code_pages.emplace(page_address, std::move(snapshot));
    }
    definition.code_pages_snapshotted = true;
    return CodePageReadResult::kSuccess;
  }

  CodePageReadResult SnapshotPendingDefinitions() {
    for (auto pending = pending_definition_snapshots.begin();
         pending != pending_definition_snapshots.end();) {
      DefinitionRecord& definition = definitions.at(*pending);
      const CodePageReadResult snapshot = SnapshotDefinition(definition);
      if (snapshot != CodePageReadResult::kSuccess) {
        return snapshot;
      }
      pending = pending_definition_snapshots.erase(pending);
    }
    return CodePageReadResult::kSuccess;
  }

  bool RegisterDefinition(uint32_t address, uint32_t end_address) {
    if (!IsValidFunctionExtent(address, end_address)) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "successful definition has an invalid function extent");
    }
    DefinitionRecord* definition = nullptr;
    if (!EnsureRegistryAddress(address, &definition)) {
      return false;
    }
    if (definition->defined) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "successful function definition was reported twice");
    }

    const uint32_t first_function_page = address & ~(kGuestPageSize - 1);
    const uint32_t last_function_page = end_address & ~(kGuestPageSize - 1);
    std::vector<uint32_t> code_page_addresses;
    if (!CollectHostProtectionPages(first_function_page, last_function_page,
                                    &code_page_addresses)) {
      return false;
    }
    const uint64_t page_count = code_page_addresses.size();
    std::set<uint32_t> new_page_addresses;
    for (uint32_t page_address : code_page_addresses) {
      if (!definition_page_write_generations.contains(page_address)) {
        new_page_addresses.insert(page_address);
      }
    }
    if (definition_page_write_generations.size() > limits.max_code_page_count ||
        new_page_addresses.size() >
            limits.max_code_page_count -
                definition_page_write_generations.size() ||
        page_count > limits.max_code_page_count) {
      return Reject(GuestInvocationRecorderRejection::kPageLimit,
                    "definition code-page catalog exceeds the page limit",
                    kGuestInvocationDependencyPageDiscoveryOverflow);
    }

    definition->code_page_addresses = std::move(code_page_addresses);
    for (uint32_t page_address : new_page_addresses) {
      definition_page_write_generations.emplace(page_address, 0);
    }
    definition->defined = true;
    definition->end_address = end_address;
    definition->definition_order =
        static_cast<uint32_t>(definition_order.size());
    definition_order.push_back(address);
    const CodePageReadResult snapshot = SnapshotDefinition(*definition);
    if (snapshot == CodePageReadResult::kRetry) {
      pending_definition_snapshots.insert(address);
      return true;
    }
    return snapshot == CodePageReadResult::kSuccess;
  }

  bool MergeDefinitionCodePages(const DefinitionRecord& definition) {
    if (!definition.code_pages_snapshotted) {
      return Reject(
          GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
          "successful definition still has a pending code-page snapshot");
    }
    std::map<uint32_t, uint32_t> new_backing_views;
    for (uint32_t page_address : definition.code_page_addresses) {
      const auto immutable_page = definition_code_pages.find(page_address);
      if (immutable_page == definition_code_pages.cend()) {
        return Reject(
            GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
            "successful definition is missing an immutable code page");
      }
      const CodePageSnapshot& snapshot = immutable_page->second;
      const auto generation =
          definition_page_write_generations.find(page_address);
      if (generation == definition_page_write_generations.cend() ||
          generation->second != snapshot.write_generation) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "guest code was written after successful translation",
                      kGuestInvocationDependencySelfModifyingCode);
      }
      const auto existing = closure_code_pages.find(page_address);
      if (existing != closure_code_pages.cend() &&
          (existing->second.write_generation != snapshot.write_generation ||
           existing->second.data != snapshot.data)) {
        return Reject(
            GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
            "immutable code-page catalog is inconsistent");
      }
      const uint32_t backing = BackingPageAddress(page_address);
      const auto code_backing = closure_code_backing_views.find(backing);
      const auto new_backing = new_backing_views.find(backing);
      if ((code_backing != closure_code_backing_views.cend() &&
           code_backing->second != page_address) ||
          (new_backing != new_backing_views.cend() &&
           new_backing->second != page_address)) {
        return Reject(GuestInvocationRecorderRejection::kUnsupportedDependency,
                      "code closure aliases 0x8 and 0x9 guest pages",
                      kGuestInvocationDependencyPhysicalAlias);
      }
      const auto data_backing = supplied_data_backing_views.find(backing);
      if (data_backing != supplied_data_backing_views.cend()) {
        const uint32_t dependency =
            data_backing->second == page_address
                ? kGuestInvocationDependencyUnsupportedMappingOrProtection
                : kGuestInvocationDependencyPhysicalAlias;
        return Reject(GuestInvocationRecorderRejection::kUnsupportedDependency,
                      "code closure overlaps supplied invocation data",
                      dependency);
      }
      if (cross_thread_written_backing_pages.contains(backing)) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      "another thread wrote a page in the code closure",
                      kGuestInvocationDependencyCrossThreadMutation);
      }
      new_backing_views.emplace(backing, page_address);
    }
    for (uint32_t page_address : definition.code_page_addresses) {
      closure_code_pages.emplace(page_address,
                                 definition_code_pages.at(page_address));
      closure_code_backing_views.emplace(BackingPageAddress(page_address),
                                         page_address);
    }
    return true;
  }

  bool ValidateClosureCodePages(std::string_view site) {
    for (const auto& [page_address, snapshot] : closure_code_pages) {
      const auto generation =
          definition_page_write_generations.find(page_address);
      if (generation == definition_page_write_generations.cend() ||
          generation->second != snapshot.write_generation) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "guest code was written after successful translation",
                      kGuestInvocationDependencySelfModifyingCode);
      }
      std::array<uint8_t, kGuestPageSize> current = {};
      if (!RequireStableCodePage(page_address, &current, site)) {
        return false;
      }
      if (current != snapshot.data) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "guest code changed after successful translation",
                      kGuestInvocationDependencySelfModifyingCode);
      }
    }
    return true;
  }

  bool AddTranslationClosureSeed(uint32_t address,
                                 uint32_t expected_end_address,
                                 bool revalidate_closure_code) {
    if (closure_code_written) {
      return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                    "guest code was written after successful translation",
                    kGuestInvocationDependencySelfModifyingCode);
    }
    const CodePageReadResult pending_snapshot = SnapshotPendingDefinitions();
    if (pending_snapshot == CodePageReadResult::kRetry) {
      return Reject(
          GuestInvocationRecorderRejection::kPageReadFailure,
          "definition snapshot contention persisted until function entry",
          kGuestInvocationDependencyUnsupportedMappingOrProtection);
    }
    if (pending_snapshot == CodePageReadResult::kFailure) {
      return false;
    }
    const auto seed = definitions.find(address);
    if (seed == definitions.cend() || !seed->second.defined) {
      return Reject(
          GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
          "capture closure contains a function without a successful "
          "definition");
    }
    if (seed->second.end_address != expected_end_address) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "runtime function extent differs from its definition");
    }

    const size_t closure_code_page_count = closure_code_pages.size();
    std::vector<uint32_t> pending = {address};
    while (!pending.empty()) {
      const uint32_t function_address = pending.back();
      pending.pop_back();
      if (closure_functions.contains(function_address)) {
        continue;
      }
      const auto definition = definitions.find(function_address);
      if (definition == definitions.cend() || !definition->second.defined) {
        // Translation declares every call target but only defines the ones the
        // title actually demands, and a declared target has no emitted code.
        // Entering one later reseeds the closure from its own definition.
        continue;
      }
      if (AnyPageWasWritten(owner_written_pages,
                            definition->second.code_page_addresses)) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "a recorded write overlaps guest code",
                      kGuestInvocationDependencySelfModifyingCode);
      }
      if (AnyPageWasWritten(cross_thread_written_pages,
                            definition->second.code_page_addresses)) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "another thread wrote guest code in the capture closure",
                      kGuestInvocationDependencySelfModifyingCode |
                          kGuestInvocationDependencyCrossThreadMutation);
      }
      if (!MergeDefinitionCodePages(definition->second)) {
        return false;
      }
      closure_functions.emplace(function_address,
                                definition->second.end_address);
      pending.insert(pending.end(), definition->second.dependencies.cbegin(),
                     definition->second.dependencies.cend());
    }
    // Resampling the closure costs two guest page reads per code page, so an
    // unchanged closure relies on the write generations tracked above until an
    // attempt boundary or the final exit resamples it.
    if (!revalidate_closure_code &&
        closure_code_pages.size() == closure_code_page_count) {
      return true;
    }
    return ValidateClosureCodePages(revalidate_closure_code ? "attempt-boundary"
                                                            : "closure-growth");
  }

  bool ValidateReturnBoundary(uint32_t function_address,
                              uint32_t function_end_address,
                              const GuestPPCRegisterState& state,
                              bool is_root_entry, uint32_t* output) {
    if (!state.link_register ||
        state.link_register > std::numeric_limits<uint32_t>::max() ||
        (state.link_register & 3)) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "function entry has an invalid return boundary");
    }
    const uint32_t return_address = static_cast<uint32_t>(state.link_register);
    if (return_address >= function_address &&
        return_address <= function_end_address) {
      const size_t depth = is_root_entry ? 0 : call_stack.size();
      const uint32_t caller =
          is_root_entry || call_stack.empty() ? 0 : call_stack.back().address;
      return Reject(
          GuestInvocationRecorderRejection::kInvalidEvent,
          fmt::format("return boundary is inside the entered function: {} "
                      "entry {:08X}-{:08X} return {:08X} depth {} caller "
                      "{:08X}",
                      is_root_entry ? "root" : "nested", function_address,
                      function_end_address, return_address, depth, caller));
    }
    *output = return_address;
    return true;
  }

  bool SnapshotSuppliedDataPages(
      std::map<uint32_t, std::array<uint8_t, kGuestPageSize>>* output) {
    output->clear();
    for (uint32_t page_address : supplied_data_pages) {
      std::array<uint8_t, kGuestPageSize> page = {};
      const bool read = page_reader.ReadPage(page_address, &page);
      if (state == GuestInvocationRecorderState::kRejected) {
        output->clear();
        return false;
      }
      if (!read) {
        output->clear();
        return Reject(GuestInvocationRecorderRejection::kPageReadFailure,
                      "unable to snapshot a supplied guest data page",
                      kGuestInvocationDependencyUnsupportedMappingOrProtection);
      }
      output->emplace(page_address, std::move(page));
    }
    return true;
  }

  bool BeginAttempt(const GuestPPCRegisterState& entry_state) {
    if (attempt_count >= limits.max_attempts) {
      return Reject(GuestInvocationRecorderRejection::kAttemptLimit,
                    "invocation recording attempt limit exceeded");
    }
    uint32_t return_address = 0;
    if (!ValidateReturnBoundary(selection.root_address,
                                selection.root_end_address, entry_state, true,
                                &return_address)) {
      return false;
    }
    ++attempt_count;
    attempt_pages.clear();
    attempt_read_backing_pages.clear();
    cross_thread_written_backing_pages.clear();
    inherited_return_address.reset();
    call_stack.clear();
    call_stack.push_back({selection.root_address, return_address});
    entered_functions.insert(selection.root_address);
    if (!AddTranslationClosureSeed(selection.root_address,
                                   selection.root_end_address, true)) {
      return false;
    }
    attempt_entry_state = entry_state;
    attempt_return_address = return_address;

    if (state == GuestInvocationRecorderState::kWaitingForFinalAttempt) {
      if (!SnapshotSuppliedDataPages(&initial_pages)) {
        return false;
      }
      state = GuestInvocationRecorderState::kRecordingFinalAttempt;
    } else {
      initial_pages.clear();
      state = GuestInvocationRecorderState::kRecordingDiscovery;
    }
    return true;
  }

  // A tail call replaces the current frame rather than nesting under it. The
  // target inherits this frame's return boundary and reports its own entry, so
  // dropping the frame here leaves that entry to push it at the same depth.
  bool TakeTailCall(uint32_t from_address) {
    if (call_stack.empty() || call_stack.back().address != from_address) {
      return Reject(GuestInvocationRecorderRejection::kUnbalancedReturn,
                    "tail call does not match the recorded call stack",
                    kGuestInvocationDependencyUnbalancedReturn);
    }
    // A non-linking branch never writes LR, so the target does not establish a
    // return boundary of its own and inherits this frame's instead.
    inherited_return_address = call_stack.back().return_address;
    call_stack.pop_back();
    return true;
  }

  bool IsRecordingAttempt() const {
    return state == GuestInvocationRecorderState::kRecordingDiscovery ||
           state == GuestInvocationRecorderState::kRecordingFinalAttempt;
  }

  bool CompleteFinalAttempt(const GuestPPCRegisterState& exit_state) {
    if (!ValidateClosureCodePages("final-exit")) {
      return false;
    }
    std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> final_pages;
    if (!SnapshotSuppliedDataPages(&final_pages)) {
      return false;
    }

    GuestInvocationRecorderResult accepted;
    accepted.invocation.function_address = selection.root_address;
    accepted.invocation.function_end_address = selection.root_end_address;
    accepted.invocation.entry_address = selection.root_address;
    accepted.invocation.expected_return_address = attempt_return_address;
    accepted.invocation.input = attempt_entry_state;
    accepted.invocation.expected_output = exit_state;
    accepted.invocation.dependency_flags = 0;
    accepted.owner = *owner;
    accepted.attempt_count = attempt_count;

    for (uint32_t page_address : supplied_data_pages) {
      const auto initial_it = initial_pages.find(page_address);
      const auto final_it = final_pages.find(page_address);
      if (initial_it == initial_pages.cend() ||
          final_it == final_pages.cend()) {
        return Reject(GuestInvocationRecorderRejection::kPageReadFailure,
                      "final page snapshots are incomplete");
      }
      if (!known_pages.contains(page_address) &&
          initial_it->second != final_it->second) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      "an untouched closure page changed during capture",
                      kGuestInvocationDependencyCrossThreadMutation);
      }

      GuestInvocationPage input_page;
      input_page.guest_address = page_address;
      input_page.data = initial_it->second;
      accepted.invocation.input_data_pages.push_back(std::move(input_page));
      if (initial_it->second != final_it->second) {
        GuestInvocationPage dirty_page;
        dirty_page.guest_address = page_address;
        dirty_page.data = final_it->second;
        accepted.invocation.expected_dirty_pages.push_back(
            std::move(dirty_page));
      }
    }

    for (size_t order = 0; order < definition_order.size(); ++order) {
      const uint32_t address = definition_order[order];
      const DefinitionRecord& definition = definitions.at(address);
      if (!definition.defined || definition.definition_order != order) {
        return Reject(
            GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
            "successful definition order registry is inconsistent");
      }
      if (closure_functions.contains(address)) {
        accepted.translation_dependencies.push_back(
            {address, definition.end_address});
      }
    }
    if (accepted.translation_dependencies.size() != closure_functions.size()) {
      return Reject(
          GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
          "capture closure is missing a successful definition order entry");
    }
    for (const auto& [page_address, snapshot] : closure_code_pages) {
      GuestInvocationPage code_page;
      code_page.guest_address = page_address;
      code_page.data = snapshot.data;
      accepted.code_pages.push_back(std::move(code_page));
    }
    for (uint32_t address : entered_functions) {
      accepted.entered_functions.push_back(
          {address, definitions.at(address).end_address});
    }
    accepted.touched_page_addresses.assign(known_pages.cbegin(),
                                           known_pages.cend());
    result = std::move(accepted);
    state = GuestInvocationRecorderState::kComplete;
    initial_pages.clear();
    return true;
  }

  bool FinishDiscoveryAttempt() {
    const bool converged = previous_discovery_pages.has_value() &&
                           *previous_discovery_pages == attempt_pages;
    previous_discovery_pages = attempt_pages;
    if (converged) {
      state = GuestInvocationRecorderState::kWaitingForFinalAttempt;
    } else {
      state = GuestInvocationRecorderState::kWaitingForDiscoveryAttempt;
    }
    if (attempt_count >= limits.max_attempts) {
      return Reject(GuestInvocationRecorderRejection::kAttemptLimit,
                    "invocation recording attempts did not leave room for a "
                    "final attempt");
    }
    return true;
  }

  bool CollectAccessPages(uint32_t address, uint32_t size,
                          bool reject_unsupported,
                          std::vector<uint32_t>* pages) {
    pages->clear();
    if (!size) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "memory access has zero size");
    }
    const uint64_t last_byte = uint64_t(address) + size - 1;
    if (last_byte > std::numeric_limits<uint32_t>::max()) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "memory access wraps the guest address space");
    }
    const uint32_t first_page = address & ~(kGuestPageSize - 1);
    const uint32_t last_page =
        static_cast<uint32_t>(last_byte) & ~(kGuestPageSize - 1);
    const uint64_t page_count =
        (uint64_t(last_page) - first_page) / kGuestPageSize + 1;
    if (page_count > limits.max_page_count) {
      return Reject(GuestInvocationRecorderRejection::kPageLimit,
                    "one memory access exceeds the page limit",
                    kGuestInvocationDependencyPageDiscoveryOverflow);
    }
    pages->reserve(static_cast<size_t>(page_count));
    for (uint64_t page = first_page; page <= last_page;
         page += kGuestPageSize) {
      const uint32_t page_address = static_cast<uint32_t>(page);
      if (!IsSupportedPageAddress(page_address)) {
        if (reject_unsupported) {
          return Reject(
              GuestInvocationRecorderRejection::kUnsupportedDependency,
              "memory access uses an unsupported guest page",
              kGuestInvocationDependencyUnsupportedMappingOrProtection);
        }
        continue;
      }
      pages->push_back(page_address);
    }
    return true;
  }

  bool CollectHostProtectionPages(uint32_t first_page, uint32_t last_page,
                                  std::vector<uint32_t>* pages) {
    pages->clear();
    const uint32_t page_mask = ~(limits.host_protection_page_size - 1);
    const uint32_t first_granule = first_page & page_mask;
    const uint32_t last_granule = last_page & page_mask;
    const uint64_t end =
        uint64_t(last_granule) + limits.host_protection_page_size;
    if (end > uint64_t(std::numeric_limits<uint32_t>::max()) + 1) {
      return Reject(GuestInvocationRecorderRejection::kUnsupportedDependency,
                    "host protection granule wraps the guest address space",
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
    }
    for (uint64_t page = first_granule; page < end; page += kGuestPageSize) {
      const uint32_t page_address = static_cast<uint32_t>(page);
      if (!IsSupportedPageAddress(page_address)) {
        return Reject(
            GuestInvocationRecorderRejection::kUnsupportedDependency,
            "host protection granule contains an unsupported guest page",
            kGuestInvocationDependencyUnsupportedMappingOrProtection);
      }
      pages->push_back(page_address);
    }
    return true;
  }

  static const uint32_t* FindBackingView(
      const std::vector<std::pair<uint32_t, uint32_t>>& views,
      uint32_t backing) {
    for (const auto& view : views) {
      if (view.first == backing) {
        return &view.second;
      }
    }
    return nullptr;
  }

  static bool AnyPageWasWritten(const std::set<uint32_t>& written_pages,
                                const std::vector<uint32_t>& pages) {
    for (uint32_t page_address : pages) {
      if (written_pages.contains(page_address)) {
        return true;
      }
    }
    return false;
  }

  // The caller has already rejected a wrapping or oversized access.
  static void RecordWrittenPages(std::set<uint32_t>* written_pages,
                                 uint32_t address, uint32_t size) {
    const uint32_t first_page = address & ~(kGuestPageSize - 1);
    const uint32_t last_page =
        static_cast<uint32_t>(uint64_t(address) + size - 1) &
        ~(kGuestPageSize - 1);
    for (uint64_t page = first_page; page <= last_page;
         page += kGuestPageSize) {
      written_pages->insert(static_cast<uint32_t>(page));
    }
  }

  // Distinguishes the three cross-thread refusal sites and reports whether the
  // page was directly accessed or only pulled in by its protection granule.
  std::string CrossThreadDetail(std::string_view site, uint32_t page) const {
    return fmt::format(
        "another thread wrote a page in the capture closure: {} page {:08X} "
        "backing {:08X} known {} attempt {} state {} granule {}",
        site, page, BackingPageAddress(page),
        known_pages.contains(page) ? 1 : 0, attempt_count,
        static_cast<uint32_t>(state), limits.host_protection_page_size);
  }

  bool AddOwnerPages(const std::vector<uint32_t>& pages) {
    for (uint32_t page : pages) {
      if (cross_thread_written_backing_pages.contains(
              BackingPageAddress(page))) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      CrossThreadDetail("owner-access", page),
                      kGuestInvocationDependencyCrossThreadMutation);
      }
      if (state == GuestInvocationRecorderState::kRecordingFinalAttempt &&
          !known_pages.contains(page)) {
        return Reject(
            GuestInvocationRecorderRejection::kIncompletePageDiscovery,
            "the final attempt touched a page absent from discovery",
            kGuestInvocationDependencyIncompletePageDiscovery);
      }
    }
    std::vector<uint32_t>& supplied_pages = supplied_page_scratch;
    if (!CollectHostProtectionPages(pages.front(), pages.back(),
                                    &supplied_pages)) {
      return false;
    }

    // At most one host protection granule of pages, so a linear scan beats a
    // node-allocating container on this per-access path.
    std::vector<std::pair<uint32_t, uint32_t>>& new_backing_views =
        backing_view_scratch;
    new_backing_views.clear();
    for (uint32_t page : supplied_pages) {
      const uint32_t backing = BackingPageAddress(page);
      const auto code_backing = closure_code_backing_views.find(backing);
      if (code_backing != closure_code_backing_views.cend()) {
        const bool same_page = code_backing->second == page;
        const uint32_t dependency =
            same_page ? kGuestInvocationDependencyUnsupportedMappingOrProtection
                      : kGuestInvocationDependencyPhysicalAlias;
        const bool directly_read =
            std::find(pages.cbegin(), pages.cend(), page) != pages.cend();
        return Reject(
            GuestInvocationRecorderRejection::kUnsupportedDependency,
            fmt::format("invocation data overlaps the code closure: page "
                        "{:08X} backing {:08X} read {} samepage {} access "
                        "{:08X}-{:08X} granule {}",
                        page, backing, directly_read ? 1 : 0, same_page ? 1 : 0,
                        pages.front(), pages.back(),
                        limits.host_protection_page_size),
            dependency);
      }
      const auto known = supplied_data_backing_views.find(backing);
      const uint32_t* added = FindBackingView(new_backing_views, backing);
      if ((known != supplied_data_backing_views.cend() &&
           known->second != page) ||
          (added && *added != page)) {
        return Reject(GuestInvocationRecorderRejection::kUnsupportedDependency,
                      "capture touches aliased 0x8 and 0x9 guest pages",
                      kGuestInvocationDependencyPhysicalAlias);
      }
      if (known == supplied_data_backing_views.cend() && !added) {
        new_backing_views.emplace_back(backing, page);
      }
    }
    const size_t tracked_page_count = supplied_data_backing_views.size() +
                                      cross_thread_written_backing_pages.size();
    if (tracked_page_count > limits.max_page_count ||
        new_backing_views.size() > limits.max_page_count - tracked_page_count) {
      return Reject(GuestInvocationRecorderRejection::kPageLimit,
                    "invocation data closure exceeds the page limit",
                    kGuestInvocationDependencyPageDiscoveryOverflow);
    }
    attempt_pages.insert(pages.cbegin(), pages.cend());
    for (uint32_t page : pages) {
      attempt_read_backing_pages.insert(BackingPageAddress(page));
    }
    known_pages.insert(pages.cbegin(), pages.cend());
    supplied_data_pages.insert(supplied_pages.cbegin(), supplied_pages.cend());
    for (const auto& [backing, page] : new_backing_views) {
      supplied_data_backing_views.emplace(backing, page);
    }
    return true;
  }

  bool AddCrossThreadWritePages(const std::vector<uint32_t>& pages) {
    for (uint32_t page : pages) {
      if (closure_code_backing_views.contains(BackingPageAddress(page))) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      CrossThreadDetail("foreign-write-code", page),
                      kGuestInvocationDependencyCrossThreadMutation);
      }
    }
    // Invocation input is snapshotted when an attempt begins, so a write
    // outside one is already reflected in what the next attempt records. Only
    // a page this attempt actually read can still change what replay must
    // reproduce; a granule neighbour is left to the initial-versus-final
    // comparison at completion.
    if (!IsRecordingAttempt()) {
      return true;
    }
    for (uint32_t page : pages) {
      if (attempt_read_backing_pages.contains(BackingPageAddress(page))) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      CrossThreadDetail("foreign-write-data", page),
                      kGuestInvocationDependencyCrossThreadMutation);
      }
    }
    std::set<uint32_t> new_backing_pages;
    for (uint32_t page : pages) {
      const uint32_t backing = BackingPageAddress(page);
      if (!cross_thread_written_backing_pages.contains(backing)) {
        new_backing_pages.insert(backing);
      }
    }
    const size_t tracked_page_count = supplied_data_backing_views.size() +
                                      cross_thread_written_backing_pages.size();
    if (tracked_page_count > limits.max_page_count ||
        new_backing_pages.size() > limits.max_page_count - tracked_page_count) {
      return Reject(GuestInvocationRecorderRejection::kPageLimit,
                    "cross-thread write tracking exceeds the page limit",
                    kGuestInvocationDependencyPageDiscoveryOverflow);
    }
    for (uint32_t page : pages) {
      cross_thread_written_backing_pages.insert(BackingPageAddress(page));
    }
    return true;
  }

  bool CheckOwnerWrite(uint32_t address, uint32_t size) {
    const uint32_t last = static_cast<uint32_t>(uint64_t(address) + size - 1);
    if (RangeSharesClosureCodePages(address, last)) {
      return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                    "a recorded write overlaps the code closure",
                    kGuestInvocationDependencySelfModifyingCode);
    }
    RecordWrittenPages(&owner_written_pages, address, size);
    return true;
  }

  bool CheckCrossThreadWrite(uint32_t address, uint32_t size) {
    const uint32_t last = static_cast<uint32_t>(uint64_t(address) + size - 1);
    if (RangeSharesClosureCodePages(address, last)) {
      return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                    "another thread wrote a page in the code closure",
                    kGuestInvocationDependencySelfModifyingCode |
                        kGuestInvocationDependencyCrossThreadMutation);
    }
    RecordWrittenPages(&cross_thread_written_pages, address, size);
    return true;
  }

  bool RangeSharesClosureCodePages(uint32_t first, uint32_t last) const {
    const uint32_t first_page = first & ~(kGuestPageSize - 1);
    const uint32_t last_page = last & ~(kGuestPageSize - 1);
    const auto page = closure_code_pages.lower_bound(first_page);
    return page != closure_code_pages.cend() && page->first <= last_page;
  }

  bool TrackDefinitionCodeWrite(uint32_t address, uint32_t size) {
    if (!size) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "memory access has zero size");
    }
    const uint64_t last_byte = uint64_t(address) + size - 1;
    if (last_byte > std::numeric_limits<uint32_t>::max()) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "memory access wraps the guest address space");
    }
    const uint32_t first_page = address & ~(kGuestPageSize - 1);
    const uint32_t last_page =
        static_cast<uint32_t>(last_byte) & ~(kGuestPageSize - 1);
    auto page = definition_page_write_generations.lower_bound(first_page);
    while (page != definition_page_write_generations.end() &&
           page->first <= last_page) {
      if (page->second == std::numeric_limits<uint64_t>::max()) {
        return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                      "definition code-page write generation overflowed");
      }
      ++page->second;
      if (closure_code_pages.contains(page->first)) {
        closure_code_written = true;
      }
      ++page;
    }
    return true;
  }

  GuestInvocationRecorderSelection selection;
  GuestInvocationRecorderLimits limits;
  GuestInvocationRecorderPageReader& page_reader;
  const GuestInvocationRecorderClock& clock;
  uint64_t deadline = 0;
  std::optional<GuestInvocationRecorderIdentity> owner;
  bool callback_active = false;

  GuestInvocationRecorderState state =
      GuestInvocationRecorderState::kWaitingForOccurrence;
  GuestInvocationRecorderRejection rejection =
      GuestInvocationRecorderRejection::kNone;
  uint32_t rejected_dependency_flags = 0;
  std::string rejection_message;
  std::optional<GuestInvocationRecorderResult> result;

  uint64_t event_count = 0;
  uint64_t access_count = 0;
  uint32_t root_occurrence_count = 0;
  uint32_t attempt_count = 0;
  uint32_t attempt_return_address = 0;
  GuestPPCRegisterState attempt_entry_state = {};

  std::vector<CallFrame> call_stack;
  std::optional<uint32_t> inherited_return_address;
  std::map<uint32_t, DefinitionRecord> definitions;
  std::vector<uint32_t> definition_order;
  std::map<uint32_t, uint64_t> definition_page_write_generations;
  std::set<uint32_t> pending_definition_snapshots;
  std::map<uint32_t, CodePageSnapshot> definition_code_pages;
  std::map<uint32_t, CodePageSnapshot> closure_code_pages;
  std::map<uint32_t, uint32_t> closure_code_backing_views;
  std::map<uint32_t, uint32_t> closure_functions;
  std::set<uint32_t> entered_functions;
  std::set<uint32_t> known_pages;
  std::set<uint32_t> supplied_data_pages;
  std::map<uint32_t, uint32_t> supplied_data_backing_views;
  std::set<uint32_t> attempt_pages;
  std::set<uint32_t> attempt_read_backing_pages;
  std::optional<std::set<uint32_t>> previous_discovery_pages;
  std::set<uint32_t> cross_thread_written_backing_pages;
  std::set<uint32_t> owner_written_pages;
  std::set<uint32_t> cross_thread_written_pages;
  bool closure_code_written = false;
  std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> initial_pages;

  // Reused by the per-access callbacks so recording a long invocation does not
  // allocate once per guest memory access.
  std::vector<uint32_t> access_page_scratch;
  std::vector<uint32_t> supplied_page_scratch;
  std::vector<std::pair<uint32_t, uint32_t>> backing_view_scratch;
};

std::unique_ptr<GuestInvocationRecorder> GuestInvocationRecorder::Create(
    const GuestInvocationRecorderSelection& selection,
    const GuestInvocationRecorderLimits& limits,
    GuestInvocationRecorderPageReader& page_reader,
    const GuestInvocationRecorderClock& clock, std::string* error) {
  if (error) {
    error->clear();
  }
  auto fail = [error](std::string_view message) {
    if (error) {
      error->assign(message);
    }
    return std::unique_ptr<GuestInvocationRecorder>();
  };
  if (!IsValidFunctionExtent(selection.root_address,
                             selection.root_end_address)) {
    return fail("recorder selection has an invalid aligned root extent");
  }
  if (!selection.occurrence) {
    return fail("recorder selection occurrence must be 1-based");
  }
  if (static_cast<bool>(selection.owner.context_id) !=
      static_cast<bool>(selection.owner.thread_id)) {
    return fail("recorder selection owner must be fully specified or omitted");
  }
  if (limits.max_attempts < 3 || !limits.max_duration_ticks ||
      !limits.max_page_count || !limits.max_code_page_count ||
      limits.max_page_count >
          GuestInvocationArtifactCodec::kMaxDataPagesPerInvocation ||
      limits.max_code_page_count >
          GuestInvocationRecorderLimits::kMaximumCodePageCount ||
      !IsPowerOfTwo(limits.host_protection_page_size) ||
      limits.host_protection_page_size <
          GuestInvocationRecorderLimits::kMinimumHostProtectionPageSize ||
      limits.host_protection_page_size >
          GuestInvocationRecorderLimits::kMaximumHostProtectionPageSize ||
      !limits.max_access_count || !limits.max_call_depth ||
      !limits.max_event_count || !limits.max_function_count ||
      selection.occurrence > limits.max_event_count) {
    return fail("recorder limits are invalid or cannot reach the selection");
  }
  const uint64_t start = clock.NowTicks();
  if (limits.max_duration_ticks >
      std::numeric_limits<uint64_t>::max() - start) {
    return fail("recorder deadline overflows the injected clock");
  }
  const uint64_t deadline = start + limits.max_duration_ticks;
  std::unique_ptr<Impl> impl =
      std::make_unique<Impl>(selection, limits, page_reader, clock, deadline);
  return std::unique_ptr<GuestInvocationRecorder>(
      new GuestInvocationRecorder(std::move(impl)));
}

GuestInvocationRecorder::GuestInvocationRecorder(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestInvocationRecorder::~GuestInvocationRecorder() = default;

bool GuestInvocationRecorder::Poll() {
  Impl::CallbackScope callback(*impl_);
  if (!callback || !impl_->CheckDeadline()) {
    return false;
  }
  return impl_->SnapshotPendingDefinitions() !=
         Impl::CodePageReadResult::kFailure;
}

bool GuestInvocationRecorder::OnFunctionDependency(
    uint32_t source_address, uint32_t dependency_address) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  return impl_->BeginEvent() &&
         impl_->RegisterDependency(source_address, dependency_address);
}

bool GuestInvocationRecorder::OnFunctionDefined(uint32_t address,
                                                uint32_t end_address) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  return impl_->BeginEvent() && impl_->RegisterDefinition(address, end_address);
}

bool GuestInvocationRecorder::OnFunctionEntry(
    const GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t end_address, const GuestPPCRegisterState& entry_state) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  if (!impl_->CheckDeadline()) {
    return false;
  }

  if (impl_->state == GuestInvocationRecorderState::kWaitingForOccurrence) {
    if (address != impl_->selection.root_address) {
      return true;
    }
    if (!impl_->BeginEvent()) {
      return false;
    }
    if (end_address != impl_->selection.root_end_address) {
      return impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                           "selected root extent changed before entry");
    }
    if (!impl_->IsValidIdentity(identity)) {
      return impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                           "root occurrence has an invalid identity");
    }
    ++impl_->root_occurrence_count;
    if (impl_->root_occurrence_count < impl_->selection.occurrence) {
      return true;
    }
    return impl_->ClaimOwner(identity) && impl_->BeginAttempt(entry_state);
  }

  if (!impl_->IsValidIdentity(identity)) {
    if (!impl_->IsRecordingAttempt()) {
      return true;
    }
    return impl_->BeginEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "function entry has an invalid identity");
  }
  if (!impl_->IsOwner(identity)) {
    if (!impl_->IsRecordingAttempt() || !impl_->AliasesOwner(identity)) {
      return true;
    }
    return impl_->BeginEvent() && impl_->RejectOwnerAlias(identity);
  }

  if (!impl_->IsRecordingAttempt()) {
    if (address != impl_->selection.root_address) {
      return true;
    }
    if (!impl_->BeginEvent()) {
      return false;
    }
    if (end_address != impl_->selection.root_end_address) {
      return impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                           "selected root extent changed before retry");
    }
    return impl_->BeginAttempt(entry_state);
  }

  if (!impl_->BeginEvent()) {
    return false;
  }
  if (std::any_of(impl_->call_stack.cbegin(), impl_->call_stack.cend(),
                  [address](const Impl::CallFrame& frame) {
                    return frame.address == address;
                  })) {
    return impl_->Reject(GuestInvocationRecorderRejection::kRecursiveInvocation,
                         "recursive guest invocation is unsupported",
                         kGuestInvocationDependencyRecursiveInvocation);
  }
  if (impl_->call_stack.size() >= impl_->limits.max_call_depth) {
    return impl_->Reject(GuestInvocationRecorderRejection::kCallDepthLimit,
                         "invocation recording call-depth limit exceeded");
  }
  if (!impl_->AddTranslationClosureSeed(address, end_address, false)) {
    return false;
  }
  uint32_t return_address = 0;
  if (impl_->inherited_return_address.has_value()) {
    return_address = *impl_->inherited_return_address;
    impl_->inherited_return_address.reset();
  } else if (!impl_->ValidateReturnBoundary(address, end_address, entry_state,
                                            false, &return_address)) {
    return false;
  }
  impl_->call_stack.push_back({address, return_address});
  impl_->entered_functions.insert(address);
  return true;
}

bool GuestInvocationRecorder::OnFunctionExit(
    const GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t return_address, const GuestPPCRegisterState& exit_state) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  if (!impl_->IsRecordingAttempt()) {
    return true;
  }
  if (!impl_->IsValidIdentity(identity)) {
    return impl_->BeginEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "function exit has an invalid identity");
  }
  if (!impl_->IsOwner(identity)) {
    if (!impl_->AliasesOwner(identity)) {
      return true;
    }
    return impl_->BeginEvent() && impl_->RejectOwnerAlias(identity);
  }
  if (!impl_->BeginEvent()) {
    return false;
  }
  if (impl_->call_stack.empty() ||
      impl_->call_stack.back().address != address ||
      impl_->call_stack.back().return_address != return_address) {
    return impl_->Reject(GuestInvocationRecorderRejection::kUnbalancedReturn,
                         "function exit does not match the recorded call stack",
                         kGuestInvocationDependencyUnbalancedReturn);
  }
  impl_->call_stack.pop_back();
  impl_->inherited_return_address.reset();
  if (!impl_->call_stack.empty()) {
    return true;
  }
  if (return_address != impl_->attempt_return_address) {
    return impl_->Reject(GuestInvocationRecorderRejection::kUnbalancedReturn,
                         "root exit does not match its normal return boundary",
                         kGuestInvocationDependencyUnbalancedReturn);
  }
  if (impl_->state == GuestInvocationRecorderState::kRecordingFinalAttempt) {
    return impl_->CompleteFinalAttempt(exit_state);
  }
  return impl_->FinishDiscoveryAttempt();
}

bool GuestInvocationRecorder::OnMemoryAccess(
    const GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t size, GuestInvocationRecorderMemoryAccess access) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  if (!impl_->CheckDeadline()) {
    return false;
  }
  if (!IsValidAccess(access)) {
    if (!impl_->attempt_count) {
      return true;
    }
    return impl_->BeginAccessEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "memory access has an invalid access type");
  }
  if (HasWriteAccess(access) &&
      !impl_->TrackDefinitionCodeWrite(address, size)) {
    return false;
  }
  if (!impl_->attempt_count) {
    return true;
  }
  if (!impl_->IsValidIdentity(identity)) {
    if (!impl_->IsRecordingAttempt() && !HasWriteAccess(access)) {
      return true;
    }
    return impl_->BeginAccessEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "memory access has an invalid identity");
  }
  if (!impl_->IsRecordingAttempt() && impl_->IsOwner(identity)) {
    return true;
  }
  const bool recording_owner =
      impl_->IsRecordingAttempt() && impl_->IsOwner(identity);
  if (!recording_owner) {
    if (impl_->IsRecordingAttempt() && impl_->AliasesOwner(identity)) {
      return impl_->BeginAccessEvent() && impl_->RejectOwnerAlias(identity);
    }
    if (!HasWriteAccess(access)) {
      return true;
    }
  }

  if (!impl_->BeginAccessEvent()) {
    return false;
  }
  std::vector<uint32_t>& pages = impl_->access_page_scratch;
  if (!impl_->CollectAccessPages(address, size, recording_owner, &pages)) {
    return false;
  }
  if (!recording_owner && impl_->IsOwner(identity)) {
    return impl_->CheckOwnerWrite(address, size);
  }
  if (recording_owner) {
    if (HasWriteAccess(access) && !impl_->CheckOwnerWrite(address, size)) {
      return false;
    }
    return impl_->AddOwnerPages(pages);
  }
  if (!impl_->CheckCrossThreadWrite(address, size)) {
    return false;
  }
  return impl_->AddCrossThreadWritePages(pages);
}

bool GuestInvocationRecorder::OnUnsupportedDependency(
    const GuestInvocationRecorderIdentity& identity,
    uint32_t dependency_flags) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete ||
      !dependency_flags) {
    return true;
  }
  if (!impl_->IsRecordingAttempt()) {
    return true;
  }
  if (!impl_->IsValidIdentity(identity)) {
    return impl_->BeginEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "dependency event has an invalid identity");
  }
  if (!impl_->IsOwner(identity)) {
    if (!impl_->AliasesOwner(identity)) {
      return true;
    }
    return impl_->BeginEvent() && impl_->RejectOwnerAlias(identity);
  }
  if (!impl_->BeginEvent()) {
    return false;
  }
  return impl_->Reject(GuestInvocationRecorderRejection::kUnsupportedDependency,
                       "invocation has an unsupported dependency",
                       dependency_flags);
}

bool GuestInvocationRecorder::OnTailCall(
    const GuestInvocationRecorderIdentity& identity, uint32_t from_address,
    uint32_t target_address) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  if (!impl_->IsRecordingAttempt()) {
    return true;
  }
  if (!impl_->IsValidIdentity(identity)) {
    return impl_->BeginEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "tail-call event has an invalid identity");
  }
  if (!impl_->IsOwner(identity)) {
    if (!impl_->AliasesOwner(identity)) {
      return true;
    }
    return impl_->BeginEvent() && impl_->RejectOwnerAlias(identity);
  }
  if (!impl_->BeginEvent()) {
    return false;
  }
  if (!from_address || !target_address) {
    return impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "tail-call event has an invalid address");
  }
  return impl_->TakeTailCall(from_address);
}

bool GuestInvocationRecorder::OnUnwindOrLongjmp(
    const GuestInvocationRecorderIdentity& identity) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete) {
    return true;
  }
  if (!impl_->IsRecordingAttempt()) {
    return true;
  }
  if (!impl_->IsValidIdentity(identity)) {
    return impl_->BeginEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "unwind event has an invalid identity");
  }
  if (!impl_->IsOwner(identity)) {
    if (!impl_->AliasesOwner(identity)) {
      return true;
    }
    return impl_->BeginEvent() && impl_->RejectOwnerAlias(identity);
  }
  if (!impl_->BeginEvent()) {
    return false;
  }
  return impl_->Reject(GuestInvocationRecorderRejection::kUnbalancedReturn,
                       "unwind or longjmp crossed the recorded invocation",
                       kGuestInvocationDependencyUnbalancedReturn);
}

bool GuestInvocationRecorder::OnAsyncReentry(
    const GuestInvocationRecorderIdentity& identity) {
  Impl::CallbackScope callback(*impl_);
  if (!callback || impl_->state == GuestInvocationRecorderState::kRejected) {
    return false;
  }
  if (impl_->state == GuestInvocationRecorderState::kComplete ||
      !impl_->IsRecordingAttempt()) {
    return true;
  }
  if (!impl_->IsValidIdentity(identity)) {
    return impl_->BeginEvent() &&
           impl_->Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                         "async event has an invalid identity");
  }
  if (!impl_->IsOwner(identity) && !impl_->AliasesOwner(identity)) {
    return true;
  }
  if (!impl_->BeginEvent()) {
    return false;
  }
  return impl_->Reject(GuestInvocationRecorderRejection::kAsyncReentry,
                       "async interrupt or reentry crossed the invocation",
                       kGuestInvocationDependencyAsyncInterrupt);
}

GuestInvocationRecorderState GuestInvocationRecorder::state() const {
  return impl_->state;
}

bool GuestInvocationRecorder::is_recording_attempt() const {
  return impl_->IsRecordingAttempt();
}

bool GuestInvocationRecorder::is_recording_identity(
    const GuestInvocationRecorderIdentity& identity) const {
  return impl_->IsRecordingAttempt() && impl_->IsOwner(identity);
}

GuestInvocationRecorderRejection GuestInvocationRecorder::rejection() const {
  return impl_->rejection;
}

uint32_t GuestInvocationRecorder::rejected_dependency_flags() const {
  return impl_->rejected_dependency_flags;
}

const std::string& GuestInvocationRecorder::rejection_message() const {
  return impl_->rejection_message;
}

const GuestInvocationRecorderResult* GuestInvocationRecorder::result() const {
  return impl_->result ? &*impl_->result : nullptr;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
