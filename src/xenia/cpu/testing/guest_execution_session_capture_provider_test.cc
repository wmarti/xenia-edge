/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_execution_session_capture_provider.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/test_module.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
namespace testing {
namespace {

constexpr uint32_t kFunctionAddress = 0x82040000u;
constexpr uint32_t kFunctionEndAddress = kFunctionAddress + 4;
constexpr uint32_t kMiddleFunctionAddress = kFunctionAddress + 0x40;
constexpr uint32_t kMiddleFunctionEndAddress = kMiddleFunctionAddress + 0xC;
constexpr uint32_t kLeafFunctionAddress = kFunctionAddress + 0x80;
constexpr uint32_t kLeafFunctionEndAddress = kLeafFunctionAddress + 0xC;
constexpr uint32_t kAliasDataAddress = 0x82080000u;
constexpr uint32_t kAliasDataMirrorAddress = 0x92080000u;
constexpr uint32_t kOuterReturnAddress = 0xBCBCBCBCu;
constexpr uint32_t kFinalOuterReturnAddress = 0xACACACACu;
constexpr uint32_t kThreadId = 0x880u;
constexpr uint32_t kSecondThreadId = 0x881u;
constexpr uint32_t kBranchToLinkRegister = 0x4E800020u;
constexpr uint32_t kExportThunkAddress = 0x8270D724u;
constexpr uint32_t kExportOrdinal = 0x1DBu;

std::unique_ptr<TestModule> CreateFunctionModule(Processor* processor,
                                                 std::string_view name,
                                                 uint32_t address,
                                                 uint32_t end_address) {
  return std::make_unique<TestModule>(
      processor, name,
      [address](uint32_t candidate) { return candidate == address; },
      [address](hir::HIRBuilder& builder) {
        builder.SourceOffset(address);
        builder.Return();
        return true;
      },
      false, end_address);
}

class ProviderHarness final {
 public:
  ProviderHarness() {
    memory = std::make_unique<Memory>();
    REQUIRE(memory->Initialize());
    protection_granule_size = static_cast<uint32_t>(xe::memory::page_size());
    REQUIRE(protection_granule_size >= JitCorpus::kPageSize);
    code_allocation_address = kFunctionAddress & ~(protection_granule_size - 1);
    BaseHeap* code_heap = memory->LookupHeap(code_allocation_address);
    REQUIRE(code_heap);
    REQUIRE(code_heap->AllocFixed(
        code_allocation_address, protection_granule_size,
        protection_granule_size,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    store_and_swap<uint32_t>(memory->TranslateVirtual(kFunctionAddress),
                             kBranchToLinkRegister);
    store_and_swap<uint32_t>(memory->TranslateVirtual(kFunctionEndAddress),
                             kBranchToLinkRegister);
    REQUIRE(code_heap->Protect(code_allocation_address, protection_granule_size,
                               kMemoryProtectRead));

    processor = std::make_unique<Processor>(memory.get(), nullptr);
    REQUIRE(processor->Setup(CreateBackend()));
    REQUIRE(processor->AddModule(
        CreateFunctionModule(processor.get(), "SessionCaptureProviderRoot",
                             kFunctionAddress, kFunctionEndAddress)));
    REQUIRE(processor->AddModule(CreateFunctionModule(
        processor.get(), "SessionCaptureProviderMiddle", kMiddleFunctionAddress,
        kMiddleFunctionEndAddress)));
    REQUIRE(processor->AddModule(
        CreateFunctionModule(processor.get(), "SessionCaptureProviderLeaf",
                             kLeafFunctionAddress, kLeafFunctionEndAddress)));
    processor->backend()->CommitExecutableRange(kFunctionAddress,
                                                kFunctionAddress + 0x1000);

    std::string error;
    provider = GuestExecutionSessionCaptureProvider::CreateAndAttach(
        *memory, *processor, {}, &error);
    REQUIRE(provider);
    REQUIRE(error.empty());
    function = processor->ResolveFunction(kFunctionAddress);
    REQUIRE(function);
    REQUIRE(function->status() == Symbol::Status::kDefined);
    REQUIRE(processor->ResolveFunction(kMiddleFunctionAddress));
    REQUIRE(processor->ResolveFunction(kLeafFunctionAddress));

    stack_address = memory->SystemHeapAlloc(64 * 1024, 4096);
    data_address = memory->SystemHeapAlloc(protection_granule_size,
                                           protection_granule_size);
    REQUIRE(stack_address);
    REQUIRE(data_address);
    REQUIRE(!(data_address & (protection_granule_size - 1)));
    std::fill_n(memory->TranslateVirtual(data_address), protection_granule_size,
                uint8_t{0x31});

    thread = std::make_unique<ThreadState>(processor.get(), kThreadId,
                                           stack_address + 64 * 1024);
    REQUIRE(thread->PublishGuestExecutionCaptureReady() ==
            GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
    thread->context()->lr = kOuterReturnAddress;
    thread->context()->r[3] = 0x1111222233334444ull;
    participant = {thread->guest_execution_capture_instance_id(), kThreadId};
  }

  ~ProviderHarness() {
    if (provider) {
      provider->EndCapture(false);
      provider.reset();
    }
    second_thread.reset();
    thread.reset();
    if (data_address) {
      memory->SystemHeapFree(data_address);
    }
    if (stack_address) {
      memory->SystemHeapFree(stack_address);
    }
    if (second_stack_address) {
      memory->SystemHeapFree(second_stack_address);
    }
    processor.reset();
    if (code_allocation_address) {
      memory->LookupHeap(code_allocation_address)
          ->Release(code_allocation_address);
    }
    if (alias_allocation_address) {
      memory->LookupHeap(alias_allocation_address)
          ->Release(alias_allocation_address);
    }
    memory.reset();
  }

  void AddSecondParticipant() {
    REQUIRE_FALSE(second_thread);
    second_stack_address = memory->SystemHeapAlloc(64 * 1024, 4096);
    REQUIRE(second_stack_address);
    second_thread = std::make_unique<ThreadState>(
        processor.get(), kSecondThreadId, second_stack_address + 64 * 1024);
    REQUIRE(second_thread->PublishGuestExecutionCaptureReady() ==
            GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept);
    second_thread->context()->lr = kOuterReturnAddress;
    second_participant = {second_thread->guest_execution_capture_instance_id(),
                          kSecondThreadId};
  }

  void AllocateAliasData() {
    REQUIRE_FALSE(alias_allocation_address);
    alias_allocation_address = kAliasDataMirrorAddress;
    BaseHeap* heap = memory->LookupHeap(alias_allocation_address);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(alias_allocation_address, protection_granule_size,
                             protection_granule_size,
                             kMemoryAllocationReserve | kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::fill_n(memory->TranslateVirtual(kAliasDataAddress),
                protection_granule_size, uint8_t{0x52});
  }

  kernel::GuestSchedulerCheckpointBarrierSnapshot Checkpoint(
      uint32_t guest_pc = kFunctionAddress) const {
    kernel::GuestSchedulerCheckpointBarrierSnapshot checkpoint;
    checkpoint.generation = 1;
    checkpoint.active = true;
    checkpoint.quiesced = true;
    checkpoint.roster_scope =
        kernel::GuestSchedulerCheckpointRosterScope::kSchedulerOwned;
    checkpoint.release_policy = kernel::GuestSchedulerCheckpointReleasePolicy::
        kRunningSafepointsRequeueAtHead;
    kernel::GuestSchedulerCheckpointParticipant checkpoint_participant;
    checkpoint_participant.thread_id = kThreadId;
    checkpoint_participant.capture_instance_id =
        participant.capture_instance_id;
    checkpoint_participant.guest_pc = guest_pc;
    checkpoint_participant.cpu = 0;
    checkpoint_participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kRunning;
    checkpoint_participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint;
    checkpoint_participant.restorable = true;
    checkpoint.participants.push_back(checkpoint_participant);
    return checkpoint;
  }

  GuestExecutionCaptureHostCallRosterSnapshot HostCalls(
      uint32_t outer_return_address = kOuterReturnAddress) const {
    GuestExecutionCaptureHostCallRosterSnapshot roster;
    roster.active_calls.push_back({{1},
                                   participant,
                                   kFunctionAddress,
                                   kFunctionEndAddress,
                                   outer_return_address,
                                   1});
    return roster;
  }

  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> Participants()
      const {
    return {
        {participant, GuestExecutionCaptureThreadStateLifecycleState::kReady}};
  }

  kernel::GuestSchedulerCheckpointBarrierSnapshot TwoThreadCheckpoint() const {
    auto checkpoint = Checkpoint();
    kernel::GuestSchedulerCheckpointParticipant checkpoint_participant;
    checkpoint_participant.thread_id = kSecondThreadId;
    checkpoint_participant.capture_instance_id =
        second_participant.capture_instance_id;
    checkpoint_participant.guest_pc = kFunctionAddress;
    checkpoint_participant.cpu = 1;
    checkpoint_participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kRunning;
    checkpoint_participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint;
    checkpoint_participant.restorable = true;
    checkpoint.participants.push_back(checkpoint_participant);
    return checkpoint;
  }

  kernel::GuestSchedulerCheckpointBarrierSnapshot PassiveSecondThreadCheckpoint(
      kernel::GuestSchedulerCheckpointResumeKind resume_kind =
          kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation)
      const {
    auto checkpoint = Checkpoint();
    kernel::GuestSchedulerCheckpointParticipant checkpoint_participant;
    checkpoint_participant.thread_id = kSecondThreadId;
    checkpoint_participant.capture_instance_id =
        second_participant.capture_instance_id;
    checkpoint_participant.cpu = 1;
    checkpoint_participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kReady;
    checkpoint_participant.resume_kind = resume_kind;
    checkpoint.participants.push_back(checkpoint_participant);
    return checkpoint;
  }

  GuestExecutionCaptureHostCallRosterSnapshot TwoThreadHostCalls() const {
    auto roster = HostCalls();
    roster.active_calls.push_back({{2},
                                   second_participant,
                                   kFunctionAddress,
                                   kFunctionEndAddress,
                                   kOuterReturnAddress,
                                   1});
    return roster;
  }

  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent>
  TwoThreadParticipants() const {
    return {
        {participant, GuestExecutionCaptureThreadStateLifecycleState::kReady},
        {second_participant,
         GuestExecutionCaptureThreadStateLifecycleState::kReady}};
  }

  void InstallExternalEventLog() {
    REQUIRE_FALSE(external_event_log);
    external_event_log =
        std::make_shared<GuestExecutionCaptureExternalEventLog>();
    REQUIRE(processor->AttachGuestExecutionCaptureExternalEventLog(
        external_event_log));
  }

  GuestExecutionCaptureExternalEventToken OpenExportDispatch(
      const GuestExecutionCaptureParticipantIdentity& dispatch_participant,
      uint32_t call_site_address, uint32_t guest_address,
      GuestExecutionSessionEventKind kind =
          GuestExecutionSessionEventKind::kKernelExport,
      bool declare_effect = true) const {
    const GuestExecutionCaptureExternalEventEffectRange range = {data_address,
                                                                 4};
    const std::array<uint8_t, 4> preimage = {};
    GuestExecutionCaptureExternalEventBegin begin;
    begin.participant = dispatch_participant;
    begin.kind = kind;
    begin.export_ordinal = kExportOrdinal;
    begin.guest_address = guest_address;
    begin.call_site_address = call_site_address;
    if (declare_effect) {
      begin.effect_ranges = std::span(&range, 1);
      return external_event_log->OnExternalEventBegin(begin, preimage);
    }
    return external_event_log->OnExternalEventBegin(begin, {});
  }

  kernel::GuestSchedulerCheckpointBarrierSnapshot BlockedSecondThreadCheckpoint(
      kernel::GuestSchedulerCaptureWaitKind wait_kind =
          kernel::GuestSchedulerCaptureWaitKind::kSingle,
      uint8_t wait_flags = 0, uint32_t guest_pc = kFunctionAddress) const {
    auto checkpoint = Checkpoint();
    kernel::GuestSchedulerCheckpointParticipant checkpoint_participant;
    checkpoint_participant.thread_id = kSecondThreadId;
    checkpoint_participant.capture_instance_id =
        second_participant.capture_instance_id;
    checkpoint_participant.guest_pc = guest_pc;
    checkpoint_participant.cpu = 1;
    checkpoint_participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kBlocked;
    checkpoint_participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport;
    checkpoint_participant.blocked_wait_kind = wait_kind;
    checkpoint_participant.blocked_wait.handle_count = 1;
    checkpoint_participant.blocked_wait.flags = wait_flags;
    checkpoint_participant.blocked_wait.handles[0] = 0x00110001u;
    checkpoint.participants.push_back(checkpoint_participant);
    return checkpoint;
  }

  // The same wait the blocked checkpoint carries, published on a ready queue:
  // the scheduler cleared the exact-PC route when it re-readied the fiber, so
  // nothing but the wait shape separates this from a thread that just yielded.
  kernel::GuestSchedulerCheckpointBarrierSnapshot WokenSecondThreadCheckpoint(
      kernel::GuestSchedulerCaptureWaitKind wait_kind =
          kernel::GuestSchedulerCaptureWaitKind::kSingle,
      uint8_t wait_flags =
          kernel::kGuestSchedulerCaptureWaitFlagGated |
          kernel::kGuestSchedulerCaptureWaitFlagInterruptible) const {
    auto checkpoint = PassiveSecondThreadCheckpoint();
    auto& woken = checkpoint.participants.back();
    woken.blocked_wait_kind = wait_kind;
    woken.blocked_wait.handle_count = 1;
    woken.blocked_wait.flags = wait_flags;
    woken.blocked_wait.handles[0] = 0x00110001u;
    return checkpoint;
  }

  ppc::GuestInvocationRecorderIdentity InvocationIdentity() const {
    return {reinterpret_cast<uintptr_t>(thread->context()), kThreadId};
  }

  ppc::GuestInvocationRecorderIdentity SecondInvocationIdentity() const {
    return {reinterpret_cast<uintptr_t>(second_thread->context()),
            kSecondThreadId};
  }

  std::unique_ptr<Memory> memory;
  std::unique_ptr<Processor> processor;
  std::unique_ptr<GuestExecutionSessionCaptureProvider> provider;
  std::unique_ptr<ThreadState> thread;
  std::unique_ptr<ThreadState> second_thread;
  std::shared_ptr<GuestExecutionCaptureExternalEventLog> external_event_log;
  Function* function = nullptr;
  GuestExecutionCaptureParticipantIdentity participant;
  GuestExecutionCaptureParticipantIdentity second_participant;
  uint32_t protection_granule_size = 0;
  uint32_t code_allocation_address = 0;
  uint32_t alias_allocation_address = 0;
  uint32_t stack_address = 0;
  uint32_t second_stack_address = 0;
  uint32_t data_address = 0;
};

const GuestExecutionSessionAssemblerContent* FindContent(
    const std::vector<GuestExecutionSessionAssemblerContent>& content,
    GuestExecutionSessionContentKind kind, uint32_t address) {
  const auto found =
      std::find_if(content.cbegin(), content.cend(), [&](const auto& entry) {
        return entry.kind == kind && entry.guest_address == address;
      });
  return found == content.cend() ? nullptr : &*found;
}

class SealDetachedGate final {
 public:
  static void Hook(void* context) {
    auto& gate = *static_cast<SealDetachedGate*>(context);
    std::unique_lock<std::mutex> lock(gate.mutex_);
    gate.entered_ = true;
    gate.condition_.notify_all();
    gate.condition_.wait(lock, [&gate] { return gate.released_; });
  }

  bool WaitForEntry(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

bool WaitForLifecycleWaiter(GuestExecutionSessionCaptureProvider& provider,
                            std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (provider.lifecycle_waiter_count_for_test()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return provider.lifecycle_waiter_count_for_test() != 0;
}

void DestroyThreadState(void* context) {
  static_cast<std::unique_ptr<ThreadState>*>(context)->reset();
}

constexpr uint32_t kWaitHandle = 0x00110001u;
constexpr uint8_t kWaitPriority = 13;

kernel::GuestSchedulerCheckpointParticipant WokenInWaitParticipant() {
  kernel::GuestSchedulerCheckpointParticipant participant;
  participant.thread_id = kSecondThreadId;
  participant.capture_instance_id = 2;
  participant.cpu = 1;
  participant.effective_priority = kWaitPriority;
  participant.base_priority = kWaitPriority;
  participant.ready_queue_level = static_cast<int8_t>(kWaitPriority);
  participant.ready_queue_fifo_ordinal = 0;
  participant.state = kernel::GuestSchedulerCheckpointParticipantState::kReady;
  participant.resume_kind =
      kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation;
  participant.blocked_wait_kind =
      kernel::GuestSchedulerCaptureWaitKind::kSingle;
  participant.blocked_wait.handle_count = 1;
  participant.blocked_wait.flags =
      kernel::kGuestSchedulerCaptureWaitFlagGated |
      kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
  participant.blocked_wait.handles[0] = kWaitHandle;
  return participant;
}

GuestExecutionSessionSchedulerTopologyParticipant WokenInWaitRow() {
  GuestExecutionSessionSchedulerTopologyParticipant participant;
  participant.ordinal = 1;
  participant.guest_thread_id = kSecondThreadId;
  participant.capture_instance_id = 2;
  participant.state = GuestExecutionSessionSchedulerParticipantState::kReady;
  participant.cpu = 1;
  participant.effective_priority = kWaitPriority;
  participant.base_priority = kWaitPriority;
  participant.suspension_count = 0;
  participant.quantum_remaining_us = 0;
  participant.ready_queue_level = kWaitPriority;
  participant.ready_queue_fifo_ordinal = 0;
  participant.resume_kind =
      GuestExecutionSessionSchedulerResumeKind::kNativeContinuation;
  return participant;
}

constexpr uint32_t kWaitEpoch = 7;
constexpr uint64_t kWaitUptimeMs = 1000;

kernel::GuestSchedulerCheckpointParticipant BlockedParityParticipant() {
  kernel::GuestSchedulerCheckpointParticipant participant;
  participant.thread_id = kSecondThreadId;
  participant.capture_instance_id = 2;
  participant.guest_pc = kFunctionAddress;
  participant.cpu = 1;
  participant.state =
      kernel::GuestSchedulerCheckpointParticipantState::kBlocked;
  participant.resume_kind =
      kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport;
  participant.blocked_wait_kind =
      kernel::GuestSchedulerCaptureWaitKind::kSingle;
  participant.blocked_wait.handle_count = 1;
  participant.blocked_wait.flags =
      kernel::kGuestSchedulerCaptureWaitFlagGated |
      kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
  participant.blocked_wait.handles[0] = kWaitHandle;
  participant.blocked_wait.wait_epoch = kWaitEpoch;
  participant.blocked_wait.observed_wait_epoch = kWaitEpoch;
  participant.blocked_wait.signal_epochs_before[0] = kWaitEpoch;
  participant.blocked_wait.signal_epochs_observed[0] = kWaitEpoch;
  participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
  return participant;
}

GuestExecutionSessionSchedulerTopologyParticipant BlockedParityRow() {
  GuestExecutionSessionSchedulerTopologyParticipant participant;
  participant.ordinal = 1;
  participant.guest_thread_id = kSecondThreadId;
  participant.capture_instance_id = 2;
  participant.state = GuestExecutionSessionSchedulerParticipantState::kBlocked;
  participant.cpu = 1;
  participant.resume_kind =
      GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport;
  participant.guest_pc = kFunctionAddress;
  participant.blocked_wait.kind =
      GuestExecutionSessionSchedulerWaitKind::kSingle;
  participant.blocked_wait.handle_count = 1;
  participant.blocked_wait.flags =
      kGuestExecutionSessionSchedulerWaitFlagGated |
      kGuestExecutionSessionSchedulerWaitFlagInterruptible;
  participant.blocked_wait.handles[0] = kWaitHandle;
  participant.blocked_wait.wait_epoch = kWaitEpoch;
  participant.blocked_wait.observed_wait_epoch = kWaitEpoch;
  participant.blocked_wait.signal_epochs_before[0] = kWaitEpoch;
  participant.blocked_wait.signal_epochs_observed[0] = kWaitEpoch;
  participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
  return participant;
}

}  // namespace

TEST_CASE(
    "guest execution session provider captures production state and pages",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  std::string error;
  const auto participants = harness.Participants();
  const auto host_calls = harness.HostCalls();
  const auto final_host_calls = harness.HostCalls(kFinalOuterReturnAddress);
  REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(), participants,
                                         host_calls, &error));
  REQUIRE(error.empty());

  std::vector<uint8_t> initial_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.participant, true, &initial_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint initial_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(initial_state_bytes,
                                                     &initial_state, &error));
  REQUIRE(initial_state.participant_ordinal == 0);
  REQUIRE(initial_state.guest_thread_id == kThreadId);
  REQUIRE(initial_state.resume_pc == kFunctionAddress);
  REQUIRE(initial_state.owning_function_address == kFunctionAddress);
  REQUIRE(initial_state.owning_function_end_address == kFunctionEndAddress);
  REQUIRE(initial_state.outer_guest_return_address == kOuterReturnAddress);
  REQUIRE(initial_state.registers.gpr[3] == 0x1111222233334444ull);

  REQUIRE(harness.provider->OnFunctionEntry(
      harness.InvocationIdentity(), kFunctionAddress, kFunctionEndAddress,
      ppc::CaptureGuestPPCRegisterState(*harness.thread->context())));
  REQUIRE(harness.provider->OnMemoryAccess(
      harness.InvocationIdentity(), harness.data_address + 16, 8,
      ppc::GuestInvocationRecorderMemoryAccess::kRead));
  harness.thread->context()->r[3] = 0x5555666677778888ull;

  REQUIRE(harness.provider->SealCapture(harness.Checkpoint(), final_host_calls,
                                        &error));
  REQUIRE(error.empty());
  REQUIRE(harness.processor->guest_invocation_capture_sink() == nullptr);

  std::vector<uint8_t> final_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(harness.participant, false,
                                                   &final_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint final_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(final_state_bytes,
                                                     &final_state, &error));
  REQUIRE(final_state.registers.gpr[3] == 0x5555666677778888ull);
  REQUIRE(final_state.outer_guest_return_address == kFinalOuterReturnAddress);

  std::vector<GuestExecutionSessionAssemblerContent> initial_content;
  std::vector<GuestExecutionSessionAssemblerContent> final_content;
  REQUIRE(harness.provider->CollectCheckpointContent(true, &initial_content,
                                                     &error));
  REQUIRE(harness.provider->CollectCheckpointContent(false, &final_content,
                                                     &error));
  const auto* initial_data =
      FindContent(initial_content, GuestExecutionSessionContentKind::kGuestPage,
                  harness.data_address);
  REQUIRE(initial_data);
  REQUIRE(initial_data->bytes[16] == 0x31);
  const auto* final_data =
      FindContent(final_content, GuestExecutionSessionContentKind::kGuestPage,
                  harness.data_address);
  REQUIRE_FALSE(final_data);
  REQUIRE(FindContent(initial_content,
                      GuestExecutionSessionContentKind::kGuestCode,
                      kFunctionAddress & ~4095u));

  std::vector<uint8_t> corpus_bytes;
  REQUIRE(harness.provider->CollectSessionCodeCorpus(&corpus_bytes, &error));
  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(corpus_bytes, &corpus, &error));
  REQUIRE(corpus.config_flags() == JitCorpus::kConfigGuestScheduler);
  REQUIRE(corpus.functions().size() == 1);
  REQUIRE(corpus.functions().front().address == kFunctionAddress);
  REQUIRE(corpus.functions().front().end_address == kFunctionEndAddress);
  REQUIRE(corpus.FindPageData(kFunctionAddress & ~4095u));

  const auto status = harness.provider->status();
  REQUIRE(status.state == GuestExecutionSessionCaptureProviderState::kSealed);
  REQUIRE(status.memory_access_count == 1);
  REQUIRE(status.corpus_function_count == 1);
  const uint32_t protection_page_count =
      static_cast<uint32_t>(xe::memory::page_size() / JitCorpus::kPageSize);
  REQUIRE(status.code_page_count == protection_page_count);
  REQUIRE(status.data_page_count == protection_page_count);
  harness.provider->EndCapture(true);
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kAccepted);
}

TEST_CASE("guest execution session provider checkpoints a dormant participant",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.second_thread->context()->r[3] = 0x8899AABBCCDDEEFFull;
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                         harness.TwoThreadParticipants(),
                                         harness.HostCalls(), &error));
  REQUIRE(error.empty());

  std::vector<uint8_t> initial_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, true, &initial_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint initial_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(initial_state_bytes,
                                                     &initial_state, &error));
  REQUIRE(initial_state.participant_ordinal == 1);
  REQUIRE(initial_state.guest_thread_id == kSecondThreadId);
  REQUIRE(initial_state.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  REQUIRE(initial_state.resume_pc == 0);
  REQUIRE(initial_state.owning_function_address == 0);
  REQUIRE(initial_state.owning_function_end_address == 0);
  REQUIRE(initial_state.outer_guest_return_address == 0);
  REQUIRE(initial_state.registers.gpr[3] == 0x8899AABBCCDDEEFFull);

  REQUIRE(harness.provider->SealCapture(harness.Checkpoint(),
                                        harness.HostCalls(), &error));
  REQUIRE(error.empty());
  std::vector<uint8_t> final_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, false, &final_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint final_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(final_state_bytes,
                                                     &final_state, &error));
  REQUIRE(final_state == initial_state);
}

TEST_CASE(
    "guest execution session provider checkpoints a passive scheduler "
    "participant",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.second_thread->context()->r[3] = 0x8899AABBCCDDEEFFull;
  std::string error;
  const auto checkpoint = harness.PassiveSecondThreadCheckpoint();
  REQUIRE(harness.provider->SupportsCheckpointParticipant(
      checkpoint.participants.back(), &error));
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.HostCalls(), &error));

  std::vector<uint8_t> initial_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, true, &initial_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint initial_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(initial_state_bytes,
                                                     &initial_state, &error));
  REQUIRE(initial_state.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  REQUIRE(initial_state.resume_pc == 0);
  REQUIRE(initial_state.owning_function_address == 0);
  REQUIRE(initial_state.owning_function_end_address == 0);
  REQUIRE(initial_state.outer_guest_return_address == 0);
  REQUIRE(initial_state.registers.gpr[3] == 0x8899AABBCCDDEEFFull);

  REQUIRE(
      harness.provider->SealCapture(checkpoint, harness.HostCalls(), &error));
  std::vector<uint8_t> final_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, false, &final_state_bytes, &error));
  REQUIRE(final_state_bytes == initial_state_bytes);
}

TEST_CASE("guest execution session provider rejects passive boundary changes",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  const auto checkpoint = harness.PassiveSecondThreadCheckpoint();
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.HostCalls(), &error));

  SECTION("raw checkpoint state") {
    harness.second_thread->context()->r[3] = 1;
    REQUIRE_FALSE(
        harness.provider->SealCapture(checkpoint, harness.HostCalls(), &error));
    REQUIRE(error.find("changed at the boundary") != std::string::npos);
  }
  SECTION("outside to executable") {
    REQUIRE_FALSE(harness.provider->SealCapture(
        harness.TwoThreadCheckpoint(), harness.TwoThreadHostCalls(), &error));
    REQUIRE(error.find("boundary class changed") != std::string::npos);
  }
}

TEST_CASE("guest execution session provider rejects an active passive call",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  REQUIRE_FALSE(harness.provider->BeginCapture(
      harness.PassiveSecondThreadCheckpoint(), harness.TwoThreadParticipants(),
      harness.TwoThreadHostCalls(), &error));
  REQUIRE(
      error.find("passive scheduler participant with an active host call") !=
      std::string::npos);
}

TEST_CASE("guest execution session provider rejects an untyped dormant entry",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                         harness.TwoThreadParticipants(),
                                         harness.HostCalls(), &error));
  SECTION("function entry") {
    REQUIRE_FALSE(harness.provider->OnFunctionEntry(
        harness.SecondInvocationIdentity(), kFunctionAddress,
        kFunctionEndAddress,
        ppc::CaptureGuestPPCRegisterState(*harness.second_thread->context())));
  }
  SECTION("memory callback") {
    REQUIRE_FALSE(harness.provider->OnMemoryAccess(
        harness.SecondInvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
  }
  SECTION("instruction counter") {
    std::atomic_ref<uint64_t>(harness.second_thread->context()
                                  ->guest_execution_session_instruction_count)
        .fetch_add(1, std::memory_order_relaxed);
    std::vector<GuestExecutionSessionInstructionCoverageDelta> deltas;
    REQUIRE_FALSE(
        harness.provider->CollectInstructionCoverageDeltas(&deltas, &error));
    REQUIRE(deltas.empty());
  }
  const auto status = harness.provider->status();
  REQUIRE(status.state == GuestExecutionSessionCaptureProviderState::kRejected);
  REQUIRE(status.message.find("outside-guest participant") !=
          std::string::npos);
}

TEST_CASE("guest execution session provider rejects an active dormant call",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  REQUIRE_FALSE(harness.provider->BeginCapture(
      harness.Checkpoint(), harness.TwoThreadParticipants(),
      harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.find("active host call as outside guest") != std::string::npos);
}

// Stands in for the session's export event bridge: the durable sequence a
// dispatch's canonical event was assigned.
class FakeExportSequenceResolver final
    : public GuestExecutionSessionCaptureExportSequenceResolver {
 public:
  bool ResolveModeledExportSequence(
      GuestExecutionCaptureExternalEventToken token,
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint64_t* global_sequence) const noexcept override {
    *global_sequence = 0;
    if (token != token_ || participant != participant_) {
      return false;
    }
    *global_sequence = sequence_;
    return true;
  }

  void Bind(GuestExecutionCaptureExternalEventToken token,
            const GuestExecutionCaptureParticipantIdentity& participant,
            uint64_t sequence) {
    token_ = token;
    participant_ = participant;
    sequence_ = sequence;
  }

 private:
  GuestExecutionCaptureExternalEventToken token_;
  GuestExecutionCaptureParticipantIdentity participant_;
  uint64_t sequence_ = 0;
};

TEST_CASE(
    "guest execution session provider checkpoints a blocked export "
    "participant",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.second_thread->context()->r[3] = 0x0102030405060708ull;
  harness.InstallExternalEventLog();
  const auto dispatch = harness.OpenExportDispatch(
      harness.second_participant, kFunctionAddress, kExportThunkAddress);
  REQUIRE(dispatch);

  std::string error;
  const auto checkpoint = harness.BlockedSecondThreadCheckpoint();
  REQUIRE(harness.provider->SupportsCheckpointParticipant(
      checkpoint.participants.back(), &error));
  REQUIRE(error.empty());
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());

  std::vector<uint8_t> state_bytes;
  SECTION("an unresolved dispatch never encodes a route") {
    REQUIRE_FALSE(harness.provider->EncodeParticipantState(
        harness.second_participant, true, &state_bytes, &error));
    REQUIRE(error.find("never reached the session tape") != std::string::npos);
    REQUIRE(state_bytes.empty());
  }
  SECTION("a resolver bound to another participant never encodes a route") {
    FakeExportSequenceResolver resolver;
    resolver.Bind(dispatch, harness.participant, 41);
    harness.provider->SetModeledExportSequenceResolver(&resolver);
    REQUIRE_FALSE(harness.provider->EncodeParticipantState(
        harness.second_participant, true, &state_bytes, &error));
    REQUIRE(error.find("never reached the session tape") != std::string::npos);
    harness.provider->SetModeledExportSequenceResolver(nullptr);
  }
  SECTION("the resolved sequence is the only value that is encoded") {
    FakeExportSequenceResolver resolver;
    resolver.Bind(dispatch, harness.second_participant, 41);
    harness.provider->SetModeledExportSequenceResolver(&resolver);
    REQUIRE(harness.provider->EncodeParticipantState(
        harness.second_participant, true, &state_bytes, &error));
    ppc::GuestPPCThreadCheckpoint state;
    REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(state_bytes, &state,
                                                       &error));
    REQUIRE(state.participant_ordinal == 1);
    REQUIRE(state.guest_thread_id == kSecondThreadId);
    REQUIRE(state.resume_kind ==
            ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
    REQUIRE(state.resume_pc == kFunctionAddress);
    REQUIRE(state.owning_function_address == kFunctionAddress);
    REQUIRE(state.owning_function_end_address == kFunctionEndAddress);
    REQUIRE(state.outer_guest_return_address == kOuterReturnAddress);
    REQUIRE(state.pending_export_guest_address == kExportThunkAddress);
    REQUIRE(state.pending_external_event_sequence == 41);
    REQUIRE(state.pending_external_event_sequence != dispatch.value);
    REQUIRE(state.registers.gpr[3] == 0x0102030405060708ull);
    harness.provider->SetModeledExportSequenceResolver(nullptr);
  }
}

TEST_CASE(
    "guest execution session provider refuses a final blocked export "
    "participant",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                     kFunctionAddress, kExportThunkAddress));

  std::string error;
  const auto blocked = harness.BlockedSecondThreadCheckpoint();
  // The same participant is admissible at the start boundary, where its export
  // still returns inside the interval.
  REQUIRE(harness.provider->BeginCapture(blocked,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  // At the final boundary the export completes after the interval, so nothing
  // on the tape can witness the route this participant would need.
  REQUIRE_FALSE(harness.provider->SealCapture(
      blocked, harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.find("blocked modeled export at the final boundary") !=
          std::string::npos);
}

TEST_CASE(
    "guest execution session provider admits a blocked waiter no dispatch owns",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  std::string error;

  SECTION("no open dispatch at all") {
    // A waiter no modeled dispatch owns cannot be resumed from the tape, so it
    // is carried unchanged instead. The root-call and parity obligations are
    // the encode arm's, not the roster gate's.
    REQUIRE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.empty());
  }
  SECTION("only another participant's dispatch is open") {
    REQUIRE(harness.OpenExportDispatch(harness.participant, kFunctionAddress,
                                       kExportThunkAddress));
    REQUIRE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.empty());
  }
}

TEST_CASE("guest execution session provider binds one blocking export dispatch",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;

  SECTION("no installed log") {
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.find("no installed modeled export event log") !=
            std::string::npos);
    REQUIRE(error.find(" export=nolog wait=1/1/0/00110001/0") !=
            std::string::npos);
  }
  SECTION("refusal after an open dispatch still names it") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kMiddleFunctionAddress,
                                       kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.find("differs from its modeled export return point") !=
            std::string::npos);
    REQUIRE(error.find(" export=475/8270D724/82040040 "
                       "wait=1/1/0/00110001/0") != std::string::npos);
  }
  SECTION("two open dispatches") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.find("more than one open modeled export dispatch") !=
            std::string::npos);
  }
  SECTION("wait kind outside the allowlist") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness
            .BlockedSecondThreadCheckpoint(
                kernel::GuestSchedulerCaptureWaitKind::kDelay)
            .participants.back(),
        &error));
    REQUIRE(error.find("outside the modeled blocking-export wait allowlist") !=
            std::string::npos);
  }
  SECTION("alertable wait") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness
            .BlockedSecondThreadCheckpoint(
                kernel::GuestSchedulerCaptureWaitKind::kSingle,
                kernel::kGuestSchedulerCaptureWaitFlagAlertable)
            .participants.back(),
        &error));
    REQUIRE(error.find("outside the modeled blocking-export wait allowlist") !=
            std::string::npos);
  }
  SECTION("pending user APC") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness
            .BlockedSecondThreadCheckpoint(
                kernel::GuestSchedulerCaptureWaitKind::kSingle,
                kernel::kGuestSchedulerCaptureWaitFlagUserApcPending)
            .participants.back(),
        &error));
    REQUIRE(error.find("outside the modeled blocking-export wait allowlist") !=
            std::string::npos);
  }
  SECTION("dispatch return point differs from the checkpoint PC") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kMiddleFunctionAddress,
                                       kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.find("differs from its modeled export return point") !=
            std::string::npos);
  }
  SECTION("dispatch declares no guest-memory effect") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(
        harness.second_participant, kFunctionAddress, kExportThunkAddress,
        GuestExecutionSessionEventKind::kKernelExport, false));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.find("no bindable export identity") != std::string::npos);
  }
  SECTION("dispatch is not a kernel export") {
    harness.InstallExternalEventLog();
    REQUIRE(harness.OpenExportDispatch(
        harness.second_participant, kFunctionAddress, kExportThunkAddress,
        GuestExecutionSessionEventKind::kExternOrBuiltin));
    REQUIRE_FALSE(harness.provider->SupportsCheckpointParticipant(
        harness.BlockedSecondThreadCheckpoint().participants.back(), &error));
    REQUIRE(error.find("outermost modeled kernel export") != std::string::npos);
  }
  REQUIRE(error.find("tid=00000881") != std::string::npos);
  REQUIRE(error.find("resume_kind=2") != std::string::npos);
}

TEST_CASE("guest execution session provider proves a blocked-parity park",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  std::string error;
  // The park claims the fiber sat below one call for the whole interval, so a
  // second call beneath it is a claim the participant cannot make.
  GuestExecutionCaptureHostCallRosterSnapshot roster =
      harness.TwoThreadHostCalls();
  roster.active_calls.push_back({{3},
                                 harness.second_participant,
                                 kMiddleFunctionAddress,
                                 kFunctionEndAddress,
                                 kOuterReturnAddress,
                                 2});
  REQUIRE_FALSE(harness.provider->BeginCapture(
      harness.BlockedSecondThreadCheckpoint(), harness.TwoThreadParticipants(),
      roster, &error));
  INFO(error);
  REQUIRE(error.find("blocked-parity participant does not own exactly one "
                     "root host call") != std::string::npos);
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kRejected);
}

TEST_CASE("guest execution session provider checkpoints a blocked-parity park",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.second_thread->context()->r[3] = 0x0123456789ABCDEFull;
  harness.InstallExternalEventLog();
  std::string error;
  // The live shape: a waiter below the root dispatch it has held since it
  // started running, with nothing modeled beneath it to bind a route to.
  const auto checkpoint = harness.BlockedSecondThreadCheckpoint();
  REQUIRE(harness.provider->SupportsCheckpointParticipant(
      checkpoint.participants.back(), &error));
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  // The class publishes no route, so nothing about it waits on the tape.
  REQUIRE_FALSE(harness.provider->DefersInitialParticipantState(
      harness.second_participant));

  std::vector<uint8_t> initial_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, true, &initial_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint initial_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(initial_state_bytes,
                                                     &initial_state, &error));
  REQUIRE(initial_state.participant_ordinal == 1);
  REQUIRE(initial_state.guest_thread_id == kSecondThreadId);
  REQUIRE(initial_state.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  REQUIRE(initial_state.resume_pc == 0);
  REQUIRE(initial_state.owning_function_address == 0);
  REQUIRE(initial_state.outer_guest_return_address == 0);
  REQUIRE(initial_state.pending_external_event_sequence == 0);
  REQUIRE(initial_state.registers.gpr[3] == 0x0123456789ABCDEFull);

  // The parity claim binds to the absence of an outcome, so unlike a modeled
  // export it is still admissible where the interval ends.
  REQUIRE(harness.provider->SealCapture(checkpoint,
                                        harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  std::vector<uint8_t> final_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, false, &final_state_bytes, &error));
  REQUIRE(final_state_bytes == initial_state_bytes);
}

TEST_CASE("guest execution session provider rejects blocked-parity drift",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  const auto checkpoint = harness.BlockedSecondThreadCheckpoint();
  std::string error;
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());

  SECTION("a different outer host call is a returned and re-entered dispatch") {
    auto host_calls = harness.HostCalls();
    host_calls.active_calls.push_back({{5},
                                       harness.second_participant,
                                       kFunctionAddress,
                                       kFunctionEndAddress,
                                       kOuterReturnAddress,
                                       1});
    REQUIRE_FALSE(
        harness.provider->SealCapture(checkpoint, host_calls, &error));
    REQUIRE(error.find("parity participant changed its outer host call") !=
            std::string::npos);
  }
  SECTION("a register that moved is not parity") {
    harness.second_thread->context()->r[3] = 9;
    REQUIRE_FALSE(harness.provider->SealCapture(
        checkpoint, harness.TwoThreadHostCalls(), &error));
    REQUIRE(error.find("changed at the boundary") != std::string::npos);
  }
  SECTION("an export dispatch opened inside the interval is not parity") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    // The dispatch takes the participant off the parity arm and onto the
    // blocked-export route, which the final boundary cannot carry.
    REQUIRE_FALSE(harness.provider->SealCapture(
        checkpoint, harness.TwoThreadHostCalls(), &error));
    REQUIRE(error.find("blocked modeled export at the final boundary") !=
            std::string::npos);
  }
}

TEST_CASE(
    "guest execution session provider checkpoints a woken export participant",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  // The fiber is still parked on the branch that entered the export thunk, so
  // the link register is the only place its return point survives.
  harness.second_thread->context()->lr = kFunctionAddress;
  harness.InstallExternalEventLog();
  const auto dispatch = harness.OpenExportDispatch(
      harness.second_participant, kFunctionAddress, kExportThunkAddress);
  REQUIRE(dispatch);

  std::string error;
  const auto checkpoint = harness.WokenSecondThreadCheckpoint();
  // The capability gate still reads the class as passive. What the class is
  // bound to is decided where the dispatch and the registers are both in hand.
  REQUIRE(harness.provider->SupportsCheckpointParticipant(
      checkpoint.participants.back(), &error));
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  REQUIRE(harness.provider->DefersInitialParticipantState(
      harness.second_participant));

  FakeExportSequenceResolver resolver;
  resolver.Bind(dispatch, harness.second_participant, 41);
  harness.provider->SetModeledExportSequenceResolver(&resolver);
  std::vector<uint8_t> state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(harness.second_participant,
                                                   true, &state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint state;
  REQUIRE(
      ppc::GuestPPCThreadCheckpointCodec::Decode(state_bytes, &state, &error));
  REQUIRE(state.participant_ordinal == 1);
  REQUIRE(state.guest_thread_id == kSecondThreadId);
  REQUIRE(state.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kPendingModeledBlockingExtern);
  REQUIRE(state.resume_pc == kFunctionAddress);
  REQUIRE(state.owning_function_address == kFunctionAddress);
  REQUIRE(state.owning_function_end_address == kFunctionEndAddress);
  REQUIRE(state.outer_guest_return_address == kOuterReturnAddress);
  REQUIRE(state.pending_export_guest_address == kExportThunkAddress);
  REQUIRE(state.pending_external_event_sequence == 41);
  REQUIRE(state.pending_external_event_sequence != dispatch.value);
  harness.provider->SetModeledExportSequenceResolver(nullptr);
}

TEST_CASE(
    "guest execution session provider refuses a final woken export participant",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.second_thread->context()->lr = kFunctionAddress;
  harness.InstallExternalEventLog();
  REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                     kFunctionAddress, kExportThunkAddress));

  std::string error;
  const auto woken = harness.WokenSecondThreadCheckpoint();
  REQUIRE(harness.provider->BeginCapture(woken, harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  // At the final boundary the export returns after the interval, so nothing on
  // the tape can witness the route this participant would need.
  REQUIRE_FALSE(harness.provider->SealCapture(
      woken, harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.find("woken modeled export at the final boundary") !=
          std::string::npos);
}

TEST_CASE("guest execution session provider binds one woken export dispatch",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  harness.second_thread->context()->lr = kFunctionAddress;
  auto checkpoint = harness.WokenSecondThreadCheckpoint();
  bool nested_host_call = false;
  std::string expected;

  // A wait shape the dispatch cannot bind leaves the participant in neither
  // class: the export is real and open, so the parity route is unavailable to
  // it as well.
  SECTION("a wait kind the export dispatch does not model") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    checkpoint = harness.WokenSecondThreadCheckpoint(
        kernel::GuestSchedulerCaptureWaitKind::kDelay);
    expected =
        "ready-parity participant is parked in an open modeled export "
        "dispatch";
  }
  SECTION("an alertable wait") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    checkpoint = harness.WokenSecondThreadCheckpoint(
        kernel::GuestSchedulerCaptureWaitKind::kSingle,
        kernel::kGuestSchedulerCaptureWaitFlagAlertable);
    expected =
        "ready-parity participant is parked in an open modeled export "
        "dispatch";
  }
  SECTION("a pending user APC") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    checkpoint = harness.WokenSecondThreadCheckpoint(
        kernel::GuestSchedulerCaptureWaitKind::kSingle,
        kernel::kGuestSchedulerCaptureWaitFlagUserApcPending);
    expected =
        "ready-parity participant is parked in an open modeled export "
        "dispatch";
  }
  SECTION("an unpublished link register") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    harness.second_thread->context()->lr = 0;
    expected =
        "woken waiter link register is not a PPC-aligned export return point";
  }
  SECTION("a misaligned link register") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    harness.second_thread->context()->lr = kFunctionAddress + 1;
    expected =
        "woken waiter link register is not a PPC-aligned export return point";
  }
  SECTION("a link register away from the dispatch call site") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kMiddleFunctionAddress,
                                       kExportThunkAddress));
    expected =
        "woken waiter link register differs from its modeled export return "
        "point";
  }
  SECTION("two open dispatches") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    expected = "woken waiter has more than one open modeled export dispatch";
  }
  SECTION("a dispatch that is not a kernel export") {
    REQUIRE(harness.OpenExportDispatch(
        harness.second_participant, kFunctionAddress, kExportThunkAddress,
        GuestExecutionSessionEventKind::kExternOrBuiltin));
    expected =
        "woken waiter is not parked in an outermost modeled kernel export";
  }
  SECTION("a dispatch declaring no guest-memory effect") {
    REQUIRE(harness.OpenExportDispatch(
        harness.second_participant, kFunctionAddress, kExportThunkAddress,
        GuestExecutionSessionEventKind::kKernelExport, false));
    expected = "no bindable export identity";
  }
  SECTION("a waiter below more than its root dispatch") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    nested_host_call = true;
    expected = "woken waiter does not own exactly one root host call";
  }

  auto host_calls = harness.TwoThreadHostCalls();
  if (nested_host_call) {
    host_calls.active_calls.push_back({{3},
                                       harness.second_participant,
                                       kMiddleFunctionAddress,
                                       kMiddleFunctionEndAddress,
                                       kOuterReturnAddress,
                                       2});
  }
  std::string error;
  REQUIRE_FALSE(harness.provider->BeginCapture(
      checkpoint, harness.TwoThreadParticipants(), host_calls, &error));
  INFO(error);
  REQUIRE(error.find(expected) != std::string::npos);
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kRejected);
}

TEST_CASE("guest execution session provider checkpoints a ready-parity park",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.second_thread->context()->r[3] = 0x5566778899AABBCCull;
  harness.InstallExternalEventLog();
  std::string error;
  // The live shape: a re-readied fiber whose only open call is the root
  // dispatch it has held since it started running, with nothing modeled
  // beneath it and no export identity to bind.
  const auto checkpoint = harness.WokenSecondThreadCheckpoint(
      kernel::GuestSchedulerCaptureWaitKind::kSingle,
      kernel::kGuestSchedulerCaptureWaitFlagInterruptible);
  REQUIRE(harness.provider->SupportsCheckpointParticipant(
      checkpoint.participants.back(), &error));
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  REQUIRE_FALSE(harness.provider->DefersInitialParticipantState(
      harness.second_participant));

  std::vector<uint8_t> initial_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, true, &initial_state_bytes, &error));
  ppc::GuestPPCThreadCheckpoint initial_state;
  REQUIRE(ppc::GuestPPCThreadCheckpointCodec::Decode(initial_state_bytes,
                                                     &initial_state, &error));
  REQUIRE(initial_state.participant_ordinal == 1);
  REQUIRE(initial_state.guest_thread_id == kSecondThreadId);
  REQUIRE(initial_state.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest);
  REQUIRE(initial_state.resume_pc == 0);
  REQUIRE(initial_state.owning_function_address == 0);
  REQUIRE(initial_state.outer_guest_return_address == 0);
  REQUIRE(initial_state.registers.gpr[3] == 0x5566778899AABBCCull);

  // The parity route binds to the absence of an outcome, so unlike a modeled
  // export it is still admissible where the interval ends.
  REQUIRE(harness.provider->SealCapture(checkpoint,
                                        harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());
  std::vector<uint8_t> final_state_bytes;
  REQUIRE(harness.provider->EncodeParticipantState(
      harness.second_participant, false, &final_state_bytes, &error));
  REQUIRE(final_state_bytes == initial_state_bytes);
}

TEST_CASE("guest execution session provider proves every ready-parity claim",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  auto checkpoint = harness.WokenSecondThreadCheckpoint(
      kernel::GuestSchedulerCaptureWaitKind::kSingle,
      kernel::kGuestSchedulerCaptureWaitFlagInterruptible);
  auto& row = checkpoint.participants.back();
  bool install_log = true;
  bool nested_host_call = false;
  std::string expected =
      "cannot encode a passive scheduler participant with an active host call";

  SECTION("a suspended fiber is not on a ready queue") {
    row.state = kernel::GuestSchedulerCheckpointParticipantState::kSuspended;
  }
  SECTION("a fiber that has never run holds no dispatch") {
    row.resume_kind = kernel::GuestSchedulerCheckpointResumeKind::kNotYetRun;
  }
  SECTION("a ready row carrying a suspension count") {
    row.suspension_count = 1;
  }
  SECTION("a fiber inside a preemption episode") {
    row.capture_declined_safepoints = 1;
  }
  SECTION("a fiber the export event log cannot answer for") {
    install_log = false;
  }
  SECTION("a fiber below more than its root dispatch") {
    nested_host_call = true;
    expected =
        "ready-parity participant does not own exactly one root host call";
  }

  if (install_log) {
    harness.InstallExternalEventLog();
  }
  auto host_calls = harness.TwoThreadHostCalls();
  if (nested_host_call) {
    host_calls.active_calls.push_back({{3},
                                       harness.second_participant,
                                       kMiddleFunctionAddress,
                                       kMiddleFunctionEndAddress,
                                       kOuterReturnAddress,
                                       2});
  }
  std::string error;
  REQUIRE_FALSE(harness.provider->BeginCapture(
      checkpoint, harness.TwoThreadParticipants(), host_calls, &error));
  INFO(error);
  REQUIRE(error.find(expected) != std::string::npos);
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kRejected);
}

TEST_CASE("guest execution session provider rejects ready-parity drift",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  // A wait no modeled export dispatch binds keeps every section on the parity
  // arm, so what the final boundary refuses is the parity claim itself.
  const auto checkpoint = harness.WokenSecondThreadCheckpoint(
      kernel::GuestSchedulerCaptureWaitKind::kDelay);
  std::string error;
  REQUIRE(harness.provider->BeginCapture(checkpoint,
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  REQUIRE(error.empty());

  SECTION("a different outer host call is a returned and re-entered dispatch") {
    auto host_calls = harness.HostCalls();
    host_calls.active_calls.push_back({{5},
                                       harness.second_participant,
                                       kFunctionAddress,
                                       kFunctionEndAddress,
                                       kOuterReturnAddress,
                                       1});
    REQUIRE_FALSE(
        harness.provider->SealCapture(checkpoint, host_calls, &error));
    REQUIRE(error.find("parity participant changed its outer host call") !=
            std::string::npos);
  }
  SECTION("a register that moved is not parity") {
    harness.second_thread->context()->r[3] = 9;
    REQUIRE_FALSE(harness.provider->SealCapture(
        checkpoint, harness.TwoThreadHostCalls(), &error));
    REQUIRE(error.find("changed at the boundary") != std::string::npos);
  }
  SECTION("an export dispatch opened inside the interval is not parity") {
    REQUIRE(harness.OpenExportDispatch(harness.second_participant,
                                       kFunctionAddress, kExportThunkAddress));
    REQUIRE_FALSE(harness.provider->SealCapture(
        checkpoint, harness.TwoThreadHostCalls(), &error));
    REQUIRE(error.find("ready-parity participant is parked in an open modeled "
                       "export dispatch") != std::string::npos);
  }
}

TEST_CASE("guest execution session provider owns instruction counters",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                         harness.Participants(),
                                         harness.HostCalls(), &error));
  ppc::PPCContext* context = harness.thread->context();
  REQUIRE(context->guest_execution_session_instruction_counter ==
          &context->guest_execution_session_instruction_count);

  std::atomic_ref<uint64_t>(context->guest_execution_session_instruction_count)
      .fetch_add(37, std::memory_order_relaxed);
  std::vector<GuestExecutionSessionInstructionCoverageDelta> deltas;
  REQUIRE(harness.provider->CollectInstructionCoverageDeltas(&deltas, &error));
  REQUIRE(deltas.size() == 1);
  REQUIRE(deltas.front().participant == harness.participant);
  REQUIRE(deltas.front().guest_instruction_delta == 37);
  REQUIRE(harness.provider->CollectInstructionCoverageDeltas(&deltas, &error));
  REQUIRE(deltas.empty());

  REQUIRE(harness.provider->SealCapture(harness.Checkpoint(),
                                        harness.HostCalls(), &error));
  REQUIRE(context->guest_execution_session_instruction_counter == nullptr);
  harness.provider->EndCapture(true);
}

TEST_CASE(
    "guest execution session provider keeps participant counters "
    "isolated",
    "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.TwoThreadCheckpoint(),
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  ppc::PPCContext* first_context = harness.thread->context();
  ppc::PPCContext* second_context = harness.second_thread->context();
  std::atomic_ref<uint64_t>(
      first_context->guest_execution_session_instruction_count)
      .fetch_add(17, std::memory_order_relaxed);
  std::atomic_ref<uint64_t>(
      second_context->guest_execution_session_instruction_count)
      .fetch_add(29, std::memory_order_relaxed);

  std::vector<GuestExecutionSessionInstructionCoverageDelta> deltas;
  REQUIRE(harness.provider->CollectInstructionCoverageDeltas(&deltas, &error));
  REQUIRE(deltas.size() == 2);
  REQUIRE(deltas[0].participant == harness.participant);
  REQUIRE(deltas[0].guest_instruction_delta == 17);
  REQUIRE(deltas[1].participant == harness.second_participant);
  REQUIRE(deltas[1].guest_instruction_delta == 29);
  REQUIRE(harness.provider->CollectInstructionCoverageDeltas(&deltas, &error));
  REQUIRE(deltas.empty());

  REQUIRE(harness.provider->SealCapture(harness.TwoThreadCheckpoint(),
                                        harness.TwoThreadHostCalls(), &error));
  REQUIRE(first_context->guest_execution_session_instruction_counter ==
          nullptr);
  REQUIRE(second_context->guest_execution_session_instruction_counter ==
          nullptr);
  harness.provider->EndCapture(true);
}

TEST_CASE("guest execution session provider disarms around participant death",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.TwoThreadCheckpoint(),
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  ppc::PPCContext* first_context = harness.thread->context();
  ppc::PPCContext* second_context = harness.second_thread->context();
  REQUIRE(first_context->guest_execution_session_instruction_counter ==
          &first_context->guest_execution_session_instruction_count);
  REQUIRE(second_context->guest_execution_session_instruction_counter ==
          &second_context->guest_execution_session_instruction_count);
  std::atomic_ref<uint64_t>(
      first_context->guest_execution_session_instruction_count)
      .store(17, std::memory_order_relaxed);
  std::atomic_ref<uint64_t>(
      second_context->guest_execution_session_instruction_count)
      .store(29, std::memory_order_relaxed);

  harness.thread.reset();
  harness.provider->EndCapture(false);

  REQUIRE(second_context->guest_execution_session_instruction_counter ==
          nullptr);
  REQUIRE(second_context->guest_execution_session_instruction_count == 0);
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kStopped);
}

TEST_CASE("guest execution session provider cleans up a failed exact disarm",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.TwoThreadCheckpoint(),
                                         harness.TwoThreadParticipants(),
                                         harness.TwoThreadHostCalls(), &error));
  ppc::PPCContext* second_context = harness.second_thread->context();
  std::atomic_ref<uint64_t>(
      second_context->guest_execution_session_instruction_count)
      .store(31, std::memory_order_relaxed);
  harness.provider->SetSealDetachedTestHook(&DestroyThreadState,
                                            &harness.thread);

  REQUIRE_FALSE(harness.provider->SealCapture(
      harness.TwoThreadCheckpoint(), harness.TwoThreadHostCalls(), &error));

  REQUIRE(second_context->guest_execution_session_instruction_counter ==
          nullptr);
  REQUIRE(second_context->guest_execution_session_instruction_count == 0);
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kRejected);
  harness.provider->SetSealDetachedTestHook(nullptr, nullptr);
}

TEST_CASE("guest execution session provider authenticates nested returns",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  std::string error;
  REQUIRE(harness.provider->BeginCapture(
      harness.Checkpoint(kLeafFunctionAddress), harness.Participants(),
      harness.HostCalls(), &error));
  const auto state =
      ppc::CaptureGuestPPCRegisterState(*harness.thread->context());
  REQUIRE(harness.provider->OnFunctionExit(harness.InvocationIdentity(),
                                           kLeafFunctionAddress,
                                           kMiddleFunctionAddress + 4, state));
  REQUIRE(harness.provider->OnFunctionExit(harness.InvocationIdentity(),
                                           kMiddleFunctionAddress,
                                           kFunctionAddress + 4, state));
  REQUIRE(harness.provider->SealCapture(harness.Checkpoint(),
                                        harness.HostCalls(), &error));

  std::vector<uint8_t> corpus_bytes;
  REQUIRE(harness.provider->CollectSessionCodeCorpus(&corpus_bytes, &error));
  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(corpus_bytes, &corpus, &error));
  REQUIRE(corpus.functions().size() == 3);
  REQUIRE(corpus.FindFunction(kFunctionAddress));
  REQUIRE(corpus.FindFunction(kMiddleFunctionAddress));
  REQUIRE(corpus.FindFunction(kLeafFunctionAddress));
}

TEST_CASE("guest execution session provider serializes seal and stop",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  std::string error;
  REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                         harness.Participants(),
                                         harness.HostCalls(), &error));
  SealDetachedGate gate;
  harness.provider->SetSealDetachedTestHook(&SealDetachedGate::Hook, &gate);

  std::atomic<bool> seal_result{false};
  std::atomic<bool> end_finished{false};
  std::thread seal_thread([&] {
    std::string seal_error;
    seal_result.store(
        harness.provider->SealCapture(harness.Checkpoint(), harness.HostCalls(),
                                      &seal_error),
        std::memory_order_release);
  });
  const bool seal_blocked = gate.WaitForEntry(std::chrono::seconds(2));

  std::thread end_thread;
  bool end_waiting = false;
  if (seal_blocked) {
    end_thread = std::thread([&] {
      harness.provider->EndCapture(false);
      end_finished.store(true, std::memory_order_release);
    });
    end_waiting =
        WaitForLifecycleWaiter(*harness.provider, std::chrono::seconds(2));
  }
  gate.Release();
  seal_thread.join();
  if (end_thread.joinable()) {
    end_thread.join();
  }
  harness.provider->SetSealDetachedTestHook(nullptr, nullptr);

  REQUIRE(seal_blocked);
  REQUIRE(end_waiting);
  REQUIRE(seal_result.load(std::memory_order_acquire));
  REQUIRE(end_finished.load(std::memory_order_acquire));
  REQUIRE(harness.provider->status().state ==
          GuestExecutionSessionCaptureProviderState::kStopped);
}

TEST_CASE("guest execution session provider rejects inexact production inputs",
          "[guest-execution-session-capture-provider]") {
  SECTION("blocked scheduler continuation") {
    ProviderHarness harness;
    auto participant = harness.Checkpoint().participants.front();
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport;
    participant.restorable = false;
    std::string error;
    REQUIRE_FALSE(
        harness.provider->SupportsCheckpointParticipant(participant, &error));
    REQUIRE(error.find("exact-PC JIT") != std::string::npos);
  }

  SECTION("not-yet-run scheduler continuation") {
    ProviderHarness harness;
    auto participant = harness.Checkpoint().participants.front();
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kReady;
    participant.guest_pc = 0;
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kNotYetRun;
    participant.restorable = false;
    std::string error;
    REQUIRE(
        harness.provider->SupportsCheckpointParticipant(participant, &error));
    REQUIRE(error.empty());
  }

  SECTION("checkpoint PC without a catalog owner") {
    ProviderHarness harness;
    std::string error;
    REQUIRE_FALSE(harness.provider->BeginCapture(
        harness.Checkpoint(kFunctionAddress + 0x100), harness.Participants(),
        harness.HostCalls(), &error));
    REQUIRE(error.find("no successful definition") != std::string::npos);
    REQUIRE(harness.provider->status().state ==
            GuestExecutionSessionCaptureProviderState::kRejected);
  }

  SECTION("non-outer return continuation without a catalog owner") {
    ProviderHarness harness;
    std::string error;
    REQUIRE(harness.provider->BeginCapture(
        harness.Checkpoint(kLeafFunctionAddress), harness.Participants(),
        harness.HostCalls(), &error));
    REQUIRE_FALSE(harness.provider->OnFunctionExit(
        harness.InvocationIdentity(), kLeafFunctionAddress,
        kFunctionAddress + 0x100,
        ppc::CaptureGuestPPCRegisterState(*harness.thread->context())));
    REQUIRE(harness.provider->status().state ==
            GuestExecutionSessionCaptureProviderState::kRejected);
  }

  SECTION("participant outer return may be ownerless") {
    ProviderHarness harness;
    std::string error;
    REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                           harness.Participants(),
                                           harness.HostCalls(), &error));
    REQUIRE(harness.provider->OnFunctionExit(
        harness.InvocationIdentity(), kFunctionAddress, kOuterReturnAddress,
        ppc::CaptureGuestPPCRegisterState(*harness.thread->context())));
    REQUIRE(harness.provider->status().state ==
            GuestExecutionSessionCaptureProviderState::kRecording);
  }

  SECTION("unobserved page mutation") {
    ProviderHarness harness;
    std::string error;
    REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                           harness.Participants(),
                                           harness.HostCalls(), &error));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), harness.data_address + 32, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
    std::fill_n(harness.memory->TranslateVirtual(harness.data_address + 32), 4,
                uint8_t{0xE1});
    REQUIRE_FALSE(harness.provider->SealCapture(harness.Checkpoint(),
                                                harness.HostCalls(), &error));
    REQUIRE(error.find("unobserved") != std::string::npos);
  }

  SECTION("unsupported atomic reservation") {
    ProviderHarness harness;
    std::string error;
    REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                           harness.Participants(),
                                           harness.HostCalls(), &error));
    REQUIRE_FALSE(harness.provider->OnUnsupportedDependency(
        harness.InvocationIdentity(),
        ppc::kGuestInvocationDependencyAtomicReservation));
    const auto status = harness.provider->status();
    REQUIRE(status.state ==
            GuestExecutionSessionCaptureProviderState::kRejected);
    REQUIRE(status.message.find("unsupported dependency") != std::string::npos);
  }

  SECTION("dirty page without complete external mutation coverage") {
    ProviderHarness harness;
    std::string error;
    REQUIRE(harness.provider->BeginCapture(harness.Checkpoint(),
                                           harness.Participants(),
                                           harness.HostCalls(), &error));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kWrite));
    std::fill_n(harness.memory->TranslateVirtual(harness.data_address), 4,
                uint8_t{0xA7});
    REQUIRE_FALSE(harness.provider->SealCapture(harness.Checkpoint(),
                                                harness.HostCalls(), &error));
    REQUIRE(error.find("complete external-memory mutation coverage") !=
            std::string::npos);
  }

  SECTION("read-shared page later written by the original participant") {
    ProviderHarness harness;
    harness.AddSecondParticipant();
    std::string error;
    REQUIRE(harness.provider->BeginCapture(
        harness.TwoThreadCheckpoint(), harness.TwoThreadParticipants(),
        harness.TwoThreadHostCalls(), &error));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.SecondInvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
    REQUIRE_FALSE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kWrite));
    const auto status = harness.provider->status();
    REQUIRE(status.state ==
            GuestExecutionSessionCaptureProviderState::kRejected);
    REQUIRE(status.message.find("cross-participant shared write") !=
            std::string::npos);
  }

  SECTION("written page later read by another participant") {
    ProviderHarness harness;
    harness.AddSecondParticipant();
    std::string error;
    REQUIRE(harness.provider->BeginCapture(
        harness.TwoThreadCheckpoint(), harness.TwoThreadParticipants(),
        harness.TwoThreadHostCalls(), &error));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(harness.provider->OnMemoryAccess(
        harness.SecondInvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
    REQUIRE(harness.provider->status().message.find(
                "cross-participant shared write") != std::string::npos);
  }

  SECTION("read page later written by another participant") {
    ProviderHarness harness;
    harness.AddSecondParticipant();
    std::string error;
    REQUIRE(harness.provider->BeginCapture(
        harness.TwoThreadCheckpoint(), harness.TwoThreadParticipants(),
        harness.TwoThreadHostCalls(), &error));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
    REQUIRE_FALSE(harness.provider->OnMemoryAccess(
        harness.SecondInvocationIdentity(), harness.data_address, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE(harness.provider->status().message.find(
                "cross-participant shared write") != std::string::npos);
  }

  SECTION("physical alias views cannot bypass shared-page ordering") {
    ProviderHarness harness;
    harness.AddSecondParticipant();
    harness.AllocateAliasData();
    std::string error;
    REQUIRE(harness.provider->BeginCapture(
        harness.TwoThreadCheckpoint(), harness.TwoThreadParticipants(),
        harness.TwoThreadHostCalls(), &error));
    REQUIRE(harness.provider->OnMemoryAccess(
        harness.InvocationIdentity(), kAliasDataAddress, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kRead));
    REQUIRE_FALSE(harness.provider->OnMemoryAccess(
        harness.SecondInvocationIdentity(), kAliasDataMirrorAddress, 4,
        ppc::GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE(harness.provider->status().state ==
            GuestExecutionSessionCaptureProviderState::kRejected);
  }
}

TEST_CASE("guest execution session classifies a woken-in-wait checkpoint",
          "[guest-execution-session-capture-provider]") {
  auto participant = WokenInWaitParticipant();
  REQUIRE(IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));

  SECTION("a suspended waiter is the same class") {
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kSuspended;
    participant.suspension_count = 1;
    REQUIRE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a running participant") {
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kRunning;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a still-blocked participant") {
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kBlocked;
    participant.guest_pc = kFunctionAddress;
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a thread that never ran") {
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kNotYetRun;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("an exact-PC JIT resume route") {
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a restorable participant") {
    participant.restorable = true;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a published link register") {
    participant.guest_pc = kFunctionAddress;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("an in-flight IRQL preemption episode") {
    participant.preempt_defers_irql = 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("an in-flight lock preemption episode") {
    participant.preempt_defers_lock = 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a declined safepoint") {
    participant.capture_declined_safepoints = 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("no wait at all") {
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kNone;
    participant.blocked_wait = {};
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a wait kind the export dispatch does not model") {
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kIoCompletion;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kDelay;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("the other modeled wait kinds") {
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kMultiAny;
    REQUIRE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kMultiAll;
    REQUIRE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a wait naming no object") {
    participant.blocked_wait.handle_count = 0;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("more handles than the binding holds") {
    participant.blocked_wait.handle_count = static_cast<uint8_t>(
        kernel::kGuestSchedulerCaptureMaximumWaitHandles + 1);
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("an alertable wait") {
    participant.blocked_wait.flags |=
        kernel::kGuestSchedulerCaptureWaitFlagAlertable;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
  SECTION("a pending user APC") {
    participant.blocked_wait.flags |=
        kernel::kGuestSchedulerCaptureWaitFlagUserApcPending;
    REQUIRE_FALSE(
        IsGuestExecutionSessionWokenInWaitCheckpointParticipant(participant));
  }
}

TEST_CASE("guest execution session classifies a woken-in-wait topology row",
          "[guest-execution-session-capture-provider]") {
  auto participant = WokenInWaitRow();
  REQUIRE(IsGuestExecutionSessionWokenInWaitParticipant(participant));

  SECTION("a participant that is not ready") {
    participant.state =
        GuestExecutionSessionSchedulerParticipantState::kSuspended;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
    participant.state =
        GuestExecutionSessionSchedulerParticipantState::kRunning;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
    participant.state =
        GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("a still-blocked row") {
    participant.state =
        GuestExecutionSessionSchedulerParticipantState::kBlocked;
    participant.resume_kind =
        GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport;
    participant.guest_pc = kFunctionAddress;
    participant.ready_queue_level = kGuestExecutionSessionSchedulerNoValue;
    participant.ready_queue_fifo_ordinal =
        kGuestExecutionSessionSchedulerNoValue;
    participant.blocked_wait.kind =
        GuestExecutionSessionSchedulerWaitKind::kSingle;
    participant.blocked_wait.handle_count = 1;
    participant.blocked_wait.handles[0] = kWaitHandle;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("a thread that never ran") {
    participant.resume_kind =
        GuestExecutionSessionSchedulerResumeKind::kNotYetRun;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("an exact-PC JIT resume route") {
    participant.resume_kind =
        GuestExecutionSessionSchedulerResumeKind::kJitSafepoint;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("an unrouted row") {
    participant.resume_kind = GuestExecutionSessionSchedulerResumeKind::kNone;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("a restorable participant") {
    participant.restorable = true;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("a published link register") {
    participant.guest_pc = kFunctionAddress;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("no effective priority") {
    participant.effective_priority = kGuestExecutionSessionSchedulerNoValue;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("a queue level away from the effective priority") {
    participant.ready_queue_level = kWaitPriority + 1;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("no seeded queue position") {
    participant.ready_queue_fifo_ordinal =
        kGuestExecutionSessionSchedulerNoValue;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
  SECTION("a serialized wait binding") {
    participant.blocked_wait.kind =
        GuestExecutionSessionSchedulerWaitKind::kSingle;
    participant.blocked_wait.handle_count = 1;
    participant.blocked_wait.handles[0] = kWaitHandle;
    REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitParticipant(participant));
  }
}

TEST_CASE("guest execution session provider still gates a woken waiter",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();

  REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitCheckpointParticipant(
      harness.BlockedSecondThreadCheckpoint().participants.back()));

  auto woken = harness.PassiveSecondThreadCheckpoint().participants.back();
  REQUIRE_FALSE(IsGuestExecutionSessionWokenInWaitCheckpointParticipant(woken));
  woken.blocked_wait_kind = kernel::GuestSchedulerCaptureWaitKind::kSingle;
  woken.blocked_wait.handle_count = 1;
  woken.blocked_wait.handles[0] = kWaitHandle;
  REQUIRE(IsGuestExecutionSessionWokenInWaitCheckpointParticipant(woken));

  // The class is a strict subset of the passive one, so the classifier moves
  // no participant across the capability gate.
  std::string error;
  REQUIRE(harness.provider->SupportsCheckpointParticipant(woken, &error));
  REQUIRE(error.empty());

  const auto dormant =
      harness
          .PassiveSecondThreadCheckpoint(
              kernel::GuestSchedulerCheckpointResumeKind::kNotYetRun)
          .participants.back();
  REQUIRE_FALSE(
      IsGuestExecutionSessionWokenInWaitCheckpointParticipant(dormant));
  REQUIRE(harness.provider->SupportsCheckpointParticipant(dormant, &error));
}

TEST_CASE("guest execution session classifies a blocked-parity checkpoint",
          "[guest-execution-session-capture-provider]") {
  auto participant = BlockedParityParticipant();
  REQUIRE(
      IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));

  SECTION("a participant that is not blocked") {
    participant.state =
        kernel::GuestSchedulerCheckpointParticipantState::kReady;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a resume kind other than after-blocking-export") {
    participant.resume_kind =
        kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a restorable participant") {
    participant.restorable = true;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("no link register") {
    participant.guest_pc = 0;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("an unaligned link register") {
    participant.guest_pc = kFunctionAddress + 2;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("no wait kind") {
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kNone;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("more handles than the binding holds") {
    participant.blocked_wait.handle_count = static_cast<uint8_t>(
        kernel::kGuestSchedulerCaptureMaximumWaitHandles + 1);
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("an in-flight IRQL preemption episode") {
    participant.preempt_defers_irql = 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("an in-flight lock preemption episode") {
    participant.preempt_defers_lock = 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a declined safepoint") {
    participant.capture_declined_safepoints = 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a handle-free untimed wait") {
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kFence;
    participant.blocked_wait = {};
    participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("an alertable wait") {
    participant.blocked_wait.flags |=
        kernel::kGuestSchedulerCaptureWaitFlagAlertable;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a pending user APC") {
    participant.blocked_wait.flags |=
        kernel::kGuestSchedulerCaptureWaitFlagUserApcPending;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a signal epoch that moved") {
    participant.blocked_wait.signal_epochs_observed[0] = kWaitEpoch + 1;
    participant.blocked_wait.observed_wait_epoch = kWaitEpoch + 1;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a deadline the boundary has reached") {
    participant.blocked_wait.deadline_ms = kWaitUptimeMs;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a deadline the boundary has not reached") {
    participant.blocked_wait.deadline_ms = kWaitUptimeMs + 1;
    REQUIRE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
  SECTION("a timed external wait") {
    participant.blocked_wait_kind =
        kernel::GuestSchedulerCaptureWaitKind::kIoCompletion;
    participant.blocked_wait = {};
    participant.blocked_wait.handle_count = 1;
    participant.blocked_wait.handles[0] = kWaitHandle;
    participant.blocked_wait.flags =
        kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
    participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
    REQUIRE_FALSE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
    participant.blocked_wait.deadline_ms = kWaitUptimeMs + 1;
    REQUIRE(
        IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  }
}

TEST_CASE("guest execution session classifies a blocked-parity topology row",
          "[guest-execution-session-capture-provider]") {
  auto participant = BlockedParityRow();
  REQUIRE(IsGuestExecutionSessionBlockedParityParticipant(participant));

  SECTION("a participant that is not blocked") {
    participant.state = GuestExecutionSessionSchedulerParticipantState::kReady;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a resume kind other than after-blocking-export") {
    participant.resume_kind =
        GuestExecutionSessionSchedulerResumeKind::kNativeContinuation;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a restorable participant") {
    participant.restorable = true;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("no link register") {
    participant.guest_pc = 0;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("an unaligned link register") {
    participant.guest_pc = kFunctionAddress + 2;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("no wait kind") {
    participant.blocked_wait.kind =
        GuestExecutionSessionSchedulerWaitKind::kNone;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a malformed wait binding") {
    participant.blocked_wait.handle_count = 2;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a handle-free untimed wait") {
    participant.blocked_wait = {};
    participant.blocked_wait.kind =
        GuestExecutionSessionSchedulerWaitKind::kFence;
    participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("an alertable wait") {
    participant.blocked_wait.flags |=
        kGuestExecutionSessionSchedulerWaitFlagAlertable;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("an alertable wait with a pending user APC") {
    participant.blocked_wait.flags |=
        kGuestExecutionSessionSchedulerWaitFlagAlertable |
        kGuestExecutionSessionSchedulerWaitFlagUserApcPending;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a signal epoch that moved") {
    participant.blocked_wait.signal_epochs_observed[0] = kWaitEpoch + 1;
    participant.blocked_wait.observed_wait_epoch = kWaitEpoch + 1;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a deadline the boundary has reached") {
    participant.blocked_wait.deadline_ms = kWaitUptimeMs;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a deadline the boundary has not reached") {
    participant.blocked_wait.deadline_ms = kWaitUptimeMs + 1;
    REQUIRE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a timed delay") {
    participant.blocked_wait = {};
    participant.blocked_wait.kind =
        GuestExecutionSessionSchedulerWaitKind::kDelay;
    participant.blocked_wait.flags =
        kGuestExecutionSessionSchedulerWaitFlagGated |
        kGuestExecutionSessionSchedulerWaitFlagInterruptible;
    participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
    participant.blocked_wait.deadline_ms = kWaitUptimeMs + 1;
    REQUIRE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
  SECTION("a timed external wait") {
    participant.blocked_wait = {};
    participant.blocked_wait.kind =
        GuestExecutionSessionSchedulerWaitKind::kIoCompletion;
    participant.blocked_wait.handle_count = 1;
    participant.blocked_wait.handles[0] = kWaitHandle;
    participant.blocked_wait.flags =
        kGuestExecutionSessionSchedulerWaitFlagInterruptible;
    participant.blocked_wait.observed_uptime_ms = kWaitUptimeMs;
    REQUIRE_FALSE(IsGuestExecutionSessionBlockedParityParticipant(participant));
    participant.blocked_wait.deadline_ms = kWaitUptimeMs + 1;
    REQUIRE(IsGuestExecutionSessionBlockedParityParticipant(participant));
  }
}

TEST_CASE("guest execution session provider admits blocked parity",
          "[guest-execution-session-capture-provider]") {
  ProviderHarness harness;
  harness.AddSecondParticipant();
  harness.InstallExternalEventLog();
  auto participant =
      harness.BlockedSecondThreadCheckpoint().participants.back();
  REQUIRE(
      IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  std::string error;
  REQUIRE(harness.provider->SupportsCheckpointParticipant(participant, &error));
  REQUIRE(error.empty());

  // An alertable wait can run guest code on the waiting thread's stack, so it
  // states nothing about whether the participant ran and cannot claim parity.
  participant.blocked_wait.flags |=
      kernel::kGuestSchedulerCaptureWaitFlagAlertable;
  REQUIRE_FALSE(
      IsGuestExecutionSessionBlockedParityCheckpointParticipant(participant));
  REQUIRE_FALSE(
      harness.provider->SupportsCheckpointParticipant(participant, &error));
  REQUIRE(error.find("outside the modeled blocking-export wait allowlist") !=
          std::string::npos);
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
