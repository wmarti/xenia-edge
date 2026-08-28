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
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/guest_execution_session_capture_event_bridge.h"
#endif

namespace xe {
namespace cpu {
namespace test {
namespace {

using Codec = GuestSchedulerRecordCodec;
using Kind = GuestSchedulerCaptureEventKind;
using Reason = GuestSchedulerCaptureReason;
using WaitKind = GuestSchedulerCaptureWaitKind;

constexpr uint64_t kInstance = 0x100;
constexpr uint32_t kThreadId = 7;

void WriteU16(std::vector<uint8_t>* payload, size_t offset, uint16_t value) {
  (*payload)[offset] = static_cast<uint8_t>(value);
  (*payload)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void WriteU32(std::vector<uint8_t>* payload, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    (*payload)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void WriteU64(std::vector<uint8_t>* payload, size_t offset, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) {
    (*payload)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

// The canned XEGSCE1 encoder. It writes the same 192-byte layout the capture
// bridge writes, so a payload it produces is a known-good tape record.
std::vector<uint8_t> EncodePayload(const DecodedSchedulerRecord& record) {
  std::vector<uint8_t> payload(Codec::kPayloadSize, 0);
  const std::array<uint8_t, 8> magic = {'X', 'E', 'G', 'S', 'C', 'E', '1', 0};
  std::copy(magic.cbegin(), magic.cend(), payload.begin());
  WriteU32(&payload, 8, Codec::kPayloadVersion);
  WriteU32(&payload, 12, static_cast<uint32_t>(record.kind));
  WriteU64(&payload, 16, record.sequence);
  WriteU64(&payload, 24, record.capture_instance_id);
  WriteU32(&payload, 32, record.guest_thread_id);
  WriteU32(&payload, 36, record.count);
  WriteU16(&payload, 40, record.flags);
  payload[42] = static_cast<uint8_t>(record.reason);
  payload[43] = static_cast<uint8_t>(record.cpu);
  payload[44] = static_cast<uint8_t>(record.target_cpu);
  payload[45] = record.priority;
  payload[46] = record.value;
  WriteU32(&payload, 48, record.guest_pc);
  WriteU32(&payload, 52, record.wait.wait_epoch);
  WriteU32(&payload, 56, record.wait.observed_wait_epoch);
  payload[60] = record.wait.handle_count;
  payload[61] = record.wait.flags;
  WriteU64(&payload, 64, record.wait.deadline_ms);
  WriteU64(&payload, 72, record.wait.observed_uptime_ms);
  for (size_t index = 0; index < kGuestSchedulerCaptureMaximumWaitHandles;
       ++index) {
    WriteU32(&payload, 80 + index * 4, record.wait.handles[index]);
    WriteU32(&payload, 112 + index * 4,
             record.wait.signal_epochs_before[index]);
    WriteU32(&payload, 144 + index * 4,
             record.wait.signal_epochs_observed[index]);
  }
  return payload;
}

DecodedSchedulerRecord MakeRecord(uint64_t sequence, Kind kind) {
  DecodedSchedulerRecord record;
  record.sequence = sequence;
  record.capture_instance_id = kInstance;
  record.guest_thread_id = kThreadId;
  record.kind = kind;
  record.cpu = 0;
  record.priority = 8;
  if (kind == Kind::kEnqueueReady || kind == Kind::kReready) {
    record.target_cpu = 0;
  } else if (kind == Kind::kMigrate) {
    record.target_cpu = 1;
  }
  switch (kind) {
    case Kind::kYield:
      record.flags = kGuestSchedulerCaptureFlagQuantumEnd;
      break;
    case Kind::kPreemptRequest:
      record.reason = Reason::kPriority;
      break;
    case Kind::kSafepoint:
      record.guest_pc = 0x82000040;
      record.reason = Reason::kForcedIrql;
      record.flags = kGuestSchedulerCaptureFlagSchedulerRequested;
      record.value = 2;
      record.count = kGuestSchedulerCaptureForcedIrqlMinimumDeclines;
      break;
    case Kind::kBlock:
    case Kind::kReready:
      record.value = static_cast<uint8_t>(WaitKind::kSingle);
      record.wait.handle_count = 1;
      record.wait.handles[0] = 0x20;
      record.wait.wait_epoch = 7;
      record.wait.observed_wait_epoch = 7;
      record.wait.signal_epochs_before[0] = 7;
      record.wait.signal_epochs_observed[0] = 7;
      record.wait.flags = kGuestSchedulerCaptureWaitFlagInterruptible;
      if (kind == Kind::kBlock) {
        record.flags = kGuestSchedulerCaptureFlagGated |
                       kGuestSchedulerCaptureFlagInterruptible;
        record.wait.flags |= kGuestSchedulerCaptureWaitFlagGated;
      } else {
        record.reason = Reason::kPolled;
      }
      break;
    default:
      break;
  }
  return record;
}

DecodedSchedulerRecord MakeBlockRecord(uint64_t sequence, WaitKind wait_kind,
                                       bool external_has_deadline = true) {
  DecodedSchedulerRecord record = MakeRecord(sequence, Kind::kBlock);
  record.value = static_cast<uint8_t>(wait_kind);
  record.flags = 0;
  record.wait = {};
  record.wait.observed_uptime_ms = 50;
  bool interruptible = true;
  switch (wait_kind) {
    case WaitKind::kSingle:
      record.wait.handle_count = 1;
      record.wait.handles[0] = 0x20;
      record.flags = kGuestSchedulerCaptureFlagGated;
      record.wait.flags = kGuestSchedulerCaptureWaitFlagGated;
      break;
    case WaitKind::kMultiAny:
    case WaitKind::kMultiAll:
      record.wait.handle_count = 2;
      record.wait.handles[0] = 0x20;
      record.wait.handles[1] = 0x24;
      record.flags = kGuestSchedulerCaptureFlagGated;
      record.wait.flags = kGuestSchedulerCaptureWaitFlagGated;
      break;
    case WaitKind::kDelay:
      record.wait.deadline_ms = 100;
      record.flags = kGuestSchedulerCaptureFlagGated |
                     kGuestSchedulerCaptureFlagHasDeadline;
      record.wait.flags = kGuestSchedulerCaptureWaitFlagGated;
      break;
    case WaitKind::kFence:
    case WaitKind::kIoOffload:
      interruptible = false;
      break;
    case WaitKind::kSpinBackoff:
      break;
    case WaitKind::kIoCompletion:
    case WaitKind::kSocketIo:
      record.wait.handle_count = 1;
      record.wait.handles[0] =
          wait_kind == WaitKind::kIoCompletion ? 0x30 : 0x34;
      if (external_has_deadline) {
        record.wait.deadline_ms = 100;
        record.flags = kGuestSchedulerCaptureFlagHasDeadline;
      }
      break;
    default:
      break;
  }
  if (interruptible) {
    record.flags |= kGuestSchedulerCaptureFlagInterruptible;
    record.wait.flags |= kGuestSchedulerCaptureWaitFlagInterruptible;
  }
  return record;
}

std::vector<DecodedSchedulerRecord> ModeledRecords() {
  std::vector<DecodedSchedulerRecord> records;
  uint64_t sequence = 100;
  for (uint8_t raw = static_cast<uint8_t>(Kind::kEnqueueReady);
       raw <= static_cast<uint8_t>(Kind::kMigrate); ++raw) {
    records.push_back(MakeRecord(sequence++, static_cast<Kind>(raw)));
  }
  for (uint8_t raw = static_cast<uint8_t>(WaitKind::kSingle);
       raw <= static_cast<uint8_t>(WaitKind::kSocketIo); ++raw) {
    records.push_back(MakeBlockRecord(sequence++, static_cast<WaitKind>(raw)));
  }
  records.push_back(
      MakeBlockRecord(sequence++, WaitKind::kIoCompletion, false));
  records.push_back(MakeBlockRecord(sequence++, WaitKind::kSocketIo, false));
  return records;
}

}  // namespace

TEST_CASE("ungated scheduler record decode round-trips every modeled record",
          "[guest-scheduler-record]") {
  std::string error;
  for (const DecodedSchedulerRecord& expected : ModeledRecords()) {
    const std::vector<uint8_t> payload = EncodePayload(expected);
    DecodedSchedulerRecord decoded;
    INFO(static_cast<uint32_t>(expected.kind));
    INFO(static_cast<uint32_t>(expected.value));
    REQUIRE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error.empty());
    REQUIRE(decoded == expected);
    REQUIRE(Codec::Validate(decoded, &error));
    REQUIRE(error.empty());
  }
}

TEST_CASE("ungated scheduler record decode fails closed",
          "[guest-scheduler-record]") {
  const DecodedSchedulerRecord source = MakeRecord(41, Kind::kDispatch);
  const std::vector<uint8_t> canonical = EncodePayload(source);
  DecodedSchedulerRecord decoded;
  std::string error;

  SECTION("a short payload rejects") {
    for (size_t size : {size_t{0}, size_t{11}, size_t{12}, size_t{191}}) {
      const std::vector<uint8_t> payload(
          canonical.cbegin(),
          canonical.cbegin() + static_cast<std::ptrdiff_t>(size));
      REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
      REQUIRE(error == "scheduler event payload envelope is invalid");
    }
  }

  SECTION("an oversized payload rejects") {
    std::vector<uint8_t> payload = canonical;
    payload.push_back(0);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
  }

  SECTION("a foreign magic rejects") {
    std::vector<uint8_t> payload = canonical;
    payload[3] = 'X';
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
  }

  SECTION("the legacy version is named rather than decoded") {
    std::vector<uint8_t> payload(
        canonical.cbegin(), canonical.cbegin() + static_cast<std::ptrdiff_t>(
                                                     Codec::kPayloadV1Size));
    WriteU32(&payload, 8, 1);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error ==
            "scheduler event payload version 1 is not deterministic-"
            "replayable");
  }

  SECTION("an unknown version rejects") {
    std::vector<uint8_t> payload = canonical;
    WriteU32(&payload, 8, 3);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
  }

  SECTION("nonzero reserved bytes reject") {
    for (size_t offset : {size_t{47}, size_t{62}, size_t{63}, size_t{176},
                          Codec::kPayloadSize - 1}) {
      std::vector<uint8_t> payload = canonical;
      payload[offset] = 1;
      INFO(offset);
      REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
      REQUIRE(error == "scheduler event payload envelope is invalid");
    }
  }

  SECTION("a kind alias above the durable identifier range rejects") {
    std::vector<uint8_t> payload = canonical;
    WriteU32(&payload, 12, 0x0103);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload kind is out of range");
  }

  SECTION("an unmodeled kind rejects") {
    for (uint32_t raw : {0u, 14u, 15u, 16u, 17u, 18u, 255u}) {
      std::vector<uint8_t> payload = canonical;
      WriteU32(&payload, 12, raw);
      INFO(raw);
      REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
      REQUIRE(error == "scheduler event payload kind is unsupported");
    }
  }

  SECTION("an unknown reason rejects") {
    for (uint8_t raw : {uint8_t{20}, uint8_t{200}, uint8_t{255}}) {
      std::vector<uint8_t> payload = canonical;
      payload[42] = raw;
      INFO(static_cast<uint32_t>(raw));
      REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
      REQUIRE(error == "scheduler event payload reason is unsupported");
    }
  }

  SECTION("an absent identity rejects") {
    std::vector<uint8_t> payload = canonical;
    WriteU64(&payload, 16, 0);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
    payload = canonical;
    WriteU64(&payload, 24, 0);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
    payload = canonical;
    WriteU32(&payload, 32, 0);
    REQUIRE_FALSE(Codec::Decode(payload, &decoded, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
  }

  SECTION("a null output rejects") {
    REQUIRE_FALSE(Codec::Decode(canonical, nullptr, &error));
    REQUIRE(error == "scheduler event payload envelope is invalid");
  }
}

TEST_CASE("ungated scheduler record validation fails closed",
          "[guest-scheduler-record]") {
  std::string error;

  SECTION("an impossible reason for the kind rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kDispatch);
    record.reason = Reason::kSignalEpoch;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler capture event is unsupported or malformed");
  }

  SECTION("a priority above the queue range rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kDispatch);
    record.priority = 32;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler capture event is unsupported or malformed");
  }

  SECTION("a flag the kind does not own rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kDispatch);
    record.flags = kGuestSchedulerCaptureFlagAtHead;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler capture event is unsupported or malformed");
  }

  SECTION("an exact PC outside a safepoint rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kDispatch);
    record.guest_pc = 0x82000040;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler capture event has unexpected provenance");
  }

  SECTION("a dispatch CPU outside the topology rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kDispatch);
    record.cpu = 6;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler capture CPU is invalid");
  }

  SECTION("a migration that does not move rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kMigrate);
    record.target_cpu = record.cpu;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler migration CPUs are invalid");
  }

  SECTION("an unmodeled wait kind rejects") {
    for (uint8_t raw : {uint8_t{0}, uint8_t{10}, uint8_t{255}}) {
      DecodedSchedulerRecord record = MakeBlockRecord(41, WaitKind::kSingle);
      record.value = raw;
      INFO(static_cast<uint32_t>(raw));
      REQUIRE_FALSE(Codec::Validate(record, &error));
      REQUIRE(error == "scheduler capture wait kind is invalid");
    }
  }

  SECTION("a single wait without its unique handle rejects") {
    DecodedSchedulerRecord record = MakeBlockRecord(41, WaitKind::kSingle);
    record.wait.handle_count = 0;
    record.wait.handles[0] = 0;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler single wait has no unique handle");
  }

  SECTION("a forced safepoint below the decline threshold rejects") {
    DecodedSchedulerRecord record = MakeRecord(41, Kind::kSafepoint);
    record.count = kGuestSchedulerCaptureForcedIrqlMinimumDeclines - 1;
    REQUIRE_FALSE(Codec::Validate(record, &error));
    REQUIRE(error == "scheduler safepoint provenance is invalid");
  }
}

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

TEST_CASE("the ungated scheduler decoder agrees with the capture bridge",
          "[guest-scheduler-record][guest-execution-session-capture-event-"
          "bridge]") {
  using Bridge = GuestExecutionSessionCaptureSchedulerEventBridge;
  static_assert(Codec::kPayloadVersion == Bridge::kSchedulerPayloadVersion);
  static_assert(Codec::kPayloadV1Size == Bridge::kSchedulerPayloadV1Size);
  static_assert(Codec::kPayloadSize == Bridge::kSchedulerPayloadSize);

  SECTION("every field of every modeled record matches the bridge decode") {
    for (const DecodedSchedulerRecord& expected : ModeledRecords()) {
      const std::vector<uint8_t> payload = EncodePayload(expected);
      DecodedSchedulerRecord record;
      kernel::GuestSchedulerCaptureEvent event;
      std::string record_error;
      std::string bridge_error;
      INFO(static_cast<uint32_t>(expected.kind));
      INFO(static_cast<uint32_t>(expected.value));
      REQUIRE(Codec::Decode(payload, &record, &record_error));
      REQUIRE(
          Bridge::DecodeSchedulerEventPayload(payload, &event, &bridge_error));
      REQUIRE(record.sequence == event.sequence);
      REQUIRE(record.capture_instance_id == event.capture_instance_id);
      REQUIRE(record.guest_thread_id == event.guest_thread_id);
      REQUIRE(record.count == event.count);
      REQUIRE(record.guest_pc == event.guest_pc);
      REQUIRE(record.flags == event.flags);
      REQUIRE(record.kind == event.kind);
      REQUIRE(record.reason == event.reason);
      REQUIRE(record.cpu == event.cpu);
      REQUIRE(record.target_cpu == event.target_cpu);
      REQUIRE(record.priority == event.priority);
      REQUIRE(record.value == event.value);
      REQUIRE(record.wait.deadline_ms == event.wait.deadline_ms);
      REQUIRE(record.wait.observed_uptime_ms == event.wait.observed_uptime_ms);
      REQUIRE(record.wait.wait_epoch == event.wait.wait_epoch);
      REQUIRE(record.wait.observed_wait_epoch ==
              event.wait.observed_wait_epoch);
      REQUIRE(record.wait.handle_count == event.wait.handle_count);
      REQUIRE(record.wait.flags == event.wait.flags);
      REQUIRE(record.wait.handles == event.wait.handles);
      REQUIRE(record.wait.signal_epochs_before ==
              event.wait.signal_epochs_before);
      REQUIRE(record.wait.signal_epochs_observed ==
              event.wait.signal_epochs_observed);
      // The transport-only delta is never serialized, so a decoded record is
      // the whole event.
      REQUIRE(event.guest_instruction_delta == 0);
      REQUIRE(record.wait == event.wait);
    }
  }

  SECTION("both decoders reject the same mutations by the same name") {
    const std::vector<uint8_t> canonical =
        EncodePayload(MakeBlockRecord(41, WaitKind::kSingle));
    std::vector<std::vector<uint8_t>> mutations;
    for (size_t offset : {size_t{3}, size_t{47}, size_t{62}, size_t{63},
                          size_t{176}, Codec::kPayloadSize - 1}) {
      std::vector<uint8_t> payload = canonical;
      payload[offset] = 0x5A;
      mutations.push_back(payload);
    }
    for (uint32_t raw : {0u, 14u, 17u, 0x0103u}) {
      std::vector<uint8_t> payload = canonical;
      WriteU32(&payload, 12, raw);
      mutations.push_back(payload);
    }
    for (size_t offset : {size_t{16}, size_t{24}}) {
      std::vector<uint8_t> payload = canonical;
      WriteU64(&payload, offset, 0);
      mutations.push_back(payload);
    }
    {
      std::vector<uint8_t> payload = canonical;
      payload[46] = 0;  // wait kind kNone
      mutations.push_back(payload);
      payload = canonical;
      payload[60] = 0;  // no unique handle
      mutations.push_back(payload);
      payload = canonical;
      payload[45] = 32;  // priority outside the queue range
      mutations.push_back(payload);
      payload = canonical;
      payload[44] = 0;  // a target CPU the kind does not own
      mutations.push_back(payload);
      payload = canonical;
      payload[42] = 20;  // an unknown reason
      mutations.push_back(payload);
    }
    for (const std::vector<uint8_t>& payload : mutations) {
      DecodedSchedulerRecord record;
      kernel::GuestSchedulerCaptureEvent event;
      std::string record_error;
      std::string bridge_error;
      const bool record_ok = Codec::Decode(payload, &record, &record_error) &&
                             Codec::Validate(record, &record_error);
      const bool bridge_ok =
          Bridge::DecodeSchedulerEventPayload(payload, &event, &bridge_error);
      REQUIRE(record_ok == bridge_ok);
      REQUIRE_FALSE(record_ok);
      REQUIRE(record_error == bridge_error);
    }
  }
}

#endif

}  // namespace test
}  // namespace cpu
}  // namespace xe
