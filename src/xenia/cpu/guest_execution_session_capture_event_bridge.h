/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_EVENT_BRIDGE_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_EVENT_BRIDGE_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenia/cpu/guest_execution_session_capture_runtime.h"

namespace xe {
namespace cpu {

class GuestExecutionSessionCaptureSchedulerEventBridge final
    : public GuestExecutionSessionCaptureRuntimeEventBridge {
 public:
  static constexpr uint32_t kSchedulerPayloadVersion = 2;
  static constexpr size_t kSchedulerPayloadV1Size = 48;
  static constexpr size_t kSchedulerPayloadSize = 192;

  GuestExecutionSessionCaptureSchedulerEventBridge() = default;
  ~GuestExecutionSessionCaptureSchedulerEventBridge() override = default;
  GuestExecutionSessionCaptureSchedulerEventBridge(
      const GuestExecutionSessionCaptureSchedulerEventBridge&) = delete;
  GuestExecutionSessionCaptureSchedulerEventBridge& operator=(
      const GuestExecutionSessionCaptureSchedulerEventBridge&) = delete;

  bool BeginSession(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          participants,
      std::string* error) noexcept override;
  GuestExecutionSessionAssemblerAction OnSchedulerEvent(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCaptureEvent& event,
      std::string* error) noexcept override;
  bool SealSession(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::string* error) noexcept override;
  bool FinalizeBundle(GuestExecutionSessionBundle* bundle,
                      uint64_t scheduler_event_count,
                      std::string* error) noexcept override;

  static bool DecodeSchedulerEventPayload(
      std::span<const uint8_t> payload,
      kernel::GuestSchedulerCaptureEvent* event,
      std::string* error = nullptr) noexcept;

 private:
  struct Participant {
    GuestExecutionCaptureParticipantIdentity identity;
    uint32_t ordinal = 0;
  };

  const Participant* FindParticipant(uint64_t capture_instance_id,
                                     uint32_t guest_thread_id) const;
  bool BuildSchedulerTopology(
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      GuestExecutionSessionSchedulerTopologyBoundary boundary,
      GuestExecutionSessionSchedulerTopologyChunk* output,
      std::string* error) const;

  std::vector<Participant> participants_;
  GuestExecutionSessionSchedulerTopologyChunk start_scheduler_topology_;
  GuestExecutionSessionSchedulerTopologyChunk final_scheduler_topology_;
  uint64_t start_checkpoint_generation_ = 0;
  uint64_t first_scheduler_sequence_ = 0;
  uint64_t last_scheduler_sequence_ = 0;
  uint64_t scheduler_event_count_ = 0;
  bool begun_ = false;
  bool sealed_ = false;
  bool finalized_ = false;
  bool rejected_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_EVENT_BRIDGE_H_
