/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_replay_tape.h"

#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace xe {
namespace cpu {
namespace {

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

}  // namespace

struct GuestExecutionReplayTape::Impl {
  struct EventRecord {
    GuestExecutionSessionEvent event;
    std::vector<uint8_t> payload;
  };

  bool OwnerIsValid(uint32_t owner) const {
    return owner == kGuestExecutionSessionNoThread || owner < participant_count;
  }

  // The coordinator parks in the slot past the last participant ordinal.
  size_t OwnerSlot(uint32_t owner) const {
    return owner == kGuestExecutionSessionNoThread ? participant_count : owner;
  }

  void NotifyAllLocked() {
    for (size_t slot = 0; slot < condition_count; ++slot) {
      conditions[slot].notify_all();
    }
  }

  void NotifyOwnerLocked(uint32_t owner) {
    if (!OwnerIsValid(owner)) {
      NotifyAllLocked();
      return;
    }
    conditions[OwnerSlot(owner)].notify_all();
  }

  void RejectLocked(GuestExecutionReplayTapeRejection value,
                    std::string message) {
    if (state == GuestExecutionReplayTapeState::kComplete ||
        state == GuestExecutionReplayTapeState::kRejected) {
      return;
    }
    state = GuestExecutionReplayTapeState::kRejected;
    rejection = value;
    active_lease = false;
    active_lease_id = 0;
    active_owner = kGuestExecutionSessionNoThread;
    this->message = std::move(message);
    // Rejection releases every parked owner, not just the next one.
    NotifyAllLocked();
  }

  bool ValidateLeaseLocked(const GuestExecutionReplayTurn& turn,
                           std::string* error) {
    bool valid = state == GuestExecutionReplayTapeState::kRunning &&
                 active_lease && turn && turn.lease_id == active_lease_id &&
                 next_event_index < events.size();
    if (valid) {
      // The recorded sequence and the borrowed span address tie the turn to
      // this record without comparing any payload bytes.
      const EventRecord& record = events[next_event_index];
      valid = turn.event.global_sequence == record.event.global_sequence &&
              turn.payload.data() == record.payload.data() &&
              turn.payload.size() == record.payload.size();
    }
    if (!valid) {
      RejectLocked(GuestExecutionReplayTapeRejection::kLeaseMismatch,
                   "replay event lease is stale, altered or missing");
      return Fail(error, message);
    }
    return true;
  }

  void CommitLocked() {
    ++next_event_index;
    active_lease = false;
    active_lease_id = 0;
    active_owner = kGuestExecutionSessionNoThread;
    if (next_event_index == events.size()) {
      state = GuestExecutionReplayTapeState::kComplete;
      NotifyAllLocked();
      return;
    }
    NotifyOwnerLocked(events[next_event_index].event.thread_ordinal);
  }

  mutable std::mutex mutex;
  std::unique_ptr<std::condition_variable[]> conditions;
  size_t condition_count = 0;
  std::vector<EventRecord> events;
  uint32_t participant_count = 0;
  size_t next_event_index = 0;
  uint64_t next_lease_id = 1;
  uint64_t active_lease_id = 0;
  uint32_t active_owner = kGuestExecutionSessionNoThread;
  GuestExecutionReplayTapeState state = GuestExecutionReplayTapeState::kReady;
  GuestExecutionReplayTapeRejection rejection =
      GuestExecutionReplayTapeRejection::kNone;
  bool active_lease = false;
  std::string message;
};

std::unique_ptr<GuestExecutionReplayTape> GuestExecutionReplayTape::Create(
    const GuestExecutionSessionBundle& bundle, std::string* error,
    GuestExecutionSessionBundleLimits limits) {
  if (error) {
    error->clear();
  }
  if (!ValidateGuestExecutionSessionBundle(bundle, error, limits)) {
    return nullptr;
  }
  if (bundle.manifest.rejected_event_count ||
      bundle.manifest.unsupported_event_count) {
    Fail(error, "replay session contains rejected or unsupported events");
    return nullptr;
  }

  std::map<GuestExecutionSessionSha256, const std::vector<uint8_t>*> blobs;
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    blobs.emplace(blob.sha256, &blob.bytes);
  }

  auto impl = std::make_unique<Impl>();
  impl->participant_count =
      static_cast<uint32_t>(bundle.manifest.participants.size());
  try {
    impl->condition_count = static_cast<size_t>(impl->participant_count) + 1;
    impl->conditions =
        std::make_unique<std::condition_variable[]>(impl->condition_count);
    impl->events.reserve(
        static_cast<size_t>(bundle.manifest.accepted_event_count));
    for (size_t chunk_index = 0; chunk_index < bundle.chunks.size();
         ++chunk_index) {
      if (bundle.manifest.chunks[chunk_index].kind !=
          GuestExecutionSessionChunkKind::kEvents) {
        continue;
      }
      GuestExecutionSessionEventChunk chunk;
      if (!GuestExecutionSessionCodec::DecodeEventChunk(
              bundle.chunks[chunk_index], &chunk, error, limits.session)) {
        return nullptr;
      }
      for (const GuestExecutionSessionEvent& event : chunk.events) {
        if (event.disposition ==
            GuestExecutionSessionEventDisposition::kRejectSession) {
          Fail(error, "replay session contains a rejecting event");
          return nullptr;
        }
        Impl::EventRecord record;
        record.event = event;
        if (event.payload_size) {
          const auto payload = blobs.find(event.payload_sha256);
          if (payload == blobs.end() ||
              payload->second->size() != event.payload_size) {
            Fail(error,
                 "replay event payload is missing or has the wrong size");
            return nullptr;
          }
          record.payload = *payload->second;
        }
        impl->events.push_back(std::move(record));
      }
    }
  } catch (...) {
    Fail(error, "replay event tape allocation failed");
    return nullptr;
  }
  if (impl->events.empty() ||
      impl->events.size() != bundle.manifest.accepted_event_count) {
    Fail(error, "replay event tape does not contain the accepted event count");
    return nullptr;
  }
  return std::unique_ptr<GuestExecutionReplayTape>(
      new GuestExecutionReplayTape(std::move(impl)));
}

GuestExecutionReplayTape::GuestExecutionReplayTape(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestExecutionReplayTape::~GuestExecutionReplayTape() = default;

bool GuestExecutionReplayTape::Start(std::string* error) {
  if (error) {
    error->clear();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->state != GuestExecutionReplayTapeState::kReady) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kInvalidCall,
                        "replay event tape can be started exactly once");
    return Fail(error, impl_->message);
  }
  impl_->state = GuestExecutionReplayTapeState::kRunning;
  impl_->NotifyAllLocked();
  return true;
}

GuestExecutionReplayAcquireResult GuestExecutionReplayTape::Acquire(
    uint32_t owner, std::chrono::milliseconds timeout,
    GuestExecutionReplayTurn* turn, std::string* error) {
  if (error) {
    error->clear();
  }
  if (turn) {
    *turn = {};
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!turn || timeout <= std::chrono::milliseconds::zero() ||
      !impl_->OwnerIsValid(owner) ||
      impl_->state == GuestExecutionReplayTapeState::kReady) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kInvalidCall,
                        "replay event acquire arguments or state are invalid");
    Fail(error, impl_->message);
    return GuestExecutionReplayAcquireResult::kRejected;
  }

  const auto ready = [&]() {
    return impl_->state != GuestExecutionReplayTapeState::kRunning ||
           (!impl_->active_lease &&
            impl_->next_event_index < impl_->events.size() &&
            impl_->events[impl_->next_event_index].event.thread_ordinal ==
                owner);
  };
  if (!impl_->conditions[impl_->OwnerSlot(owner)].wait_for(lock, timeout,
                                                           ready)) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kTimeout,
                        "replay event owner did not reach its recorded turn");
    Fail(error, impl_->message);
    return GuestExecutionReplayAcquireResult::kRejected;
  }
  if (impl_->state == GuestExecutionReplayTapeState::kComplete) {
    return GuestExecutionReplayAcquireResult::kComplete;
  }
  if (impl_->state == GuestExecutionReplayTapeState::kRejected) {
    Fail(error, impl_->message);
    return GuestExecutionReplayAcquireResult::kRejected;
  }
  if (impl_->next_lease_id == std::numeric_limits<uint64_t>::max()) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kInvalidCall,
                        "replay event lease identifier overflowed");
    Fail(error, impl_->message);
    return GuestExecutionReplayAcquireResult::kRejected;
  }

  const Impl::EventRecord& record = impl_->events[impl_->next_event_index];
  impl_->active_lease = true;
  impl_->active_lease_id = impl_->next_lease_id++;
  impl_->active_owner = owner;
  turn->lease_id = impl_->active_lease_id;
  turn->event = record.event;
  turn->payload = std::span<const uint8_t>(record.payload);
  return GuestExecutionReplayAcquireResult::kAcquired;
}

bool GuestExecutionReplayTape::CommitCaptured(
    const GuestExecutionReplayTurn& turn, std::string* error) {
  if (error) {
    error->clear();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->ValidateLeaseLocked(turn, error)) {
    return false;
  }
  if (impl_->events[impl_->next_event_index].event.disposition !=
      GuestExecutionSessionEventDisposition::kReplayCaptured) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kDispositionMismatch,
                        "deterministic replay event was committed as captured");
    return Fail(error, impl_->message);
  }
  impl_->CommitLocked();
  return true;
}

bool GuestExecutionReplayTape::CommitDeterministic(
    const GuestExecutionReplayTurn& turn,
    const GuestExecutionSessionEvent& observed, std::string* error) {
  if (error) {
    error->clear();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->ValidateLeaseLocked(turn, error)) {
    return false;
  }
  const Impl::EventRecord& record = impl_->events[impl_->next_event_index];
  if (record.event.disposition !=
      GuestExecutionSessionEventDisposition::kValidateDeterministic) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kDispositionMismatch,
                        "captured replay event was committed as deterministic");
    return Fail(error, impl_->message);
  }
  if (observed != record.event) {
    impl_->RejectLocked(
        GuestExecutionReplayTapeRejection::kDeterministicMismatch,
        "observed replay event differs from the capture");
    return Fail(error, impl_->message);
  }
  impl_->CommitLocked();
  return true;
}

bool GuestExecutionReplayTape::CommitOrderingOnly(
    const GuestExecutionReplayTurn& turn, std::string* error) {
  if (error) {
    error->clear();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->ValidateLeaseLocked(turn, error)) {
    return false;
  }
  if (impl_->events[impl_->next_event_index].event.kind !=
      GuestExecutionSessionEventKind::kInstructionCoverage) {
    impl_->RejectLocked(
        GuestExecutionReplayTapeRejection::kOrderingOnlyMismatch,
        "ordering-only commit is admitted only for instruction coverage");
    return Fail(error, impl_->message);
  }
  impl_->CommitLocked();
  return true;
}

void GuestExecutionReplayTape::Abandon(const GuestExecutionReplayTurn& turn,
                                       std::string message) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active_lease || !turn ||
      turn.lease_id != impl_->active_lease_id) {
    impl_->RejectLocked(GuestExecutionReplayTapeRejection::kLeaseMismatch,
                        "replay event abandon used an invalid lease");
    return;
  }
  impl_->RejectLocked(
      GuestExecutionReplayTapeRejection::kCancelled,
      message.empty() ? "replay event was abandoned" : std::move(message));
}

void GuestExecutionReplayTape::Cancel(std::string message) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->RejectLocked(
      GuestExecutionReplayTapeRejection::kCancelled,
      message.empty() ? "replay event tape was cancelled" : std::move(message));
}

GuestExecutionReplayTapeStatus GuestExecutionReplayTape::status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  GuestExecutionReplayTapeStatus status;
  status.state = impl_->state;
  status.rejection = impl_->rejection;
  status.consumed_event_count = impl_->next_event_index;
  status.total_event_count = impl_->events.size();
  if (impl_->next_event_index < impl_->events.size()) {
    status.next_event_sequence =
        impl_->events[impl_->next_event_index].event.global_sequence;
  }
  status.has_active_lease = impl_->active_lease;
  status.active_owner = impl_->active_owner;
  status.message = impl_->message;
  return status;
}

}  // namespace cpu
}  // namespace xe
