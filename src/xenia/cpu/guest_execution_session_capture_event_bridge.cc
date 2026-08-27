/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_capture_event_bridge.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "xenia/cpu/guest_execution_continuous_event.h"

namespace xe {
namespace cpu {
namespace {

using AssemblerAction = GuestExecutionSessionAssemblerAction;
using AssemblerState = GuestExecutionSessionAssemblerState;
using CaptureEvent = kernel::GuestSchedulerCaptureEvent;
using CaptureKind = kernel::GuestSchedulerCaptureEventKind;
using CaptureReason = kernel::GuestSchedulerCaptureReason;

constexpr std::array<uint8_t, 8> kSchedulerPayloadMagic = {'X', 'E', 'G', 'S',
                                                           'C', 'E', '1', 0};
constexpr uint16_t kKnownCaptureFlags =
    kernel::kGuestSchedulerCaptureFlagAtHead |
    kernel::kGuestSchedulerCaptureFlagYieldToOther |
    kernel::kGuestSchedulerCaptureFlagHonoredYield |
    kernel::kGuestSchedulerCaptureFlagFirstRun |
    kernel::kGuestSchedulerCaptureFlagFreshQuantum |
    kernel::kGuestSchedulerCaptureFlagQuantumEnd |
    kernel::kGuestSchedulerCaptureFlagToLower |
    kernel::kGuestSchedulerCaptureFlagPreempted |
    kernel::kGuestSchedulerCaptureFlagSchedulerRequested |
    kernel::kGuestSchedulerCaptureFlagCaptureRequested |
    kernel::kGuestSchedulerCaptureFlagGated |
    kernel::kGuestSchedulerCaptureFlagAlertable |
    kernel::kGuestSchedulerCaptureFlagInterruptible |
    kernel::kGuestSchedulerCaptureFlagHasDeadline;
constexpr uint8_t kKnownWaitFlags =
    kernel::kGuestSchedulerCaptureWaitFlagGated |
    kernel::kGuestSchedulerCaptureWaitFlagAlertable |
    kernel::kGuestSchedulerCaptureWaitFlagInterruptible |
    kernel::kGuestSchedulerCaptureWaitFlagUserApcPending;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

void WriteU16(std::vector<uint8_t>* output, size_t offset, uint16_t value) {
  (*output)[offset] = static_cast<uint8_t>(value);
  (*output)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void WriteU32(std::vector<uint8_t>* output, size_t offset, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    (*output)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void WriteU64(std::vector<uint8_t>* output, size_t offset, uint64_t value) {
  for (uint32_t i = 0; i < 8; ++i) {
    (*output)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

uint16_t ReadU16(std::span<const uint8_t> input, size_t offset) {
  return uint16_t(input[offset]) | (uint16_t(input[offset + 1]) << 8);
}

uint32_t ReadU32(std::span<const uint8_t> input, size_t offset) {
  return uint32_t(input[offset]) | (uint32_t(input[offset + 1]) << 8) |
         (uint32_t(input[offset + 2]) << 16) |
         (uint32_t(input[offset + 3]) << 24);
}

uint64_t ReadU64(std::span<const uint8_t> input, size_t offset) {
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    value |= uint64_t(input[offset + i]) << (i * 8);
  }
  return value;
}

bool IsKnownReason(CaptureReason reason) {
  switch (reason) {
    case CaptureReason::kNone:
    case CaptureReason::kPriority:
    case CaptureReason::kWake:
    case CaptureReason::kTimeslice:
    case CaptureReason::kTerminate:
    case CaptureReason::kShutdown:
    case CaptureReason::kDeferredLock:
    case CaptureReason::kDeferredIrql:
    case CaptureReason::kForcedIrql:
    case CaptureReason::kYielded:
    case CaptureReason::kPolled:
    case CaptureReason::kSignalEpoch:
    case CaptureReason::kDeadline:
    case CaptureReason::kUserApc:
    case CaptureReason::kBackstop:
    case CaptureReason::kDetached:
    case CaptureReason::kPreemptRequested:
    case CaptureReason::kReadied:
    case CaptureReason::kNeverRan:
    case CaptureReason::kDeferredToDispatcher:
      return true;
    default:
      return false;
  }
}

bool IsSupportedKind(CaptureKind kind) {
  switch (kind) {
    case CaptureKind::kEnqueueReady:
    case CaptureKind::kDequeueReady:
    case CaptureKind::kDispatch:
    case CaptureKind::kSwitchOut:
    case CaptureKind::kYield:
    case CaptureKind::kPreemptRequest:
    case CaptureKind::kSafepoint:
    case CaptureKind::kBlock:
    case CaptureKind::kReready:
    case CaptureKind::kParkSuspended:
    case CaptureKind::kResume:
    case CaptureKind::kPriorityChange:
    case CaptureKind::kMigrate:
      return true;
    case CaptureKind::kNone:
    case CaptureKind::kExit:
    case CaptureKind::kTerminate:
    case CaptureKind::kForget:
    case CaptureKind::kShutdown:
    default:
      return false;
  }
}

bool HasParticipantActor(CaptureKind kind) {
  return kind == CaptureKind::kYield || kind == CaptureKind::kSafepoint ||
         kind == CaptureKind::kBlock;
}

bool IsInstructionDrainBoundary(CaptureKind kind) {
  return kind == CaptureKind::kSwitchOut || kind == CaptureKind::kYield ||
         kind == CaptureKind::kSafepoint || kind == CaptureKind::kBlock;
}

GuestExecutionSessionEventKind CanonicalKind(CaptureKind kind) {
  switch (kind) {
    case CaptureKind::kSafepoint:
    case CaptureKind::kBlock:
    case CaptureKind::kReready:
    case CaptureKind::kParkSuspended:
    case CaptureKind::kResume:
      return GuestExecutionSessionEventKind::kSynchronization;
    default:
      return GuestExecutionSessionEventKind::kThreadDispatch;
  }
}

uint16_t AllowedFlags(CaptureKind kind) {
  switch (kind) {
    case CaptureKind::kEnqueueReady:
      return kernel::kGuestSchedulerCaptureFlagAtHead |
             kernel::kGuestSchedulerCaptureFlagYieldToOther;
    case CaptureKind::kDequeueReady:
      return kernel::kGuestSchedulerCaptureFlagHonoredYield;
    case CaptureKind::kDispatch:
      return kernel::kGuestSchedulerCaptureFlagFirstRun |
             kernel::kGuestSchedulerCaptureFlagFreshQuantum;
    case CaptureKind::kYield:
      return kernel::kGuestSchedulerCaptureFlagQuantumEnd |
             kernel::kGuestSchedulerCaptureFlagToLower |
             kernel::kGuestSchedulerCaptureFlagPreempted;
    case CaptureKind::kSafepoint:
      return kernel::kGuestSchedulerCaptureFlagSchedulerRequested |
             kernel::kGuestSchedulerCaptureFlagCaptureRequested;
    case CaptureKind::kBlock:
      return kernel::kGuestSchedulerCaptureFlagGated |
             kernel::kGuestSchedulerCaptureFlagAlertable |
             kernel::kGuestSchedulerCaptureFlagInterruptible |
             kernel::kGuestSchedulerCaptureFlagHasDeadline;
    case CaptureKind::kReready:
    case CaptureKind::kMigrate:
      return kernel::kGuestSchedulerCaptureFlagAtHead;
    default:
      return 0;
  }
}

bool IsReasonAllowed(CaptureKind kind, CaptureReason reason) {
  switch (kind) {
    case CaptureKind::kPreemptRequest:
      return reason == CaptureReason::kPriority ||
             reason == CaptureReason::kWake ||
             reason == CaptureReason::kTimeslice ||
             reason == CaptureReason::kTerminate ||
             reason == CaptureReason::kShutdown;
    case CaptureKind::kSafepoint:
      return reason == CaptureReason::kDeferredLock ||
             reason == CaptureReason::kDeferredIrql ||
             reason == CaptureReason::kForcedIrql ||
             reason == CaptureReason::kYielded;
    case CaptureKind::kReready:
      return reason == CaptureReason::kPolled ||
             reason == CaptureReason::kSignalEpoch ||
             reason == CaptureReason::kDeadline ||
             reason == CaptureReason::kUserApc ||
             reason == CaptureReason::kBackstop;
    default:
      return reason == CaptureReason::kNone;
  }
}

bool IsCpu(int8_t cpu) { return cpu >= 0 && cpu < 6; }

bool IsEmptyWaitState(const kernel::GuestSchedulerCaptureWaitState& wait) {
  return wait == kernel::GuestSchedulerCaptureWaitState{};
}

bool ValidateWaitState(const CaptureEvent& event, std::string* error) {
  const auto& wait = event.wait;
  const auto wait_kind =
      static_cast<kernel::GuestSchedulerCaptureWaitKind>(event.value);
  if (wait.handle_count > kernel::kGuestSchedulerCaptureMaximumWaitHandles ||
      (wait.flags & ~kKnownWaitFlags) ||
      ((wait.flags & kernel::kGuestSchedulerCaptureWaitFlagUserApcPending) &&
       !(wait.flags & kernel::kGuestSchedulerCaptureWaitFlagAlertable))) {
    return Fail(error, "scheduler capture wait state is malformed");
  }
  uint32_t epochs_before = 0;
  uint32_t epochs_observed = 0;
  bool has_signal_epochs = false;
  for (size_t index = 0;
       index < kernel::kGuestSchedulerCaptureMaximumWaitHandles; ++index) {
    if (index < wait.handle_count) {
      if (!wait.handles[index]) {
        return Fail(error, "scheduler capture wait handle is invalid");
      }
    } else if (wait.handles[index] || wait.signal_epochs_before[index] ||
               wait.signal_epochs_observed[index]) {
      return Fail(error, "scheduler capture wait padding is nonzero");
    }
    epochs_before += wait.signal_epochs_before[index];
    epochs_observed += wait.signal_epochs_observed[index];
    has_signal_epochs = has_signal_epochs || wait.signal_epochs_before[index] ||
                        wait.signal_epochs_observed[index];
  }
  if (epochs_before != wait.wait_epoch ||
      epochs_observed != wait.observed_wait_epoch) {
    return Fail(error, "scheduler capture wait epochs are inconsistent");
  }

  switch (wait_kind) {
    case kernel::GuestSchedulerCaptureWaitKind::kSingle:
      if (wait.handle_count != 1) {
        return Fail(error, "scheduler single wait has no unique handle");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kMultiAny:
    case kernel::GuestSchedulerCaptureWaitKind::kMultiAll:
      if (!wait.handle_count) {
        return Fail(error, "scheduler multi-wait has no handles");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kDelay:
    case kernel::GuestSchedulerCaptureWaitKind::kFence:
    case kernel::GuestSchedulerCaptureWaitKind::kIoOffload:
    case kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff:
      if (wait.handle_count) {
        return Fail(error, "scheduler handle-free wait names an object");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kIoCompletion:
    case kernel::GuestSchedulerCaptureWaitKind::kSocketIo:
      if (wait.handle_count != 1) {
        return Fail(error, "scheduler external wait has no unique handle");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kNone:
    default:
      return Fail(error, "scheduler capture wait kind is invalid");
  }

  if ((wait_kind == kernel::GuestSchedulerCaptureWaitKind::kDelay ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kFence ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kIoOffload ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kIoCompletion ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kSocketIo) &&
      has_signal_epochs) {
    return Fail(error, "scheduler non-object wait carries signal epochs");
  }
  if ((wait_kind == kernel::GuestSchedulerCaptureWaitKind::kFence ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kIoOffload ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff) &&
      wait.deadline_ms) {
    return Fail(error, "scheduler untimed wait carries a deadline");
  }
  if (wait_kind == kernel::GuestSchedulerCaptureWaitKind::kDelay &&
      !wait.deadline_ms) {
    return Fail(error, "scheduler delay wait has no deadline");
  }
  const bool gated = wait.flags & kernel::kGuestSchedulerCaptureWaitFlagGated;
  const bool alertable =
      wait.flags & kernel::kGuestSchedulerCaptureWaitFlagAlertable;
  const bool interruptible =
      wait.flags & kernel::kGuestSchedulerCaptureWaitFlagInterruptible;
  if ((wait_kind == kernel::GuestSchedulerCaptureWaitKind::kFence ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kIoOffload ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kIoCompletion ||
       wait_kind == kernel::GuestSchedulerCaptureWaitKind::kSocketIo) &&
      gated) {
    return Fail(error, "scheduler polling wait is incorrectly gated");
  }
  switch (wait_kind) {
    case kernel::GuestSchedulerCaptureWaitKind::kSingle:
    case kernel::GuestSchedulerCaptureWaitKind::kMultiAny:
    case kernel::GuestSchedulerCaptureWaitKind::kMultiAll:
      if (!interruptible) {
        return Fail(error,
                    "scheduler object wait is incorrectly non-interruptible");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kDelay:
      if (!interruptible || gated == alertable) {
        return Fail(error, "scheduler delay wait flags are impossible");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kFence:
    case kernel::GuestSchedulerCaptureWaitKind::kIoOffload:
      if (alertable || interruptible) {
        return Fail(error, "scheduler stack-owned wait flags are impossible");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff:
    case kernel::GuestSchedulerCaptureWaitKind::kIoCompletion:
    case kernel::GuestSchedulerCaptureWaitKind::kSocketIo:
      if (alertable || !interruptible) {
        return Fail(error, "scheduler polling wait flags are impossible");
      }
      break;
    case kernel::GuestSchedulerCaptureWaitKind::kNone:
    default:
      return Fail(error, "scheduler capture wait kind is invalid");
  }
  if (event.kind == CaptureKind::kBlock) {
    const uint8_t expected_wait_flags =
        ((event.flags & kernel::kGuestSchedulerCaptureFlagGated)
             ? kernel::kGuestSchedulerCaptureWaitFlagGated
             : 0) |
        ((event.flags & kernel::kGuestSchedulerCaptureFlagAlertable)
             ? kernel::kGuestSchedulerCaptureWaitFlagAlertable
             : 0) |
        ((event.flags & kernel::kGuestSchedulerCaptureFlagInterruptible)
             ? kernel::kGuestSchedulerCaptureWaitFlagInterruptible
             : 0);
    if ((wait.flags & ~kernel::kGuestSchedulerCaptureWaitFlagUserApcPending) !=
            expected_wait_flags ||
        bool(event.flags & kernel::kGuestSchedulerCaptureFlagHasDeadline) !=
            bool(wait.deadline_ms)) {
      return Fail(error, "scheduler block wait provenance is inconsistent");
    }
    return true;
  }

  switch (event.reason) {
    case CaptureReason::kPolled:
      if (gated) {
        return Fail(error, "scheduler polled wake names a gated wait");
      }
      break;
    case CaptureReason::kSignalEpoch:
      if (!gated || wait.wait_epoch == wait.observed_wait_epoch) {
        return Fail(error, "scheduler signal wake has no epoch transition");
      }
      break;
    case CaptureReason::kDeadline:
      if (!gated || !wait.deadline_ms ||
          wait.observed_uptime_ms < wait.deadline_ms) {
        return Fail(error, "scheduler deadline wake has no expired deadline");
      }
      break;
    case CaptureReason::kUserApc:
      if (!gated || !(wait.flags &
                      kernel::kGuestSchedulerCaptureWaitFlagUserApcPending)) {
        return Fail(error, "scheduler APC wake has no pending user APC");
      }
      break;
    case CaptureReason::kBackstop:
      if (!gated) {
        return Fail(error, "scheduler backstop wake names an ungated wait");
      }
      break;
    default:
      return Fail(error, "scheduler wake reason is invalid");
  }
  return true;
}

bool ValidateSchedulerEvent(const CaptureEvent& event, std::string* error) {
  if (!event.sequence || !event.capture_instance_id || !event.guest_thread_id ||
      !IsSupportedKind(event.kind) || !IsKnownReason(event.reason) ||
      !IsReasonAllowed(event.kind, event.reason) || event.priority > 31 ||
      (event.flags & ~kKnownCaptureFlags) ||
      (event.flags & ~AllowedFlags(event.kind)) ||
      (event.guest_instruction_delta &&
       !IsInstructionDrainBoundary(event.kind))) {
    return Fail(error, "scheduler capture event is unsupported or malformed");
  }
  const bool wait_event =
      event.kind == CaptureKind::kBlock || event.kind == CaptureKind::kReready;
  if ((!wait_event && !IsEmptyWaitState(event.wait)) ||
      (event.kind != CaptureKind::kSafepoint && event.guest_pc) ||
      (event.kind != CaptureKind::kSafepoint && event.count) ||
      (event.kind != CaptureKind::kSafepoint &&
       event.kind != CaptureKind::kBlock &&
       event.kind != CaptureKind::kReready &&
       event.kind != CaptureKind::kPriorityChange && event.value)) {
    return Fail(error, "scheduler capture event has unexpected provenance");
  }
  switch (event.kind) {
    case CaptureKind::kEnqueueReady:
      if ((event.cpu != -1 && !IsCpu(event.cpu)) || !IsCpu(event.target_cpu)) {
        return Fail(error, "scheduler enqueue CPU is invalid");
      }
      break;
    case CaptureKind::kReready:
      if (!IsCpu(event.cpu) || !IsCpu(event.target_cpu)) {
        return Fail(error, "scheduler reready CPU is invalid");
      }
      break;
    case CaptureKind::kMigrate:
      if (!IsCpu(event.cpu) || !IsCpu(event.target_cpu) ||
          event.cpu == event.target_cpu) {
        return Fail(error, "scheduler migration CPUs are invalid");
      }
      break;
    case CaptureKind::kPriorityChange:
      if ((event.cpu != -1 && !IsCpu(event.cpu)) || event.target_cpu != -1) {
        return Fail(error, "scheduler priority-change CPU is invalid");
      }
      break;
    default:
      if (!IsCpu(event.cpu) || event.target_cpu != -1) {
        return Fail(error, "scheduler capture CPU is invalid");
      }
      break;
  }
  if (event.kind == CaptureKind::kSafepoint) {
    if (!event.guest_pc || (event.guest_pc & 3) ||
        !(event.flags & kernel::kGuestSchedulerCaptureFlagSchedulerRequested)) {
      return Fail(error, "scheduler safepoint provenance is invalid");
    }
    switch (event.reason) {
      case CaptureReason::kDeferredLock:
        if (event.count) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      case CaptureReason::kDeferredIrql:
        if (event.value < 2 || event.count) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      case CaptureReason::kForcedIrql:
        if (event.value < 2 ||
            event.count <
                kernel::kGuestSchedulerCaptureForcedIrqlMinimumDeclines) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      case CaptureReason::kYielded:
        if (event.value >= 2) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      default:
        return Fail(error, "scheduler safepoint provenance is invalid");
    }
  }
  if (event.kind == CaptureKind::kPriorityChange && event.value > 31) {
    return Fail(error, "scheduler previous priority is invalid");
  }
  if (wait_event && !ValidateWaitState(event, error)) {
    return false;
  }
  return true;
}

bool EncodeSchedulerPayload(const CaptureEvent& event,
                            std::vector<uint8_t>* output, std::string* error) {
  if (!output || !ValidateSchedulerEvent(event, error)) {
    return false;
  }
  output->assign(
      GuestExecutionSessionCaptureSchedulerEventBridge::kSchedulerPayloadSize,
      0);
  std::copy(kSchedulerPayloadMagic.cbegin(), kSchedulerPayloadMagic.cend(),
            output->begin());
  WriteU32(output, 8,
           GuestExecutionSessionCaptureSchedulerEventBridge::
               kSchedulerPayloadVersion);
  WriteU32(output, 12, static_cast<uint32_t>(event.kind));
  WriteU64(output, 16, event.sequence);
  WriteU64(output, 24, event.capture_instance_id);
  WriteU32(output, 32, event.guest_thread_id);
  WriteU32(output, 36, event.count);
  WriteU16(output, 40, event.flags);
  (*output)[42] = static_cast<uint8_t>(event.reason);
  (*output)[43] = static_cast<uint8_t>(event.cpu);
  (*output)[44] = static_cast<uint8_t>(event.target_cpu);
  (*output)[45] = event.priority;
  (*output)[46] = event.value;
  WriteU32(output, 48, event.guest_pc);
  WriteU32(output, 52, event.wait.wait_epoch);
  WriteU32(output, 56, event.wait.observed_wait_epoch);
  (*output)[60] = event.wait.handle_count;
  (*output)[61] = event.wait.flags;
  WriteU64(output, 64, event.wait.deadline_ms);
  WriteU64(output, 72, event.wait.observed_uptime_ms);
  for (size_t index = 0;
       index < kernel::kGuestSchedulerCaptureMaximumWaitHandles; ++index) {
    WriteU32(output, 80 + index * 4, event.wait.handles[index]);
    WriteU32(output, 112 + index * 4, event.wait.signal_epochs_before[index]);
    WriteU32(output, 144 + index * 4, event.wait.signal_epochs_observed[index]);
  }
  return true;
}

GuestExecutionContinuousEventIdentity ContinuousIdentity(
    const GuestExecutionSessionParticipant& participant) {
  return {participant.ordinal, participant.guest_thread_id};
}

ppc::GuestPPCThreadCheckpointBinding CheckpointBinding(
    const ppc::GuestPPCThreadCheckpoint& checkpoint) {
  ppc::GuestPPCThreadCheckpointBinding binding;
  binding.participant_ordinal = checkpoint.participant_ordinal;
  binding.guest_thread_id = checkpoint.guest_thread_id;
  binding.resume_kind = checkpoint.resume_kind;
  binding.resume_pc = checkpoint.resume_pc;
  binding.owning_function_address = checkpoint.owning_function_address;
  binding.owning_function_end_address = checkpoint.owning_function_end_address;
  binding.outer_guest_return_address = checkpoint.outer_guest_return_address;
  binding.pending_external_event_sequence =
      checkpoint.pending_external_event_sequence;
  binding.pending_export_guest_address =
      checkpoint.pending_export_guest_address;
  return binding;
}

GuestExecutionSessionChunkReference ChunkReference(
    GuestExecutionSessionChunkKind kind, uint32_t ordinal,
    uint64_t first_sequence, uint64_t last_sequence, uint32_t record_count,
    const std::vector<uint8_t>& bytes) {
  GuestExecutionSessionChunkReference reference;
  reference.kind = kind;
  reference.ordinal = ordinal;
  reference.first_event_sequence = first_sequence;
  reference.last_event_sequence = last_sequence;
  reference.record_count = record_count;
  reference.encoded_size = bytes.size();
  reference.encoded_sha256 = GuestExecutionSessionCodec::HashBytes(bytes);
  return reference;
}

}  // namespace

const GuestExecutionSessionCaptureSchedulerEventBridge::Participant*
GuestExecutionSessionCaptureSchedulerEventBridge::FindParticipant(
    uint64_t capture_instance_id, uint32_t guest_thread_id) const {
  const auto it = std::lower_bound(
      participants_.cbegin(), participants_.cend(), capture_instance_id,
      [](const Participant& participant, uint64_t value) {
        return participant.identity.capture_instance_id < value;
      });
  if (it == participants_.cend() ||
      it->identity.capture_instance_id != capture_instance_id ||
      it->identity.guest_thread_id != guest_thread_id) {
    return nullptr;
  }
  return &*it;
}

bool GuestExecutionSessionCaptureSchedulerEventBridge::BeginSession(
    GuestExecutionSessionAssembler& assembler,
    const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
    std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
        participants,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (begun_ || rejected_ || participants.empty() || !checkpoint.generation ||
      checkpoint.rejection !=
          kernel::GuestSchedulerCheckpointBarrierRejection::kNone ||
      !checkpoint.active || !checkpoint.quiesced) {
    return Fail(error, "scheduler event bridge start boundary is invalid");
  }
  const GuestExecutionSessionAssemblerStatus status = assembler.status();
  if (status.state != AssemblerState::kIdle ||
      status.coverage_mode !=
          GuestExecutionReelCoverageMode::kContinuousInstructions ||
      status.participants.size() != participants.size()) {
    return Fail(error, "scheduler event bridge assembler roster is invalid");
  }
  try {
    std::vector<Participant> catalog;
    catalog.reserve(participants.size());
    for (const GuestExecutionCaptureThreadStateLifecycleEvent& seed :
         participants) {
      if (seed.state !=
              GuestExecutionCaptureThreadStateLifecycleState::kReady ||
          !seed.participant.capture_instance_id ||
          !seed.participant.guest_thread_id ||
          std::none_of(
              status.participants.cbegin(), status.participants.cend(),
              [&seed](const GuestExecutionSessionAssemblerParticipantStatus&
                          participant) {
                return participant.identity == seed.participant;
              })) {
        return Fail(error,
                    "scheduler event bridge participant roster is invalid");
      }
      for (const Participant& existing : catalog) {
        if (existing.identity.capture_instance_id ==
                seed.participant.capture_instance_id ||
            existing.identity.guest_thread_id ==
                seed.participant.guest_thread_id) {
          return Fail(error,
                      "scheduler event bridge participant is duplicated");
        }
      }
      catalog.push_back({seed.participant, 0});
    }
    std::sort(catalog.begin(), catalog.end(),
              [](const Participant& left, const Participant& right) {
                return left.identity.capture_instance_id <
                       right.identity.capture_instance_id;
              });
    for (size_t index = 0; index < catalog.size(); ++index) {
      catalog[index].ordinal = static_cast<uint32_t>(index);
    }
    participants_ = std::move(catalog);
  } catch (...) {
    return Fail(error, "scheduler event bridge could not allocate its roster");
  }
  start_checkpoint_generation_ = checkpoint.generation;
  begun_ = true;
  return true;
}

AssemblerAction
GuestExecutionSessionCaptureSchedulerEventBridge::OnSchedulerEvent(
    GuestExecutionSessionAssembler& assembler, const CaptureEvent& event,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!begun_ || sealed_ || finalized_ || rejected_) {
    rejected_ = true;
    Fail(error, "scheduler event bridge is not recording");
    return AssemblerAction::kReject;
  }
  if (!ValidateSchedulerEvent(event, error)) {
    rejected_ = true;
    return AssemblerAction::kReject;
  }
  const Participant* participant =
      FindParticipant(event.capture_instance_id, event.guest_thread_id);
  if (!participant) {
    rejected_ = true;
    Fail(error, "scheduler event bridge observed an unknown participant");
    return AssemblerAction::kReject;
  }
  if (scheduler_event_count_ &&
      (last_scheduler_sequence_ == std::numeric_limits<uint64_t>::max() ||
       event.sequence != last_scheduler_sequence_ + 1)) {
    rejected_ = true;
    Fail(error, "scheduler event bridge source sequence has a gap");
    return AssemblerAction::kReject;
  }
  if (event.guest_instruction_delta &&
      assembler.OnInstructionCoverage(participant->identity,
                                      event.guest_instruction_delta) !=
          AssemblerAction::kContinue) {
    rejected_ = true;
    const std::string message = assembler.status().message;
    Fail(error, message.empty()
                    ? "scheduler event bridge instruction coverage was "
                      "rejected"
                    : message);
    return AssemblerAction::kReject;
  }
  try {
    GuestExecutionSessionAssemblerExternalEvent canonical;
    canonical.kind = CanonicalKind(event.kind);
    canonical.disposition =
        GuestExecutionSessionEventDisposition::kReplayCaptured;
    canonical.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
    if (!EncodeSchedulerPayload(event, &canonical.payload, error)) {
      rejected_ = true;
      return AssemblerAction::kReject;
    }
    const std::optional<GuestExecutionCaptureParticipantIdentity> actor =
        HasParticipantActor(event.kind)
            ? std::optional<GuestExecutionCaptureParticipantIdentity>(
                  participant->identity)
            : std::nullopt;
    const AssemblerAction action =
        assembler.OnExternalEvent(actor, std::move(canonical));
    if (action != AssemblerAction::kContinue) {
      rejected_ = true;
      const std::string message = assembler.status().message;
      Fail(error, message.empty()
                      ? "scheduler event bridge canonical event was rejected"
                      : message);
      return AssemblerAction::kReject;
    }
  } catch (...) {
    rejected_ = true;
    Fail(error, "scheduler event bridge could not allocate an event payload");
    return AssemblerAction::kReject;
  }
  if (!scheduler_event_count_) {
    first_scheduler_sequence_ = event.sequence;
  }
  last_scheduler_sequence_ = event.sequence;
  ++scheduler_event_count_;
  return AssemblerAction::kContinue;
}

bool GuestExecutionSessionCaptureSchedulerEventBridge::SealSession(
    GuestExecutionSessionAssembler& assembler,
    const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!begun_ || sealed_ || finalized_ || rejected_ || !checkpoint.generation ||
      checkpoint.generation == start_checkpoint_generation_ ||
      checkpoint.rejection !=
          kernel::GuestSchedulerCheckpointBarrierRejection::kNone ||
      !checkpoint.active || !checkpoint.quiesced ||
      assembler.status().state != AssemblerState::kPublishing) {
    return Fail(error, "scheduler event bridge stop boundary is invalid");
  }
  sealed_ = true;
  return true;
}

bool GuestExecutionSessionCaptureSchedulerEventBridge::FinalizeBundle(
    GuestExecutionSessionBundle* bundle, uint64_t scheduler_event_count,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!begun_ || !sealed_ || finalized_ || rejected_ || !bundle ||
      scheduler_event_count != scheduler_event_count_ ||
      bundle->manifest.participants.empty() ||
      bundle->manifest.participants.size() != participants_.size() ||
      bundle->chunks.size() != bundle->manifest.chunks.size() ||
      bundle->chunks.empty() ||
      bundle->manifest.chunks.back().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint) {
    return Fail(error, "scheduler event bridge cannot close this bundle");
  }

  try {
    for (const Participant& participant : participants_) {
      if (participant.ordinal >= bundle->manifest.participants.size()) {
        return Fail(error,
                    "scheduler event bridge bundle roster is out of range");
      }
      const GuestExecutionSessionParticipant& durable =
          bundle->manifest.participants[participant.ordinal];
      if (durable.ordinal != participant.ordinal ||
          durable.capture_instance_id !=
              participant.identity.capture_instance_id ||
          durable.guest_thread_id != participant.identity.guest_thread_id) {
        return Fail(error, "scheduler event bridge bundle roster has changed");
      }
    }

    std::map<GuestExecutionSessionSha256, const std::vector<uint8_t>*>
        blob_catalog;
    for (const GuestExecutionSessionContentBlob& blob : bundle->content_blobs) {
      if (GuestExecutionSessionCodec::HashBytes(blob.bytes) != blob.sha256 ||
          !blob_catalog.emplace(blob.sha256, &blob.bytes).second) {
        return Fail(error, "scheduler event bridge content catalog is invalid");
      }
    }

    GuestExecutionSessionCheckpointChunk final_checkpoint;
    if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
            bundle->chunks.back(), &final_checkpoint, error) ||
        final_checkpoint.checkpoint.global_sequence !=
            bundle->manifest.last_event_sequence ||
        final_checkpoint.checkpoint.thread_states.size() !=
            bundle->manifest.participants.size()) {
      return Fail(error, "scheduler event bridge final checkpoint is invalid");
    }

    std::vector<GuestExecutionSessionEvent> canonical_events;
    if (bundle->manifest.accepted_event_count >
        std::numeric_limits<size_t>::max()) {
      return Fail(error, "scheduler event bridge event count is too large");
    }
    canonical_events.reserve(
        static_cast<size_t>(bundle->manifest.accepted_event_count));
    for (size_t index = 0; index + 1 < bundle->chunks.size(); ++index) {
      if (bundle->manifest.chunks[index].kind !=
          GuestExecutionSessionChunkKind::kEvents) {
        continue;
      }
      GuestExecutionSessionEventChunk chunk;
      if (!GuestExecutionSessionCodec::DecodeEventChunk(bundle->chunks[index],
                                                        &chunk, error)) {
        return false;
      }
      canonical_events.insert(canonical_events.end(), chunk.events.begin(),
                              chunk.events.end());
    }
    if (canonical_events.empty() ||
        canonical_events.front().global_sequence !=
            bundle->manifest.first_event_sequence ||
        canonical_events.back().global_sequence !=
            bundle->manifest.last_event_sequence) {
      return Fail(error, "scheduler event bridge canonical tape is incomplete");
    }

    std::vector<GuestExecutionContinuousEvent> overlay;
    overlay.reserve(canonical_events.size());
    uint64_t decoded_scheduler_count = 0;
    uint64_t decoded_first_scheduler_sequence = 0;
    uint64_t decoded_last_scheduler_sequence = 0;
    for (const GuestExecutionSessionEvent& canonical : canonical_events) {
      GuestExecutionContinuousEvent continuous;
      continuous.global_sequence = canonical.global_sequence;
      continuous.kind = canonical.kind;
      if (canonical.thread_ordinal != kGuestExecutionSessionNoThread) {
        if (canonical.thread_ordinal >= bundle->manifest.participants.size()) {
          return Fail(error,
                      "scheduler event bridge canonical actor is invalid");
        }
        continuous.actor = ContinuousIdentity(
            bundle->manifest.participants[canonical.thread_ordinal]);
      }

      const bool scheduler_kind =
          canonical.kind == GuestExecutionSessionEventKind::kThreadDispatch ||
          canonical.kind == GuestExecutionSessionEventKind::kSynchronization;
      if (scheduler_kind) {
        if (canonical.disposition !=
                GuestExecutionSessionEventDisposition::kReplayCaptured ||
            canonical.payload_kind !=
                GuestExecutionSessionPayloadKind::kGuestBytes ||
            (canonical.payload_size != kSchedulerPayloadSize &&
             canonical.payload_size != kSchedulerPayloadV1Size)) {
          return Fail(error,
                      "scheduler event bridge canonical payload is invalid");
        }
        const auto blob = blob_catalog.find(canonical.payload_sha256);
        if (blob == blob_catalog.end()) {
          return Fail(error, "scheduler event bridge payload blob is missing");
        }
        CaptureEvent decoded;
        if (!DecodeSchedulerEventPayload(*blob->second, &decoded, error) ||
            CanonicalKind(decoded.kind) != canonical.kind) {
          return false;
        }
        const Participant* participant = FindParticipant(
            decoded.capture_instance_id, decoded.guest_thread_id);
        if (!participant ||
            (decoded_scheduler_count &&
             (decoded_last_scheduler_sequence ==
                  std::numeric_limits<uint64_t>::max() ||
              decoded.sequence != decoded_last_scheduler_sequence + 1))) {
          return Fail(error,
                      "scheduler event bridge decoded sequence is invalid");
        }
        const uint32_t expected_actor = HasParticipantActor(decoded.kind)
                                            ? participant->ordinal
                                            : kGuestExecutionSessionNoThread;
        if (canonical.thread_ordinal != expected_actor) {
          return Fail(error,
                      "scheduler event bridge canonical actor is incorrect");
        }
        continuous.subject = ContinuousIdentity(
            bundle->manifest.participants[participant->ordinal]);
        if (!decoded_scheduler_count) {
          decoded_first_scheduler_sequence = decoded.sequence;
        }
        decoded_last_scheduler_sequence = decoded.sequence;
        ++decoded_scheduler_count;
      }
      overlay.push_back(std::move(continuous));
    }
    if (decoded_scheduler_count != scheduler_event_count_ ||
        decoded_first_scheduler_sequence != first_scheduler_sequence_ ||
        decoded_last_scheduler_sequence != last_scheduler_sequence_) {
      return Fail(error,
                  "scheduler event bridge source tape does not match bundle");
    }

    if (overlay.back().kind != GuestExecutionSessionEventKind::kBoundaryHeld) {
      return Fail(error,
                  "scheduler event bridge final event is not boundary-held");
    }
    for (const GuestExecutionSessionParticipant& checkpoint_subject :
         bundle->manifest.participants) {
      if (checkpoint_subject.ordinal >=
              final_checkpoint.checkpoint.thread_states.size() ||
          checkpoint_subject.held_after_event_sequence <
              bundle->manifest.first_event_sequence ||
          checkpoint_subject.held_after_event_sequence >
              bundle->manifest.last_event_sequence) {
        return Fail(error, "scheduler event bridge boundary route is invalid");
      }
      const uint64_t arrival_index =
          checkpoint_subject.held_after_event_sequence -
          bundle->manifest.first_event_sequence;
      if (arrival_index >= overlay.size()) {
        return Fail(error, "scheduler event bridge boundary route is missing");
      }
      GuestExecutionContinuousEvent& arrival = overlay[arrival_index];
      GuestExecutionSessionEventKind expected_arrival_kind;
      switch (checkpoint_subject.boundary_arrival_kind) {
        case GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside:
          expected_arrival_kind =
              GuestExecutionSessionEventKind::kBoundaryRequest;
          break;
        case GuestExecutionSessionBoundaryArrivalKind::kOuterHostCallReturn:
          expected_arrival_kind =
              GuestExecutionSessionEventKind::kOuterHostCallEnd;
          break;
        case GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint:
          expected_arrival_kind =
              GuestExecutionSessionEventKind::kJitSafepointArrival;
          break;
        default:
          return Fail(error, "scheduler event bridge boundary kind is invalid");
      }
      if (arrival.kind != expected_arrival_kind ||
          arrival.checkpoint.kind !=
              GuestExecutionContinuousCheckpointReferenceKind::kNone) {
        return Fail(error,
                    "scheduler event bridge boundary route is not unique");
      }

      const GuestExecutionSessionThreadStateReference& final_state =
          final_checkpoint.checkpoint.thread_states[checkpoint_subject.ordinal];
      if (final_state.thread_ordinal != checkpoint_subject.ordinal) {
        return Fail(error,
                    "scheduler event bridge final state order is invalid");
      }
      const auto final_blob = blob_catalog.find(final_state.sha256);
      if (final_blob == blob_catalog.end() ||
          final_blob->second->size() != final_state.byte_size) {
        return Fail(error,
                    "scheduler event bridge final state blob is missing");
      }
      ppc::GuestPPCThreadCheckpoint decoded_checkpoint;
      if (!ppc::GuestPPCThreadCheckpointCodec::Decode(
              *final_blob->second, &decoded_checkpoint, error)) {
        return false;
      }
      if (decoded_checkpoint.participant_ordinal !=
              checkpoint_subject.ordinal ||
          decoded_checkpoint.guest_thread_id !=
              checkpoint_subject.guest_thread_id) {
        return Fail(error,
                    "scheduler event bridge final state identity is invalid");
      }
      arrival.subject = ContinuousIdentity(checkpoint_subject);
      arrival.checkpoint.kind =
          GuestExecutionContinuousCheckpointReferenceKind::kThreadState;
      arrival.checkpoint.checkpoint_global_sequence =
          final_checkpoint.checkpoint.global_sequence;
      arrival.checkpoint.state_size = final_state.byte_size;
      arrival.checkpoint.state_sha256 = final_state.sha256;
      arrival.checkpoint.binding = CheckpointBinding(decoded_checkpoint);
    }

    GuestExecutionContinuousEventLimits overlay_limits;
    const uint64_t maximum_records_by_size =
        (overlay_limits.maximum_encoded_bytes -
         GuestExecutionContinuousEventCodec::kHeaderSize) /
        GuestExecutionContinuousEventCodec::kRecordSize;
    const uint64_t records_per_chunk =
        std::min(overlay_limits.maximum_records, maximum_records_by_size);
    if (!records_per_chunk) {
      return Fail(error, "scheduler event bridge overlay limits are invalid");
    }
    std::vector<std::vector<uint8_t>> overlay_chunks;
    std::vector<GuestExecutionSessionChunkReference> overlay_references;
    const size_t final_index = bundle->chunks.size() - 1;
    for (size_t begin = 0; begin < overlay.size();) {
      const size_t remaining = overlay.size() - begin;
      const size_t count =
          static_cast<size_t>(std::min<uint64_t>(remaining, records_per_chunk));
      std::vector<GuestExecutionContinuousEvent> records(
          overlay.begin() + begin, overlay.begin() + begin + count);
      std::vector<uint8_t> encoded;
      if (!GuestExecutionContinuousEventCodec::Encode(records, &encoded,
                                                      error)) {
        return false;
      }
      const uint64_t ordinal = final_index + overlay_chunks.size();
      if (ordinal > std::numeric_limits<uint32_t>::max()) {
        return Fail(error, "scheduler event bridge overlay ordinal overflows");
      }
      overlay_references.push_back(ChunkReference(
          GuestExecutionSessionChunkKind::kContinuousEvents,
          static_cast<uint32_t>(ordinal), records.front().global_sequence,
          records.back().global_sequence, static_cast<uint32_t>(records.size()),
          encoded));
      overlay_chunks.push_back(std::move(encoded));
      begin += count;
    }

    const uint64_t final_ordinal = final_index + overlay_chunks.size();
    if (final_ordinal > std::numeric_limits<uint32_t>::max()) {
      return Fail(error, "scheduler event bridge final ordinal overflows");
    }
    final_checkpoint.ordinal = static_cast<uint32_t>(final_ordinal);
    std::vector<uint8_t> final_bytes;
    if (!GuestExecutionSessionCodec::EncodeCheckpointChunk(
            final_checkpoint, &final_bytes, error)) {
      return false;
    }
    GuestExecutionSessionChunkReference final_reference = ChunkReference(
        GuestExecutionSessionChunkKind::kCheckpoint, final_checkpoint.ordinal,
        final_checkpoint.checkpoint.global_sequence,
        final_checkpoint.checkpoint.global_sequence, 1, final_bytes);

    bundle->chunks.back() = std::move(final_bytes);
    bundle->chunks.insert(bundle->chunks.end() - 1,
                          std::make_move_iterator(overlay_chunks.begin()),
                          std::make_move_iterator(overlay_chunks.end()));
    bundle->manifest.chunks.back() = std::move(final_reference);
    bundle->manifest.chunks.insert(
        bundle->manifest.chunks.end() - 1,
        std::make_move_iterator(overlay_references.begin()),
        std::make_move_iterator(overlay_references.end()));
  } catch (...) {
    return Fail(error, "scheduler event bridge could not allocate the overlay");
  }
  finalized_ = true;
  return true;
}

bool GuestExecutionSessionCaptureSchedulerEventBridge::
    DecodeSchedulerEventPayload(std::span<const uint8_t> payload,
                                CaptureEvent* event,
                                std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!event || payload.size() < 12 ||
      !std::equal(kSchedulerPayloadMagic.cbegin(),
                  kSchedulerPayloadMagic.cend(), payload.begin())) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  const uint32_t version = ReadU32(payload, 8);
  if (version == 1) {
    if (payload.size() != kSchedulerPayloadV1Size || payload[47]) {
      return Fail(error, "scheduler event payload envelope is invalid");
    }
    return Fail(error,
                "scheduler event payload version 1 is not deterministic-"
                "replayable");
  }
  if (version != kSchedulerPayloadVersion ||
      payload.size() != kSchedulerPayloadSize || payload[47] || payload[62] ||
      payload[63] ||
      std::any_of(payload.begin() + 176, payload.end(),
                  [](uint8_t value) { return value != 0; })) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  const uint32_t raw_kind = ReadU32(payload, 12);
  if (raw_kind > std::numeric_limits<uint8_t>::max()) {
    return Fail(error, "scheduler event payload kind is out of range");
  }
  CaptureEvent decoded;
  decoded.kind = static_cast<CaptureKind>(raw_kind);
  decoded.sequence = ReadU64(payload, 16);
  decoded.capture_instance_id = ReadU64(payload, 24);
  decoded.guest_thread_id = ReadU32(payload, 32);
  decoded.count = ReadU32(payload, 36);
  decoded.flags = ReadU16(payload, 40);
  decoded.reason = static_cast<CaptureReason>(payload[42]);
  decoded.cpu = static_cast<int8_t>(payload[43]);
  decoded.target_cpu = static_cast<int8_t>(payload[44]);
  decoded.priority = payload[45];
  decoded.value = payload[46];
  decoded.guest_pc = ReadU32(payload, 48);
  decoded.wait.wait_epoch = ReadU32(payload, 52);
  decoded.wait.observed_wait_epoch = ReadU32(payload, 56);
  decoded.wait.handle_count = payload[60];
  decoded.wait.flags = payload[61];
  decoded.wait.deadline_ms = ReadU64(payload, 64);
  decoded.wait.observed_uptime_ms = ReadU64(payload, 72);
  for (size_t index = 0;
       index < kernel::kGuestSchedulerCaptureMaximumWaitHandles; ++index) {
    decoded.wait.handles[index] = ReadU32(payload, 80 + index * 4);
    decoded.wait.signal_epochs_before[index] =
        ReadU32(payload, 112 + index * 4);
    decoded.wait.signal_epochs_observed[index] =
        ReadU32(payload, 144 + index * 4);
  }
  if (!ValidateSchedulerEvent(decoded, error)) {
    return false;
  }
  *event = decoded;
  return true;
}

}  // namespace cpu
}  // namespace xe

#endif
