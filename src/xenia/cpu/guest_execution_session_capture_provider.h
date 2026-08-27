/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_PROVIDER_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_PROVIDER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenia/cpu/guest_execution_session_capture_runtime.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/jit_corpus.h"

namespace xe {

class Memory;

namespace cpu {

class Processor;

struct GuestExecutionSessionCaptureProviderConfig {
  uint32_t jit_corpus_config_flags = JitCorpus::kConfigGuestScheduler;
  // Zero selects the native host protection granule.
  uint32_t host_protection_page_size = 0;
  uint32_t maximum_data_page_count = 8192;
  uint32_t maximum_code_page_count = 65536;
  uint64_t maximum_memory_access_count = 1'000'000;
  uint32_t maximum_function_count = 32768;
  uint32_t maximum_dependency_count = 131072;
};

enum class GuestExecutionSessionCaptureProviderState : uint8_t {
  kCataloging,
  kRecording,
  kSealed,
  kAccepted,
  kRejected,
  kStopped,
};

struct GuestExecutionSessionCaptureProviderStatus {
  GuestExecutionSessionCaptureProviderState state =
      GuestExecutionSessionCaptureProviderState::kCataloging;
  uint64_t memory_access_count = 0;
  uint32_t catalog_function_count = 0;
  uint32_t corpus_function_count = 0;
  uint32_t code_page_count = 0;
  uint32_t data_page_count = 0;
  std::string message;
};

// Production exact-PC, memory and code source for one continuous session.
// CreateAndAttach must run before title translation so the definition catalog
// is complete. The Processor keeps a non-owning event-sink pointer, therefore
// this object must be ended and destroyed before the Processor.
// Dirty guest pages remain fail-closed until the runtime owns a lifetime-scoped
// coverage lease for every non-JIT guest-memory writer. Final byte comparison
// alone is not such an attestation.
class GuestExecutionSessionCaptureProvider final
    : public GuestExecutionSessionCaptureRuntimeProvider,
      public GuestInvocationCaptureEventSink {
 public:
  static std::unique_ptr<GuestExecutionSessionCaptureProvider> CreateAndAttach(
      Memory& memory, Processor& processor,
      const GuestExecutionSessionCaptureProviderConfig& config,
      std::string* error = nullptr);

  ~GuestExecutionSessionCaptureProvider() override;
  GuestExecutionSessionCaptureProvider(
      const GuestExecutionSessionCaptureProvider&) = delete;
  GuestExecutionSessionCaptureProvider& operator=(
      const GuestExecutionSessionCaptureProvider&) = delete;

  bool SupportsCheckpointParticipant(
      const kernel::GuestSchedulerCheckpointParticipant& participant,
      std::string* error) noexcept override;
  bool BeginCapture(
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          participants,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) noexcept override;
  bool SealCapture(
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) noexcept override;
  void EndCapture(bool accepted) noexcept override;

  bool EncodeParticipantState(
      const GuestExecutionCaptureParticipantIdentity& participant,
      std::vector<uint8_t>* output, std::string* error) noexcept override;
  bool CollectCheckpointContent(
      bool initial_checkpoint,
      std::vector<GuestExecutionSessionAssemblerContent>* output,
      std::string* error) noexcept override;
  bool CollectSessionCodeCorpus(std::vector<uint8_t>* output,
                                std::string* error) noexcept override;

  uint32_t root_address() const override { return 0; }
  uint8_t initial_event_mask() const override;
  uint8_t event_mask(
      const ppc::GuestInvocationRecorderIdentity& identity) const override;
  bool Poll() noexcept override;
  bool OnFunctionDependency(uint32_t source_address,
                            uint32_t dependency_address) noexcept override;
  bool OnFunctionDefined(uint32_t address,
                         uint32_t end_address) noexcept override;
  bool OnFunctionEntry(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t end_address,
      const ppc::GuestPPCRegisterState& state) noexcept override;
  bool OnFunctionExit(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t return_address,
      const ppc::GuestPPCRegisterState& state) noexcept override;
  bool OnMemoryAccess(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t size,
      ppc::GuestInvocationRecorderMemoryAccess access) noexcept override;
  bool OnUnsupportedDependency(
      const ppc::GuestInvocationRecorderIdentity& identity,
      uint32_t dependency_flags) noexcept override;
  bool OnTailCall(const ppc::GuestInvocationRecorderIdentity& identity,
                  uint32_t from_address,
                  uint32_t target_address) noexcept override;
  bool OnUnwindOrLongjmp(
      const ppc::GuestInvocationRecorderIdentity& identity) noexcept override;
  bool OnAsyncReentry(
      const ppc::GuestInvocationRecorderIdentity& identity) noexcept override;

  void SetSealDetachedTestHook(void (*hook)(void*), void* context) noexcept;
  uint32_t lifecycle_waiter_count_for_test() const noexcept;
  GuestExecutionSessionCaptureProviderStatus status() const;

 private:
  struct Impl;
  explicit GuestExecutionSessionCaptureProvider(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_PROVIDER_H_
