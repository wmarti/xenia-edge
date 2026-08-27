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
#include <string_view>
#include <utility>

namespace xe {
namespace cpu {
namespace ppc {

namespace {

constexpr uint32_t kGuestPageSize = GuestInvocationArtifactCodec::kPageSize;

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

bool RangesSharePage(uint32_t first, uint32_t last, uint32_t other_first,
                     uint32_t other_last) {
  constexpr uint32_t kPageMask = ~(kGuestPageSize - 1);
  return (first & kPageMask) <= (other_last & kPageMask) &&
         (last & kPageMask) >= (other_first & kPageMask);
}

}  // namespace

struct GuestInvocationRecorder::Impl {
  struct AddressRange {
    uint32_t first = 0;
    uint32_t last = 0;
  };

  struct DefinitionRecord {
    uint32_t end_address = 0;
    uint32_t definition_order = 0;
    bool defined = false;
    std::set<uint32_t> dependencies;
    std::vector<uint32_t> code_page_addresses;
  };

  struct CodePageSnapshot {
    std::array<uint8_t, kGuestPageSize> data = {};
    uint64_t write_generation = 0;
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

  bool ReadStableCodePage(uint32_t page_address,
                          std::array<uint8_t, kGuestPageSize>* stable_page) {
    std::array<uint8_t, kGuestPageSize> verification = {};
    const bool first_read = page_reader.ReadPage(page_address, stable_page);
    if (state == GuestInvocationRecorderState::kRejected) {
      return false;
    }
    const bool second_read =
        first_read && page_reader.ReadPage(page_address, &verification);
    if (state == GuestInvocationRecorderState::kRejected) {
      return false;
    }
    if (!first_read || !second_read) {
      return Reject(GuestInvocationRecorderRejection::kPageReadFailure,
                    "unable to read a guest code page",
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
    }
    if (*stable_page != verification) {
      return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                    "guest code changed while it was sampled",
                    kGuestInvocationDependencySelfModifyingCode);
    }
    return true;
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

    const uint32_t first_page = address & ~(kGuestPageSize - 1);
    const uint32_t last_page = end_address & ~(kGuestPageSize - 1);
    const uint64_t page_count =
        (uint64_t(last_page) - first_page) / kGuestPageSize + 1;
    std::set<uint32_t> new_page_addresses;
    for (uint64_t page = first_page; page <= last_page;
         page += kGuestPageSize) {
      const uint32_t page_address = static_cast<uint32_t>(page);
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

    std::map<uint32_t, CodePageSnapshot> code_pages;
    for (uint64_t page = first_page; page <= last_page;
         page += kGuestPageSize) {
      const uint32_t page_address = static_cast<uint32_t>(page);
      CodePageSnapshot snapshot;
      if (!ReadStableCodePage(page_address, &snapshot.data)) {
        return false;
      }
      const auto generation =
          definition_page_write_generations.find(page_address);
      snapshot.write_generation =
          generation == definition_page_write_generations.cend()
              ? 0
              : generation->second;
      const auto existing = definition_code_pages.find(page_address);
      if (existing != definition_code_pages.cend() &&
          (existing->second.write_generation != snapshot.write_generation ||
           existing->second.data != snapshot.data)) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "shared code page changed between successful definitions",
                      kGuestInvocationDependencySelfModifyingCode);
      }
      code_pages.emplace(page_address, std::move(snapshot));
    }
    for (uint32_t page_address : new_page_addresses) {
      definition_page_write_generations.emplace(page_address, 0);
    }
    definition->code_page_addresses.reserve(code_pages.size());
    for (auto& [page_address, snapshot] : code_pages) {
      definition->code_page_addresses.push_back(page_address);
      definition_code_pages.emplace(page_address, std::move(snapshot));
    }
    definition->defined = true;
    definition->end_address = end_address;
    definition->definition_order =
        static_cast<uint32_t>(definition_order.size());
    definition_order.push_back(address);
    return true;
  }

  bool MergeDefinitionCodePages(const DefinitionRecord& definition) {
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
      closure_code_pages.emplace(page_address, snapshot);
    }
    return true;
  }

  bool ValidateClosureCodePages() {
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
      if (!ReadStableCodePage(page_address, &current)) {
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
                                 uint32_t expected_end_address) {
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

    std::vector<uint32_t> pending = {address};
    while (!pending.empty()) {
      const uint32_t function_address = pending.back();
      pending.pop_back();
      if (closure_functions.contains(function_address)) {
        continue;
      }
      const auto definition = definitions.find(function_address);
      if (definition == definitions.cend() || !definition->second.defined) {
        return Reject(
            GuestInvocationRecorderRejection::kIncompleteTranslationClosure,
            "capture dependency lacks a successful definition/extent/order");
      }
      for (const AddressRange& write : owner_writes) {
        if (RangesSharePage(write.first, write.last, function_address,
                            definition->second.end_address)) {
          return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                        "a recorded write overlaps guest code",
                        kGuestInvocationDependencySelfModifyingCode);
        }
      }
      for (const AddressRange& write : cross_thread_writes) {
        if (RangesSharePage(write.first, write.last, function_address,
                            definition->second.end_address)) {
          return Reject(
              GuestInvocationRecorderRejection::kSelfModifyingCode,
              "another thread wrote guest code in the capture closure",
              kGuestInvocationDependencySelfModifyingCode |
                  kGuestInvocationDependencyCrossThreadMutation);
        }
      }
      if (!MergeDefinitionCodePages(definition->second)) {
        return false;
      }
      closure_functions.emplace(function_address,
                                definition->second.end_address);
      pending.insert(pending.end(), definition->second.dependencies.cbegin(),
                     definition->second.dependencies.cend());
    }
    return ValidateClosureCodePages();
  }

  bool ValidateReturnBoundary(uint32_t function_address,
                              uint32_t function_end_address,
                              const GuestPPCRegisterState& state,
                              uint32_t* output) {
    if (!state.link_register ||
        state.link_register > std::numeric_limits<uint32_t>::max() ||
        (state.link_register & 3)) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "function entry has an invalid return boundary");
    }
    const uint32_t return_address = static_cast<uint32_t>(state.link_register);
    if (return_address >= function_address &&
        return_address <= function_end_address) {
      return Reject(GuestInvocationRecorderRejection::kInvalidEvent,
                    "return boundary is inside the entered function");
    }
    *output = return_address;
    return true;
  }

  bool SnapshotKnownPages(
      std::map<uint32_t, std::array<uint8_t, kGuestPageSize>>* output) {
    output->clear();
    for (uint32_t page_address : known_pages) {
      std::array<uint8_t, kGuestPageSize> page = {};
      const bool read = page_reader.ReadPage(page_address, &page);
      if (state == GuestInvocationRecorderState::kRejected) {
        output->clear();
        return false;
      }
      if (!read) {
        output->clear();
        return Reject(GuestInvocationRecorderRejection::kPageReadFailure,
                      "unable to snapshot a discovered guest page",
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
                                selection.root_end_address, entry_state,
                                &return_address)) {
      return false;
    }
    ++attempt_count;
    attempt_pages.clear();
    call_stack.clear();
    call_stack.push_back({selection.root_address, return_address});
    entered_functions.insert(selection.root_address);
    if (!AddTranslationClosureSeed(selection.root_address,
                                   selection.root_end_address)) {
      return false;
    }
    attempt_entry_state = entry_state;
    attempt_return_address = return_address;

    if (state == GuestInvocationRecorderState::kWaitingForFinalAttempt) {
      if (!SnapshotKnownPages(&initial_pages)) {
        return false;
      }
      state = GuestInvocationRecorderState::kRecordingFinalAttempt;
    } else {
      initial_pages.clear();
      state = GuestInvocationRecorderState::kRecordingDiscovery;
    }
    return true;
  }

  bool IsRecordingAttempt() const {
    return state == GuestInvocationRecorderState::kRecordingDiscovery ||
           state == GuestInvocationRecorderState::kRecordingFinalAttempt;
  }

  bool IsCodePage(uint32_t page_address) const {
    const uint64_t page_end = uint64_t(page_address) + kGuestPageSize - 1;
    for (const auto& [function_address, function_end] : closure_functions) {
      if (function_address > page_end) {
        break;
      }
      if (function_end >= page_address) {
        return true;
      }
    }
    return false;
  }

  bool CompleteFinalAttempt(const GuestPPCRegisterState& exit_state) {
    if (!ValidateClosureCodePages()) {
      return false;
    }
    std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> final_pages;
    if (!SnapshotKnownPages(&final_pages)) {
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

    for (uint32_t page_address : known_pages) {
      const auto initial_it = initial_pages.find(page_address);
      const auto final_it = final_pages.find(page_address);
      if (initial_it == initial_pages.cend() ||
          final_it == final_pages.cend()) {
        return Reject(GuestInvocationRecorderRejection::kPageReadFailure,
                      "final page snapshots are incomplete");
      }
      if (IsCodePage(page_address)) {
        if (initial_it->second != final_it->second) {
          return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                        "guest code changed during the final attempt",
                        kGuestInvocationDependencySelfModifyingCode);
        }
        continue;
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

  bool CheckPhysicalAliases(const std::vector<uint32_t>& pages) {
    std::map<uint32_t, uint32_t> access_backing_to_view;
    for (uint32_t page : pages) {
      const uint32_t backing = BackingPageAddress(page);
      const auto known = known_backing_views.find(backing);
      const auto access = access_backing_to_view.find(backing);
      if ((known != known_backing_views.cend() && known->second != page) ||
          (access != access_backing_to_view.cend() && access->second != page)) {
        return Reject(GuestInvocationRecorderRejection::kUnsupportedDependency,
                      "capture touches aliased 0x8 and 0x9 guest pages",
                      kGuestInvocationDependencyPhysicalAlias);
      }
      access_backing_to_view.emplace(backing, page);
    }
    return true;
  }

  bool AddOwnerPages(const std::vector<uint32_t>& pages) {
    for (uint32_t page : pages) {
      if (cross_thread_written_backing_pages.contains(
              BackingPageAddress(page))) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      "another thread wrote a page in the capture closure",
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
    if (!CheckPhysicalAliases(pages)) {
      return false;
    }

    std::set<uint32_t> new_backing_pages;
    for (uint32_t page : pages) {
      const uint32_t backing = BackingPageAddress(page);
      if (!known_backing_views.contains(backing) &&
          !cross_thread_written_backing_pages.contains(backing)) {
        new_backing_pages.insert(backing);
      }
    }
    const size_t tracked_page_count =
        known_backing_views.size() + cross_thread_written_backing_pages.size();
    if (tracked_page_count > limits.max_page_count ||
        new_backing_pages.size() > limits.max_page_count - tracked_page_count) {
      return Reject(GuestInvocationRecorderRejection::kPageLimit,
                    "invocation recording page limit exceeded",
                    kGuestInvocationDependencyPageDiscoveryOverflow);
    }
    attempt_pages.insert(pages.cbegin(), pages.cend());
    known_pages.insert(pages.cbegin(), pages.cend());
    for (uint32_t page : pages) {
      known_backing_views.emplace(BackingPageAddress(page), page);
    }
    return true;
  }

  bool AddCrossThreadWritePages(const std::vector<uint32_t>& pages) {
    for (uint32_t page : pages) {
      const uint32_t backing = BackingPageAddress(page);
      if (known_backing_views.contains(backing)) {
        return Reject(GuestInvocationRecorderRejection::kCrossThreadMutation,
                      "another thread wrote a page in the capture closure",
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
    const size_t tracked_page_count =
        known_backing_views.size() + cross_thread_written_backing_pages.size();
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
    for (const auto& [function_address, function_end] : closure_functions) {
      if ((function_address & ~(kGuestPageSize - 1)) >
          (last & ~(kGuestPageSize - 1))) {
        break;
      }
      if (RangesSharePage(address, last, function_address, function_end)) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "a recorded write overlaps guest code",
                      kGuestInvocationDependencySelfModifyingCode);
      }
    }
    owner_writes.push_back({address, last});
    return true;
  }

  bool CheckCrossThreadWrite(uint32_t address, uint32_t size) {
    const uint32_t last = static_cast<uint32_t>(uint64_t(address) + size - 1);
    for (const auto& [function_address, function_end] : closure_functions) {
      if ((function_address & ~(kGuestPageSize - 1)) >
          (last & ~(kGuestPageSize - 1))) {
        break;
      }
      if (RangesSharePage(address, last, function_address, function_end)) {
        return Reject(GuestInvocationRecorderRejection::kSelfModifyingCode,
                      "another thread wrote guest code in the capture closure",
                      kGuestInvocationDependencySelfModifyingCode |
                          kGuestInvocationDependencyCrossThreadMutation);
      }
    }
    cross_thread_writes.push_back({address, last});
    return true;
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
  std::map<uint32_t, DefinitionRecord> definitions;
  std::vector<uint32_t> definition_order;
  std::map<uint32_t, uint64_t> definition_page_write_generations;
  std::map<uint32_t, CodePageSnapshot> definition_code_pages;
  std::map<uint32_t, CodePageSnapshot> closure_code_pages;
  std::map<uint32_t, uint32_t> closure_functions;
  std::set<uint32_t> entered_functions;
  std::set<uint32_t> known_pages;
  std::map<uint32_t, uint32_t> known_backing_views;
  std::set<uint32_t> attempt_pages;
  std::optional<std::set<uint32_t>> previous_discovery_pages;
  std::set<uint32_t> cross_thread_written_backing_pages;
  std::vector<AddressRange> owner_writes;
  std::vector<AddressRange> cross_thread_writes;
  std::map<uint32_t, std::array<uint8_t, kGuestPageSize>> initial_pages;
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
  return callback && impl_->CheckDeadline();
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
  if (!impl_->AddTranslationClosureSeed(address, end_address)) {
    return false;
  }
  uint32_t return_address = 0;
  if (!impl_->ValidateReturnBoundary(address, end_address, entry_state,
                                     &return_address)) {
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
  if (address != impl_->selection.root_address) {
    return true;
  }
  if (!impl_->call_stack.empty() ||
      return_address != impl_->attempt_return_address) {
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
  std::vector<uint32_t> pages;
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
  return impl_->Reject(GuestInvocationRecorderRejection::kUnbalancedReturn,
                       "tail calls are unsupported in invocation recording",
                       kGuestInvocationDependencyUnbalancedReturn);
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
