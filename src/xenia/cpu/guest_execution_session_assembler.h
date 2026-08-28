/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_ASSEMBLER_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_ASSEMBLER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/guest_execution_reel_collector.h"
#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_execution_session_bundle.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/guest_invocation_recorder.h"

namespace xe {
namespace cpu {

enum class GuestExecutionSessionAssemblerState : uint8_t {
  kIdle,
  kArming,
  kStartRendezvous,
  kRecording,
  kStopRequested,
  kStopRendezvous,
  kPublishing,
  kComplete,
  kRejected,
};

// kHold means the caller must park the participant, sink or outer dispatch
// until the session is terminal or recording; a held outer dispatch was not
// admitted and must be presented again after release.
enum class GuestExecutionSessionAssemblerAction : uint8_t {
  kContinue,
  kHold,
  kReject,
};

enum class GuestExecutionSessionAssemblerRejection : uint8_t {
  kNone,
  kInvalidCall,
  kCallbackReentry,
  kLifecycleChanged,
  kParticipantNotHeld,
  kExternalSinkNotHeld,
  kAbnormalHostCall,
  kStartRendezvousTimeout,
  kStopTailLimit,
  kHardLimit,
  kReelRejected,
  kUnsupportedDependency,
  kUnmodeledMutation,
  kRejectedSegment,
  kCheckpointFailure,
  kContentFailure,
  kEncodingFailure,
  kStorageLimit,
  kPublicationFailure,
  kCancelled,
};

// Duration policy values are portable nanoseconds; hard maxima are host ticks.
struct GuestExecutionSessionAssemblerConfig {
  uint64_t session_epoch = 0;
  GuestExecutionSessionBoundaryPolicy boundary;
  GuestExecutionReelLimits limits;
  GuestExecutionReelCoverageMode coverage_mode =
      GuestExecutionReelCoverageMode::kInvocationSegments;
  uint64_t maximum_stop_tail_event_count = 0;
  uint64_t maximum_stop_tail_guest_instruction_count = 0;
  uint64_t maximum_stop_tail_ticks = 0;
  uint64_t maximum_start_rendezvous_ticks = 0;
  uint64_t capture_tick_frequency = 0;
  uint32_t maximum_events_per_chunk = 0;
  // A runtime with a real quiescence controller defers time-based requests
  // until it has stopped every participant and drained the causal event queue.
  bool defer_duration_boundaries = false;
  // Optional external sink that owns PM4 swap markers; once held it may not
  // emit them.
  uint32_t pm4_marker_sink_ordinal = UINT32_MAX;
  GuestExecutionSessionSha256 capture_build_sha256 = {};
  GuestExecutionSessionSha256 replay_config_sha256 = {};
  GuestExecutionSessionSha256 title_identity_sha256 = {};
  GuestExecutionSessionSha256 module_identity_sha256 = {};
  GuestExecutionSessionBundleLimits bundle_limits;
};

struct GuestExecutionSessionAssemblerContent {
  GuestExecutionSessionContentKind kind =
      GuestExecutionSessionContentKind::kGuestPage;
  uint64_t guest_address = 0;
  std::vector<uint8_t> bytes;
};

// Serializes one participant's pointer-free PPC state. Called only while every
// participant is held; the byte size must be identical for every checkpoint.
// A production provider visits ThreadStates under Processor's capture
// registry lock while the session lock is held, so OnParticipantLifecycle must
// never run under that registry lock: the adapter answers Processor lifecycle
// callbacks from its own armed flag and forwards them afterwards.
class GuestExecutionSessionAssemblerStateProvider {
 public:
  virtual ~GuestExecutionSessionAssemblerStateProvider() = default;

  // initial_checkpoint selects the boundary rather than the provider's own
  // phase, because a deferred initial state is asked for after the seal.
  virtual bool EncodeParticipantState(
      const GuestExecutionCaptureParticipantIdentity& participant,
      bool initial_checkpoint, std::vector<uint8_t>* output,
      std::string* error) noexcept = 0;

  // True when this participant's initial state cannot be serialized at the
  // start rendezvous because its continuation route names a captured event
  // that has not happened yet. Such a participant is serialized at
  // publication, with the tape closed; every other one is serialized at the
  // start barrier as before, so a failing provider still rejects early.
  virtual bool DefersInitialParticipantState(
      const GuestExecutionCaptureParticipantIdentity& participant) noexcept {
    return false;
  }
};

// Supplies checkpoint content and the session code corpus, all collected at
// the stop rendezvous while every participant and external sink is held. The
// initial checkpoint is the preimage accumulated over the session of every
// page or code range later touched, dirtied or referenced, so a copy-on-first-
// write provider never snapshots whole memory at start. The final checkpoint
// is the sparse set of every page dirtied since the initial checkpoint, never
// a delta of a delta. The corpus is the permanent definition catalog for the
// whole session; a segment that names a different corpus digest rejects.
class GuestExecutionSessionAssemblerContentProvider {
 public:
  virtual ~GuestExecutionSessionAssemblerContentProvider() = default;

  virtual bool CollectCheckpointContent(
      bool initial_checkpoint,
      std::vector<GuestExecutionSessionAssemblerContent>* output,
      std::string* error) noexcept = 0;
  virtual bool CollectSessionCodeCorpus(std::vector<uint8_t>* output,
                                        std::string* error) noexcept = 0;
};

// Must publish atomically or return false leaving no canonical output.
class GuestExecutionSessionAssemblerPublisher {
 public:
  virtual ~GuestExecutionSessionAssemblerPublisher() = default;

  virtual bool Publish(const GuestExecutionSessionBundle& bundle,
                       std::string* error) noexcept = 0;
};

class GuestExecutionSessionDirectoryPublisher final
    : public GuestExecutionSessionAssemblerPublisher {
 public:
  explicit GuestExecutionSessionDirectoryPublisher(
      std::filesystem::path output_directory,
      GuestExecutionSessionBundleLimits limits = {});

  bool Publish(const GuestExecutionSessionBundle& bundle,
               std::string* error) noexcept override;

  const std::filesystem::path& output_directory() const {
    return output_directory_;
  }

 private:
  std::filesystem::path output_directory_;
  GuestExecutionSessionBundleLimits limits_;
};

// All dependencies must outlive the assembler and must not call back into it.
struct GuestExecutionSessionAssemblerDependencies {
  const ppc::GuestInvocationRecorderClock* clock = nullptr;
  GuestExecutionSessionAssemblerStateProvider* state_provider = nullptr;
  GuestExecutionSessionAssemblerContentProvider* content_provider = nullptr;
  GuestExecutionSessionAssemblerPublisher* publisher = nullptr;
};

struct GuestExecutionSessionAssemblerExternalEvent {
  GuestExecutionSessionEventKind kind =
      GuestExecutionSessionEventKind::kKernelExport;
  GuestExecutionSessionEventDisposition disposition =
      GuestExecutionSessionEventDisposition::kReplayCaptured;
  // Exact dispatch target for kernel/extern events, or range base for MMIO and
  // atomic events. Kernel/extern targets are aligned and have byte_count zero.
  uint64_t guest_address = 0;
  uint64_t byte_count = 0;
  GuestExecutionSessionPayloadKind payload_kind =
      GuestExecutionSessionPayloadKind::kNone;
  std::vector<uint8_t> payload;
};

// GPU, DMA and host sources must name a registered, unheld external sink.
struct GuestExecutionSessionAssemblerMutation {
  GuestExecutionSessionMutationSource source =
      GuestExecutionSessionMutationSource::kNone;
  GuestExecutionSessionEventDisposition disposition =
      GuestExecutionSessionEventDisposition::kReplayCaptured;
  uint32_t external_sink_ordinal = UINT32_MAX;
  uint64_t guest_address = 0;
  uint64_t byte_count = 0;
  std::vector<uint8_t> bytes;
};

// code_corpus_sha256 is optional producer evidence; when nonzero it must match
// the session corpus collected at stop.
struct GuestExecutionSessionAssemblerSegmentEnd {
  GuestInvocationCaptureStatus status;
  GuestExecutionSessionSha256 code_corpus_sha256 = {};
  std::vector<uint8_t> segment;
};

struct GuestExecutionSessionAssemblerParticipantStatus {
  GuestExecutionCaptureParticipantIdentity identity;
  uint32_t ordinal = 0;
  uint32_t host_call_depth = 0;
  bool arrived = false;
  bool held = false;
  GuestExecutionSessionInitialOuterCallState initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kOutside;
  GuestExecutionSessionBoundaryArrivalKind boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  uint64_t first_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint64_t held_after_event_sequence = 0;
};

struct GuestExecutionSessionAssemblerExternalSinkStatus {
  std::string name;
  bool held = false;
};

struct GuestExecutionSessionAssemblerStatus {
  GuestExecutionSessionAssemblerState state =
      GuestExecutionSessionAssemblerState::kIdle;
  GuestExecutionReelCoverageMode coverage_mode =
      GuestExecutionReelCoverageMode::kInvocationSegments;
  GuestExecutionSessionAssemblerRejection rejection =
      GuestExecutionSessionAssemblerRejection::kNone;
  GuestExecutionReelStopReason stop_reason =
      GuestExecutionReelStopReason::kNone;
  uint64_t next_event_sequence = 1;
  uint64_t next_segment_ordinal = 0;
  uint64_t stop_request_event_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint64_t capture_start_tick = 0;
  uint64_t stop_request_tick = 0;
  uint64_t capture_end_tick = 0;
  uint64_t accepted_segment_count = 0;
  uint64_t guest_instruction_count = 0;
  uint32_t encoded_chunk_count = 0;
  uint64_t content_blob_count = 0;
  // Work currently staged in memory; a rejected session drops it to zero.
  uint64_t staged_event_count = 0;
  uint64_t staged_chunk_bytes = 0;
  uint64_t content_blob_bytes = 0;
  bool published = false;
  std::string message;
  std::vector<GuestExecutionSessionAssemblerParticipantStatus> participants;
  std::vector<GuestExecutionSessionAssemblerExternalSinkStatus> external_sinks;
};

// Fixed-participant continuous session owner. Participants are ThreadState
// lifetime instances identified by capture instance id; the roster is frozen
// at Arm() and any later lifecycle event rejects the session. Every input is
// serialized, assigned exactly one global sequence and bounded by the reel
// collector, the stop-tail maxima and the bundle limits; a bound that cannot
// be met rejects rather than truncates. Start and stop wait for every
// participant to be reported by the adapter as already outside guest code,
// arrived at a JIT safepoint or returning from its outer host call, and for
// every external sink to be held, before any checkpoint is taken. The class
// never parks a thread itself. Outside participants are not parked, so any
// host-side write between hold and checkpoint must arrive through a
// registered kHost sink. Invocation coverage requires exactly one segment per
// admitted outer dispatch. Continuous coverage rejects segments and requires
// participant instruction progress. A segment straddling the start rejects.
// The adapter must snapshot roster depth, seed and start forwarding host-call
// callbacks atomically under its own lock. kHold has no wake-up: parked
// callers poll status(). There is one global open segment, so concurrent root
// invocations on two participants reject with kInvalidCall by design. A live
// XThread sits at host-call depth one or more for its whole life, so kOutside
// auto-arrival covers only never-dispatched or fully-returned threads and a
// blocked thread arrives only through a safepoint. std::bad_alloc through the
// noexcept adapters terminates, so configure tight bundle_limits. Nothing is
// published before Publish() succeeds and a rejected session never publishes.
// This class has been exercised only with a fake clock, fake providers and a
// fake publisher; no title capture has occurred.
class GuestExecutionSessionAssembler final {
 public:
  static constexpr uint32_t kNoExternalSink = UINT32_MAX;

  static std::unique_ptr<GuestExecutionSessionAssembler> Create(
      const GuestExecutionSessionAssemblerConfig& config,
      const GuestExecutionSessionAssemblerDependencies& dependencies,
      std::string* error = nullptr);

  ~GuestExecutionSessionAssembler();
  GuestExecutionSessionAssembler(const GuestExecutionSessionAssembler&) =
      delete;
  GuestExecutionSessionAssembler& operator=(
      const GuestExecutionSessionAssembler&) = delete;

  // Accepts the observer's whole kReady seed transaction once while idle, or
  // rejects it without change. Active host-call depth is derived from the
  // roster snapshot taken in the same transaction; a roster call whose owner
  // is not seeded, or a roster already rejected, rejects the seed.
  bool SeedParticipants(
      std::span<const GuestExecutionCaptureThreadStateLifecycleEvent> seeds,
      const GuestExecutionCaptureHostCallRosterSnapshot& roster);
  GuestExecutionSessionAssemblerAction OnParticipantLifecycle(
      const GuestExecutionCaptureThreadStateLifecycleEvent& event);
  bool RegisterExternalSink(std::string name, uint32_t* out_ordinal);

  bool Arm(std::string* error = nullptr);
  bool RequestStart(std::string* error = nullptr);
  GuestExecutionSessionAssemblerAction RequestStop();
  GuestExecutionSessionAssemblerAction RequestDeferredDurationStop();
  GuestExecutionSessionAssemblerAction Poll();

  GuestExecutionSessionAssemblerAction OnExternalSinkHeld(
      uint32_t sink_ordinal);
  GuestExecutionSessionAssemblerAction ArriveAtSafepoint(
      const GuestExecutionCaptureParticipantIdentity& participant);
  GuestExecutionSessionAssemblerAction OnOuterHostCallBegin(
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint32_t function_address, uint32_t function_end_address,
      uint32_t return_address);
  GuestExecutionSessionAssemblerAction OnOuterHostCallEnd(
      const GuestExecutionCaptureParticipantIdentity& participant,
      GuestExecutionCaptureHostCallOutcome outcome);

  GuestExecutionSessionAssemblerAction OnSegmentBegin(
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint32_t entry_guest_address, uint32_t end_guest_address);
  GuestExecutionSessionAssemblerAction OnSegmentEnd(
      const GuestExecutionCaptureParticipantIdentity& participant,
      GuestExecutionSessionAssemblerSegmentEnd segment_end);
  GuestExecutionSessionAssemblerAction OnInstructionCoverage(
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint64_t guest_instruction_delta);
  GuestExecutionSessionAssemblerAction OnGuestMarker(
      const std::optional<GuestExecutionCaptureParticipantIdentity>&
          participant,
      GuestExecutionSessionMarkerSource source, uint64_t marker_identity);
  // global_sequence receives the sequence assigned to the accepted event, and
  // zero on any rejection, so a producer that must name its own event durably
  // never has to guess it.
  GuestExecutionSessionAssemblerAction OnExternalEvent(
      const std::optional<GuestExecutionCaptureParticipantIdentity>&
          participant,
      GuestExecutionSessionAssemblerExternalEvent event,
      uint64_t* global_sequence = nullptr);
  GuestExecutionSessionAssemblerAction OnMemoryMutation(
      const std::optional<GuestExecutionCaptureParticipantIdentity>&
          participant,
      GuestExecutionSessionAssemblerMutation mutation);
  GuestExecutionSessionAssemblerAction OnUnsupportedDependency(
      const std::optional<GuestExecutionCaptureParticipantIdentity>&
          participant,
      uint32_t dependency_flags);

  // Runs the checkpoint providers, encodes every chunk, validates the complete
  // session and hands it to the publisher exactly once with the session lock
  // released. Legal only after every participant and external sink is held.
  // status().published reports the publisher's result even if the session was
  // cancelled meanwhile.
  bool Publish(std::string* error = nullptr);
  void Cancel(std::string message);

  GuestExecutionSessionAssemblerStatus status() const;

 private:
  struct Impl;

  explicit GuestExecutionSessionAssembler(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_ASSEMBLER_H_
