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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/execution_jit_corpus.h"
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
    checkpoint.participants.push_back(
        {kThreadId, guest_pc, 0,
         kernel::GuestSchedulerCheckpointParticipantState::kRunning,
         kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint, true});
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
    checkpoint.participants.push_back(
        {kSecondThreadId, kFunctionAddress, 1,
         kernel::GuestSchedulerCheckpointParticipantState::kRunning,
         kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint, true});
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
      harness.participant, &initial_state_bytes, &error));
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
  REQUIRE(harness.provider->EncodeParticipantState(harness.participant,
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

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
