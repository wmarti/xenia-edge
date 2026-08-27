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
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/guest_execution_session_assembler.h"
#include "xenia/gpu/pm4_marker_sink.h"
#include "xenia/kernel/guest_scheduler_capture_observer.h"
#include "xenia/kernel/guest_scheduler_checkpoint.h"

namespace xe {
class Memory;
namespace gpu {
class CommandProcessor;
}
namespace kernel {
class GuestScheduler;
}

namespace cpu {

class Processor;
class GuestExecutionMarkerController;
class GuestExecutionMarkerClock;
struct GuestExecutionMarkerControllerConfig;
class GuestExecutionSessionCaptureProvider;
class GuestExecutionSessionCaptureRuntimePm4Wiring;
class GuestExecutionSessionTitleCaptureRuntimeTestAccess;

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
  kExternalSinkControl,
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

struct GuestExecutionSessionInstructionCoverageDelta {
  GuestExecutionCaptureParticipantIdentity participant;
  uint64_t guest_instruction_delta = 0;
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
  virtual bool CollectInstructionCoverageDeltas(
      std::vector<GuestExecutionSessionInstructionCoverageDelta>* output,
      std::string* error) noexcept = 0;
  virtual bool SealCapture(
      const kernel::GuestSchedulerCheckpointBarrierSnapshot& checkpoint,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
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

// Control-worker seam for the configured PM4 marker source. Hold returns an
// attestation only after admission is closed and every callback in its sink
// generation has returned. A repeated healthy hold returns the same token;
// false must not change attachment state. AcknowledgeArmAndResumeAfterStart
// must acknowledge the marker controller's arm boundary while the source is
// still held, then resume using exactly that token. False leaves it held.
// IsSourceHealthy covers dispatcher loss and marker-controller rejection after
// attach. SealAndDetach validates the final hold token and atomically removes
// the source before the runtime releases its stop checkpoint or publishes.
// AbortAndDetach drains/removes the source on every rejected or cancelled path.
// The owner must keep this object and its callback target alive through runtime
// Shutdown.
class GuestExecutionSessionCaptureRuntimeExternalSink {
 public:
  virtual ~GuestExecutionSessionCaptureRuntimeExternalSink() = default;

  virtual bool Hold(gpu::Pm4MarkerHoldToken* token,
                    std::string* error) noexcept = 0;
  virtual bool AcknowledgeArmAndResumeAfterStart(
      const gpu::Pm4MarkerHoldToken& token, std::string* error) noexcept = 0;
  virtual bool IsSourceHealthy(std::string* error) const noexcept = 0;
  virtual bool SealAndDetach(const gpu::Pm4MarkerHoldToken& token,
                             std::string* error) noexcept = 0;
  virtual bool AbortAndDetach(std::string* error) noexcept = 0;
};

// Production control path joining the session runtime to one command
// processor and its real marker controller. The owning PM4 wiring below
// activates it only after the runtime callback target exists.
class GuestExecutionSessionCaptureRuntimePm4ExternalSink final
    : public GuestExecutionSessionCaptureRuntimeExternalSink {
 public:
  ~GuestExecutionSessionCaptureRuntimePm4ExternalSink() override;
  GuestExecutionSessionCaptureRuntimePm4ExternalSink(
      const GuestExecutionSessionCaptureRuntimePm4ExternalSink&) = delete;
  GuestExecutionSessionCaptureRuntimePm4ExternalSink& operator=(
      const GuestExecutionSessionCaptureRuntimePm4ExternalSink&) = delete;

  bool Hold(gpu::Pm4MarkerHoldToken* token,
            std::string* error) noexcept override;
  bool AcknowledgeArmAndResumeAfterStart(const gpu::Pm4MarkerHoldToken& token,
                                         std::string* error) noexcept override;
  bool IsSourceHealthy(std::string* error) const noexcept override;
  bool SealAndDetach(const gpu::Pm4MarkerHoldToken& token,
                     std::string* error) noexcept override;
  bool AbortAndDetach(std::string* error) noexcept override;

 private:
  friend class GuestExecutionSessionCaptureRuntimePm4Wiring;

  GuestExecutionSessionCaptureRuntimePm4ExternalSink(
      gpu::CommandProcessor& command_processor,
      std::shared_ptr<GuestExecutionMarkerController> marker_controller);

  bool Activate(std::string* error) noexcept;
  bool IsSourceHealthyLocked(std::string* error) const noexcept;

  gpu::CommandProcessor& command_processor_;
  std::shared_ptr<GuestExecutionMarkerController> marker_controller_;
  mutable std::mutex mutex_;
  bool attached_ = false;
  bool terminally_detached_ = false;
};

struct GuestExecutionSessionCaptureRuntimeDependencies {
  const ppc::GuestInvocationRecorderClock* clock = nullptr;
  GuestExecutionSessionCaptureRuntimeProvider* provider = nullptr;
  GuestExecutionSessionCaptureRuntimeEventBridge* event_bridge = nullptr;
  GuestExecutionSessionAssemblerPublisher* publisher = nullptr;
  GuestExecutionSessionCaptureRuntimeCheckpointController*
      checkpoint_controller = nullptr;
  GuestExecutionSessionCaptureRuntimeExternalSink* pm4_external_sink = nullptr;
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
  bool external_sink_registered = false;
  bool external_sink_held = false;
  bool external_sink_terminally_detached = false;
  bool external_sink_control_failed = false;
  // Most recent successful Hold attestation. These remain observable after a
  // start resume and are replaced by the terminal-stop hold.
  uint64_t external_sink_attested_generation = 0;
  uint64_t external_sink_hold_epoch = 0;
  uint64_t external_sink_last_ordinal = 0;
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
  // Fixed-size source ingress for PM4 and other no-thread guest markers. The
  // control worker is the only caller of the assembler; markers before the
  // session becomes active are intentionally ignored.
  bool OnGuestMarker(GuestExecutionSessionMarkerSource source,
                     uint64_t marker_identity) noexcept;
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
  friend class GuestExecutionSessionCaptureRuntimePm4Wiring;

  struct Impl;

  explicit GuestExecutionSessionCaptureRuntime(std::unique_ptr<Impl> impl);
  bool Attach(std::string* error);
  bool IsControlWorkerThread() const noexcept;
  void SetRequestStartPrequeueTestHook(void (*hook)(void*),
                                       void* context) noexcept;

  std::unique_ptr<Impl> impl_;
};

// Complete production PM4 capture composition. It installs the no-allocation
// marker bridge before exposing the begun controller to the command processor,
// and owns the runtime/source lifetime order as one unit. Destroy it from a
// non-control-worker thread while Processor, GuestScheduler, CommandProcessor,
// and every object in dependencies are still alive. Like std::thread, violating
// the non-worker destruction precondition terminates instead of returning with
// callbacks that can access torn-down caller-owned state.
class GuestExecutionSessionCaptureRuntimePm4Wiring final {
 public:
  static std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4Wiring>
  CreateAndAttach(
      Processor& processor, kernel::GuestScheduler& scheduler,
      gpu::CommandProcessor& command_processor,
      const GuestExecutionSessionCaptureRuntimeConfig& runtime_config,
      const GuestExecutionSessionCaptureRuntimeDependencies& dependencies,
      const GuestExecutionMarkerControllerConfig& marker_config,
      const GuestExecutionMarkerClock& marker_clock,
      std::string* error = nullptr);

  ~GuestExecutionSessionCaptureRuntimePm4Wiring();
  GuestExecutionSessionCaptureRuntimePm4Wiring(
      const GuestExecutionSessionCaptureRuntimePm4Wiring&) = delete;
  GuestExecutionSessionCaptureRuntimePm4Wiring& operator=(
      const GuestExecutionSessionCaptureRuntimePm4Wiring&) = delete;

  GuestExecutionSessionCaptureRuntime& runtime() noexcept;
  const GuestExecutionSessionCaptureRuntime& runtime() const noexcept;
  const std::shared_ptr<GuestExecutionMarkerController>& marker_controller()
      const noexcept;

 private:
  struct MarkerBridge;
  struct Lifetime;

  explicit GuestExecutionSessionCaptureRuntimePm4Wiring(
      std::unique_ptr<Lifetime> lifetime) noexcept;

  std::unique_ptr<Lifetime> lifetime_;
};

struct GuestExecutionSessionTitleCaptureConfig {
  std::filesystem::path output_directory;
  uint64_t warmup_milliseconds = 100000;
  uint64_t stop_marker_count = 1;
  uint64_t maximum_bundle_bytes = 1ull << 30;
};

// Title-lifetime production composition. Provider attachment is deliberately
// separate from runtime attachment: the provider must observe definitions made
// while the executable module loads, while stable title/module identities are
// not available until that load completes. Shutdown destroys the PM4 wiring
// before the provider and must run before CommandProcessor, GuestScheduler,
// Processor, or Memory teardown.
class GuestExecutionSessionTitleCaptureRuntime final {
 public:
  static bool IsRequested() noexcept;
  static std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>
  CreateAndAttachProvider(Memory& memory, Processor& processor,
                          bool guest_scheduler_enabled,
                          std::string* error = nullptr);
  static std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>
  CreateAndAttachProvider(Memory& memory, Processor& processor,
                          const GuestExecutionSessionTitleCaptureConfig& config,
                          bool guest_scheduler_enabled,
                          std::string* error = nullptr);

  ~GuestExecutionSessionTitleCaptureRuntime();
  GuestExecutionSessionTitleCaptureRuntime(
      const GuestExecutionSessionTitleCaptureRuntime&) = delete;
  GuestExecutionSessionTitleCaptureRuntime& operator=(
      const GuestExecutionSessionTitleCaptureRuntime&) = delete;

  bool AttachRuntime(kernel::GuestScheduler& scheduler,
                     gpu::CommandProcessor& command_processor,
                     std::string_view title_identity,
                     std::string_view module_identity,
                     std::string* error = nullptr);
  void Shutdown() noexcept;
  bool runtime_attached() const noexcept;
  const std::filesystem::path& output_directory() const noexcept;

 private:
  friend class GuestExecutionSessionTitleCaptureRuntimeTestAccess;
  struct Impl;
  explicit GuestExecutionSessionTitleCaptureRuntime(
      std::unique_ptr<Impl> impl) noexcept;
  static std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>
  CreateAndAttachProviderWithProvenance(
      Memory& memory, Processor& processor,
      const GuestExecutionSessionTitleCaptureConfig& config,
      bool guest_scheduler_enabled,
      const GuestExecutionSessionSha256& capture_build_sha256,
      const GuestExecutionSessionSha256& replay_config_sha256,
      std::string* error);

  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_RUNTIME_H_
