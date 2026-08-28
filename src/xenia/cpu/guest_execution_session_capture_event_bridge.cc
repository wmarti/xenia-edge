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
#include "xenia/cpu/guest_scheduler_record.h"

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

// The session codec and the ungated record codec both read this payload;
// these anchor the envelope values all three agree on.
static_assert(GuestExecutionSessionCaptureSchedulerEventBridge::
                  kSchedulerPayloadVersion ==
              GuestExecutionSessionCodec::kSchedulerEventPayloadVersion);
static_assert(
    GuestExecutionSessionCaptureSchedulerEventBridge::kSchedulerPayloadSize ==
    GuestExecutionSessionCodec::kSchedulerEventPayloadSize);
static_assert(GuestExecutionSessionCaptureSchedulerEventBridge::
                  kSchedulerPayloadVersion ==
              GuestSchedulerRecordCodec::kPayloadVersion);
static_assert(
    GuestExecutionSessionCaptureSchedulerEventBridge::kSchedulerPayloadV1Size ==
    GuestSchedulerRecordCodec::kPayloadV1Size);
static_assert(
    GuestExecutionSessionCaptureSchedulerEventBridge::kSchedulerPayloadSize ==
    GuestSchedulerRecordCodec::kPayloadSize);
static_assert(static_cast<uint32_t>(CaptureKind::kEnqueueReady) == 1 &&
              static_cast<uint32_t>(CaptureKind::kDequeueReady) == 2 &&
              static_cast<uint32_t>(CaptureKind::kDispatch) == 3 &&
              static_cast<uint32_t>(CaptureKind::kSwitchOut) == 4 &&
              static_cast<uint32_t>(CaptureKind::kYield) == 5 &&
              static_cast<uint32_t>(CaptureKind::kPreemptRequest) == 6 &&
              static_cast<uint32_t>(CaptureKind::kSafepoint) == 7 &&
              static_cast<uint32_t>(CaptureKind::kBlock) == 8 &&
              static_cast<uint32_t>(CaptureKind::kReready) == 9 &&
              static_cast<uint32_t>(CaptureKind::kParkSuspended) == 10 &&
              static_cast<uint32_t>(CaptureKind::kResume) == 11 &&
              static_cast<uint32_t>(CaptureKind::kPriorityChange) == 12 &&
              static_cast<uint32_t>(CaptureKind::kMigrate) == 13);

// The session validator decodes these durable identifiers without the kernel
// enumerations, so the two spellings are pinned to each other here.
static_assert(
    static_cast<uint32_t>(CaptureKind::kBlock) ==
        static_cast<uint32_t>(GuestExecutionSessionSchedulerEventKind::kBlock),
    "durable scheduler block identifier drifted");
static_assert(static_cast<uint32_t>(CaptureKind::kReready) ==
                  static_cast<uint32_t>(
                      GuestExecutionSessionSchedulerEventKind::kReready),
              "durable scheduler reready identifier drifted");
static_assert(static_cast<uint32_t>(CaptureReason::kPolled) ==
                  static_cast<uint32_t>(
                      GuestExecutionSessionSchedulerEventReason::kPolled),
              "durable scheduler polled reason drifted");
static_assert(static_cast<uint32_t>(CaptureReason::kSignalEpoch) ==
                  static_cast<uint32_t>(
                      GuestExecutionSessionSchedulerEventReason::kSignalEpoch),
              "durable scheduler signal-epoch reason drifted");
static_assert(static_cast<uint32_t>(CaptureReason::kDeadline) ==
                  static_cast<uint32_t>(
                      GuestExecutionSessionSchedulerEventReason::kDeadline),
              "durable scheduler deadline reason drifted");
static_assert(static_cast<uint32_t>(CaptureReason::kUserApc) ==
                  static_cast<uint32_t>(
                      GuestExecutionSessionSchedulerEventReason::kUserApc),
              "durable scheduler user-APC reason drifted");
static_assert(static_cast<uint32_t>(CaptureReason::kBackstop) ==
                  static_cast<uint32_t>(
                      GuestExecutionSessionSchedulerEventReason::kBackstop),
              "durable scheduler backstop reason drifted");
static_assert(GuestExecutionSessionCaptureSchedulerEventBridge::
                      kSchedulerPayloadVersion == 2,
              "durable scheduler payload version drifted");
static_assert(kGuestExecutionSessionSchedulerMaximumWaitHandles ==
              kernel::kGuestSchedulerCaptureMaximumWaitHandles);
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kNone) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kNone));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kSingle) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSingle));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kMultiAny) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kMultiAny));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kMultiAll) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kMultiAll));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kDelay) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kDelay));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kFence) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kFence));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kIoOffload) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kIoOffload));
static_assert(
    static_cast<uint32_t>(
        GuestExecutionSessionSchedulerWaitKind::kSpinBackoff) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSpinBackoff));
static_assert(
    static_cast<uint32_t>(
        GuestExecutionSessionSchedulerWaitKind::kIoCompletion) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kIoCompletion));
static_assert(
    static_cast<uint32_t>(GuestExecutionSessionSchedulerWaitKind::kSocketIo) ==
    static_cast<uint8_t>(kernel::GuestSchedulerCaptureWaitKind::kSocketIo));
static_assert(
    static_cast<uint32_t>(kGuestExecutionSessionSchedulerWaitFlagGated) ==
    static_cast<uint8_t>(kernel::kGuestSchedulerCaptureWaitFlagGated));
static_assert(
    static_cast<uint32_t>(kGuestExecutionSessionSchedulerWaitFlagAlertable) ==
    static_cast<uint8_t>(kernel::kGuestSchedulerCaptureWaitFlagAlertable));
static_assert(
    static_cast<uint32_t>(
        kGuestExecutionSessionSchedulerWaitFlagInterruptible) ==
    static_cast<uint8_t>(kernel::kGuestSchedulerCaptureWaitFlagInterruptible));
static_assert(
    static_cast<uint32_t>(
        kGuestExecutionSessionSchedulerWaitFlagUserApcPending) ==
    static_cast<uint8_t>(kernel::kGuestSchedulerCaptureWaitFlagUserApcPending));

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

DecodedSchedulerRecord SchedulerRecordOf(const CaptureEvent& event) {
  DecodedSchedulerRecord record;
  record.sequence = event.sequence;
  record.capture_instance_id = event.capture_instance_id;
  record.guest_thread_id = event.guest_thread_id;
  record.count = event.count;
  record.guest_pc = event.guest_pc;
  record.flags = event.flags;
  record.kind = event.kind;
  record.reason = event.reason;
  record.cpu = event.cpu;
  record.target_cpu = event.target_cpu;
  record.priority = event.priority;
  record.value = event.value;
  record.wait = event.wait;
  return record;
}

CaptureEvent SchedulerEventOf(const DecodedSchedulerRecord& record) {
  CaptureEvent event;
  event.sequence = record.sequence;
  event.capture_instance_id = record.capture_instance_id;
  event.guest_thread_id = record.guest_thread_id;
  event.count = record.count;
  event.guest_pc = record.guest_pc;
  event.flags = record.flags;
  event.kind = record.kind;
  event.reason = record.reason;
  event.cpu = record.cpu;
  event.target_cpu = record.target_cpu;
  event.priority = record.priority;
  event.value = record.value;
  event.wait = record.wait;
  return event;
}

bool ValidateSchedulerEvent(const CaptureEvent& event, std::string* error) {
  // The instruction delta is capture transport only and never reaches the
  // payload, so the record validator cannot see it.
  if (event.guest_instruction_delta &&
      !IsInstructionDrainBoundary(event.kind)) {
    return Fail(error, "scheduler capture event is unsupported or malformed");
  }
  return GuestSchedulerRecordCodec::Validate(SchedulerRecordOf(event), error);
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

bool GuestExecutionSessionCaptureSchedulerEventBridge::BuildSchedulerTopology(
    const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
    GuestExecutionSessionSchedulerTopologyBoundary boundary,
    GuestExecutionSessionSchedulerTopologyChunk* output,
    std::string* error) const {
  if (!output || participants_.empty() || !checkpoint.generation ||
      checkpoint.rejection !=
          kernel::GuestSchedulerCheckpointBarrierRejection::kNone ||
      !checkpoint.active || !checkpoint.quiesced ||
      checkpoint.roster_scope !=
          kernel::GuestSchedulerCheckpointRosterScope::kSchedulerOwned ||
      checkpoint.release_policy !=
          kernel::GuestSchedulerCheckpointReleasePolicy::
              kRunningSafepointsRequeueAtHead) {
    return Fail(error, "scheduler topology checkpoint is invalid");
  }

  GuestExecutionSessionSchedulerTopologyChunk topology;
  topology.boundary = boundary;
  topology.global_sequence =
      boundary == GuestExecutionSessionSchedulerTopologyBoundary::kStart ? 0
                                                                         : 1;
  try {
    topology.participants.resize(participants_.size());
    std::vector<bool> seen(participants_.size());
    for (const Participant& participant : participants_) {
      auto& durable = topology.participants[participant.ordinal];
      durable.ordinal = participant.ordinal;
      durable.guest_thread_id = participant.identity.guest_thread_id;
      durable.capture_instance_id = participant.identity.capture_instance_id;
    }

    for (const auto& participant : checkpoint.participants) {
      const Participant* durable_identity = FindParticipant(
          participant.capture_instance_id, participant.thread_id);
      if (!durable_identity || seen[durable_identity->ordinal] ||
          participant.cpu < 0 || participant.cpu >= 6 ||
          participant.effective_priority >= 32 ||
          participant.base_priority >= 32) {
        return Fail(error,
                    "scheduler topology snapshot participant is invalid");
      }
      seen[durable_identity->ordinal] = true;
      auto& durable = topology.participants[durable_identity->ordinal];
      durable.cpu = static_cast<uint32_t>(participant.cpu);
      durable.effective_priority = participant.effective_priority;
      durable.base_priority = participant.base_priority;
      durable.suspension_count = participant.suspension_count;
      durable.quantum_remaining_us = participant.quantum_remaining_us;
      durable.guest_pc = participant.guest_pc;
      durable.restorable = participant.restorable;
      switch (participant.resume_kind) {
        case kernel::GuestSchedulerCheckpointResumeKind::kJitSafepoint:
          durable.resume_kind =
              GuestExecutionSessionSchedulerResumeKind::kJitSafepoint;
          break;
        case kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation:
          durable.resume_kind =
              GuestExecutionSessionSchedulerResumeKind::kNativeContinuation;
          break;
        case kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport:
          durable.resume_kind =
              GuestExecutionSessionSchedulerResumeKind::kAfterBlockingExport;
          break;
        case kernel::GuestSchedulerCheckpointResumeKind::kNotYetRun:
          durable.resume_kind =
              GuestExecutionSessionSchedulerResumeKind::kNotYetRun;
          break;
        default:
          return Fail(error, "scheduler topology resume kind is unknown");
      }
      switch (participant.state) {
        case kernel::GuestSchedulerCheckpointParticipantState::kRunning:
          durable.state =
              GuestExecutionSessionSchedulerParticipantState::kRunning;
          break;
        case kernel::GuestSchedulerCheckpointParticipantState::kReady:
          if (participant.ready_queue_level < 0 ||
              participant.ready_queue_level >= 32 ||
              participant.ready_queue_level != participant.effective_priority ||
              participant.ready_queue_fifo_ordinal == UINT32_MAX) {
            return Fail(error, "scheduler ready topology snapshot is invalid");
          }
          durable.state =
              GuestExecutionSessionSchedulerParticipantState::kReady;
          durable.ready_queue_level =
              static_cast<uint32_t>(participant.ready_queue_level);
          durable.ready_queue_fifo_ordinal =
              participant.ready_queue_fifo_ordinal;
          break;
        case kernel::GuestSchedulerCheckpointParticipantState::kBlocked:
          durable.state =
              GuestExecutionSessionSchedulerParticipantState::kBlocked;
          durable.blocked_wait.kind =
              static_cast<GuestExecutionSessionSchedulerWaitKind>(
                  participant.blocked_wait_kind);
          durable.blocked_wait.deadline_ms =
              participant.blocked_wait.deadline_ms;
          durable.blocked_wait.observed_uptime_ms =
              participant.blocked_wait.observed_uptime_ms;
          durable.blocked_wait.wait_epoch = participant.blocked_wait.wait_epoch;
          durable.blocked_wait.observed_wait_epoch =
              participant.blocked_wait.observed_wait_epoch;
          durable.blocked_wait.handle_count =
              participant.blocked_wait.handle_count;
          durable.blocked_wait.flags = participant.blocked_wait.flags;
          std::copy(participant.blocked_wait.handles.cbegin(),
                    participant.blocked_wait.handles.cend(),
                    durable.blocked_wait.handles.begin());
          std::copy(participant.blocked_wait.signal_epochs_before.cbegin(),
                    participant.blocked_wait.signal_epochs_before.cend(),
                    durable.blocked_wait.signal_epochs_before.begin());
          std::copy(participant.blocked_wait.signal_epochs_observed.cbegin(),
                    participant.blocked_wait.signal_epochs_observed.cend(),
                    durable.blocked_wait.signal_epochs_observed.begin());
          break;
        case kernel::GuestSchedulerCheckpointParticipantState::kSuspended:
          if (!participant.suspension_count) {
            return Fail(error, "scheduler suspended topology count is invalid");
          }
          durable.state =
              GuestExecutionSessionSchedulerParticipantState::kSuspended;
          break;
        default:
          return Fail(error, "scheduler topology snapshot state is unknown");
      }
    }
  } catch (...) {
    return Fail(error, "scheduler topology snapshot could not be allocated");
  }
  *output = std::move(topology);
  return true;
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
  if (!BuildSchedulerTopology(
          checkpoint, GuestExecutionSessionSchedulerTopologyBoundary::kStart,
          &start_scheduler_topology_, error)) {
    rejected_ = true;
    return false;
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
  if (!BuildSchedulerTopology(
          checkpoint, GuestExecutionSessionSchedulerTopologyBoundary::kFinal,
          &final_scheduler_topology_, error)) {
    rejected_ = true;
    return false;
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
          checkpoint_subject.ordinal >=
              final_scheduler_topology_.participants.size() ||
          checkpoint_subject.held_after_event_sequence <
              bundle->manifest.first_event_sequence ||
          checkpoint_subject.held_after_event_sequence >
              bundle->manifest.last_event_sequence) {
        return Fail(error, "scheduler event bridge boundary route is invalid");
      }
      const GuestExecutionSessionThreadStateReference& boundary_state =
          final_checkpoint.checkpoint.thread_states[checkpoint_subject.ordinal];
      const auto boundary_blob = blob_catalog.find(boundary_state.sha256);
      ppc::GuestPPCThreadCheckpoint boundary_checkpoint;
      if (boundary_state.thread_ordinal != checkpoint_subject.ordinal ||
          boundary_blob == blob_catalog.end() ||
          boundary_blob->second->size() != boundary_state.byte_size ||
          !ppc::GuestPPCThreadCheckpointCodec::Decode(
              *boundary_blob->second, &boundary_checkpoint, error) ||
          boundary_checkpoint.participant_ordinal !=
              checkpoint_subject.ordinal ||
          boundary_checkpoint.guest_thread_id !=
              checkpoint_subject.guest_thread_id) {
        return Fail(error,
                    "scheduler event bridge final state identity is invalid");
      }
      const bool scheduler_unowned =
          final_scheduler_topology_.participants[checkpoint_subject.ordinal]
              .state ==
          GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned;
      const GuestExecutionSessionSchedulerTopologyParticipant&
          topology_participant = final_scheduler_topology_
                                     .participants[checkpoint_subject.ordinal];
      if (boundary_checkpoint.resume_kind ==
          ppc::GuestPPCThreadResumeKind::kOutsideGuest) {
        const bool passive_scheduler_owned =
            !scheduler_unowned && !topology_participant.restorable &&
            (topology_participant.resume_kind ==
                 GuestExecutionSessionSchedulerResumeKind::
                     kNativeContinuation ||
             topology_participant.resume_kind ==
                 GuestExecutionSessionSchedulerResumeKind::kNotYetRun);
        if ((!scheduler_unowned && !passive_scheduler_owned) ||
            checkpoint_subject.boundary_arrival_kind !=
                GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside) {
          return Fail(error, "outside-guest boundary has an executable route");
        }
        continue;
      }
      if (scheduler_unowned) {
        return Fail(error,
                    "scheduler-unowned boundary has an executable route");
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

    const uint64_t start_topology_ordinal = final_index + overlay_chunks.size();
    const uint64_t final_topology_ordinal = start_topology_ordinal + 1;
    const uint64_t final_ordinal = final_topology_ordinal + 1;
    if (final_ordinal > std::numeric_limits<uint32_t>::max()) {
      return Fail(error, "scheduler event bridge final ordinal overflows");
    }
    start_scheduler_topology_.session_epoch = bundle->manifest.session_epoch;
    start_scheduler_topology_.ordinal =
        static_cast<uint32_t>(start_topology_ordinal);
    start_scheduler_topology_.global_sequence = 0;
    final_scheduler_topology_.session_epoch = bundle->manifest.session_epoch;
    final_scheduler_topology_.ordinal =
        static_cast<uint32_t>(final_topology_ordinal);
    final_scheduler_topology_.global_sequence =
        bundle->manifest.last_event_sequence;
    std::vector<uint8_t> start_topology_bytes;
    std::vector<uint8_t> final_topology_bytes;
    if (!GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
            start_scheduler_topology_, &start_topology_bytes, error) ||
        !GuestExecutionSessionCodec::EncodeSchedulerTopologyChunk(
            final_scheduler_topology_, &final_topology_bytes, error)) {
      return false;
    }
    const GuestExecutionSessionChunkReference start_topology_reference =
        ChunkReference(GuestExecutionSessionChunkKind::kSchedulerTopology,
                       start_scheduler_topology_.ordinal, 0, 0,
                       static_cast<uint32_t>(
                           start_scheduler_topology_.participants.size()),
                       start_topology_bytes);
    const GuestExecutionSessionChunkReference final_topology_reference =
        ChunkReference(GuestExecutionSessionChunkKind::kSchedulerTopology,
                       final_scheduler_topology_.ordinal,
                       bundle->manifest.last_event_sequence,
                       bundle->manifest.last_event_sequence,
                       static_cast<uint32_t>(
                           final_scheduler_topology_.participants.size()),
                       final_topology_bytes);
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
    bundle->chunks.insert(bundle->chunks.end() - 1,
                          std::move(start_topology_bytes));
    bundle->chunks.insert(bundle->chunks.end() - 1,
                          std::move(final_topology_bytes));
    bundle->manifest.chunks.back() = std::move(final_reference);
    bundle->manifest.chunks.insert(
        bundle->manifest.chunks.end() - 1,
        std::make_move_iterator(overlay_references.begin()),
        std::make_move_iterator(overlay_references.end()));
    bundle->manifest.chunks.insert(bundle->manifest.chunks.end() - 1,
                                   start_topology_reference);
    bundle->manifest.chunks.insert(bundle->manifest.chunks.end() - 1,
                                   final_topology_reference);
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
  if (!event) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  DecodedSchedulerRecord record;
  if (!GuestSchedulerRecordCodec::Decode(payload, &record, error)) {
    return false;
  }
  const CaptureEvent decoded = SchedulerEventOf(record);
  if (!ValidateSchedulerEvent(decoded, error)) {
    return false;
  }
  *event = decoded;
  return true;
}

}  // namespace cpu
}  // namespace xe

#endif
