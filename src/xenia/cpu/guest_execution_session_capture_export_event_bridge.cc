/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_capture_export_event_bridge.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <utility>

namespace xe {
namespace cpu {

namespace {

using Action = GuestExecutionSessionAssemblerAction;

bool Fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

bool IsModeledExportKind(GuestExecutionSessionEventKind kind) {
  return kind == GuestExecutionSessionEventKind::kKernelExport ||
         kind == GuestExecutionSessionEventKind::kExternOrBuiltin;
}

}  // namespace

Action GuestExecutionSessionCaptureExportEventBridge::OnModeledExportRecord(
    GuestExecutionSessionAssembler& assembler,
    GuestExecutionCaptureExternalEventToken token,
    const GuestExecutionCaptureExternalEventRecord& record,
    std::string* error) noexcept {
  if (error) {
    error->clear();
  }
  if (rejected_) {
    Fail(error, "modeled export event bridge is no longer canonicalizing");
    return Action::kReject;
  }
  const GuestExecutionCaptureParticipantIdentity identity = record.participant;
  uint64_t declared_bytes = 0;
  for (const GuestExecutionCaptureExternalEventEffectRange& range :
       record.effect_ranges) {
    declared_bytes += range.byte_count;
  }
  if (!token || !IsModeledExportKind(record.kind) ||
      !identity.capture_instance_id || !identity.guest_thread_id ||
      !record.guest_address || (record.guest_address & 3) ||
      declared_bytes != record.effect_byte_count ||
      record.postimage.size() != record.effect_byte_count) {
    rejected_ = true;
    Fail(error, "modeled export record has no canonical session identity");
    return Action::kReject;
  }
  if (resolutions_.find(token.value) != resolutions_.cend()) {
    rejected_ = true;
    Fail(error, "modeled export dispatch was canonicalized twice");
    return Action::kReject;
  }

  const auto reject_from_assembler = [&](const char* fallback) {
    rejected_ = true;
    const std::string message = assembler.status().message;
    Fail(error, message.empty() ? std::string(fallback) : message);
    return Action::kReject;
  };

  uint64_t global_sequence = 0;
  try {
    GuestExecutionSessionAssemblerExternalEvent canonical;
    canonical.kind = record.kind;
    canonical.disposition = record.disposition;
    canonical.guest_address = record.guest_address;
    canonical.byte_count = 0;
    if (record.has_returned_value) {
      canonical.payload_kind =
          GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
      canonical.payload.assign(record.returned_value_le.cbegin(),
                               record.returned_value_le.cend());
    }
    if (assembler.OnExternalEvent(identity, std::move(canonical),
                                  &global_sequence) != Action::kContinue) {
      return reject_from_assembler(
          "modeled export canonical event was rejected");
    }
    if (!global_sequence) {
      rejected_ = true;
      Fail(error, "modeled export canonical event has no global sequence");
      return Action::kReject;
    }
    // The declared effect is the export's guest-memory delta, so it is carried
    // as ordinary active-guest-thread mutations rather than as opaque bytes
    // hanging off the export event.
    size_t offset = 0;
    for (const GuestExecutionCaptureExternalEventEffectRange& range :
         record.effect_ranges) {
      GuestExecutionSessionAssemblerMutation mutation;
      mutation.source = record.mutation_source;
      mutation.disposition = record.disposition;
      mutation.guest_address = range.address;
      mutation.byte_count = range.byte_count;
      mutation.bytes.assign(
          record.postimage.cbegin() + offset,
          record.postimage.cbegin() + offset + range.byte_count);
      offset += range.byte_count;
      if (assembler.OnMemoryMutation(identity, std::move(mutation)) !=
          Action::kContinue) {
        return reject_from_assembler(
            "modeled export effect mutation was rejected");
      }
    }
    resolutions_.emplace(token.value, Resolution{identity, global_sequence});
  } catch (...) {
    rejected_ = true;
    Fail(error, "modeled export event bridge could not allocate an event");
    return Action::kReject;
  }
  return Action::kContinue;
}

bool GuestExecutionSessionCaptureExportEventBridge::
    ResolveModeledExportSequence(
        GuestExecutionCaptureExternalEventToken token,
        const GuestExecutionCaptureParticipantIdentity& participant,
        uint64_t* global_sequence) const noexcept {
  if (!global_sequence) {
    return false;
  }
  *global_sequence = 0;
  if (rejected_ || !token) {
    return false;
  }
  const auto resolution = resolutions_.find(token.value);
  if (resolution == resolutions_.cend() ||
      resolution->second.participant != participant ||
      !resolution->second.global_sequence) {
    return false;
  }
  *global_sequence = resolution->second.global_sequence;
  return true;
}

}  // namespace cpu
}  // namespace xe

#endif
