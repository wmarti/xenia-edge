/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_RUNNER_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_RUNNER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_execution_continuous_event.h"
#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_execution_session_bundle.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_runner.h"

namespace xe {
class Memory;

namespace cpu {

class ExactJitCorpusModule;
class Function;
class GuestExecutionReplayTape;
class GuestFunction;
class Processor;
class ThreadState;

namespace backend {
class Backend;
}  // namespace backend

// What the replay does at one tape cursor. Participant control events are
// committed as the tape record at the cursor the worker reaches; they carry no
// auxiliary fields, so the commit itself cannot diverge. What execution
// generates and the runner compares is ordering (kSegmentEnd is reached only
// after Function::Call returned) plus the register and page comparisons at
// segment entry and exit. Coordinator roles inject only the typed payload of a
// captured event.
enum class GuestExecutionSessionReplayEventRole : uint32_t {
  kOuterHostCallBegin = 1,
  kSegmentBegin = 2,
  kSegmentEnd = 3,
  kOuterHostCallEnd = 4,
  kBoundaryControl = 5,
  kGuestMarker = 6,
  kAsynchronousMutation = 7,
};

constexpr uint32_t kGuestExecutionSessionReplayNoIndex = UINT32_MAX;

struct GuestExecutionSessionReplayEvent {
  GuestExecutionSessionEvent event;
  GuestExecutionSessionReplayEventRole role =
      GuestExecutionSessionReplayEventRole::kBoundaryControl;
  uint32_t segment_index = kGuestExecutionSessionReplayNoIndex;
  // An intermediate checkpoint verified by the owner before this event commits
  // during a full-verification repetition.
  uint32_t checkpoint_index = kGuestExecutionSessionReplayNoIndex;
};

struct GuestExecutionSessionReplaySegment {
  uint32_t thread_ordinal = 0;
  uint64_t begin_sequence = 0;
  uint64_t end_sequence = 0;
  ppc::GuestFunctionInvocation invocation;
};

// initial_state_blob borrows the canonical ppc::GuestPPCRegisterStateCodec
// bytes from the bundle; initial_state is its decoded, re-encoded-equal form.
struct GuestExecutionSessionReplayParticipant {
  uint32_t guest_thread_id = 0;
  ppc::GuestPPCRegisterState initial_state;
  const std::vector<uint8_t>* initial_state_blob = nullptr;
  std::vector<uint32_t> event_indices;
  uint32_t segment_count = 0;
};

// Initial bytes are borrowed from the exact corpus or a bundle content blob.
struct GuestExecutionSessionReplayPage {
  uint32_t guest_address = 0;
  bool code = false;
  const uint8_t* initial_data = nullptr;
};

// Pure planning result. The plan borrows page bytes and blobs from the bundle
// and its own decoded corpus, so it is move-only and the bundle must outlive
// it.
//
// Accepted session shape (anything else rejects, naming the reason):
// - exactly one exact code corpus across all segments;
// - every participant starts outside an outer host call, or is parked below
//   one it never arrived at and then owns no segment and no event, with a
//   non-safepoint boundary arrival, and its state blob round-trips
//   byte-exactly through ppc::GuestPPCRegisterStateCodec;
// - every segment input page, every later-checkpoint page and every
//   asynchronous-mutation page is supplied by the initial checkpoint; corpus
//   pages are the only code source and checkpoint code references must equal
//   them;
// - participant-owned events are only kOuterHostCallBegin/kSegmentBegin/
//   kSegmentEnd/kOuterHostCallEnd, an outer call brackets exactly one segment,
//   and no segment begins while another is open on any participant;
// - coordinator-owned events are only kBoundaryRequest/kBoundaryHeld, PM4
//   kGuestMarker and asynchronous kMemoryMutation at quiescent cursors;
// - intermediate checkpoints sit at quiescent cursors and are compared only
//   for the states and pages they reference; the final checkpoint additionally
//   requires every unreferenced data page to equal its initial bytes, which
//   rejects a sparse delta final checkpoint.
// Participant-owned kMemoryMutation, kInstructionCoverage, kThreadDispatch,
// kSynchronization, kernel/extern, MMIO, clock, atomic, interrupt and guest-
// defined marker events, kActive initial outer-call state and kJitSafepoint
// arrival all reject, so a real title session cannot replay through this
// runner until those hooks exist. A parked participant is restored once and
// compared unchanged, which is the whole of what its class claims happened.
struct GuestExecutionSessionReplayPlan {
  GuestExecutionSessionReplayPlan() = default;
  GuestExecutionSessionReplayPlan(GuestExecutionSessionReplayPlan&&) = default;
  GuestExecutionSessionReplayPlan& operator=(
      GuestExecutionSessionReplayPlan&&) = default;
  GuestExecutionSessionReplayPlan(const GuestExecutionSessionReplayPlan&) =
      delete;
  GuestExecutionSessionReplayPlan& operator=(
      const GuestExecutionSessionReplayPlan&) = delete;

  uint32_t host_page_size = 0;
  // Corpus workload of the single shared corpus; every segment plan reports
  // the same values and the planner rejects if they differ.
  uint64_t eager_function_count = 0;
  uint64_t eager_guest_code_bytes = 0;
  uint64_t captured_host_code_bytes = 0;
  ExecutionJitCorpus corpus;
  std::vector<GuestExecutionSessionReplaySegment> segments;
  std::vector<GuestExecutionSessionReplayParticipant> participants;
  std::vector<GuestExecutionSessionReplayEvent> events;
  std::vector<uint32_t> coordinator_event_indices;
  std::vector<GuestExecutionSessionReplayPage> pages;
  std::vector<GuestInvocationReplayProtectionGranule> protection_granules;
  std::vector<uint32_t> reset_page_addresses;
  // In chunk order: the initial checkpoint first, the final checkpoint last.
  std::vector<GuestExecutionSessionCheckpoint> checkpoints;
  std::map<GuestExecutionSessionSha256, const std::vector<uint8_t>*> blobs;
};

// Validates the complete bundle, resolves every segment digest to its exact
// invocation artifact and corpus blob, binds provenance hashes and derives the
// memory closure and event roles. Events without a replay hook in this runner
// reject the session with the offending kind instead of being skipped.
bool BuildGuestExecutionSessionReplayPlan(
    const GuestExecutionSessionBundle& bundle, uint32_t host_page_size,
    GuestExecutionSessionReplayPlan* output, std::string* error = nullptr);

struct GuestExecutionContinuousReplayResumeEntry {
  uint32_t resume_pc = 0;
  uint32_t owning_function_address = 0;
  uint32_t owning_function_end_address = 0;

  bool operator==(const GuestExecutionContinuousReplayResumeEntry&) const =
      default;
};

struct GuestExecutionContinuousReplayParticipant {
  uint32_t ordinal = 0;
  uint32_t guest_thread_id = 0;
  GuestExecutionSessionInitialOuterCallState initial_outer_call_state =
      GuestExecutionSessionInitialOuterCallState::kOutside;
  GuestExecutionSessionBoundaryArrivalKind boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  uint64_t held_after_event_sequence = 0;
  ppc::GuestPPCThreadCheckpoint initial_checkpoint;
  ppc::GuestPPCThreadCheckpoint final_checkpoint;
};

struct GuestExecutionContinuousReplayEvent {
  GuestExecutionSessionEvent canonical;
  GuestExecutionContinuousEvent control;
  const std::vector<uint8_t>* payload = nullptr;
};

struct GuestExecutionContinuousReplayPlan {
  GuestExecutionContinuousReplayPlan() = default;
  GuestExecutionContinuousReplayPlan(GuestExecutionContinuousReplayPlan&&) =
      default;
  GuestExecutionContinuousReplayPlan& operator=(
      GuestExecutionContinuousReplayPlan&&) = default;
  GuestExecutionContinuousReplayPlan(
      const GuestExecutionContinuousReplayPlan&) = delete;
  GuestExecutionContinuousReplayPlan& operator=(
      const GuestExecutionContinuousReplayPlan&) = delete;

  uint32_t host_page_size = 0;
  ExecutionJitCorpus corpus;
  GuestExecutionSessionCheckpoint initial_session_checkpoint;
  GuestExecutionSessionCheckpoint final_session_checkpoint;
  // Authoritative scheduler seed and closure decoded from the quiescent
  // boundary snapshots. Replay event transitions must start from these rather
  // than inferring queue state from the first observed scheduler event.
  GuestExecutionSessionSchedulerTopologyChunk initial_scheduler_topology;
  GuestExecutionSessionSchedulerTopologyChunk final_scheduler_topology;
  // Provenance of every cooperative signal the interval recorded. Empty when
  // the bundle carries no witness table, which no consumer may read as
  // evidence that a signal-epoch wake had no cause.
  std::vector<GuestExecutionSessionSignalWitness> signal_witnesses;
  std::vector<GuestExecutionContinuousReplayParticipant> participants;
  std::vector<GuestExecutionContinuousReplayEvent> events;
  std::vector<GuestExecutionContinuousReplayResumeEntry> resume_entries;
  std::vector<GuestExecutionSessionReplayPage> pages;
  std::vector<GuestInvocationReplayProtectionGranule> protection_granules;
  std::vector<uint32_t> reset_page_addresses;
  std::map<GuestExecutionSessionSha256, const std::vector<uint8_t>*> blobs;
};

bool BuildGuestExecutionContinuousReplayPlan(
    const GuestExecutionSessionBundle& bundle, uint32_t host_page_size,
    GuestExecutionContinuousReplayPlan* output, std::string* error = nullptr);

struct GuestExecutionSessionReplayParticipantMetrics {
  uint64_t thread_cpu_nanoseconds = 0;
  uint64_t uptime_raw_nanoseconds = 0;
  uint64_t consumed_event_count = 0;
  uint64_t restore_thread_cpu_nanoseconds = 0;
  uint64_t in_interval_verification_thread_cpu_nanoseconds = 0;
  uint64_t in_interval_verification_count = 0;
};

// Restore, the interval and final verification are accounted separately.
// participant_thread_cpu_nanoseconds is the sum of every worker's own Mach
// thread CPU between its first acquire and its last commit, including the
// stop-tail segments after stop_request_event_sequence. In timed repetitions
// no verification runs inside the interval, so that sum is the primary
// metric; the full-verification warmup repetition compares segment entry and
// exit state and intermediate checkpoints inside the interval and reports
// that work in in_interval_verification_* (sampled, so it cannot be
// subtracted exactly). restore_thread_cpu_nanoseconds covers the coordinator
// page reset plus every worker's own register restore. Coordinator CPU covers
// tape control and payload injection plus, in warmup only, coordinator-owned
// checkpoint verification.
struct GuestExecutionSessionReplayMetrics {
  uint64_t repetition_count = 0;
  uint64_t consumed_event_count_per_repetition = 0;
  uint64_t intermediate_checkpoint_count = 0;
  uint64_t restore_thread_cpu_nanoseconds = 0;
  uint64_t participant_thread_cpu_nanoseconds = 0;
  uint64_t coordinator_thread_cpu_nanoseconds = 0;
  uint64_t uptime_raw_nanoseconds = 0;
  uint64_t in_interval_verification_thread_cpu_nanoseconds = 0;
  uint64_t in_interval_verification_count = 0;
  uint64_t verification_thread_cpu_nanoseconds = 0;
  uint64_t placement_generation_before = 0;
  uint64_t placement_generation_after = 0;
  uint64_t reset_page_count_per_repetition = 0;
  uint64_t reset_bytes_per_repetition = 0;
  GuestInvocationReplayCodeShape code_shape;
  std::vector<GuestExecutionSessionReplayParticipantMetrics> participants;
};

// Owns a bare Memory, Processor, backend, one persistent host worker and one
// real ThreadState per participant on Apple A64. Workers are created once with
// a guest-sized host stack, restored once per repetition on their own thread
// and reused for the complete event interval. The bundle is borrowed and must
// outlive the runner. A guest segment that never returns cannot be
// interrupted: only the tape waits are bounded, so the caller's subprocess
// timeout is the containment boundary. File decoding, provenance policy,
// subprocess fault containment and marker output remain the caller's
// responsibility.
class GuestExecutionSessionRunner {
 public:
  struct Options {
    // Bounds every single wait for a recorded turn. A participant's first
    // acquire and the coordinator's boundary wait span the whole preceding
    // tape, so this must exceed one repetition's wall time; a wait that
    // expires rejects the repetition.
    std::chrono::milliseconds event_timeout = std::chrono::seconds(60);
    // Guest-work sufficiency is enforced here: a timed repetition whose summed
    // participant CPU is below this floor rejects. Must be positive.
    uint64_t minimum_participant_cpu_nanoseconds_per_repetition = 1'000'000;
  };

  static constexpr uint64_t kMaxRepetitionCount = 1'000'000;

  static std::unique_ptr<GuestExecutionSessionRunner> Create(
      const GuestExecutionSessionBundle& bundle,
      std::unique_ptr<backend::Backend> backend, Options options,
      std::string* error = nullptr);

  ~GuestExecutionSessionRunner();

  GuestExecutionSessionRunner(const GuestExecutionSessionRunner&) = delete;
  GuestExecutionSessionRunner& operator=(const GuestExecutionSessionRunner&) =
      delete;

  // Defines every corpus function in captured successful-definition order,
  // then restores, replays and verifies one complete untimed repetition with
  // segment entry/exit and intermediate-checkpoint verification inside the
  // interval.
  bool WarmAndVerify(std::string* error = nullptr);

  // Replays exactly repetition_count restore/interval/verify cycles with no
  // verification inside the interval. Every repetition must consume the
  // complete tape in recorded order, match the final participant states and
  // pages, keep code placement unchanged and clear the participant CPU floor.
  bool RunTimed(uint64_t repetition_count,
                GuestExecutionSessionReplayMetrics* metrics,
                std::string* error = nullptr);

  const GuestExecutionSessionReplayPlan& plan() const { return plan_; }
  const GuestExecutionSessionReplayMetrics& warmup_metrics() const {
    return warmup_metrics_;
  }

 private:
  struct Worker;
  enum class WorkerCommand : uint32_t {
    kIdle = 0,
    kRestore = 1,
    kRun = 2,
    kExit = 3,
  };

  GuestExecutionSessionRunner(const GuestExecutionSessionBundle& bundle,
                              GuestExecutionSessionReplayPlan plan,
                              Options options);

  bool Initialize(std::unique_ptr<backend::Backend> backend,
                  std::string* error);
  bool CommitAndLoadPages(std::string* error);
  bool CloseAndReopenGuestViews(std::string* error);
  bool PrepareResetPageCopies(std::string* error);
  bool StartWorkers(std::string* error);
  void StopWorkers();
  bool ResolveFunctionsInCaptureOrder(std::string* error);
  bool CaptureWarmedCodeShape(GuestInvocationReplayCodeShape* output,
                              std::string* error) const;

  bool RunRepetition(GuestExecutionSessionReplayMetrics* metrics,
                     bool full_verification, std::string* error);
  bool RestoreAll(GuestExecutionSessionReplayMetrics* metrics,
                  std::string* error);
  bool RunInterval(GuestExecutionSessionReplayMetrics* metrics,
                   std::string* error);
  bool RunCoordinator(uint64_t* verification_cpu_nanoseconds,
                      uint64_t* verification_count, std::string* error);
  void DispatchWorkers(WorkerCommand command);
  bool WaitForWorkers(std::string* error);
  bool VerifyFinalState(std::string* error) const;
  bool VerifyCheckpoint(const GuestExecutionSessionCheckpoint& checkpoint,
                        std::string* error) const;
  bool VerifyParticipantState(uint32_t ordinal,
                              const ppc::GuestPPCRegisterState& expected,
                              std::string_view message,
                              std::string* error) const;
  bool VerifySegmentEntry(const Worker& worker,
                          const GuestExecutionSessionReplaySegment& segment,
                          std::string* error) const;
  bool VerifySegmentExit(const Worker& worker,
                         const GuestExecutionSessionReplaySegment& segment,
                         std::string* error) const;
  const GuestExecutionSessionReplayPage* FindPage(uint32_t guest_address) const;

  void WorkerMain(Worker* worker);
  bool RestoreParticipant(Worker* worker, std::string* error);
  bool RunParticipant(Worker* worker, std::string* error);
  bool ReplayParticipantEvent(Worker* worker,
                              const GuestExecutionSessionReplayEvent& planned,
                              std::string* error);

  const GuestExecutionSessionBundle* bundle_ = nullptr;
  GuestExecutionSessionReplayPlan plan_;
  Options options_;

  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  ExactJitCorpusModule* module_ = nullptr;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<GuestFunction*> resolved_functions_;
  std::vector<Function*> segment_functions_;
  std::unique_ptr<GuestExecutionReplayTape> tape_;
  GuestInvocationReplayCodeShape warmed_code_shape_;
  GuestExecutionSessionReplayMetrics warmup_metrics_;
  struct ResetPageCopy {
    uint8_t* destination = nullptr;
    const uint8_t* source = nullptr;
  };
  std::vector<ResetPageCopy> reset_page_copies_;
  // Set by the coordinator before workers are dispatched for an interval.
  bool full_verification_ = false;
  bool warmed_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_RUNNER_H_
