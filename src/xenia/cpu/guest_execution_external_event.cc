/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_external_event.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <limits>
#include <mutex>

namespace xe {
namespace cpu {
namespace {

// The session enumerations carry more values than this class accepts, so the
// checks are explicit allow-lists rather than exhaustive switches.
bool IsKnownKind(GuestExecutionCaptureExternalEventKind kind) {
  return kind == GuestExecutionCaptureExternalEventKind::kKernelExport ||
         kind == GuestExecutionCaptureExternalEventKind::kExternOrBuiltin;
}

bool IsKnownDisposition(
    GuestExecutionCaptureExternalEventDisposition disposition) {
  return disposition ==
             GuestExecutionCaptureExternalEventDisposition::kReplayCaptured ||
         disposition == GuestExecutionCaptureExternalEventDisposition::
                            kValidateDeterministic ||
         disposition ==
             GuestExecutionCaptureExternalEventDisposition::kRejectSession;
}

bool IsKnownMutationSource(
    GuestExecutionCaptureExternalEventMutationSource source) {
  return source == GuestExecutionCaptureExternalEventMutationSource::kNone ||
         source == GuestExecutionCaptureExternalEventMutationSource::
                       kActiveGuestThread;
}

// Canonical form: non-empty ranges, ascending by address, separated by at
// least one byte, and none wrapping the guest address space. Overlapping or
// abutting ranges would let one byte reach the images twice and would give a
// single write region more than one representation.
bool AreCanonicalEffectRanges(
    std::span<const GuestExecutionCaptureExternalEventEffectRange> ranges,
    uint64_t* total_byte_count) {
  constexpr uint64_t kGuestAddressSpaceEnd =
      uint64_t(std::numeric_limits<uint32_t>::max()) + 1;
  uint64_t total = 0;
  uint64_t previous_end = 0;
  bool have_previous = false;
  for (const GuestExecutionCaptureExternalEventEffectRange& range : ranges) {
    const uint64_t end = uint64_t(range.address) + range.byte_count;
    if (!range.byte_count || end > kGuestAddressSpaceEnd) {
      return false;
    }
    if (have_previous && uint64_t(range.address) <= previous_end) {
      return false;
    }
    previous_end = end;
    have_previous = true;
    total += range.byte_count;
  }
  *total_byte_count = total;
  return true;
}

std::array<uint8_t, 8> EncodeLittleEndian(uint64_t value) {
  std::array<uint8_t, 8> bytes = {};
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(value >> (i * 8));
  }
  return bytes;
}

}  // namespace

struct GuestExecutionCaptureExternalEventLog::Impl {
  void RejectLocked(GuestExecutionCaptureExternalEventRejection value) {
    if (rejection == GuestExecutionCaptureExternalEventRejection::kNone) {
      rejection = value;
    }
  }

  mutable std::mutex mutex;
  GuestExecutionCaptureExternalEventLimits limits;
  uint64_t next_token = 1;
  uint64_t next_sequence = 1;
  uint64_t total_payload_bytes = 0;
  uint64_t reject_session_count = 0;
  uint64_t first_reject_session_sequence = 0;
  GuestExecutionCaptureExternalEventRejection rejection =
      GuestExecutionCaptureExternalEventRejection::kNone;
  std::vector<GuestExecutionCaptureExternalEventActiveCall> active_calls;
  std::vector<std::vector<uint8_t>> active_preimages;
  std::vector<GuestExecutionCaptureExternalEventRecord> events;
};

GuestExecutionCaptureExternalEventLog::GuestExecutionCaptureExternalEventLog(
    const GuestExecutionCaptureExternalEventLimits& limits)
    : impl_(std::make_unique<Impl>()) {
  impl_->limits = limits;
}

GuestExecutionCaptureExternalEventLog::
    ~GuestExecutionCaptureExternalEventLog() = default;

GuestExecutionCaptureExternalEventToken
GuestExecutionCaptureExternalEventLog::OnExternalEventBegin(
    const GuestExecutionCaptureExternalEventBegin& begin,
    std::span<const uint8_t> effect_preimage) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->rejection != GuestExecutionCaptureExternalEventRejection::kNone) {
    return {};
  }
  if (!begin.participant.capture_instance_id || !IsKnownKind(begin.kind) ||
      begin.effect_ranges.size() > impl_->limits.max_effect_ranges ||
      effect_preimage.size() > impl_->limits.max_effect_bytes) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kInvalidBegin);
    return {};
  }
  uint64_t declared_byte_count = 0;
  // The preimage must account for every declared byte and no others, so a
  // range list can never claim guest memory the images do not carry.
  if (!AreCanonicalEffectRanges(begin.effect_ranges, &declared_byte_count) ||
      declared_byte_count != effect_preimage.size()) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kInvalidBegin);
    return {};
  }
  const uint32_t effect_byte_count =
      static_cast<uint32_t>(effect_preimage.size());
  if (impl_->active_calls.size() >= impl_->limits.max_active_calls) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kActiveCallLimit);
    return {};
  }
  if (impl_->next_token == std::numeric_limits<uint64_t>::max()) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kTokenOverflow);
    return {};
  }

  uint32_t participant_depth = 1;
  for (const GuestExecutionCaptureExternalEventActiveCall& active_call :
       impl_->active_calls) {
    if (active_call.participant == begin.participant) {
      participant_depth =
          std::max(participant_depth, active_call.participant_depth + 1);
    }
  }

  const GuestExecutionCaptureExternalEventToken token = {impl_->next_token};
  try {
    GuestExecutionCaptureExternalEventActiveCall active_call;
    active_call.token = token;
    active_call.participant = begin.participant;
    active_call.kind = begin.kind;
    active_call.export_ordinal = begin.export_ordinal;
    active_call.call_site_address = begin.call_site_address;
    active_call.effect_ranges.assign(begin.effect_ranges.begin(),
                                     begin.effect_ranges.end());
    active_call.effect_byte_count = effect_byte_count;
    active_call.participant_depth = participant_depth;
    impl_->active_preimages.emplace_back(effect_preimage.begin(),
                                         effect_preimage.end());
    impl_->active_calls.push_back(std::move(active_call));
  } catch (...) {
    // Keep the two side arrays balanced before latching the failure.
    if (impl_->active_preimages.size() > impl_->active_calls.size()) {
      impl_->active_preimages.pop_back();
    }
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kAllocationFailure);
    return {};
  }
  ++impl_->next_token;
  return token;
}

bool GuestExecutionCaptureExternalEventLog::OnExternalEventEnd(
    GuestExecutionCaptureExternalEventToken token,
    const GuestExecutionCaptureExternalEventEnd& end,
    std::span<const uint8_t> effect_postimage) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->rejection != GuestExecutionCaptureExternalEventRejection::kNone) {
    return false;
  }
  const auto active_it = std::find_if(
      impl_->active_calls.begin(), impl_->active_calls.end(),
      [token](const GuestExecutionCaptureExternalEventActiveCall& call) {
        return call.token == token;
      });
  if (!token || active_it == impl_->active_calls.end() ||
      !IsKnownDisposition(end.disposition) ||
      !IsKnownMutationSource(end.mutation_source)) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kInvalidEnd);
    return false;
  }
  const GuestExecutionCaptureExternalEventActiveCall& active_call = *active_it;
  // Last-in-first-out per participant: a still-open later call on the same
  // participant means this end arrived out of order.
  for (auto later_call = active_it + 1; later_call != impl_->active_calls.end();
       ++later_call) {
    if (later_call->participant == active_call.participant) {
      impl_->RejectLocked(
          GuestExecutionCaptureExternalEventRejection::kInvalidEnd);
      return false;
    }
  }

  const bool has_effect = active_call.effect_byte_count != 0;
  const bool source_is_active =
      end.mutation_source ==
      GuestExecutionCaptureExternalEventMutationSource::kActiveGuestThread;
  if (effect_postimage.size() != active_call.effect_byte_count ||
      has_effect != source_is_active) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kInvalidEnd);
    return false;
  }

  if (impl_->events.size() >= impl_->limits.max_recorded_events) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kEventLimit);
    return false;
  }
  const size_t active_index =
      static_cast<size_t>(active_it - impl_->active_calls.begin());
  const uint64_t payload_bytes = uint64_t(active_call.effect_byte_count) * 2 +
                                 (end.has_returned_value ? 8 : 0);
  if (payload_bytes >
      impl_->limits.max_total_payload_bytes - impl_->total_payload_bytes) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kPayloadLimit);
    return false;
  }
  if (impl_->next_sequence == std::numeric_limits<uint64_t>::max()) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kSequenceOverflow);
    return false;
  }

  GuestExecutionCaptureExternalEventRecord record;
  record.sequence = impl_->next_sequence;
  record.participant = active_call.participant;
  record.kind = active_call.kind;
  record.disposition = end.disposition;
  record.mutation_source = end.mutation_source;
  record.export_ordinal = active_call.export_ordinal;
  record.call_site_address = active_call.call_site_address;
  record.has_returned_value = end.has_returned_value;
  if (end.has_returned_value) {
    record.returned_value_le = EncodeLittleEndian(end.returned_value);
  }
  record.effect_byte_count = active_call.effect_byte_count;
  try {
    record.effect_ranges = active_call.effect_ranges;
    record.preimage = impl_->active_preimages[active_index];
    record.postimage.assign(effect_postimage.begin(), effect_postimage.end());
    impl_->events.push_back(std::move(record));
  } catch (...) {
    impl_->RejectLocked(
        GuestExecutionCaptureExternalEventRejection::kAllocationFailure);
    return false;
  }

  impl_->active_calls.erase(active_it);
  impl_->active_preimages.erase(impl_->active_preimages.begin() + active_index);
  impl_->total_payload_bytes += payload_bytes;
  if (end.disposition ==
      GuestExecutionCaptureExternalEventDisposition::kRejectSession) {
    // Bounded by max_recorded_events, so this cannot wrap.
    if (!impl_->reject_session_count) {
      impl_->first_reject_session_sequence = impl_->next_sequence;
    }
    ++impl_->reject_session_count;
  }
  ++impl_->next_sequence;
  return true;
}

bool GuestExecutionCaptureExternalEventLog::CanDetach() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // A latched log will never close its open calls; do not pin the owner.
  return impl_->rejection !=
             GuestExecutionCaptureExternalEventRejection::kNone ||
         impl_->active_calls.empty();
}

GuestExecutionCaptureExternalEventSnapshot
GuestExecutionCaptureExternalEventLog::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  GuestExecutionCaptureExternalEventSnapshot result;
  result.recorded_event_count = impl_->events.size();
  result.total_payload_bytes = impl_->total_payload_bytes;
  result.rejection = impl_->rejection;
  result.reject_session_count = impl_->reject_session_count;
  result.first_reject_session_sequence = impl_->first_reject_session_sequence;
  result.active_calls = impl_->active_calls;
  result.events = impl_->events;
  return result;
}

}  // namespace cpu
}  // namespace xe

#endif
