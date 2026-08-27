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
#include <span>
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

enum class GuestExecutionCaptureThreadStateLifecycleState : uint8_t {
  kPending,
  kReady,
  kDestroying,
};

struct GuestExecutionCaptureThreadStateLifecycleEvent {
  GuestExecutionCaptureParticipantIdentity participant;
  GuestExecutionCaptureThreadStateLifecycleState state =
      GuestExecutionCaptureThreadStateLifecycleState::kPending;
  // Transport-only coverage cut made synchronously before a destroying PPC
  // context can be freed. Consumers serialize it as a separate coverage event.
  uint64_t guest_instruction_delta = 0;

  bool operator==(const GuestExecutionCaptureThreadStateLifecycleEvent&) const =
      default;
};

enum class GuestExecutionCaptureThreadStateLifecycleDisposition : uint8_t {
  kAccept,
  kReject,
};

enum class GuestExecutionCaptureThreadStateRegistryRejection : uint8_t {
  kNone,
  kObserverRejectedRuntimeEvent,
  kObserverRejectedJitSafepoint,
  kInvalidReadyTransition,
  kDuplicateRegistration,
  kMissingRegistration,
  kInvalidDestroyTransition,
  kInvalidInstructionCounter,
  kObserverCallbackReentry,
};

struct GuestExecutionCaptureThreadStateRegistrySnapshot {
  GuestExecutionCaptureThreadStateRegistryRejection rejection =
      GuestExecutionCaptureThreadStateRegistryRejection::kNone;
  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> participants;

  bool all_ready() const {
    if (rejection != GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
      return false;
    }
    for (const auto& participant : participants) {
      if (participant.state !=
          GuestExecutionCaptureThreadStateLifecycleState::kReady) {
        return false;
      }
    }
    return true;
  }
};

enum class GuestExecutionCaptureThreadStateVisitResult : uint8_t {
  kCompleted,
  kStoppedByVisitor,
  kParticipantNotReady,
  kRegistryRejected,
  kObserverCallbackReentry,
};

enum class GuestExecutionCaptureJitSafepointDisposition : uint8_t {
  kAccept,
  kReject,
};

enum class GuestExecutionCaptureJitSafepointResult : uint8_t {
  kDelivered,
  kNotRequested,
  kNoObserver,
  kParticipantNotReady,
  kRegistryRejected,
  kObserverRejected,
  kObserverCallbackReentry,
  kInvalidContext,
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

// Provides scoped access to ready ThreadState objects while Processor holds
// the capture-only lifetime registry lock. Implementations must not retain the
// reference or call back into Processor. Returning false stops the visit.
// CompleteVisit runs under the same lock after the exact roster has been
// visited, allowing an implementation to commit a validated transaction.
class GuestExecutionCaptureThreadStateVisitor {
 public:
  virtual ~GuestExecutionCaptureThreadStateVisitor() = default;

  virtual bool VisitThreadState(const ThreadState& thread_state) noexcept = 0;
  virtual bool CompleteVisit() noexcept { return true; }
  virtual void CancelVisit() noexcept {}
};

// Capture-build-only observer for generic host-to-guest dispatches. Install one
// continuous observer before title dispatch and arm/disarm capture internally;
// Processor permanently rejects later attachment after the first dispatch.
// Registration is shared-owned by Processor. Implementations must serialize
// their own state, and callbacks must not retain the Function or ThreadState
// references. The token is runtime-local pairing metadata, never a durable
// session sequence.
class GuestExecutionCaptureHostCallObserver {
 public:
  virtual ~GuestExecutionCaptureHostCallObserver() = default;

  // Processor serializes lifecycle events with its ThreadState registry.
  // Attachment first pre-validates that every existing lifetime is ready, then
  // offers the entire kReady roster in one transaction. An implementation must
  // either accept and commit the whole span, or reject it without changing its
  // state. The span is callback-scoped and must not be retained. Runtime
  // registration, publication and destruction produce individual kPending,
  // kReady and kDestroying events respectively. No observer callback may call
  // Processor or any capture-registry API. Implementations must not retain host
  // references. A rejected seed prevents installation; a rejected runtime
  // event is latched by Processor while the observer remains attached for
  // active-call cleanup.
  virtual GuestExecutionCaptureThreadStateLifecycleDisposition
  OnThreadStateSeed(
      std::span<
          const GuestExecutionCaptureThreadStateLifecycleEvent>) noexcept {
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
  }

  virtual GuestExecutionCaptureThreadStateLifecycleDisposition
  OnThreadStateLifecycle(
      GuestExecutionCaptureThreadStateLifecycleEvent) noexcept {
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
  }

  // Runs on the guest thread at a block-head JIT safepoint. Implementations
  // may park until a capture transition is released, but must not retain the
  // ThreadState reference or call Processor or another capture-registry API.
  virtual GuestExecutionCaptureJitSafepointDisposition OnJitSafepoint(
      const ThreadState& thread_state, uint32_t guest_address) noexcept {
    return GuestExecutionCaptureJitSafepointDisposition::kAccept;
  }

  virtual GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept = 0;
  virtual bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept = 0;

  // Detachment is permitted only before the first generic host-to-guest
  // dispatch. After that, Processor retains the observer through teardown;
  // continuous capture arms and disarms within this permanent observer.
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
