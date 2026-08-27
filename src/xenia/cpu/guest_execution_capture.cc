/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_capture.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/cpu/function.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {
namespace {

GuestExecutionCaptureParticipantIdentity MakeParticipantIdentity(
    const ThreadState& thread_state) {
  return {
      thread_state.guest_execution_capture_instance_id(),
      thread_state.thread_id(),
  };
}

bool IsKnownOutcome(GuestExecutionCaptureHostCallOutcome outcome) {
  switch (outcome) {
    case GuestExecutionCaptureHostCallOutcome::kReturnedToHost:
    case GuestExecutionCaptureHostCallOutcome::kFailedToEnter:
    case GuestExecutionCaptureHostCallOutcome::kAbortedByHostUnwind:
      return true;
  }
  return false;
}

}  // namespace

struct GuestExecutionCaptureHostCallRoster::Impl {
  void RejectLocked(GuestExecutionCaptureHostCallRosterRejection value) {
    if (rejection == GuestExecutionCaptureHostCallRosterRejection::kNone) {
      rejection = value;
    }
  }

  mutable std::mutex mutex;
  uint64_t next_token = 1;
  uint64_t returned_host_call_count = 0;
  uint64_t failed_to_enter_host_call_count = 0;
  uint64_t aborted_host_call_count = 0;
  GuestExecutionCaptureHostCallRosterRejection rejection =
      GuestExecutionCaptureHostCallRosterRejection::kNone;
  std::vector<GuestExecutionCaptureActiveHostCall> active_calls;
};

GuestExecutionCaptureHostCallRoster::GuestExecutionCaptureHostCallRoster()
    : impl_(std::make_unique<Impl>()) {}

GuestExecutionCaptureHostCallRoster::~GuestExecutionCaptureHostCallRoster() =
    default;

GuestExecutionCaptureHostCallToken
GuestExecutionCaptureHostCallRoster::OnHostGuestCallBegin(
    const ThreadState& thread_state, const GuestFunction& function,
    uint32_t return_address) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->rejection != GuestExecutionCaptureHostCallRosterRejection::kNone ||
      !thread_state.context() ||
      !thread_state.guest_execution_capture_instance_id()) {
    impl_->RejectLocked(
        GuestExecutionCaptureHostCallRosterRejection::kInvalidBegin);
    return {};
  }
  if (impl_->next_token == std::numeric_limits<uint64_t>::max()) {
    impl_->RejectLocked(
        GuestExecutionCaptureHostCallRosterRejection::kTokenOverflow);
    return {};
  }
  const GuestExecutionCaptureParticipantIdentity participant =
      MakeParticipantIdentity(thread_state);
  uint32_t participant_depth = 1;
  for (const GuestExecutionCaptureActiveHostCall& active_call :
       impl_->active_calls) {
    if (active_call.participant != participant) {
      continue;
    }
    if (active_call.participant_depth == std::numeric_limits<uint32_t>::max()) {
      impl_->RejectLocked(
          GuestExecutionCaptureHostCallRosterRejection::kCounterOverflow);
      return {};
    }
    participant_depth =
        std::max(participant_depth, active_call.participant_depth + 1);
  }

  const GuestExecutionCaptureHostCallToken token = {impl_->next_token};
  try {
    impl_->active_calls.push_back({token, participant, function.address(),
                                   function.end_address(), return_address,
                                   participant_depth});
  } catch (...) {
    impl_->RejectLocked(
        GuestExecutionCaptureHostCallRosterRejection::kAllocationFailure);
    return {};
  }
  ++impl_->next_token;
  return token;
}

bool GuestExecutionCaptureHostCallRoster::OnHostGuestCallEnd(
    GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
    const GuestFunction& function,
    GuestExecutionCaptureHostCallOutcome outcome) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto active_it =
      std::find_if(impl_->active_calls.begin(), impl_->active_calls.end(),
                   [token](const GuestExecutionCaptureActiveHostCall& call) {
                     return call.token == token;
                   });
  if (!token || active_it == impl_->active_calls.end() ||
      !IsKnownOutcome(outcome)) {
    impl_->RejectLocked(
        GuestExecutionCaptureHostCallRosterRejection::kInvalidEnd);
    return false;
  }
  const GuestExecutionCaptureActiveHostCall& active_call = *active_it;
  const GuestExecutionCaptureParticipantIdentity participant =
      MakeParticipantIdentity(thread_state);
  if (participant != active_call.participant ||
      function.address() != active_call.function_address ||
      function.end_address() != active_call.function_end_address) {
    impl_->RejectLocked(
        GuestExecutionCaptureHostCallRosterRejection::kInvalidEnd);
    return false;
  }
  for (auto later_call = active_it + 1; later_call != impl_->active_calls.end();
       ++later_call) {
    if (later_call->participant == participant) {
      impl_->RejectLocked(
          GuestExecutionCaptureHostCallRosterRejection::kInvalidEnd);
      return false;
    }
  }

  bool counters_valid = true;
  uint64_t* outcome_counter = nullptr;
  switch (outcome) {
    case GuestExecutionCaptureHostCallOutcome::kReturnedToHost:
      outcome_counter = &impl_->returned_host_call_count;
      break;
    case GuestExecutionCaptureHostCallOutcome::kFailedToEnter:
      outcome_counter = &impl_->failed_to_enter_host_call_count;
      break;
    case GuestExecutionCaptureHostCallOutcome::kAbortedByHostUnwind:
      outcome_counter = &impl_->aborted_host_call_count;
      break;
  }
  if (*outcome_counter == std::numeric_limits<uint64_t>::max()) {
    impl_->RejectLocked(
        GuestExecutionCaptureHostCallRosterRejection::kCounterOverflow);
    counters_valid = false;
  }

  impl_->active_calls.erase(active_it);
  if (counters_valid) {
    ++*outcome_counter;
  }
  return counters_valid;
}

bool GuestExecutionCaptureHostCallRoster::CanDetach() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->active_calls.empty();
}

GuestExecutionCaptureHostCallRosterSnapshot
GuestExecutionCaptureHostCallRoster::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  GuestExecutionCaptureHostCallRosterSnapshot result;
  result.returned_host_call_count = impl_->returned_host_call_count;
  result.failed_to_enter_host_call_count =
      impl_->failed_to_enter_host_call_count;
  result.aborted_host_call_count = impl_->aborted_host_call_count;
  result.rejection = impl_->rejection;
  result.active_calls = impl_->active_calls;
  return result;
}

}  // namespace cpu
}  // namespace xe

#endif
