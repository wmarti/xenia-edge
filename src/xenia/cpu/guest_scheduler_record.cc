/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_scheduler_record.h"

#include <algorithm>
#include <limits>
#include <string_view>

namespace xe {
namespace cpu {
namespace {

constexpr std::array<uint8_t, 8> kPayloadMagic = {'X', 'E', 'G', 'S',
                                                  'C', 'E', '1', 0};

static_assert(176 + 16 == GuestSchedulerRecordCodec::kPayloadSize);
static_assert(80 + 3 * 4 * kGuestSchedulerCaptureMaximumWaitHandles == 176);

constexpr uint16_t kKnownCaptureFlags =
    kGuestSchedulerCaptureFlagAtHead | kGuestSchedulerCaptureFlagYieldToOther |
    kGuestSchedulerCaptureFlagHonoredYield |
    kGuestSchedulerCaptureFlagFirstRun |
    kGuestSchedulerCaptureFlagFreshQuantum |
    kGuestSchedulerCaptureFlagQuantumEnd | kGuestSchedulerCaptureFlagToLower |
    kGuestSchedulerCaptureFlagPreempted |
    kGuestSchedulerCaptureFlagSchedulerRequested |
    kGuestSchedulerCaptureFlagCaptureRequested |
    kGuestSchedulerCaptureFlagGated | kGuestSchedulerCaptureFlagAlertable |
    kGuestSchedulerCaptureFlagInterruptible |
    kGuestSchedulerCaptureFlagHasDeadline;
constexpr uint8_t kKnownWaitFlags =
    kGuestSchedulerCaptureWaitFlagGated |
    kGuestSchedulerCaptureWaitFlagAlertable |
    kGuestSchedulerCaptureWaitFlagInterruptible |
    kGuestSchedulerCaptureWaitFlagUserApcPending;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
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

bool IsKnownReason(GuestSchedulerCaptureReason reason) {
  switch (reason) {
    case GuestSchedulerCaptureReason::kNone:
    case GuestSchedulerCaptureReason::kPriority:
    case GuestSchedulerCaptureReason::kWake:
    case GuestSchedulerCaptureReason::kTimeslice:
    case GuestSchedulerCaptureReason::kTerminate:
    case GuestSchedulerCaptureReason::kShutdown:
    case GuestSchedulerCaptureReason::kDeferredLock:
    case GuestSchedulerCaptureReason::kDeferredIrql:
    case GuestSchedulerCaptureReason::kForcedIrql:
    case GuestSchedulerCaptureReason::kYielded:
    case GuestSchedulerCaptureReason::kPolled:
    case GuestSchedulerCaptureReason::kSignalEpoch:
    case GuestSchedulerCaptureReason::kDeadline:
    case GuestSchedulerCaptureReason::kUserApc:
    case GuestSchedulerCaptureReason::kBackstop:
    case GuestSchedulerCaptureReason::kDetached:
    case GuestSchedulerCaptureReason::kPreemptRequested:
    case GuestSchedulerCaptureReason::kReadied:
    case GuestSchedulerCaptureReason::kNeverRan:
    case GuestSchedulerCaptureReason::kDeferredToDispatcher:
      return true;
    default:
      return false;
  }
}

bool IsSupportedKind(GuestSchedulerCaptureEventKind kind) {
  switch (kind) {
    case GuestSchedulerCaptureEventKind::kEnqueueReady:
    case GuestSchedulerCaptureEventKind::kDequeueReady:
    case GuestSchedulerCaptureEventKind::kDispatch:
    case GuestSchedulerCaptureEventKind::kSwitchOut:
    case GuestSchedulerCaptureEventKind::kYield:
    case GuestSchedulerCaptureEventKind::kPreemptRequest:
    case GuestSchedulerCaptureEventKind::kSafepoint:
    case GuestSchedulerCaptureEventKind::kBlock:
    case GuestSchedulerCaptureEventKind::kReready:
    case GuestSchedulerCaptureEventKind::kParkSuspended:
    case GuestSchedulerCaptureEventKind::kResume:
    case GuestSchedulerCaptureEventKind::kPriorityChange:
    case GuestSchedulerCaptureEventKind::kMigrate:
      return true;
    case GuestSchedulerCaptureEventKind::kNone:
    case GuestSchedulerCaptureEventKind::kExit:
    case GuestSchedulerCaptureEventKind::kTerminate:
    case GuestSchedulerCaptureEventKind::kForget:
    case GuestSchedulerCaptureEventKind::kShutdown:
    default:
      return false;
  }
}

uint16_t AllowedFlags(GuestSchedulerCaptureEventKind kind) {
  switch (kind) {
    case GuestSchedulerCaptureEventKind::kEnqueueReady:
      return kGuestSchedulerCaptureFlagAtHead |
             kGuestSchedulerCaptureFlagYieldToOther;
    case GuestSchedulerCaptureEventKind::kDequeueReady:
      return kGuestSchedulerCaptureFlagHonoredYield;
    case GuestSchedulerCaptureEventKind::kDispatch:
      return kGuestSchedulerCaptureFlagFirstRun |
             kGuestSchedulerCaptureFlagFreshQuantum;
    case GuestSchedulerCaptureEventKind::kYield:
      return kGuestSchedulerCaptureFlagQuantumEnd |
             kGuestSchedulerCaptureFlagToLower |
             kGuestSchedulerCaptureFlagPreempted;
    case GuestSchedulerCaptureEventKind::kSafepoint:
      return kGuestSchedulerCaptureFlagSchedulerRequested |
             kGuestSchedulerCaptureFlagCaptureRequested;
    case GuestSchedulerCaptureEventKind::kBlock:
      return kGuestSchedulerCaptureFlagGated |
             kGuestSchedulerCaptureFlagAlertable |
             kGuestSchedulerCaptureFlagInterruptible |
             kGuestSchedulerCaptureFlagHasDeadline;
    case GuestSchedulerCaptureEventKind::kReready:
    case GuestSchedulerCaptureEventKind::kMigrate:
      return kGuestSchedulerCaptureFlagAtHead;
    default:
      return 0;
  }
}

bool IsReasonAllowed(GuestSchedulerCaptureEventKind kind,
                     GuestSchedulerCaptureReason reason) {
  switch (kind) {
    case GuestSchedulerCaptureEventKind::kPreemptRequest:
      return reason == GuestSchedulerCaptureReason::kPriority ||
             reason == GuestSchedulerCaptureReason::kWake ||
             reason == GuestSchedulerCaptureReason::kTimeslice ||
             reason == GuestSchedulerCaptureReason::kTerminate ||
             reason == GuestSchedulerCaptureReason::kShutdown;
    case GuestSchedulerCaptureEventKind::kSafepoint:
      return reason == GuestSchedulerCaptureReason::kDeferredLock ||
             reason == GuestSchedulerCaptureReason::kDeferredIrql ||
             reason == GuestSchedulerCaptureReason::kForcedIrql ||
             reason == GuestSchedulerCaptureReason::kYielded;
    case GuestSchedulerCaptureEventKind::kReready:
      return reason == GuestSchedulerCaptureReason::kPolled ||
             reason == GuestSchedulerCaptureReason::kSignalEpoch ||
             reason == GuestSchedulerCaptureReason::kDeadline ||
             reason == GuestSchedulerCaptureReason::kUserApc ||
             reason == GuestSchedulerCaptureReason::kBackstop;
    default:
      return reason == GuestSchedulerCaptureReason::kNone;
  }
}

bool IsCpu(int8_t cpu) { return cpu >= 0 && cpu < 6; }

bool IsEmptyWaitState(const GuestSchedulerCaptureWaitState& wait) {
  return wait == GuestSchedulerCaptureWaitState{};
}

bool ValidateWaitState(const DecodedSchedulerRecord& record,
                       std::string* error) {
  const auto& wait = record.wait;
  const auto wait_kind =
      static_cast<GuestSchedulerCaptureWaitKind>(record.value);
  if (wait.handle_count > kGuestSchedulerCaptureMaximumWaitHandles ||
      (wait.flags & ~kKnownWaitFlags) ||
      ((wait.flags & kGuestSchedulerCaptureWaitFlagUserApcPending) &&
       !(wait.flags & kGuestSchedulerCaptureWaitFlagAlertable))) {
    return Fail(error, "scheduler capture wait state is malformed");
  }
  uint32_t epochs_before = 0;
  uint32_t epochs_observed = 0;
  bool has_signal_epochs = false;
  for (size_t index = 0; index < kGuestSchedulerCaptureMaximumWaitHandles;
       ++index) {
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
    case GuestSchedulerCaptureWaitKind::kSingle:
      if (wait.handle_count != 1) {
        return Fail(error, "scheduler single wait has no unique handle");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kMultiAny:
    case GuestSchedulerCaptureWaitKind::kMultiAll:
      if (!wait.handle_count) {
        return Fail(error, "scheduler multi-wait has no handles");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kDelay:
    case GuestSchedulerCaptureWaitKind::kFence:
    case GuestSchedulerCaptureWaitKind::kIoOffload:
    case GuestSchedulerCaptureWaitKind::kSpinBackoff:
      if (wait.handle_count) {
        return Fail(error, "scheduler handle-free wait names an object");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kIoCompletion:
    case GuestSchedulerCaptureWaitKind::kSocketIo:
      if (wait.handle_count != 1) {
        return Fail(error, "scheduler external wait has no unique handle");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kNone:
    default:
      return Fail(error, "scheduler capture wait kind is invalid");
  }

  if ((wait_kind == GuestSchedulerCaptureWaitKind::kDelay ||
       wait_kind == GuestSchedulerCaptureWaitKind::kFence ||
       wait_kind == GuestSchedulerCaptureWaitKind::kIoOffload ||
       wait_kind == GuestSchedulerCaptureWaitKind::kSpinBackoff ||
       wait_kind == GuestSchedulerCaptureWaitKind::kIoCompletion ||
       wait_kind == GuestSchedulerCaptureWaitKind::kSocketIo) &&
      has_signal_epochs) {
    return Fail(error, "scheduler non-object wait carries signal epochs");
  }
  if ((wait_kind == GuestSchedulerCaptureWaitKind::kFence ||
       wait_kind == GuestSchedulerCaptureWaitKind::kIoOffload ||
       wait_kind == GuestSchedulerCaptureWaitKind::kSpinBackoff) &&
      wait.deadline_ms) {
    return Fail(error, "scheduler untimed wait carries a deadline");
  }
  if (wait_kind == GuestSchedulerCaptureWaitKind::kDelay && !wait.deadline_ms) {
    return Fail(error, "scheduler delay wait has no deadline");
  }
  const bool gated = wait.flags & kGuestSchedulerCaptureWaitFlagGated;
  const bool alertable = wait.flags & kGuestSchedulerCaptureWaitFlagAlertable;
  const bool interruptible =
      wait.flags & kGuestSchedulerCaptureWaitFlagInterruptible;
  if ((wait_kind == GuestSchedulerCaptureWaitKind::kFence ||
       wait_kind == GuestSchedulerCaptureWaitKind::kIoOffload ||
       wait_kind == GuestSchedulerCaptureWaitKind::kSpinBackoff ||
       wait_kind == GuestSchedulerCaptureWaitKind::kIoCompletion ||
       wait_kind == GuestSchedulerCaptureWaitKind::kSocketIo) &&
      gated) {
    return Fail(error, "scheduler polling wait is incorrectly gated");
  }
  switch (wait_kind) {
    case GuestSchedulerCaptureWaitKind::kSingle:
    case GuestSchedulerCaptureWaitKind::kMultiAny:
    case GuestSchedulerCaptureWaitKind::kMultiAll:
      if (!interruptible) {
        return Fail(error,
                    "scheduler object wait is incorrectly non-interruptible");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kDelay:
      if (!interruptible || gated == alertable) {
        return Fail(error, "scheduler delay wait flags are impossible");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kFence:
    case GuestSchedulerCaptureWaitKind::kIoOffload:
      if (alertable || interruptible) {
        return Fail(error, "scheduler stack-owned wait flags are impossible");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kSpinBackoff:
    case GuestSchedulerCaptureWaitKind::kIoCompletion:
    case GuestSchedulerCaptureWaitKind::kSocketIo:
      if (alertable || !interruptible) {
        return Fail(error, "scheduler polling wait flags are impossible");
      }
      break;
    case GuestSchedulerCaptureWaitKind::kNone:
    default:
      return Fail(error, "scheduler capture wait kind is invalid");
  }
  if (record.kind == GuestSchedulerCaptureEventKind::kBlock) {
    const uint8_t expected_wait_flags =
        ((record.flags & kGuestSchedulerCaptureFlagGated)
             ? kGuestSchedulerCaptureWaitFlagGated
             : 0) |
        ((record.flags & kGuestSchedulerCaptureFlagAlertable)
             ? kGuestSchedulerCaptureWaitFlagAlertable
             : 0) |
        ((record.flags & kGuestSchedulerCaptureFlagInterruptible)
             ? kGuestSchedulerCaptureWaitFlagInterruptible
             : 0);
    if ((wait.flags & ~kGuestSchedulerCaptureWaitFlagUserApcPending) !=
            expected_wait_flags ||
        bool(record.flags & kGuestSchedulerCaptureFlagHasDeadline) !=
            bool(wait.deadline_ms)) {
      return Fail(error, "scheduler block wait provenance is inconsistent");
    }
    return true;
  }

  switch (record.reason) {
    case GuestSchedulerCaptureReason::kPolled:
      if (gated) {
        return Fail(error, "scheduler polled wake names a gated wait");
      }
      break;
    case GuestSchedulerCaptureReason::kSignalEpoch:
      if (!gated || wait.wait_epoch == wait.observed_wait_epoch) {
        return Fail(error, "scheduler signal wake has no epoch transition");
      }
      break;
    case GuestSchedulerCaptureReason::kDeadline:
      if (!gated || !wait.deadline_ms ||
          wait.observed_uptime_ms < wait.deadline_ms) {
        return Fail(error, "scheduler deadline wake has no expired deadline");
      }
      break;
    case GuestSchedulerCaptureReason::kUserApc:
      if (!gated ||
          !(wait.flags & kGuestSchedulerCaptureWaitFlagUserApcPending)) {
        return Fail(error, "scheduler APC wake has no pending user APC");
      }
      break;
    case GuestSchedulerCaptureReason::kBackstop:
      if (!gated) {
        return Fail(error, "scheduler backstop wake names an ungated wait");
      }
      break;
    default:
      return Fail(error, "scheduler wake reason is invalid");
  }
  return true;
}

}  // namespace

bool GuestSchedulerRecordCodec::Decode(std::span<const uint8_t> payload,
                                       DecodedSchedulerRecord* record,
                                       std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!record || payload.size() < 12 ||
      !std::equal(kPayloadMagic.cbegin(), kPayloadMagic.cend(),
                  payload.begin())) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  const uint32_t version = ReadU32(payload, 8);
  if (version == 1) {
    if (payload.size() != kPayloadV1Size || payload[47]) {
      return Fail(error, "scheduler event payload envelope is invalid");
    }
    return Fail(error,
                "scheduler event payload version 1 is not deterministic-"
                "replayable");
  }
  if (version != kPayloadVersion || payload.size() != kPayloadSize ||
      payload[47] || payload[62] || payload[63] ||
      std::any_of(payload.begin() + 176, payload.end(),
                  [](uint8_t value) { return value != 0; })) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  const uint32_t raw_kind = ReadU32(payload, 12);
  if (raw_kind > std::numeric_limits<uint8_t>::max()) {
    return Fail(error, "scheduler event payload kind is out of range");
  }
  DecodedSchedulerRecord decoded;
  decoded.kind = static_cast<GuestSchedulerCaptureEventKind>(raw_kind);
  decoded.sequence = ReadU64(payload, 16);
  decoded.capture_instance_id = ReadU64(payload, 24);
  decoded.guest_thread_id = ReadU32(payload, 32);
  decoded.count = ReadU32(payload, 36);
  decoded.flags = ReadU16(payload, 40);
  decoded.reason = static_cast<GuestSchedulerCaptureReason>(payload[42]);
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
  for (size_t index = 0; index < kGuestSchedulerCaptureMaximumWaitHandles;
       ++index) {
    decoded.wait.handles[index] = ReadU32(payload, 80 + index * 4);
    decoded.wait.signal_epochs_before[index] =
        ReadU32(payload, 112 + index * 4);
    decoded.wait.signal_epochs_observed[index] =
        ReadU32(payload, 144 + index * 4);
  }
  if (!IsSupportedKind(decoded.kind)) {
    return Fail(error, "scheduler event payload kind is unsupported");
  }
  if (!IsKnownReason(decoded.reason)) {
    return Fail(error, "scheduler event payload reason is unsupported");
  }
  if (!decoded.sequence || !decoded.capture_instance_id ||
      !decoded.guest_thread_id) {
    return Fail(error, "scheduler event payload envelope is invalid");
  }
  *record = decoded;
  return true;
}

bool GuestSchedulerRecordCodec::Validate(const DecodedSchedulerRecord& record,
                                         std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (!record.sequence || !record.capture_instance_id ||
      !record.guest_thread_id || !IsSupportedKind(record.kind) ||
      !IsKnownReason(record.reason) ||
      !IsReasonAllowed(record.kind, record.reason) || record.priority > 31 ||
      (record.flags & ~kKnownCaptureFlags) ||
      (record.flags & ~AllowedFlags(record.kind))) {
    return Fail(error, "scheduler capture event is unsupported or malformed");
  }
  const bool wait_event =
      record.kind == GuestSchedulerCaptureEventKind::kBlock ||
      record.kind == GuestSchedulerCaptureEventKind::kReready;
  if ((!wait_event && !IsEmptyWaitState(record.wait)) ||
      (record.kind != GuestSchedulerCaptureEventKind::kSafepoint &&
       record.guest_pc) ||
      (record.kind != GuestSchedulerCaptureEventKind::kSafepoint &&
       record.count) ||
      (record.kind != GuestSchedulerCaptureEventKind::kSafepoint &&
       record.kind != GuestSchedulerCaptureEventKind::kBlock &&
       record.kind != GuestSchedulerCaptureEventKind::kReready &&
       record.kind != GuestSchedulerCaptureEventKind::kPriorityChange &&
       record.value)) {
    return Fail(error, "scheduler capture event has unexpected provenance");
  }
  switch (record.kind) {
    case GuestSchedulerCaptureEventKind::kEnqueueReady:
      if ((record.cpu != -1 && !IsCpu(record.cpu)) ||
          !IsCpu(record.target_cpu)) {
        return Fail(error, "scheduler enqueue CPU is invalid");
      }
      break;
    case GuestSchedulerCaptureEventKind::kReready:
      if (!IsCpu(record.cpu) || !IsCpu(record.target_cpu)) {
        return Fail(error, "scheduler reready CPU is invalid");
      }
      break;
    case GuestSchedulerCaptureEventKind::kMigrate:
      if (!IsCpu(record.cpu) || !IsCpu(record.target_cpu) ||
          record.cpu == record.target_cpu) {
        return Fail(error, "scheduler migration CPUs are invalid");
      }
      break;
    case GuestSchedulerCaptureEventKind::kPriorityChange:
      if ((record.cpu != -1 && !IsCpu(record.cpu)) || record.target_cpu != -1) {
        return Fail(error, "scheduler priority-change CPU is invalid");
      }
      break;
    default:
      if (!IsCpu(record.cpu) || record.target_cpu != -1) {
        return Fail(error, "scheduler capture CPU is invalid");
      }
      break;
  }
  if (record.kind == GuestSchedulerCaptureEventKind::kSafepoint) {
    if (!record.guest_pc || (record.guest_pc & 3) ||
        !(record.flags & kGuestSchedulerCaptureFlagSchedulerRequested)) {
      return Fail(error, "scheduler safepoint provenance is invalid");
    }
    switch (record.reason) {
      case GuestSchedulerCaptureReason::kDeferredLock:
        if (record.count) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      case GuestSchedulerCaptureReason::kDeferredIrql:
        if (record.value < 2 || record.count) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      case GuestSchedulerCaptureReason::kForcedIrql:
        if (record.value < 2 ||
            record.count < kGuestSchedulerCaptureForcedIrqlMinimumDeclines) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      case GuestSchedulerCaptureReason::kYielded:
        if (record.value >= 2) {
          return Fail(error, "scheduler safepoint provenance is invalid");
        }
        break;
      default:
        return Fail(error, "scheduler safepoint provenance is invalid");
    }
  }
  if (record.kind == GuestSchedulerCaptureEventKind::kPriorityChange &&
      record.value > 31) {
    return Fail(error, "scheduler previous priority is invalid");
  }
  if (wait_event && !ValidateWaitState(record, error)) {
    return false;
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
