/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_RUNTIME_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_RUNTIME_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/guest_execution_session_assembler.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/kernel/guest_scheduler_checkpoint.h"

namespace xe {
namespace kernel {
class GuestScheduler;
}

namespace cpu {

class Processor;

enum class GuestExecutionSessionCaptureRuntimeState : uint8_t {
  kIdle,
  kStarting,
  kRecording,
  kStopping,
  kPublishing,
  kComplete,
  kRejected,
  kShutdown,
};

enum class GuestExecutionSessionCaptureRuntimeRejection : uint8_t {
  kNone,
  kInvalidConfiguration,
  kSourceAttachment,
  kSourceRejected,
  kQueueOverflow,
  kSchedulerSequence,
  kCheckpointBarrier,
  kCheckpointRoster,
  kMissingCheckpointCapability,
  kProviderFailure,
  kAssemblerFailure,
  kEventBridgeFailure,
  kBundleValidation,
  kPublicationFailure,
  kCancelled,
};

struct GuestExecutionSessionCaptureRuntimeConfig {
  GuestExecutionSessionAssemblerConfig assembler;
  // A power of two. All callback records are fixed-size and allocated before
  // either permanent observer is attached.
  size_t event_queue_capacity = 1u << 16;
  std::chrono::milliseconds checkpoint_timeout =
      std::chrono::milliseconds(5000);
  std::chrono::milliseconds control_poll_interval =
      std::chrono::milliseconds(1);
  // Failed release attempts retain the generation and are retried this many
  // times before the terminal status exposes the still-owned barrier.
  size_t checkpoint_release_attempts = 3;
};

// Owns the exact-PC checkpoint encoding and sparse memory/code observation.
// BeginCapture runs while the provisional start barrier is held and must arm
// every writer before returning. SealCapture runs while the stop barrier is
// held and must freeze the final sparse view. The inherited assembler methods
// are called only between those operations on the control worker.
class GuestExecutionSessionCaptureRuntimeProvider
    : public GuestExecutionSessionAssemblerStateProvider,
      public GuestExecutionSessionAssemblerContentProvider {
 public:
  ~GuestExecutionSessionCaptureRuntimeProvider() override = default;

  virtual bool SupportsCheckpointParticipant(
      const kernel::GuestSchedulerCheckpointParticipant& participant,
      std::string* error) noexcept = 0;
  virtual bool BeginCapture(
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          participants,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) noexcept = 0;
  virtual bool SealCapture(
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::string* error) noexcept = 0;
  // Disarms every writer after the staged bundle no longer needs the source.
  // accepted is true only after the scheduler generation was revalidated.
  virtual void EndCapture(bool accepted) noexcept = 0;
};

// Translates the scheduler's pointer-free observation stream into the session
// event model and closes the authenticated continuous-event overlay in the
// staged bundle. Implementations must account for every forwarded scheduler
// event. No implementation means no scheduler-on capture capability.
class GuestExecutionSessionCaptureRuntimeEventBridge {
 public:
  virtual ~GuestExecutionSessionCaptureRuntimeEventBridge() = default;

  virtual bool BeginSession(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          participants,
      std::string* error) noexcept = 0;
  virtual GuestExecutionSessionAssemblerAction OnSchedulerEvent(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCaptureEvent& event,
      std::string* error) noexcept = 0;
  virtual bool SealSession(
      GuestExecutionSessionAssembler& assembler,
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      std::string* error) noexcept = 0;
  virtual bool FinalizeBundle(GuestExecutionSessionBundle* bundle,
                              uint64_t scheduler_event_count,
                              std::string* error) noexcept = 0;
};

// Test and alternate-runtime seam. The production path leaves this null and
// calls GuestScheduler's three checkpoint barrier methods directly.
class GuestExecutionSessionCaptureRuntimeCheckpointController {
 public:
  virtual ~GuestExecutionSessionCaptureRuntimeCheckpointController() = default;
  virtual kernel::GuestSchedulerCheckpointBarrierRejection Pause(
      std::chrono::milliseconds timeout,
      kernel::GuestSchedulerCheckpointBarrierSnapshot* snapshot) = 0;
  virtual kernel::GuestSchedulerCheckpointBarrierRejection Finalize(
      uint64_t generation,
      kernel::GuestSchedulerCheckpointBarrierSnapshot* snapshot) = 0;
  virtual kernel::GuestSchedulerCheckpointBarrierRejection Cancel(
      uint64_t generation,
      kernel::GuestSchedulerCheckpointBarrierSnapshot* snapshot) = 0;
};

struct GuestExecutionSessionCaptureRuntimeDependencies {
  const ppc::GuestInvocationRecorderClock* clock = nullptr;
  GuestExecutionSessionCaptureRuntimeProvider* provider = nullptr;
  GuestExecutionSessionCaptureRuntimeEventBridge* event_bridge = nullptr;
  GuestExecutionSessionAssemblerPublisher* publisher = nullptr;
  GuestExecutionSessionCaptureRuntimeCheckpointController*
      checkpoint_controller = nullptr;
};

struct GuestExecutionSessionCaptureRuntimeStatus {
  GuestExecutionSessionCaptureRuntimeState state =
      GuestExecutionSessionCaptureRuntimeState::kIdle;
  GuestExecutionSessionCaptureRuntimeRejection rejection =
      GuestExecutionSessionCaptureRuntimeRejection::kNone;
  uint64_t checkpoint_generation = 0;
  uint64_t queued_event_count = 0;
  uint64_t processed_event_count = 0;
  uint64_t scheduler_event_count = 0;
  uint64_t last_scheduler_sequence = 0;
  bool processor_attached = false;
  bool scheduler_attached = false;
  bool provider_armed = false;
  bool canonical_output_published = false;
  bool worker_running = false;
  bool shutdown_pending = false;
  std::string message;
};

// One-shot scheduler-on capture coordinator. Permanent source callbacks only
// update the host-call roster or copy a fixed-size value into the bounded MPSC
// queue. The sole worker owns every assembler, provider, checkpoint and
// publication call. Before RequestStart, scheduler events still pay for
// permanent observation and sequence validation, and host calls still update
// the live roster; no assembler, provider or event queue work is performed.
// Publication decodes every initial and final thread state and binds its
// exact-PC route to the corresponding scheduler checkpoint held at that
// boundary; currently only restorable JIT safepoints are representable.
// CreateAndAttach is the only operation that installs observers, and
// RequestStart is required to arm a session.
class GuestExecutionSessionCaptureRuntime final
    : public GuestExecutionCaptureHostCallObserver,
      public kernel::GuestSchedulerCaptureObserver,
      public std::enable_shared_from_this<GuestExecutionSessionCaptureRuntime> {
 public:
  static std::shared_ptr<GuestExecutionSessionCaptureRuntime> CreateAndAttach(
      Processor& processor, kernel::GuestScheduler& scheduler,
      const GuestExecutionSessionCaptureRuntimeConfig& config,
      const GuestExecutionSessionCaptureRuntimeDependencies& dependencies,
      std::string* error = nullptr);

  ~GuestExecutionSessionCaptureRuntime() override;
  GuestExecutionSessionCaptureRuntime(
      const GuestExecutionSessionCaptureRuntime&) = delete;
  GuestExecutionSessionCaptureRuntime& operator=(
      const GuestExecutionSessionCaptureRuntime&) = delete;

  bool RequestStart() noexcept;
  bool RequestStop() noexcept;
  // Establishes a linearizable publication fence and joins the worker. A
  // canonical Publish call that entered first completes before this returns;
  // after this returns, no publication can still start or complete.
  // A Publisher reentering this method on the worker establishes the fence but
  // cannot join itself; its already-admitted Publish may finish, and a later
  // non-worker call performs the join and observer detachment.
  void Shutdown() noexcept;
  bool WaitForTerminal(std::chrono::milliseconds timeout) const;
  GuestExecutionSessionCaptureRuntimeStatus status() const;

  GuestExecutionCaptureThreadStateLifecycleDisposition OnThreadStateSeed(
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
          events) noexcept override;
  GuestExecutionCaptureThreadStateLifecycleDisposition OnThreadStateLifecycle(
      GuestExecutionCaptureThreadStateLifecycleEvent event) noexcept override;
  GuestExecutionCaptureJitSafepointDisposition OnJitSafepoint(
      const ThreadState& thread_state,
      uint32_t guest_address) noexcept override;
  GuestExecutionCaptureHostCallToken OnHostGuestCallBegin(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address) noexcept override;
  bool OnHostGuestCallEnd(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome) noexcept override;
  bool OnSchedulerEvent(
      const kernel::GuestSchedulerCaptureEvent& event) noexcept override;
  bool CanDetach() const noexcept override;

 private:
  friend class GuestExecutionSessionCaptureRuntimeTestAccess;

  struct Impl;

  explicit GuestExecutionSessionCaptureRuntime(std::unique_ptr<Impl> impl);
  bool Attach(std::string* error);
  void SetRequestStartPrequeueTestHook(void (*hook)(void*),
                                       void* context) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_RUNTIME_H_
