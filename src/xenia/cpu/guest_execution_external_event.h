/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_EXTERNAL_EVENT_H_
#define XENIA_CPU_GUEST_EXECUTION_EXTERNAL_EVENT_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/guest_execution_session.h"

namespace xe {
namespace cpu {

// The session codec enumerations are reused directly so every recorded value
// is the durable one and no parallel numbering can drift.
//
// Kind: only kKernelExport and kExternOrBuiltin are accepted. Both are
// synchronous host work invoked by the active guest thread on its own call
// stack. Asynchronous classes (scheduler, MMIO, clocks, interrupts, atomics,
// GPU/DMA/host and other-thread writes) are out of scope here and belong to
// sibling adapters.
using GuestExecutionCaptureExternalEventKind = GuestExecutionSessionEventKind;

// Disposition: explicit for every event so no nondeterministic result is
// silently trusted or ignored. A kRejectSession event is still recorded as
// durable diagnostic metadata, and the snapshot's sticky reject-session
// indicator marks the whole log non-replayable.
using GuestExecutionCaptureExternalEventDisposition =
    GuestExecutionSessionEventDisposition;

// Mutation source: only kNone and kActiveGuestThread are accepted. A
// synchronous export runs on behalf of the calling guest thread, so its
// guest-memory writes belong to that thread; GPU, DMA, host and
// other-guest-thread sources are rejected so an active-guest store can never be
// mislabeled as an asynchronous one.
using GuestExecutionCaptureExternalEventMutationSource =
    GuestExecutionSessionMutationSource;

// Runtime-local pairing metadata for one begin/end span. Never a durable
// session sequence.
struct GuestExecutionCaptureExternalEventToken {
  uint64_t value = 0;

  explicit operator bool() const { return value != 0; }
  bool operator==(const GuestExecutionCaptureExternalEventToken&) const =
      default;
};

// Bounds on buffering and per-event payload. Every limit rejects the log
// instead of silently truncating.
struct GuestExecutionCaptureExternalEventLimits {
  uint64_t max_active_calls = 256;
  uint64_t max_recorded_events = 1u << 20;
  // Ranges per event, and bytes summed across them.
  uint32_t max_effect_ranges = 16;
  uint32_t max_effect_bytes = 64u * 1024u;
  uint64_t max_total_payload_bytes = 64ull * 1024ull * 1024ull;
};

// One contiguous guest-memory region an external event declares it writes. A
// range list is canonical: every range is non-empty, the list ascends by
// address, and no two ranges overlap or abut, so one declared write region has
// exactly one representation and two records compare field by field.
struct GuestExecutionCaptureExternalEventEffectRange {
  uint32_t address = 0;
  uint32_t byte_count = 0;

  bool operator==(const GuestExecutionCaptureExternalEventEffectRange&) const =
      default;
};

// Post-return volatile register state, in this order: r0, r3-r12, ctr, cr.
// The PPC ABI lets a callee clobber these, so a caller resumed at the return
// address must not read them; but that is a property of well-formed caller
// code, not a guarantee the recording can make, so the record carries the
// values the dispatch actually left rather than trusting title code.
inline constexpr size_t kGuestExecutionCaptureExternalEventVolatileRegisters =
    13;
inline constexpr size_t
    kGuestExecutionCaptureExternalEventVolatileRegisterBytes =
        kGuestExecutionCaptureExternalEventVolatileRegisters * 8;

struct GuestExecutionCaptureExternalEventVolatileRegisterState {
  std::array<uint64_t, kGuestExecutionCaptureExternalEventVolatileRegisters>
      values = {};

  bool operator==(
      const GuestExecutionCaptureExternalEventVolatileRegisterState&) const =
      default;
};

// Opens one external event. The declared guest-memory effect is a list of
// canonical ranges the export is expected to write; their exact preimage bytes
// are supplied separately, concatenated in range order, and snapshotted before
// the export body runs. An empty range list means the event declares no
// guest-memory effect. The ranges stay separate rather than collapsing to one
// hull because a modeled blocking export writes the guest thread structure,
// each waited dispatch header and an output status word, and a hull would
// claim bytes the export never wrote.
struct GuestExecutionCaptureExternalEventBegin {
  GuestExecutionCaptureParticipantIdentity participant;
  GuestExecutionCaptureExternalEventKind kind =
      GuestExecutionCaptureExternalEventKind::kKernelExport;
  // Stable export identity within its module; zero when none applies.
  uint32_t export_ordinal = 0;
  // Guest address the call site branched to, PPC-aligned. Zero when the
  // dispatch could not be attributed to one, and no replay route can bind to
  // a record that carries no address.
  uint32_t guest_address = 0;
  // Guest return address at the dispatch boundary, for provenance only.
  uint32_t call_site_address = 0;
  // Declared guest-memory effect ranges; the preimage span is exactly their
  // byte counts concatenated in this order.
  std::span<const GuestExecutionCaptureExternalEventEffectRange> effect_ranges;
};

// Closes one external event with its observed result and postimage. The
// postimage span must be empty when the begin declared no effect, and
// otherwise exactly as long as the preimage, in the same range order.
// mutation_source must be kActiveGuestThread exactly when an effect is
// present. The volatile register state is optional; a record without it
// carries no claim about what the dispatch left in those registers.
struct GuestExecutionCaptureExternalEventEnd {
  GuestExecutionCaptureExternalEventDisposition disposition =
      GuestExecutionCaptureExternalEventDisposition::kReplayCaptured;
  GuestExecutionCaptureExternalEventMutationSource mutation_source =
      GuestExecutionCaptureExternalEventMutationSource::kNone;
  bool has_returned_value = false;
  // Raw return register value; canonicalized to little-endian in the record.
  uint64_t returned_value = 0;
  bool has_volatile_registers = false;
  GuestExecutionCaptureExternalEventVolatileRegisterState volatile_registers;
};

// One finished external event. Payload bytes are canonical: the return value
// is little-endian, and the guest-memory pre/postimages are the exact guest
// bytes for the recorded ranges, concatenated in range order.
struct GuestExecutionCaptureExternalEventRecord {
  uint64_t sequence = 0;
  GuestExecutionCaptureParticipantIdentity participant;
  GuestExecutionCaptureExternalEventKind kind =
      GuestExecutionCaptureExternalEventKind::kKernelExport;
  GuestExecutionCaptureExternalEventDisposition disposition =
      GuestExecutionCaptureExternalEventDisposition::kReplayCaptured;
  GuestExecutionCaptureExternalEventMutationSource mutation_source =
      GuestExecutionCaptureExternalEventMutationSource::kNone;
  uint32_t export_ordinal = 0;
  uint32_t guest_address = 0;
  uint32_t call_site_address = 0;
  bool has_returned_value = false;
  std::array<uint8_t, 8> returned_value_le = {};
  bool has_volatile_registers = false;
  std::array<uint8_t, kGuestExecutionCaptureExternalEventVolatileRegisterBytes>
      volatile_registers_le = {};
  std::vector<GuestExecutionCaptureExternalEventEffectRange> effect_ranges;
  // Summed range bytes, which is also each image's length.
  uint32_t effect_byte_count = 0;
  std::vector<uint8_t> preimage;
  std::vector<uint8_t> postimage;

  bool operator==(const GuestExecutionCaptureExternalEventRecord&) const =
      default;
};

enum class GuestExecutionCaptureExternalEventRejection : uint8_t {
  kNone,
  kInvalidBegin,
  kInvalidEnd,
  kAllocationFailure,
  kTokenOverflow,
  kSequenceOverflow,
  kActiveCallLimit,
  kEventLimit,
  kPayloadLimit,
};

// One still-open external event.
struct GuestExecutionCaptureExternalEventActiveCall {
  GuestExecutionCaptureExternalEventToken token;
  GuestExecutionCaptureParticipantIdentity participant;
  GuestExecutionCaptureExternalEventKind kind =
      GuestExecutionCaptureExternalEventKind::kKernelExport;
  uint32_t export_ordinal = 0;
  uint32_t guest_address = 0;
  uint32_t call_site_address = 0;
  std::vector<GuestExecutionCaptureExternalEventEffectRange> effect_ranges;
  uint32_t effect_byte_count = 0;
  uint32_t participant_depth = 0;

  bool is_outermost() const { return participant_depth == 1; }
  bool operator==(const GuestExecutionCaptureExternalEventActiveCall&) const =
      default;
};

struct GuestExecutionCaptureExternalEventSnapshot {
  uint64_t recorded_event_count = 0;
  uint64_t total_payload_bytes = 0;
  GuestExecutionCaptureExternalEventRejection rejection =
      GuestExecutionCaptureExternalEventRejection::kNone;
  // Sticky: a recorded kRejectSession event never clears these, so a consumer
  // that only checks rejection cannot mistake this log for a replayable one.
  uint64_t reject_session_count = 0;
  uint64_t first_reject_session_sequence = 0;
  std::vector<GuestExecutionCaptureExternalEventActiveCall> active_calls;
  std::vector<GuestExecutionCaptureExternalEventRecord> events;

  bool replayable() const {
    return rejection == GuestExecutionCaptureExternalEventRejection::kNone &&
           reject_session_count == 0;
  }
};

// Thread-safe, observational external-event state machine for the synchronous
// kernel-export / extern-dispatch class. It carries no capture start, stop,
// checkpoint, event-sequence or publication policy: a future bounded
// execution-capture runtime owns those and calls this at the semantically
// correct dispatch boundary. Every method is fail-closed. Once any rejection
// latches, later begins return an empty token and later ends return false, so
// an incomplete log can never be mistaken for a complete one. A latched log
// also reports CanDetach() true so its owner can always release it.
//
// Pairing is last-in-first-out per participant so a nested export that reenters
// guest code and dispatches another export closes in the correct order; an
// out-of-order end latches kInvalidEnd rather than silently reordering.
//
// A kRejectSession end is accepted and recorded, but the snapshot's sticky
// reject-session indicator makes replayable() false from then on.
class GuestExecutionCaptureExternalEventLog final {
 public:
  explicit GuestExecutionCaptureExternalEventLog(
      const GuestExecutionCaptureExternalEventLimits& limits = {});
  ~GuestExecutionCaptureExternalEventLog();
  GuestExecutionCaptureExternalEventLog(
      const GuestExecutionCaptureExternalEventLog&) = delete;
  GuestExecutionCaptureExternalEventLog& operator=(
      const GuestExecutionCaptureExternalEventLog&) = delete;

  // Opens an event, snapshotting the declared effect ranges' preimage. Returns
  // an empty token on rejection.
  GuestExecutionCaptureExternalEventToken OnExternalEventBegin(
      const GuestExecutionCaptureExternalEventBegin& begin,
      std::span<const uint8_t> effect_preimage) noexcept;

  // Closes the event named by token, recording the return value and effect
  // postimage. Returns false on any rejection.
  bool OnExternalEventEnd(GuestExecutionCaptureExternalEventToken token,
                          const GuestExecutionCaptureExternalEventEnd& end,
                          std::span<const uint8_t> effect_postimage) noexcept;

  // True when no event is still open, or once any rejection has latched so a
  // failed log never pins its owner.
  bool CanDetach() const noexcept;

  // Copies the still-open calls owned by one participant. snapshot() copies
  // every recorded payload as well, which a per-participant checkpoint query
  // cannot afford. False once any rejection or reject-session event has
  // latched, so no consumer can bind a checkpoint to an unusable log.
  bool CopyParticipantActiveCalls(
      const GuestExecutionCaptureParticipantIdentity& participant,
      std::vector<GuestExecutionCaptureExternalEventActiveCall>* output)
      const noexcept;

  GuestExecutionCaptureExternalEventSnapshot snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_EXTERNAL_EVENT_H_
