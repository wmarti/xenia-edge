/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_CAPTURE_H_
#define XENIA_CPU_GUEST_EXECUTION_CAPTURE_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <memory>
#include <vector>

namespace xe {
namespace cpu {

class GuestFunction;
class ThreadState;

struct GuestExecutionCaptureParticipantIdentity {
  // Runtime-local identity for one ThreadState lifetime. Neither field is a
  // durable replay identifier; session codecs assign their own participant
  // identifiers without exposing host addresses.
  uint64_t capture_instance_id = 0;
  uint32_t guest_thread_id = 0;

  bool operator==(const GuestExecutionCaptureParticipantIdentity&) const =
      default;
};

struct GuestExecutionCaptureHostCallToken {
  uint64_t value = 0;

  explicit operator bool() const { return value != 0; }
  bool operator==(const GuestExecutionCaptureHostCallToken&) const = default;
};

// Describes how the generic host-to-guest dispatch wrapper finished. This is
// deliberately not a guest-function exit: emitted function entry and exit
// callbacks retain that separate, nested control-flow meaning.
enum class GuestExecutionCaptureHostCallOutcome : uint8_t {
  kReturnedToHost,
  kFailedToEnter,
  kAbortedByHostUnwind,
};

struct GuestExecutionCaptureActiveHostCall {
  GuestExecutionCaptureHostCallToken token;
  GuestExecutionCaptureParticipantIdentity participant;
  uint32_t function_address = 0;
  uint32_t function_end_address = 0;
  uint32_t return_address = 0;
  uint32_t participant_depth = 0;

  bool is_outermost() const { return participant_depth == 1; }
  bool operator==(const GuestExecutionCaptureActiveHostCall&) const = default;
};

enum class GuestExecutionCaptureHostCallRosterRejection : uint8_t {
  kNone,
  kInvalidBegin,
  kInvalidEnd,
  kAllocationFailure,
  kTokenOverflow,
  kCounterOverflow,
};

struct GuestExecutionCaptureHostCallRosterSnapshot {
  uint64_t returned_host_call_count = 0;
  uint64_t failed_to_enter_host_call_count = 0;
  uint64_t aborted_host_call_count = 0;
  GuestExecutionCaptureHostCallRosterRejection rejection =
      GuestExecutionCaptureHostCallRosterRejection::kNone;
  std::vector<GuestExecutionCaptureActiveHostCall> active_calls;
};

// Capture-build-only observer for generic host-to-guest dispatches. Processor
// registration is shared-owned by Processor. Implementations must serialize
// their own state, and callbacks must not retain the Function or ThreadState
// references. The token is runtime-local pairing metadata, never a durable
// session sequence.
class GuestExecutionCaptureHostCallObserver {
 public:
  virtual ~GuestExecutionCaptureHostCallObserver() = default;

  virtual GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept = 0;
  virtual bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept = 0;

  // Detachment is permitted only after every successfully begun call has
  // ended. Processor retains the observer if a detach attempt races a call.
  virtual bool CanDetach() const noexcept = 0;
};

// Thread-safe, observational active-call state machine for a future bounded
// execution-capture runtime. It defines no capture start, stop, checkpoint,
// event-sequence or publication policy.
class GuestExecutionCaptureHostCallRoster final
    : public GuestExecutionCaptureHostCallObserver {
 public:
  GuestExecutionCaptureHostCallRoster();
  ~GuestExecutionCaptureHostCallRoster() override;
  GuestExecutionCaptureHostCallRoster(
      const GuestExecutionCaptureHostCallRoster&) = delete;
  GuestExecutionCaptureHostCallRoster& operator=(
      const GuestExecutionCaptureHostCallRoster&) = delete;

  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override;
  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override;
  bool CanDetach() const noexcept override;

  GuestExecutionCaptureHostCallRosterSnapshot snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_CAPTURE_H_
