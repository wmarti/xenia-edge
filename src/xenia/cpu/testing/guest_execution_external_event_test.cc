/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/guest_execution_session.h"

namespace xe {
namespace cpu {
namespace testing {
namespace {

using Log = GuestExecutionCaptureExternalEventLog;
using Kind = GuestExecutionCaptureExternalEventKind;
using Disposition = GuestExecutionCaptureExternalEventDisposition;
using MutationSource = GuestExecutionCaptureExternalEventMutationSource;
using Rejection = GuestExecutionCaptureExternalEventRejection;
using Begin = GuestExecutionCaptureExternalEventBegin;
using End = GuestExecutionCaptureExternalEventEnd;
using Range = GuestExecutionCaptureExternalEventEffectRange;

constexpr std::span<const uint8_t> kNoEffect = {};

GuestExecutionCaptureParticipantIdentity Participant(uint64_t instance_id,
                                                     uint32_t thread_id) {
  return {instance_id, thread_id};
}

// A range list the images cannot exactly account for must latch on begin, so
// no event ever claims guest bytes it did not snapshot.
void RequireInvalidBegin(
    const std::vector<Range>& ranges, const std::vector<uint8_t>& preimage,
    const GuestExecutionCaptureExternalEventLimits& limits = {}) {
  Log log(limits);
  Begin begin;
  begin.participant = Participant(0xB1, 0xB1B);
  begin.effect_ranges = ranges;
  REQUIRE_FALSE(log.OnExternalEventBegin(begin, preimage));
  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kInvalidBegin);
  REQUIRE(snapshot.active_calls.empty());
  REQUIRE(snapshot.events.empty());
}

}  // namespace

TEST_CASE("External event log records a canonical returned scalar",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0x11, 0x101);
  begin.kind = Kind::kKernelExport;
  begin.export_ordinal = 0x123;
  begin.call_site_address = 0x82004000;

  const GuestExecutionCaptureExternalEventToken token =
      log.OnExternalEventBegin(begin, kNoEffect);
  REQUIRE(token);
  REQUIRE_FALSE(log.CanDetach());

  End end;
  end.disposition = Disposition::kReplayCaptured;
  end.mutation_source = MutationSource::kNone;
  end.has_returned_value = true;
  end.returned_value = 0x00000000C0000034ull;
  REQUIRE(log.OnExternalEventEnd(token, end, kNoEffect));
  REQUIRE(log.CanDetach());

  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 1);
  const auto& record = snapshot.events[0];
  REQUIRE(record.sequence == 1);
  REQUIRE(record.kind == Kind::kKernelExport);
  REQUIRE(record.disposition == Disposition::kReplayCaptured);
  REQUIRE(record.export_ordinal == 0x123);
  REQUIRE(record.call_site_address == 0x82004000);
  REQUIRE(record.has_returned_value);
  const std::array<uint8_t, 8> expected = {0x34, 0x00, 0x00, 0xC0,
                                           0x00, 0x00, 0x00, 0x00};
  REQUIRE(record.returned_value_le == expected);
  REQUIRE(record.effect_ranges.empty());
  REQUIRE(record.effect_byte_count == 0);
  REQUIRE(record.preimage.empty());
  REQUIRE(record.postimage.empty());
  REQUIRE(snapshot.total_payload_bytes == 8);
}

TEST_CASE("External event log snapshots the guest buffer pre/postimage",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0x22, 0x202);
  begin.kind = Kind::kKernelExport;
  const std::vector<Range> ranges = {{0x30001000, 4}};
  begin.effect_ranges = ranges;

  const std::vector<uint8_t> preimage = {0xAA, 0xBB, 0xCC, 0xDD};
  const GuestExecutionCaptureExternalEventToken token =
      log.OnExternalEventBegin(begin, preimage);
  REQUIRE(token);

  const std::vector<uint8_t> postimage = {0x01, 0x02, 0x03, 0x04};
  End end;
  end.mutation_source = MutationSource::kActiveGuestThread;
  REQUIRE(log.OnExternalEventEnd(token, end, postimage));

  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.events.size() == 1);
  const auto& record = snapshot.events[0];
  REQUIRE(record.mutation_source == MutationSource::kActiveGuestThread);
  REQUIRE(record.effect_ranges == ranges);
  REQUIRE(record.effect_byte_count == 4);
  REQUIRE(record.preimage == preimage);
  REQUIRE(record.postimage == postimage);
  REQUIRE_FALSE(record.has_returned_value);
  REQUIRE(snapshot.total_payload_bytes == 8);
}

TEST_CASE("External event log snapshots disjoint guest effect ranges",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0x23, 0x203);
  begin.kind = Kind::kKernelExport;
  begin.call_site_address = 0x82081740;
  // One modeled blocking export writes the guest thread structure, a waited
  // dispatch header and an output status word, which are not contiguous.
  const std::vector<Range> ranges = {
      {0x30001000, 4}, {0x30002000, 2}, {0x30002010, 1}};
  begin.effect_ranges = ranges;

  const std::vector<uint8_t> preimage = {0xAA, 0xBB, 0xCC, 0xDD,
                                         0x10, 0x11, 0x20};
  const GuestExecutionCaptureExternalEventToken token =
      log.OnExternalEventBegin(begin, preimage);
  REQUIRE(token);

  const GuestExecutionCaptureExternalEventSnapshot open = log.snapshot();
  REQUIRE(open.active_calls.size() == 1);
  REQUIRE(open.active_calls[0].effect_ranges == ranges);
  REQUIRE(open.active_calls[0].effect_byte_count == 7);

  const std::vector<uint8_t> postimage = {0x01, 0x02, 0x03, 0x04,
                                          0x12, 0x13, 0x21};
  End end;
  end.mutation_source = MutationSource::kActiveGuestThread;
  end.has_returned_value = true;
  end.returned_value = 0;
  REQUIRE(log.OnExternalEventEnd(token, end, postimage));

  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 1);
  const auto& record = snapshot.events[0];
  REQUIRE(record.call_site_address == 0x82081740);
  REQUIRE(record.effect_ranges == ranges);
  REQUIRE(record.effect_byte_count == 7);
  REQUIRE(record.preimage == preimage);
  REQUIRE(record.postimage == postimage);
  // Range order is payload order: the second range's bytes follow the first
  // range's four, with no padding between regions.
  REQUIRE(record.postimage[4] == 0x12);
  REQUIRE(record.postimage[6] == 0x21);
  REQUIRE(snapshot.total_payload_bytes == 7 * 2 + 8);
}

TEST_CASE("External event log rejects a non-canonical effect range list",
          "[guest-external-event]") {
  SECTION("ranges overlap") {
    RequireInvalidBegin({{0x30001000, 8}, {0x30001004, 4}},
                        std::vector<uint8_t>(12, 0x00));
  }
  SECTION("ranges abut") {
    RequireInvalidBegin({{0x30001000, 4}, {0x30001004, 4}},
                        std::vector<uint8_t>(8, 0x00));
  }
  SECTION("ranges descend") {
    RequireInvalidBegin({{0x30002000, 4}, {0x30001000, 4}},
                        std::vector<uint8_t>(8, 0x00));
  }
  SECTION("a range is empty") { RequireInvalidBegin({{0x30001000, 0}}, {}); }
  SECTION("a range wraps the guest address space") {
    RequireInvalidBegin({{0xFFFFFFFC, 8}}, std::vector<uint8_t>(8, 0x00));
  }
  SECTION("the preimage is shorter than the declared bytes") {
    RequireInvalidBegin({{0x30001000, 4}}, std::vector<uint8_t>(3, 0x00));
  }
  SECTION("the preimage carries undeclared bytes") {
    RequireInvalidBegin({}, std::vector<uint8_t>(4, 0x00));
  }
  SECTION("ranges exceed the per-event range limit") {
    GuestExecutionCaptureExternalEventLimits limits;
    limits.max_effect_ranges = 2;
    RequireInvalidBegin({{0x30001000, 1}, {0x30001002, 1}, {0x30001004, 1}},
                        std::vector<uint8_t>(3, 0x00), limits);
  }
}

TEST_CASE("External event log rejects mislabeled mutation sources",
          "[guest-external-event]") {
  SECTION("effect present but labeled kNone") {
    Log log;
    Begin begin;
    begin.participant = Participant(0x33, 0x303);
    const std::vector<Range> ranges = {{0x30001000, 2}};
    begin.effect_ranges = ranges;
    const std::vector<uint8_t> preimage = {0x00, 0x11};
    const auto token = log.OnExternalEventBegin(begin, preimage);
    REQUIRE(token);
    const std::vector<uint8_t> postimage = {0x22, 0x33};
    End end;
    end.mutation_source = MutationSource::kNone;
    REQUIRE_FALSE(log.OnExternalEventEnd(token, end, postimage));
    REQUIRE(log.snapshot().rejection == Rejection::kInvalidEnd);
  }
  SECTION("no effect but labeled active guest thread") {
    Log log;
    Begin begin;
    begin.participant = Participant(0x33, 0x303);
    const auto token = log.OnExternalEventBegin(begin, kNoEffect);
    REQUIRE(token);
    End end;
    end.mutation_source = MutationSource::kActiveGuestThread;
    REQUIRE_FALSE(log.OnExternalEventEnd(token, end, kNoEffect));
    REQUIRE(log.snapshot().rejection == Rejection::kInvalidEnd);
  }
  SECTION("postimage length disagrees with preimage") {
    Log log;
    Begin begin;
    begin.participant = Participant(0x33, 0x303);
    const std::vector<Range> ranges = {{0x30001000, 3}};
    begin.effect_ranges = ranges;
    const std::vector<uint8_t> preimage = {0x00, 0x11, 0x22};
    const auto token = log.OnExternalEventBegin(begin, preimage);
    REQUIRE(token);
    const std::vector<uint8_t> postimage = {0x22, 0x33};
    End end;
    end.mutation_source = MutationSource::kActiveGuestThread;
    REQUIRE_FALSE(log.OnExternalEventEnd(token, end, postimage));
    REQUIRE(log.snapshot().rejection == Rejection::kInvalidEnd);
  }
}

TEST_CASE("External event log pairs nested reentry last-in-first-out",
          "[guest-external-event]") {
  Log log;
  const auto participant = Participant(0x44, 0x404);

  Begin outer;
  outer.participant = participant;
  outer.export_ordinal = 0xA0;
  const auto outer_token = log.OnExternalEventBegin(outer, kNoEffect);
  REQUIRE(outer_token);

  Begin inner;
  inner.participant = participant;
  inner.export_ordinal = 0xB0;
  const auto inner_token = log.OnExternalEventBegin(inner, kNoEffect);
  REQUIRE(inner_token);

  const GuestExecutionCaptureExternalEventSnapshot open = log.snapshot();
  REQUIRE(open.active_calls.size() == 2);
  REQUIRE(open.active_calls[0].participant_depth == 1);
  REQUIRE(open.active_calls[0].is_outermost());
  REQUIRE(open.active_calls[1].participant_depth == 2);
  REQUIRE_FALSE(open.active_calls[1].is_outermost());

  End end;
  end.mutation_source = MutationSource::kNone;
  // Closing the outer token first is an out-of-order end and fails closed.
  REQUIRE_FALSE(log.OnExternalEventEnd(outer_token, end, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidEnd);
}

TEST_CASE("External event log interleaves concurrent participants",
          "[guest-external-event]") {
  Log log;
  Begin first;
  first.participant = Participant(0x55, 0x505);
  first.export_ordinal = 0xC0;
  const auto first_token = log.OnExternalEventBegin(first, kNoEffect);
  REQUIRE(first_token);

  Begin second;
  second.participant = Participant(0x66, 0x606);
  second.export_ordinal = 0xD0;
  const auto second_token = log.OnExternalEventBegin(second, kNoEffect);
  REQUIRE(second_token);

  // Two different participants may close in either order.
  End end;
  end.mutation_source = MutationSource::kNone;
  REQUIRE(log.OnExternalEventEnd(first_token, end, kNoEffect));
  REQUIRE(log.OnExternalEventEnd(second_token, end, kNoEffect));

  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 2);
  REQUIRE(snapshot.events[0].participant == first.participant);
  REQUIRE(snapshot.events[0].sequence == 1);
  REQUIRE(snapshot.events[1].participant == second.participant);
  REQUIRE(snapshot.events[1].sequence == 2);
  REQUIRE(log.CanDetach());
}

TEST_CASE("External event log rejects an unknown or reused end token",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0x77, 0x707);
  const auto token = log.OnExternalEventBegin(begin, kNoEffect);
  REQUIRE(token);

  End end;
  end.mutation_source = MutationSource::kNone;
  REQUIRE(log.OnExternalEventEnd(token, end, kNoEffect));

  // The token is spent; ending again latches a rejection.
  REQUIRE_FALSE(log.OnExternalEventEnd(token, end, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidEnd);
}

TEST_CASE("External event log rejects an invalid participant on begin",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0, 0x808);
  REQUIRE_FALSE(log.OnExternalEventBegin(begin, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidBegin);
}

TEST_CASE("External event log rejects a kind outside the synchronous class",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0x81, 0x818);
  begin.kind = Kind::kMmio;
  REQUIRE_FALSE(log.OnExternalEventBegin(begin, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidBegin);
}

TEST_CASE("External event log records durable session enumeration values",
          "[guest-external-event]") {
  // The recorded numbers are the session codec's, not a private renumbering.
  REQUIRE(static_cast<uint32_t>(Kind::kKernelExport) ==
          static_cast<uint32_t>(GuestExecutionSessionEventKind::kKernelExport));
  REQUIRE(
      static_cast<uint32_t>(Kind::kExternOrBuiltin) ==
      static_cast<uint32_t>(GuestExecutionSessionEventKind::kExternOrBuiltin));
  REQUIRE(static_cast<uint32_t>(Disposition::kRejectSession) ==
          static_cast<uint32_t>(
              GuestExecutionSessionEventDisposition::kRejectSession));
  REQUIRE(static_cast<uint32_t>(MutationSource::kActiveGuestThread) ==
          static_cast<uint32_t>(
              GuestExecutionSessionMutationSource::kActiveGuestThread));
  // A default-constructed record is distinguishable from a kernel export.
  REQUIRE(static_cast<uint32_t>(Kind::kKernelExport) != 0);
}

TEST_CASE("External event log stays detachable after a latched rejection",
          "[guest-external-event]") {
  Log log;
  Begin open;
  open.participant = Participant(0x91, 0x919);
  const auto token = log.OnExternalEventBegin(open, kNoEffect);
  REQUIRE(token);
  REQUIRE_FALSE(log.CanDetach());

  Begin invalid;
  invalid.participant = Participant(0, 0);
  REQUIRE_FALSE(log.OnExternalEventBegin(invalid, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidBegin);

  // The open call can never close now, so the owner must still be released.
  REQUIRE(log.CanDetach());
  End end;
  end.mutation_source = MutationSource::kNone;
  REQUIRE_FALSE(log.OnExternalEventEnd(token, end, kNoEffect));
  REQUIRE(log.CanDetach());
}

TEST_CASE("External event log marks a reject-session event stickily",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0xA1, 0xA1A);
  End captured;
  captured.disposition = Disposition::kReplayCaptured;
  captured.mutation_source = MutationSource::kNone;

  const auto first = log.OnExternalEventBegin(begin, kNoEffect);
  REQUIRE(first);
  REQUIRE(log.OnExternalEventEnd(first, captured, kNoEffect));
  REQUIRE(log.snapshot().replayable());

  End rejected;
  rejected.disposition = Disposition::kRejectSession;
  rejected.mutation_source = MutationSource::kNone;
  const auto second = log.OnExternalEventBegin(begin, kNoEffect);
  REQUIRE(second);
  // The event is still recorded as durable diagnostic metadata.
  REQUIRE(log.OnExternalEventEnd(second, rejected, kNoEffect));

  const auto third = log.OnExternalEventBegin(begin, kNoEffect);
  REQUIRE(third);
  REQUIRE(log.OnExternalEventEnd(third, captured, kNoEffect));

  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.size() == 3);
  REQUIRE(snapshot.events[1].disposition == Disposition::kRejectSession);
  REQUIRE(snapshot.reject_session_count == 1);
  REQUIRE(snapshot.first_reject_session_sequence == 2);
  REQUIRE_FALSE(snapshot.replayable());
}

TEST_CASE("External event log bounds effect size, buffering and payload",
          "[guest-external-event]") {
  SECTION("per-event effect exceeds the byte limit") {
    GuestExecutionCaptureExternalEventLimits limits;
    limits.max_effect_bytes = 4;
    Log log(limits);
    Begin begin;
    begin.participant = Participant(0x99, 0x909);
    const std::vector<Range> ranges = {{0x30001000, 5}};
    begin.effect_ranges = ranges;
    const std::vector<uint8_t> preimage(5, 0x00);
    REQUIRE_FALSE(log.OnExternalEventBegin(begin, preimage));
    REQUIRE(log.snapshot().rejection == Rejection::kInvalidBegin);
  }
  SECTION("open calls exceed the active-call limit") {
    GuestExecutionCaptureExternalEventLimits limits;
    limits.max_active_calls = 1;
    Log log(limits);
    Begin begin;
    begin.participant = Participant(0x99, 0x909);
    REQUIRE(log.OnExternalEventBegin(begin, kNoEffect));
    REQUIRE_FALSE(log.OnExternalEventBegin(begin, kNoEffect));
    REQUIRE(log.snapshot().rejection == Rejection::kActiveCallLimit);
  }
  SECTION("recorded events exceed the event limit") {
    GuestExecutionCaptureExternalEventLimits limits;
    limits.max_recorded_events = 1;
    Log log(limits);
    Begin begin;
    begin.participant = Participant(0x99, 0x909);
    End end;
    end.mutation_source = MutationSource::kNone;
    const auto first = log.OnExternalEventBegin(begin, kNoEffect);
    REQUIRE(first);
    REQUIRE(log.OnExternalEventEnd(first, end, kNoEffect));
    const auto second = log.OnExternalEventBegin(begin, kNoEffect);
    REQUIRE(second);
    REQUIRE_FALSE(log.OnExternalEventEnd(second, end, kNoEffect));
    REQUIRE(log.snapshot().rejection == Rejection::kEventLimit);
  }
  SECTION("total payload exceeds the byte limit") {
    GuestExecutionCaptureExternalEventLimits limits;
    limits.max_total_payload_bytes = 4;
    Log log(limits);
    Begin begin;
    begin.participant = Participant(0x99, 0x909);
    const std::vector<Range> ranges = {{0x40000000, 3}};
    begin.effect_ranges = ranges;
    const std::vector<uint8_t> preimage = {0x00, 0x11, 0x22};
    const std::vector<uint8_t> postimage = {0x33, 0x44, 0x55};
    const auto token = log.OnExternalEventBegin(begin, preimage);
    REQUIRE(token);
    End end;
    end.mutation_source = MutationSource::kActiveGuestThread;
    REQUIRE_FALSE(log.OnExternalEventEnd(token, end, postimage));
    REQUIRE(log.snapshot().rejection == Rejection::kPayloadLimit);
  }
}

TEST_CASE("External event log latches the first rejection permanently",
          "[guest-external-event]") {
  Log log;
  Begin begin;
  begin.participant = Participant(0, 0);
  REQUIRE_FALSE(log.OnExternalEventBegin(begin, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidBegin);

  // A well-formed begin after a latched rejection still returns no token.
  Begin valid;
  valid.participant = Participant(0xAB, 0xABC);
  REQUIRE_FALSE(log.OnExternalEventBegin(valid, kNoEffect));
  REQUIRE(log.snapshot().rejection == Rejection::kInvalidBegin);
  REQUIRE(log.snapshot().events.empty());
}

TEST_CASE("External event log starts idle and detachable",
          "[guest-external-event]") {
  Log log;
  const GuestExecutionCaptureExternalEventSnapshot snapshot = log.snapshot();
  REQUIRE(snapshot.rejection == Rejection::kNone);
  REQUIRE(snapshot.events.empty());
  REQUIRE(snapshot.active_calls.empty());
  REQUIRE(snapshot.recorded_event_count == 0);
  REQUIRE(snapshot.total_payload_bytes == 0);
  REQUIRE(snapshot.reject_session_count == 0);
  REQUIRE(snapshot.first_reject_session_sequence == 0);
  REQUIRE(snapshot.replayable());
  REQUIRE(log.CanDetach());
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
