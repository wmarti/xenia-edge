/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_scheduler_record.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#endif

namespace xe {
namespace cpu {
namespace test {

namespace {

using Codec = GuestExecutionSessionCodec;
using Disposition = GuestExecutionSessionSignalWitnessDisposition;
using Source = GuestExecutionSessionSignalWitnessSource;

constexpr uint64_t kEpoch = 0x123456789ABCDEF0ull;
constexpr uint64_t kInstanceId = 0x1001;
constexpr uint32_t kThreadId = 0x101;
constexpr uint32_t kHandle = 0x2000;

void WriteU32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  REQUIRE(offset + 4 <= bytes->size());
  for (size_t i = 0; i < 4; ++i) {
    (*bytes)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

// The envelope binds the payload digest twice, so a payload edit that is meant
// to reach the payload decoder has to restore both.
void RewritePayloadHash(std::vector<uint8_t>* bytes) {
  const size_t payload_offset = Codec::kEnvelopeHeaderSize;
  const size_t payload_size =
      bytes->size() - Codec::kEnvelopeHeaderSize - Codec::kEnvelopeFooterSize;
  const GuestExecutionSessionSha256 digest =
      Codec::HashBytes(bytes->data() + payload_offset, payload_size);
  std::copy(digest.cbegin(), digest.cend(), bytes->begin() + 64);
  const size_t footer_offset = bytes->size() - Codec::kEnvelopeFooterSize;
  std::copy(digest.cbegin(), digest.cend(),
            bytes->begin() + footer_offset + 64);
}

GuestExecutionSessionSignalWitness Witness(uint64_t after_scheduler_sequence,
                                           uint32_t object_handle,
                                           uint32_t signal_epoch) {
  GuestExecutionSessionSignalWitness witness;
  witness.after_scheduler_sequence = after_scheduler_sequence;
  witness.capture_instance_id = kInstanceId;
  witness.guest_thread_id = kThreadId;
  witness.object_handle = object_handle;
  witness.signal_epoch = signal_epoch;
  witness.object_type = 2;
  witness.source = Source::kParticipant;
  return witness;
}

GuestExecutionSessionSignalWitnessChunk Fixture() {
  GuestExecutionSessionSignalWitnessChunk chunk;
  chunk.session_epoch = kEpoch;
  chunk.ordinal = 6;
  chunk.witnesses.push_back(Witness(0, kHandle, 1));
  GuestExecutionSessionSignalWitness off_roster = Witness(3, kHandle + 4, 7);
  off_roster.capture_instance_id = 0;
  off_roster.guest_thread_id = 0x999;
  off_roster.source = Source::kOffRoster;
  chunk.witnesses.push_back(off_roster);
  GuestExecutionSessionSignalWitness host = Witness(3, kHandle + 8, 2);
  host.capture_instance_id = 0;
  host.guest_thread_id = 0;
  host.source = Source::kHost;
  chunk.witnesses.push_back(host);
  return chunk;
}

std::vector<GuestExecutionSessionParticipant> Roster() {
  GuestExecutionSessionParticipant participant;
  participant.ordinal = 0;
  participant.guest_thread_id = kThreadId;
  participant.capture_instance_id = kInstanceId;
  participant.initial_state_size = 8;
  participant.initial_state_sha256 = {1};
  return {participant};
}

DecodedSchedulerRecord SignalEpochWake(uint64_t sequence, uint32_t handle,
                                       uint32_t before, uint32_t observed) {
  DecodedSchedulerRecord wake;
  wake.sequence = sequence;
  wake.capture_instance_id = kInstanceId;
  wake.guest_thread_id = kThreadId;
  wake.kind = GuestSchedulerCaptureEventKind::kReready;
  wake.reason = GuestSchedulerCaptureReason::kSignalEpoch;
  wake.cpu = 0;
  wake.target_cpu = 0;
  wake.value = static_cast<uint8_t>(GuestSchedulerCaptureWaitKind::kSingle);
  wake.wait.handle_count = 1;
  wake.wait.flags = kGuestSchedulerCaptureWaitFlagGated |
                    kGuestSchedulerCaptureWaitFlagInterruptible;
  wake.wait.handles[0] = handle;
  wake.wait.signal_epochs_before[0] = before;
  wake.wait.signal_epochs_observed[0] = observed;
  wake.wait.wait_epoch = before;
  wake.wait.observed_wait_epoch = observed;
  return wake;
}

}  // namespace

TEST_CASE("signal witness table round-trips through its chunk codec",
          "[guest-execution-signal-witness]") {
  std::string error;
  std::vector<uint8_t> encoded;
  GuestExecutionSessionSignalWitnessChunk decoded;

  SECTION("a table of every source round-trips byte for byte") {
    const GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    REQUIRE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error.empty());
    REQUIRE(encoded.size() == Codec::kEnvelopeHeaderSize +
                                  Codec::kEnvelopeFooterSize +
                                  Codec::kSignalWitnessPayloadHeaderSize +
                                  3 * Codec::kSignalWitnessRecordSize);
    REQUIRE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(decoded == chunk);
    std::vector<uint8_t> reencoded;
    REQUIRE(Codec::EncodeSignalWitnessChunk(decoded, &reencoded, &error));
    REQUIRE(reencoded == encoded);
  }

  SECTION("an interval that witnessed no signal still publishes its table") {
    GuestExecutionSessionSignalWitnessChunk chunk;
    chunk.session_epoch = kEpoch;
    chunk.ordinal = 6;
    REQUIRE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(decoded == chunk);
    REQUIRE(decoded.witnesses.empty());
  }
}

TEST_CASE("signal witness chunk refuses an envelope it cannot read",
          "[guest-execution-signal-witness]") {
  std::string error;
  std::vector<uint8_t> encoded;
  GuestExecutionSessionSignalWitnessChunk decoded;
  REQUIRE(Codec::EncodeSignalWitnessChunk(Fixture(), &encoded, &error));
  const size_t payload = Codec::kEnvelopeHeaderSize;

  SECTION("an unknown payload version is refused by name") {
    WriteU32(&encoded, payload + 8, Codec::kSignalWitnessVersion + 1);
    RewritePayloadHash(&encoded);
    REQUIRE_FALSE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(error == "signal witness payload version is unsupported");
  }

  SECTION("a resized record is refused by name") {
    WriteU32(&encoded, payload + 16, Codec::kSignalWitnessRecordSize + 8);
    RewritePayloadHash(&encoded);
    REQUIRE_FALSE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(error == "signal witness payload version is unsupported");
  }

  SECTION("a used header reserved field is refused by name") {
    WriteU32(&encoded, payload + 24, 1);
    RewritePayloadHash(&encoded);
    REQUIRE_FALSE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(error == "signal witness payload version is unsupported");
  }

  SECTION("a used record reserved field is refused by name") {
    WriteU32(&encoded, payload + Codec::kSignalWitnessPayloadHeaderSize + 36,
             1);
    RewritePayloadHash(&encoded);
    REQUIRE_FALSE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(error == "signal witness record is truncated");
  }

  SECTION("a truncated table is refused rather than sliced") {
    WriteU32(&encoded, payload + 20, 2);
    RewritePayloadHash(&encoded);
    REQUIRE_FALSE(Codec::DecodeSignalWitnessChunk(encoded, &decoded, &error));
    REQUIRE(error == "signal witness payload byte count is invalid");
  }
}

TEST_CASE("signal witness table refuses provenance it cannot mean",
          "[guest-execution-signal-witness]") {
  std::string error;
  std::vector<uint8_t> encoded;

  SECTION("a table with no session identity cannot be encoded") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.session_epoch = 0;
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness envelope is invalid");
  }

  SECTION("a table beyond the configured limit is refused, not sliced") {
    GuestExecutionSessionLimits limits;
    limits.maximum_signal_witnesses = 2;
    REQUIRE_FALSE(
        Codec::EncodeSignalWitnessChunk(Fixture(), &encoded, &error, limits));
    REQUIRE(error == "signal witness envelope is invalid");
  }

  SECTION("an unsignalled object cannot be a witness") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.witnesses[0].signal_epoch = 0;
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness provenance is invalid");
  }

  SECTION("an unknown source is refused") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.witnesses[0].source = static_cast<Source>(9);
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness provenance is invalid");
  }

  SECTION("a host signal that names a guest thread is refused") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.witnesses[2].guest_thread_id = kThreadId;
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness identity contradicts its source");
  }

  SECTION("a participant signal without a capture instance is refused") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.witnesses[0].capture_instance_id = 0;
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness identity contradicts its source");
  }

  SECTION("an anchor that moves backwards is refused") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.witnesses[2].after_scheduler_sequence = 1;
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness table is not in tape order");
  }

  SECTION("one object cannot reach the same epoch twice") {
    GuestExecutionSessionSignalWitnessChunk chunk = Fixture();
    chunk.witnesses[1].object_handle = kHandle;
    chunk.witnesses[1].signal_epoch = 1;
    REQUIRE_FALSE(Codec::EncodeSignalWitnessChunk(chunk, &encoded, &error));
    REQUIRE(error == "signal witness repeats an object signal epoch");
  }
}

TEST_CASE("signal-epoch wake authorization names why a wake has no provenance",
          "[guest-execution-signal-witness]") {
  const std::vector<GuestExecutionSessionParticipant> roster = Roster();
  const DecodedSchedulerRecord wake = SignalEpochWake(10, kHandle, 4, 5);
  const GuestExecutionSessionSignalWitness* authorizing = nullptr;
  uint32_t signaller = 0;

  SECTION("a roster signal inside the transition and before the wake") {
    const std::vector<GuestExecutionSessionSignalWitness> witnesses = {
        Witness(3, kHandle, 5)};
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, witnesses, &authorizing, &signaller) ==
            Disposition::kAuthorized);
    REQUIRE(authorizing == &witnesses.front());
    REQUIRE(signaller == 0);
  }

  SECTION("no recorded signal moved an epoch this wake watched") {
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(wake, roster, {}) ==
            Disposition::kMissing);
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {Witness(3, kHandle + 4, 5)}) ==
            Disposition::kMissing);
    // The transition is (4, 5]: neither end-exclusive epoch covers it.
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {Witness(3, kHandle, 4)}) ==
            Disposition::kMissing);
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {Witness(3, kHandle, 6)}) ==
            Disposition::kMissing);
  }

  SECTION("a signal the cursor has not passed cannot authorize the wake") {
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {Witness(10, kHandle, 5)}) ==
            Disposition::kNotEarlier);
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {Witness(11, kHandle, 5)}) ==
            Disposition::kNotEarlier);
  }

  SECTION("the latest covering signal decides, not the first") {
    const std::vector<GuestExecutionSessionSignalWitness> witnesses = {
        Witness(3, kHandle, 5), Witness(10, kHandle, 6)};
    const DecodedSchedulerRecord wide = SignalEpochWake(10, kHandle, 4, 6);
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wide, roster, witnesses) == Disposition::kNotEarlier);
  }

  SECTION("a signal outside the roster carries no re-enactable provenance") {
    GuestExecutionSessionSignalWitness off_roster = Witness(3, kHandle, 5);
    off_roster.capture_instance_id = 0;
    off_roster.guest_thread_id = 0x999;
    off_roster.source = Source::kOffRoster;
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {off_roster}, &authorizing, &signaller) ==
            Disposition::kUnrostered);
    REQUIRE(authorizing != nullptr);
    REQUIRE(signaller == kGuestExecutionSessionNoThread);

    GuestExecutionSessionSignalWitness claimed = Witness(3, kHandle, 5);
    claimed.guest_thread_id = 0x999;
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {claimed}) == Disposition::kUnrostered);

    GuestExecutionSessionSignalWitness host = Witness(3, kHandle, 5);
    host.capture_instance_id = 0;
    host.guest_thread_id = 0;
    host.source = Source::kHost;
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                wake, roster, {host}) == Disposition::kUnrostered);
  }

  SECTION("records that are not signal-epoch wakes need no witness") {
    DecodedSchedulerRecord other = wake;
    other.reason = GuestSchedulerCaptureReason::kDeadline;
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                other, roster, {Witness(3, kHandle, 5)}) ==
            Disposition::kNotSignalEpoch);
    other = wake;
    other.kind = GuestSchedulerCaptureEventKind::kBlock;
    other.reason = GuestSchedulerCaptureReason::kNone;
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                other, roster, {Witness(3, kHandle, 5)}) ==
            Disposition::kNotSignalEpoch);
  }

  SECTION("a multi-object wait is authorized by the handle that moved") {
    DecodedSchedulerRecord multi = wake;
    multi.value =
        static_cast<uint8_t>(GuestSchedulerCaptureWaitKind::kMultiAny);
    multi.wait.handle_count = 2;
    multi.wait.handles[1] = kHandle + 4;
    multi.wait.signal_epochs_before[1] = 9;
    multi.wait.signal_epochs_observed[1] = 9;
    multi.wait.wait_epoch = 4 + 9;
    multi.wait.observed_wait_epoch = 5 + 9;
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                multi, roster, {Witness(3, kHandle + 4, 9)}) ==
            Disposition::kMissing);
    REQUIRE(GuestExecutionSessionAuthorizeSignalEpochWake(
                multi, roster, {Witness(3, kHandle, 5)}) ==
            Disposition::kAuthorized);
  }
}

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

namespace {

kernel::GuestSchedulerCaptureEvent DispatchEvent(uint64_t sequence) {
  kernel::GuestSchedulerCaptureEvent event;
  event.sequence = sequence;
  event.capture_instance_id = kInstanceId;
  event.guest_thread_id = kThreadId;
  event.kind = kernel::GuestSchedulerCaptureEventKind::kDispatch;
  event.cpu = 0;
  return event;
}

kernel::GuestSchedulerCaptureSignalWitness CaptureWitness(uint64_t anchor,
                                                          uint32_t epoch) {
  kernel::GuestSchedulerCaptureSignalWitness witness;
  witness.after_scheduler_sequence = anchor;
  witness.capture_instance_id = kInstanceId;
  witness.guest_thread_id = kThreadId;
  witness.object_handle = kHandle;
  witness.signal_epoch = epoch;
  witness.object_type = 2;
  return witness;
}

}  // namespace

TEST_CASE("scheduler capture recorder anchors signal provenance to the tape",
          "[guest-execution-signal-witness]") {
  kernel::GuestSchedulerCaptureEventRecorder recorder(8);
  REQUIRE(recorder.Arm());

  SECTION("a witness records against the sequence it was delivered at") {
    REQUIRE(recorder.OnSchedulerSignalWitness(CaptureWitness(0, 1)));
    REQUIRE(recorder.OnSchedulerEvent(DispatchEvent(1)));
    REQUIRE(recorder.OnSchedulerSignalWitness(CaptureWitness(1, 2)));
    REQUIRE(recorder.OnSchedulerEvent(DispatchEvent(2)));
    const auto snapshot = recorder.snapshot();
    REQUIRE(snapshot.rejection ==
            kernel::GuestSchedulerCaptureRecorderRejection::kNone);
    REQUIRE(snapshot.events.size() == 2);
    REQUIRE(snapshot.signal_witnesses.size() == 2);
    REQUIRE(snapshot.signal_witnesses[0].after_scheduler_sequence == 0);
    REQUIRE(snapshot.signal_witnesses[1].after_scheduler_sequence == 1);
  }

  SECTION("an anchor ahead of the tape position is refused") {
    REQUIRE(recorder.OnSchedulerEvent(DispatchEvent(1)));
    REQUIRE_FALSE(recorder.OnSchedulerSignalWitness(CaptureWitness(2, 1)));
    REQUIRE(
        recorder.snapshot().rejection ==
        kernel::GuestSchedulerCaptureRecorderRejection::kInvalidSignalWitness);
  }

  SECTION("an unsignalled object is refused") {
    REQUIRE_FALSE(recorder.OnSchedulerSignalWitness(CaptureWitness(0, 0)));
    REQUIRE(
        recorder.snapshot().rejection ==
        kernel::GuestSchedulerCaptureRecorderRejection::kInvalidSignalWitness);
  }
}

#endif

}  // namespace test
}  // namespace cpu
}  // namespace xe
