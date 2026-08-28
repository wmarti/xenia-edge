/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_SCHEDULER_CAPTURE_OBSERVER_H_
#define XENIA_KERNEL_GUEST_SCHEDULER_CAPTURE_OBSERVER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "xenia/cpu/guest_scheduler_record.h"

namespace xe {
namespace kernel {

// The tape vocabulary is declared by the ungated CPU record header so a
// capture-disabled replay binary can still decode a recorded scheduler tape;
// these aliases keep the capture-side spelling.
using GuestSchedulerCaptureEventKind = cpu::GuestSchedulerCaptureEventKind;
using GuestSchedulerCaptureReason = cpu::GuestSchedulerCaptureReason;
using GuestSchedulerCaptureEventFlags = cpu::GuestSchedulerCaptureEventFlags;
using cpu::kGuestSchedulerCaptureFlagAlertable;
using cpu::kGuestSchedulerCaptureFlagAtHead;
using cpu::kGuestSchedulerCaptureFlagCaptureRequested;
using cpu::kGuestSchedulerCaptureFlagFirstRun;
using cpu::kGuestSchedulerCaptureFlagFreshQuantum;
using cpu::kGuestSchedulerCaptureFlagGated;
using cpu::kGuestSchedulerCaptureFlagHasDeadline;
using cpu::kGuestSchedulerCaptureFlagHonoredYield;
using cpu::kGuestSchedulerCaptureFlagInterruptible;
using cpu::kGuestSchedulerCaptureFlagPreempted;
using cpu::kGuestSchedulerCaptureFlagQuantumEnd;
using cpu::kGuestSchedulerCaptureFlagSchedulerRequested;
using cpu::kGuestSchedulerCaptureFlagToLower;
using cpu::kGuestSchedulerCaptureFlagYieldToOther;
using cpu::kGuestSchedulerCaptureForcedIrqlMinimumDeclines;
using cpu::kGuestSchedulerCaptureMaximumWaitHandles;
using GuestSchedulerCaptureWaitKind = cpu::GuestSchedulerCaptureWaitKind;
using GuestSchedulerCaptureWaitFlags = cpu::GuestSchedulerCaptureWaitFlags;
using cpu::kGuestSchedulerCaptureWaitFlagAlertable;
using cpu::kGuestSchedulerCaptureWaitFlagGated;
using cpu::kGuestSchedulerCaptureWaitFlagInterruptible;
using cpu::kGuestSchedulerCaptureWaitFlagUserApcPending;
using GuestSchedulerCaptureWaitState = cpu::GuestSchedulerCaptureWaitState;

// Fixed-size, pointer-free record of one scheduler transition. The sequence
// is the single global order of scheduler state mutation. Participants are
// identified by their ThreadState capture instance ID; the guest thread ID is
// diagnostic only. cpu is the dispatch CPU whose state the event mutates, or
// the caller's CPU for a decision, and -1 off a dispatch thread.
struct GuestSchedulerCaptureEvent {
  uint64_t sequence = 0;
  uint64_t capture_instance_id = 0;
  // Capture-transport-only executed-basic-block instruction weight accumulated
  // by this participant since its previous actor-owned scheduler cut. The
  // session bridge emits
  // this as a separate instruction-coverage event immediately before the
  // scheduler event; it is not duplicated in the durable scheduler payload.
  uint64_t guest_instruction_delta = 0;
  uint32_t guest_thread_id = 0;
  // kSafepoint kForcedIrql and kYielded: safepoints declined since the
  // participant's previous terminal outcome, both lock and IRQL episodes.
  uint32_t count = 0;
  // Exact PPC address of kSafepoint. Zero for every other event kind.
  uint32_t guest_pc = 0;
  uint16_t flags = 0;
  GuestSchedulerCaptureEventKind kind = GuestSchedulerCaptureEventKind::kNone;
  GuestSchedulerCaptureReason reason = GuestSchedulerCaptureReason::kNone;
  int8_t cpu = -1;
  int8_t target_cpu = -1;
  // Effective level of the participant at the event. For kPriorityChange this
  // is the new level and value is the previous level.
  uint8_t priority = 0;
  // kSafepoint: guest IRQL. kBlock and kReready: XThread::CooperativeWaitKind.
  // kPriorityChange: the previous level.
  uint8_t value = 0;
  GuestSchedulerCaptureWaitState wait;

  bool operator==(const GuestSchedulerCaptureEvent&) const = default;
};

// One cooperative signal, anchored to the scheduler tape rather than given a
// sequence of its own: after_scheduler_sequence is the last event sequence
// assigned when the signalling thread bumped the object's epoch. The bump and
// this anchor read happen in one scheduler lock section, so a reready that
// observes the bump is always assigned a strictly greater sequence. The
// identity fields are zero when no guest thread ran the signal.
struct GuestSchedulerCaptureSignalWitness {
  uint64_t after_scheduler_sequence = 0;
  uint64_t capture_instance_id = 0;
  uint32_t guest_thread_id = 0;
  uint32_t object_handle = 0;
  uint32_t signal_epoch = 0;
  uint32_t object_type = 0;

  bool operator==(const GuestSchedulerCaptureSignalWitness&) const = default;
};

// Capture-build-only observer of GuestScheduler transitions. Install one
// continuous observer before the first fiber dispatch and arm or disarm
// capture inside it; GuestScheduler permanently rejects later attachment.
//
// Every callback runs while GuestScheduler holds its scheduler lock, at the
// point of the state mutation the event describes, so the sequence is a total
// order across every dispatch CPU, the watchdog and external callers. The
// callback must therefore be bounded and nonblocking, must not call any
// GuestScheduler, XThread, KernelState or Processor API, and must not retain
// anything beyond the event value. Returning false latches a sticky rejection
// and no further event, including kShutdown, is delivered.
class GuestSchedulerCaptureObserver {
 public:
  virtual ~GuestSchedulerCaptureObserver() = default;

  virtual bool OnSchedulerEvent(
      const GuestSchedulerCaptureEvent& event) noexcept = 0;

  // Delivered under the same lock and the same contract, once per cooperative
  // signal. It consumes no sequence of its own, so a witness never appears in
  // the event stream and never interrupts its continuity.
  virtual bool OnSchedulerSignalWitness(
      const GuestSchedulerCaptureSignalWitness& witness) noexcept = 0;

  // Queried at attachment and at a detachment request made before the first
  // dispatch.
  virtual bool CanDetach() const noexcept = 0;
};

enum class GuestSchedulerCaptureRecorderRejection : uint8_t {
  kNone,
  kInvalidEvent,
  kSequenceGap,
  kSequenceRegression,
  kEventAfterShutdown,
  kOverflow,
  kInvalidSignalWitness,
};

struct GuestSchedulerCaptureRecorderSnapshot {
  GuestSchedulerCaptureRecorderRejection rejection =
      GuestSchedulerCaptureRecorderRejection::kNone;
  bool armed = false;
  bool shutdown_seen = false;
  // Every delivered event, whether or not it was recorded.
  uint64_t delivered_count = 0;
  uint64_t last_sequence = 0;
  std::vector<GuestSchedulerCaptureEvent> events;
  std::vector<GuestSchedulerCaptureSignalWitness> signal_witnesses;
};

// Bounded, nonblocking event recorder satisfying the observer contract. The
// buffer is reserved at construction so no callback allocates. Sequence
// continuity is validated for every delivered event, armed or not, and an
// overflow while armed fails closed instead of dropping events.
class GuestSchedulerCaptureEventRecorder final
    : public GuestSchedulerCaptureObserver {
 public:
  explicit GuestSchedulerCaptureEventRecorder(size_t capacity);
  ~GuestSchedulerCaptureEventRecorder() override;
  GuestSchedulerCaptureEventRecorder(
      const GuestSchedulerCaptureEventRecorder&) = delete;
  GuestSchedulerCaptureEventRecorder& operator=(
      const GuestSchedulerCaptureEventRecorder&) = delete;

  // Begins recording at the next delivered event. Fails after a rejection or
  // shutdown, or while already armed.
  bool Arm();
  // Stops recording; delivered events keep validating continuity.
  bool Disarm();

  bool OnSchedulerEvent(
      const GuestSchedulerCaptureEvent& event) noexcept override;
  bool OnSchedulerSignalWitness(
      const GuestSchedulerCaptureSignalWitness& witness) noexcept override;
  bool CanDetach() const noexcept override;

  GuestSchedulerCaptureRecorderSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  const size_t capacity_;
  std::vector<GuestSchedulerCaptureEvent> events_;
  std::vector<GuestSchedulerCaptureSignalWitness> signal_witnesses_;
  bool armed_ = false;
  bool shutdown_seen_ = false;
  uint64_t delivered_count_ = 0;
  uint64_t last_sequence_ = 0;
  GuestSchedulerCaptureRecorderRejection rejection_ =
      GuestSchedulerCaptureRecorderRejection::kNone;
};

}  // namespace kernel
}  // namespace xe

#endif

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_CAPTURE_OBSERVER_H_
