/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_capture_runtime.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_continuous_event.h"
#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/guest_execution_marker_controller.h"
#include "xenia/cpu/guest_execution_session_capture_event_bridge.h"
#include "xenia/cpu/guest_execution_session_capture_provider.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_replay_cli.h"
#include "xenia/cpu/guest_invocation_replay_config.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/memory.h"

DEFINE_transient_path(
    guest_execution_capture_output, "",
    "New local directory for one scheduler-on guest execution session bundle.",
    "Guest Execution Session Capture");
DEFINE_transient_string(
    guest_execution_capture_marker_source, "pm4_swap",
    "Marker source for a session capture. The production title path currently "
    "accepts only pm4_swap.",
    "Guest Execution Session Capture");
DEFINE_transient_string(
    guest_execution_capture_boundary, "guest_marker_count:1",
    "Session stop boundary. The production title path currently accepts only "
    "guest_marker_count:<positive count>.",
    "Guest Execution Session Capture");
DEFINE_CVar(guest_execution_capture_warmup_ms, 100000,
            "Warmup before the next PM4 swap arms capture, in milliseconds.",
            "Guest Execution Session Capture", true, uint64_t);
DEFINE_CVar(guest_execution_capture_bundle_cap_bytes, 1ull << 30,
            "Maximum encoded session bundle size in bytes.",
            "Guest Execution Session Capture", true, uint64_t);
DEFINE_CVar(guest_execution_capture_arm_retry_limit, 8,
            "Arm attempts rejected by boundary conditions that are retried at "
            "a later marker. Zero arms exactly once.",
            "Guest Execution Session Capture", true, uint64_t);
DEFINE_transient_path(
    guest_execution_capture_stop_file, "",
    "Reserved manual-stop path. Manual title capture is not implemented yet.",
    "Guest Execution Session Capture");

namespace xe {
namespace cpu {
namespace {

using RuntimeState = GuestExecutionSessionCaptureRuntimeState;
using RuntimeRejection = GuestExecutionSessionCaptureRuntimeRejection;
using AssemblerAction = GuestExecutionSessionAssemblerAction;
using AssemblerState = GuestExecutionSessionAssemblerState;
using CheckpointParticipant = kernel::GuestSchedulerCheckpointParticipant;
using CheckpointRejection = kernel::GuestSchedulerCheckpointBarrierRejection;
using CheckpointResumeKind = kernel::GuestSchedulerCheckpointResumeKind;
using CheckpointSnapshot = kernel::GuestSchedulerCheckpointBarrierSnapshot;

bool Fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

bool IsPowerOfTwo(size_t value) { return value && !(value & (value - 1)); }

bool IsTerminal(RuntimeState state) {
  return state == RuntimeState::kComplete || state == RuntimeState::kRejected ||
         state == RuntimeState::kShutdown;
}

bool IsRestorableJitParticipant(const CheckpointParticipant& participant) {
  return participant.restorable && participant.guest_pc &&
         !(participant.guest_pc & 3) &&
         participant.resume_kind == CheckpointResumeKind::kJitSafepoint;
}

bool IsPassiveOutsideGuestParticipant(
    const CheckpointParticipant& participant) {
  if (participant.restorable || participant.guest_pc ||
      (participant.state !=
           kernel::GuestSchedulerCheckpointParticipantState::kReady &&
       participant.state !=
           kernel::GuestSchedulerCheckpointParticipantState::kSuspended)) {
    return false;
  }
  return participant.resume_kind == CheckpointResumeKind::kNativeContinuation ||
         participant.resume_kind == CheckpointResumeKind::kNotYetRun;
}

// The wait classes whose link register is the modeled export's single return
// point, matching the allowlist the provider applies to a still-blocked
// participant. A delay, a fence or spin poll and the I/O classes reach that
// link register once per poll rather than once per export, an alertable or
// APC-pending wait can run guest code on the waiting thread's stack before the
// export returns, and a wait naming more handles than the binding holds is not
// representable at all.
bool IsWokenInWaitAllowlist(
    kernel::GuestSchedulerCaptureWaitKind kind,
    const kernel::GuestSchedulerCaptureWaitState& wait) {
  const bool modeled_kind =
      kind == kernel::GuestSchedulerCaptureWaitKind::kSingle ||
      kind == kernel::GuestSchedulerCaptureWaitKind::kMultiAny ||
      kind == kernel::GuestSchedulerCaptureWaitKind::kMultiAll;
  constexpr uint8_t kRefusedWaitFlags =
      kernel::kGuestSchedulerCaptureWaitFlagAlertable |
      kernel::kGuestSchedulerCaptureWaitFlagUserApcPending;
  return modeled_kind && wait.handle_count &&
         wait.handle_count <=
             kernel::kGuestSchedulerCaptureMaximumWaitHandles &&
         !(wait.flags & kRefusedWaitFlags);
}

// The rejection diagnostic is one parsed log line, so its census is bounded.
constexpr size_t kMaxReportedUnsupportedParticipants = 32;

std::string DescribeCheckpointParticipant(
    const CheckpointParticipant& participant) {
  return fmt::format(
      "tid={:08X} state={} resume_kind={} restorable={} pc={:08X}",
      participant.thread_id, static_cast<uint32_t>(participant.state),
      static_cast<uint32_t>(participant.resume_kind), participant.restorable,
      participant.guest_pc);
}

bool IsBlockedParityWait(kernel::GuestSchedulerCaptureWaitKind kind,
                         const kernel::GuestSchedulerCaptureWaitState& wait) {
  // The durable shape of the binding is ValidateSchedulerBlockedWait's to
  // enforce once the row exists; only the bound its parity reasoning depends on
  // is restated here.
  if (kind == kernel::GuestSchedulerCaptureWaitKind::kNone ||
      wait.handle_count > kernel::kGuestSchedulerCaptureMaximumWaitHandles) {
    return false;
  }
  // A wait is witnessed as unsatisfied by a signalable object's frozen epochs
  // or by a deadline the boundary has not reached; a kind carrying neither
  // states nothing about whether it woke.
  const bool object_wait =
      kind == kernel::GuestSchedulerCaptureWaitKind::kSingle ||
      kind == kernel::GuestSchedulerCaptureWaitKind::kMultiAny ||
      kind == kernel::GuestSchedulerCaptureWaitKind::kMultiAll;
  if (!(object_wait && wait.handle_count) && !wait.deadline_ms) {
    return false;
  }
  constexpr uint8_t kRefusedWaitFlags =
      kernel::kGuestSchedulerCaptureWaitFlagAlertable |
      kernel::kGuestSchedulerCaptureWaitFlagUserApcPending;
  if (wait.flags & kRefusedWaitFlags) {
    return false;
  }
  if (wait.observed_wait_epoch != wait.wait_epoch ||
      wait.signal_epochs_observed != wait.signal_epochs_before) {
    return false;
  }
  // observed_uptime_ms is a per-snapshot host clock read rather than thread
  // state, so the deadline is the only durable statement of how near this wait
  // is to its own timeout.
  return !wait.deadline_ms || wait.deadline_ms > wait.observed_uptime_ms;
}

bool CurrentTitleCaptureConfig(GuestExecutionSessionTitleCaptureConfig* output,
                               std::string* error) noexcept {
  if (!output) {
    return Fail(error, "session title capture config output is missing");
  }
  GuestExecutionSessionTitleCaptureConfig config;
  config.output_directory = cvars::guest_execution_capture_output;
  config.warmup_milliseconds = cvars::guest_execution_capture_warmup_ms;
  config.maximum_bundle_bytes = cvars::guest_execution_capture_bundle_cap_bytes;
  config.arm_retry_limit = cvars::guest_execution_capture_arm_retry_limit;
  if (config.output_directory.empty()) {
    return Fail(error, "session title capture output directory is missing");
  }
  if (cvars::guest_execution_capture_marker_source != "pm4_swap") {
    return Fail(error, "session title capture marker source must be pm4_swap");
  }
  if (!cvars::guest_execution_capture_stop_file.empty()) {
    return Fail(error, "manual session title capture is not implemented yet");
  }
  constexpr std::string_view kMarkerBoundaryPrefix = "guest_marker_count:";
  const std::string_view boundary = cvars::guest_execution_capture_boundary;
  if (!boundary.starts_with(kMarkerBoundaryPrefix)) {
    return Fail(error,
                "session title capture requires a guest marker count boundary");
  }
  const std::string_view count_text =
      boundary.substr(kMarkerBoundaryPrefix.size());
  const char* count_end = count_text.data() + count_text.size();
  const std::from_chars_result count_result =
      std::from_chars(count_text.data(), count_end, config.stop_marker_count);
  if (count_text.empty() || count_result.ec != std::errc() ||
      count_result.ptr != count_end || !config.stop_marker_count) {
    return Fail(error,
                "session title capture marker count is not a positive integer");
  }
  *output = std::move(config);
  return true;
}

bool ValidateTitleCaptureConfig(
    const GuestExecutionSessionTitleCaptureConfig& config,
    std::string* error) noexcept {
  if (config.output_directory.empty() || !config.warmup_milliseconds ||
      !config.stop_marker_count || config.maximum_bundle_bytes < (1u << 20) ||
      config.maximum_bundle_bytes > (32ull << 30)) {
    return Fail(error, "session title capture configuration is invalid");
  }
  return true;
}

bool TicksFromMilliseconds(uint64_t milliseconds, uint64_t frequency,
                           uint64_t* output) noexcept {
  if (!milliseconds || !frequency || !output) {
    return false;
  }
  const uint64_t seconds = milliseconds / 1000;
  const uint64_t remainder = milliseconds % 1000;
  if (seconds > std::numeric_limits<uint64_t>::max() / frequency) {
    return false;
  }
  const uint64_t whole_ticks = seconds * frequency;
  if (remainder >
      (std::numeric_limits<uint64_t>::max() - whole_ticks) / frequency) {
    return false;
  }
  const uint64_t fractional_ticks = remainder * frequency / 1000;
  if (!whole_ticks && !fractional_ticks) {
    return false;
  }
  *output = whole_ticks + fractional_ticks;
  return true;
}

GuestExecutionSessionBundleLimits MakeTitleCaptureBundleLimits(
    uint64_t maximum_bundle_bytes) {
  GuestExecutionSessionBundleLimits limits;
  limits.maximum_bundle_bytes = maximum_bundle_bytes;
  limits.maximum_total_content_bytes =
      std::min(limits.maximum_total_content_bytes, maximum_bundle_bytes);
  limits.session.maximum_total_chunk_bytes =
      std::min(limits.session.maximum_total_chunk_bytes, maximum_bundle_bytes);
  limits.session.maximum_manifest_bytes =
      std::min(limits.session.maximum_manifest_bytes, maximum_bundle_bytes);
  limits.session.maximum_chunk_bytes =
      std::min(limits.session.maximum_chunk_bytes, maximum_bundle_bytes);
  limits.session.maximum_event_payload_bytes = std::min(
      limits.session.maximum_event_payload_bytes, maximum_bundle_bytes);
  limits.session.maximum_content_blob_bytes =
      std::min(limits.session.maximum_content_blob_bytes, maximum_bundle_bytes);
  return limits;
}

const CheckpointParticipant* FindCheckpointParticipant(
    const CheckpointSnapshot& checkpoint, uint32_t guest_thread_id);

bool ValidateRuntimeCheckpointStateBindings(
    const GuestExecutionSessionBundle& bundle,
    const GuestExecutionSessionCheckpointChunk& checkpoint,
    const CheckpointSnapshot& scheduler_checkpoint, const char* boundary,
    std::string* error) {
  if (checkpoint.checkpoint.thread_states.size() !=
          bundle.manifest.participants.size() ||
      scheduler_checkpoint.participants.size() >
          bundle.manifest.participants.size()) {
    return Fail(error, std::string("capture runtime ") + boundary +
                           " checkpoint roster differs from the bundle");
  }
  size_t scheduler_owned_count = 0;
  for (const GuestExecutionSessionThreadStateReference& state :
       checkpoint.checkpoint.thread_states) {
    if (state.thread_ordinal >= bundle.manifest.participants.size()) {
      return Fail(error, std::string("capture runtime ") + boundary +
                             " checkpoint state ordinal is invalid");
    }
    const GuestExecutionSessionParticipant& participant =
        bundle.manifest.participants[state.thread_ordinal];
    const auto blob = std::find_if(
        bundle.content_blobs.cbegin(), bundle.content_blobs.cend(),
        [&state](const GuestExecutionSessionContentBlob& candidate) {
          return candidate.sha256 == state.sha256;
        });
    if (blob == bundle.content_blobs.cend() ||
        blob->bytes.size() != state.byte_size) {
      return Fail(error, std::string("capture runtime ") + boundary +
                             " checkpoint state blob is missing");
    }
    ppc::GuestPPCThreadCheckpoint decoded;
    std::string decode_error;
    if (!ppc::GuestPPCThreadCheckpointCodec::Decode(blob->bytes, &decoded,
                                                    &decode_error)) {
      return Fail(error, std::string("capture runtime ") + boundary +
                             " checkpoint state is not a PPC continuation: " +
                             decode_error);
    }
    if (decoded.participant_ordinal != state.thread_ordinal ||
        decoded.participant_ordinal != participant.ordinal ||
        decoded.guest_thread_id != participant.guest_thread_id) {
      return Fail(error, std::string("capture runtime ") + boundary +
                             " checkpoint state identity differs from the "
                             "held scheduler roster");
    }
    const CheckpointParticipant* scheduler_participant =
        FindCheckpointParticipant(scheduler_checkpoint,
                                  decoded.guest_thread_id);
    if (!scheduler_participant) {
      if (decoded.resume_kind != ppc::GuestPPCThreadResumeKind::kOutsideGuest) {
        return Fail(error, std::string("capture runtime ") + boundary +
                               " scheduler-unowned participant has an "
                               "executable PPC continuation");
      }
      continue;
    }
    ++scheduler_owned_count;
    if (scheduler_participant->capture_instance_id !=
        participant.capture_instance_id) {
      return Fail(error, std::string("capture runtime ") + boundary +
                             " scheduler and bundle identities differ");
    }
    if (IsPassiveOutsideGuestParticipant(*scheduler_participant)) {
      if (decoded.resume_kind != ppc::GuestPPCThreadResumeKind::kOutsideGuest) {
        return Fail(error, std::string("capture runtime ") + boundary +
                               " passive scheduler participant has an "
                               "executable PPC continuation");
      }
      continue;
    }
    if (!IsRestorableJitParticipant(*scheduler_participant) ||
        decoded.resume_kind != ppc::GuestPPCThreadResumeKind::kGuestBlockHead ||
        decoded.resume_pc != scheduler_participant->guest_pc) {
      return Fail(error, std::string("capture runtime ") + boundary +
                             " PPC continuation differs from the held "
                             "scheduler JIT safepoint");
    }
  }
  if (scheduler_owned_count != scheduler_checkpoint.participants.size()) {
    return Fail(error, std::string("capture runtime ") + boundary +
                           " scheduler checkpoint has no bundle identity");
  }
  return true;
}

bool ValidateRuntimePublicationBundle(
    const GuestExecutionSessionBundle& bundle,
    const CheckpointSnapshot& initial_scheduler_checkpoint,
    const CheckpointSnapshot& final_scheduler_checkpoint,
    const GuestExecutionSessionBundleLimits& limits, std::string* error) {
  if (!bundle.manifest.segments.empty()) {
    return Fail(error,
                "capture runtime publication is not a continuous session");
  }
  if (bundle.manifest.chunks.size() < 5 ||
      bundle.manifest.chunks.size() != bundle.chunks.size() ||
      bundle.manifest.chunks.front().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint ||
      bundle.manifest.chunks[1].kind !=
          GuestExecutionSessionChunkKind::kCodeCorpus ||
      bundle.manifest.chunks.back().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint) {
    return Fail(error,
                "capture runtime publication lacks the continuous checkpoint "
                "and code-corpus envelope");
  }
  bool saw_canonical_events = false;
  bool saw_continuous_events = false;
  uint32_t scheduler_topology_count = 0;
  for (size_t index = 2; index + 1 < bundle.manifest.chunks.size(); ++index) {
    const GuestExecutionSessionChunkKind kind =
        bundle.manifest.chunks[index].kind;
    if (kind == GuestExecutionSessionChunkKind::kEvents &&
        !saw_continuous_events && !scheduler_topology_count) {
      saw_canonical_events = true;
    } else if (kind == GuestExecutionSessionChunkKind::kContinuousEvents &&
               saw_canonical_events && !scheduler_topology_count) {
      saw_continuous_events = true;
    } else if (kind == GuestExecutionSessionChunkKind::kSchedulerTopology &&
               saw_continuous_events && scheduler_topology_count < 2) {
      GuestExecutionSessionSchedulerTopologyChunk topology;
      if (!GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
              bundle.chunks[index], &topology, error, limits.session)) {
        return false;
      }
      const bool expected_start = scheduler_topology_count == 0;
      if ((expected_start &&
           (topology.boundary !=
                GuestExecutionSessionSchedulerTopologyBoundary::kStart ||
            topology.global_sequence != 0)) ||
          (!expected_start &&
           (topology.boundary !=
                GuestExecutionSessionSchedulerTopologyBoundary::kFinal ||
            topology.global_sequence != bundle.manifest.last_event_sequence))) {
        return Fail(error,
                    "capture runtime scheduler topology order is invalid");
      }
      ++scheduler_topology_count;
    } else {
      return Fail(error,
                  "capture runtime publication chunk order is not initial "
                  "checkpoint, corpus, canonical events, overlay, scheduler "
                  "topologies, final checkpoint");
    }
  }
  if (!saw_canonical_events || !saw_continuous_events ||
      scheduler_topology_count != 2) {
    return Fail(error,
                "capture runtime publication lacks its authenticated event "
                "overlay or scheduler topology boundaries");
  }
  if (!ValidateGuestExecutionSessionBundle(bundle, error, limits)) {
    return false;
  }

  GuestExecutionSessionCheckpointChunk initial_checkpoint;
  GuestExecutionSessionCheckpointChunk final_checkpoint;
  if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
          bundle.chunks.front(), &initial_checkpoint, error, limits.session) ||
      !GuestExecutionSessionCodec::DecodeCheckpointChunk(
          bundle.chunks.back(), &final_checkpoint, error, limits.session) ||
      final_checkpoint.checkpoint.global_sequence !=
          bundle.manifest.last_event_sequence ||
      !ValidateRuntimeCheckpointStateBindings(bundle, initial_checkpoint,
                                              initial_scheduler_checkpoint,
                                              "initial", error) ||
      !ValidateRuntimeCheckpointStateBindings(bundle, final_checkpoint,
                                              final_scheduler_checkpoint,
                                              "final", error)) {
    if (error && !error->empty()) {
      return false;
    }
    return Fail(error,
                "capture runtime final checkpoint does not cover its roster");
  }
  std::vector<bool> participant_binding_seen(
      bundle.manifest.participants.size(), false);
  std::vector<bool> participant_binding_required(
      bundle.manifest.participants.size(), false);
  for (const GuestExecutionSessionParticipant& participant :
       bundle.manifest.participants) {
    const CheckpointParticipant* scheduler_participant =
        FindCheckpointParticipant(final_scheduler_checkpoint,
                                  participant.guest_thread_id);
    participant_binding_required[participant.ordinal] =
        scheduler_participant &&
        IsRestorableJitParticipant(*scheduler_participant);
  }
  GuestExecutionContinuousEventLimits continuous_limits;
  continuous_limits.maximum_encoded_bytes = limits.session.maximum_chunk_bytes;
  continuous_limits.maximum_records = limits.session.maximum_events_per_chunk;
  for (size_t index = 2; index + 1 < bundle.manifest.chunks.size(); ++index) {
    if (bundle.manifest.chunks[index].kind !=
        GuestExecutionSessionChunkKind::kContinuousEvents) {
      continue;
    }
    std::vector<GuestExecutionContinuousEvent> events;
    if (!GuestExecutionContinuousEventCodec::Decode(
            bundle.chunks[index], &events, error, continuous_limits)) {
      return false;
    }
    for (const GuestExecutionContinuousEvent& event : events) {
      if (event.checkpoint.kind !=
              GuestExecutionContinuousCheckpointReferenceKind::kThreadState ||
          event.checkpoint.checkpoint_global_sequence !=
              bundle.manifest.last_event_sequence) {
        continue;
      }
      const uint32_t ordinal = event.subject.participant_ordinal;
      if (ordinal >= bundle.manifest.participants.size()) {
        return Fail(error,
                    "capture runtime overlay checkpoint subject is invalid");
      }
      const GuestExecutionSessionParticipant& participant =
          bundle.manifest.participants[ordinal];
      const GuestExecutionSessionThreadStateReference& state =
          final_checkpoint.checkpoint.thread_states[ordinal];
      if (!participant_binding_required[ordinal] ||
          participant_binding_seen[ordinal] ||
          event.subject.guest_thread_id != participant.guest_thread_id ||
          event.checkpoint.state_size != state.byte_size ||
          event.checkpoint.state_sha256 != state.sha256) {
        return Fail(error,
                    "capture runtime overlay checkpoint differs from the "
                    "final participant state");
      }
      participant_binding_seen[ordinal] = true;
    }
  }
  for (size_t ordinal = 0; ordinal < participant_binding_seen.size();
       ++ordinal) {
    if (participant_binding_seen[ordinal] !=
        participant_binding_required[ordinal]) {
      return Fail(error,
                  "capture runtime publication final state bindings differ "
                  "from scheduler ownership");
    }
  }
  return true;
}

const CheckpointParticipant* FindCheckpointParticipant(
    const CheckpointSnapshot& checkpoint, uint32_t guest_thread_id) {
  const auto it = std::find_if(
      checkpoint.participants.begin(), checkpoint.participants.end(),
      [guest_thread_id](const CheckpointParticipant& entry) {
        return entry.thread_id == guest_thread_id;
      });
  return it == checkpoint.participants.end() ? nullptr : &*it;
}

const GuestExecutionCaptureActiveHostCall* FindOutermostActiveHostCall(
    const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
    const GuestExecutionCaptureParticipantIdentity& identity) {
  const GuestExecutionCaptureActiveHostCall* outermost = nullptr;
  for (const GuestExecutionCaptureActiveHostCall& call :
       host_calls.active_calls) {
    if (call.participant == identity &&
        (!outermost || call.participant_depth < outermost->participant_depth)) {
      outermost = &call;
    }
  }
  return outermost;
}

std::string DescribeActiveHostCall(
    const GuestExecutionCaptureActiveHostCall& call) {
  return fmt::format("hostcall=depth={},fn={:08X},ret={:08X}",
                     call.participant_depth, call.function_address,
                     call.return_address);
}

// The census names every offender, not only the one that stopped the loop.
std::string CensusPassiveParticipantHostCalls(
    const CheckpointSnapshot& checkpoint,
    const GuestExecutionCaptureThreadStateRegistrySnapshot& registry,
    const GuestExecutionCaptureHostCallRosterSnapshot& host_calls) {
  std::string message =
      "capture runtime passive scheduler participant has an active outer "
      "host call";
  size_t unsupported_count = 0;
  for (const auto& lifecycle : registry.participants) {
    const CheckpointParticipant* participant = FindCheckpointParticipant(
        checkpoint, lifecycle.participant.guest_thread_id);
    if (!participant || !IsPassiveOutsideGuestParticipant(*participant)) {
      continue;
    }
    const GuestExecutionCaptureActiveHostCall* call =
        FindOutermostActiveHostCall(host_calls, lifecycle.participant);
    if (!call) {
      continue;
    }
    if (unsupported_count < kMaxReportedUnsupportedParticipants) {
      message.append(unsupported_count ? "; also: " : ": ");
      message.append(DescribeCheckpointParticipant(*participant));
      message.push_back(' ');
      message.append(DescribeActiveHostCall(*call));
    }
    ++unsupported_count;
  }
  return message + fmt::format("; unsupported={}/{}", unsupported_count,
                               checkpoint.participants.size());
}

bool ActiveHostCallHasDurableContinuation(
    const CheckpointParticipant* participant) {
  if (!participant) {
    return false;
  }
  const bool restorable_safepoint =
      participant->resume_kind == CheckpointResumeKind::kJitSafepoint &&
      participant->restorable;
  const bool blocked_in_export =
      participant->resume_kind == CheckpointResumeKind::kAfterBlockingExport &&
      !participant->restorable &&
      participant->state ==
          kernel::GuestSchedulerCheckpointParticipantState::kBlocked;
  return restorable_safepoint || blocked_in_export;
}

std::string CensusUnrepresentableActiveHostCalls(
    const CheckpointSnapshot& checkpoint,
    const GuestExecutionCaptureHostCallRosterSnapshot& host_calls) {
  std::string message =
      "capture runtime active outer call lacks an exact-PC JIT safepoint or "
      "modeled blocking-export continuation";
  size_t unsupported_count = 0;
  for (const GuestExecutionCaptureActiveHostCall& call :
       host_calls.active_calls) {
    const CheckpointParticipant* participant =
        FindCheckpointParticipant(checkpoint, call.participant.guest_thread_id);
    if (ActiveHostCallHasDurableContinuation(participant)) {
      continue;
    }
    if (unsupported_count < kMaxReportedUnsupportedParticipants) {
      message.append(unsupported_count ? "; also: " : ": ");
      message.append(participant
                         ? DescribeCheckpointParticipant(*participant)
                         : fmt::format("tid={:08X} state=none resume_kind=none "
                                       "restorable=none pc=none",
                                       call.participant.guest_thread_id));
      message.push_back(' ');
      message.append(DescribeActiveHostCall(call));
    }
    ++unsupported_count;
  }
  return message + fmt::format("; unsupported={}/{}", unsupported_count,
                               host_calls.active_calls.size());
}

class DirectCheckpointController final
    : public GuestExecutionSessionCaptureRuntimeCheckpointController {
 public:
  explicit DirectCheckpointController(kernel::GuestScheduler& scheduler)
      : scheduler_(scheduler) {}

  CheckpointRejection Pause(std::chrono::milliseconds timeout,
                            CheckpointSnapshot* snapshot) override {
    return scheduler_.PauseForCheckpointBarrier(timeout, snapshot);
  }

  CheckpointRejection Finalize(uint64_t generation,
                               CheckpointSnapshot* snapshot) override {
    return scheduler_.FinalizeAndResumeCheckpointBarrier(generation, snapshot);
  }

  CheckpointRejection Cancel(uint64_t generation,
                             CheckpointSnapshot* snapshot) override {
    return scheduler_.CancelCheckpointBarrier(generation, snapshot);
  }

 private:
  kernel::GuestScheduler& scheduler_;
};

class DeferredPublisher final : public GuestExecutionSessionAssemblerPublisher {
 public:
  bool Publish(const GuestExecutionSessionBundle& bundle,
               std::string* error) noexcept override {
    if (bundle_) {
      return Fail(error, "capture runtime received a second staged bundle");
    }
    try {
      bundle_ = bundle;
    } catch (...) {
      return Fail(error, "capture runtime could not stage the session bundle");
    }
    return true;
  }

  std::optional<GuestExecutionSessionBundle> Take() {
    std::optional<GuestExecutionSessionBundle> result = std::move(bundle_);
    bundle_.reset();
    return result;
  }

  void Clear() { bundle_.reset(); }

 private:
  std::optional<GuestExecutionSessionBundle> bundle_;
};

enum class RuntimeEventKind : uint8_t {
  kStart,
  kStop,
  kLifecycle,
  kHostCallBegin,
  kHostCallEnd,
  kScheduler,
  kGuestMarker,
};

struct RuntimeEvent {
  RuntimeEventKind kind = RuntimeEventKind::kStart;
  GuestExecutionCaptureParticipantIdentity participant;
  uint64_t guest_instruction_delta = 0;
  GuestExecutionCaptureThreadStateLifecycleEvent lifecycle;
  GuestExecutionCaptureHostCallToken host_call_token;
  GuestExecutionCaptureHostCallOutcome host_call_outcome =
      GuestExecutionCaptureHostCallOutcome::kReturnedToHost;
  uint32_t function_address = 0;
  uint32_t function_end_address = 0;
  uint32_t return_address = 0;
  kernel::GuestSchedulerCaptureEvent scheduler;
  GuestExecutionSessionMarkerSource marker_source =
      GuestExecutionSessionMarkerSource::kNone;
  uint64_t marker_identity = 0;
};

// Dmitry Vyukov's bounded sequence-cell queue. There is one consumer and any
// number of callback producers. Producers perform no allocation or waiting.
class RuntimeEventQueue final {
 public:
  explicit RuntimeEventQueue(size_t capacity)
      : cells_(std::make_unique<Cell[]>(capacity)),
        mask_(capacity - 1),
        capacity_(capacity) {
    for (size_t index = 0; index < capacity; ++index) {
      cells_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  bool TryPush(const RuntimeEvent& event) noexcept {
    size_t position = enqueue_position_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[position & mask_];
      const size_t sequence = cell.sequence.load(std::memory_order_acquire);
      const intptr_t difference =
          static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
      if (!difference) {
        if (enqueue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          cell.event = event;
          cell.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (difference < 0) {
        return false;
      } else {
        position = enqueue_position_.load(std::memory_order_relaxed);
      }
    }
  }

  bool TryPop(RuntimeEvent* event) noexcept {
    Cell& cell = cells_[dequeue_position_ & mask_];
    const size_t sequence = cell.sequence.load(std::memory_order_acquire);
    const intptr_t difference = static_cast<intptr_t>(sequence) -
                                static_cast<intptr_t>(dequeue_position_ + 1);
    if (difference) {
      return false;
    }
    *event = cell.event;
    cell.sequence.store(dequeue_position_ + capacity_,
                        std::memory_order_release);
    ++dequeue_position_;
    return true;
  }

  bool MaybeNotEmpty() const noexcept {
    return enqueue_position_.load(std::memory_order_acquire) !=
           dequeue_position_;
  }

 private:
  struct Cell {
    std::atomic<size_t> sequence{0};
    RuntimeEvent event;
  };

  std::unique_ptr<Cell[]> cells_;
  const size_t mask_;
  const size_t capacity_;
  alignas(64) std::atomic<size_t> enqueue_position_{0};
  alignas(64) size_t dequeue_position_ = 0;
};

enum class AsyncFailure : uint8_t {
  kNone,
  kQueueOverflow,
  kSchedulerSequence,
  kHostCallRoster,
  kInstructionCounter,
  kUnexpectedJitSafepoint,
  kSourceAfterSeal,
};

enum class ArmAttemptDisposition : uint8_t {
  kTerminal,
  kRetryable,
};

// A barrier code describes the quiesce window it was raised in, so only the
// two codes that report that window's own conditions can differ at the next
// marker. Everything else names a deterministic barrier state error.
ArmAttemptDisposition ClassifyCheckpointBarrierRejection(
    CheckpointRejection rejection) {
  switch (rejection) {
    case CheckpointRejection::kTopologyChanged:
    case CheckpointRejection::kTimedOut:
      return ArmAttemptDisposition::kRetryable;
    case CheckpointRejection::kNone:
    case CheckpointRejection::kAlreadyActive:
    case CheckpointRejection::kNotActive:
    case CheckpointRejection::kStaleGeneration:
    case CheckpointRejection::kReleasePending:
    case CheckpointRejection::kNotStarted:
    case CheckpointRejection::kCalledFromDispatchThread:
    case CheckpointRejection::kInvalidTopology:
    case CheckpointRejection::kUnexpectedSafepoint:
    case CheckpointRejection::kDuplicateSafepoint:
    case CheckpointRejection::kInvalidGuestPc:
    case CheckpointRejection::kUnexpectedSwitchOut:
    case CheckpointRejection::kShutdown:
    case CheckpointRejection::kCancelled:
      return ArmAttemptDisposition::kTerminal;
  }
  return ArmAttemptDisposition::kTerminal;
}

// Session arm policy. A rejection is retryable only when the guest state the
// next marker boundary presents can differ; every configuration, attachment
// and encoding failure is a property of this session and never changes. An
// unlisted kind is terminal.
ArmAttemptDisposition ClassifyArmRejection(RuntimeRejection rejection,
                                           CheckpointRejection barrier) {
  switch (rejection) {
    case RuntimeRejection::kCheckpointBarrier:
      return ClassifyCheckpointBarrierRejection(barrier);
    case RuntimeRejection::kCheckpointRoster:
    case RuntimeRejection::kMissingCheckpointCapability:
      return ArmAttemptDisposition::kRetryable;
    case RuntimeRejection::kNone:
    case RuntimeRejection::kInvalidConfiguration:
    case RuntimeRejection::kSourceAttachment:
    case RuntimeRejection::kSourceRejected:
    case RuntimeRejection::kQueueOverflow:
    case RuntimeRejection::kSchedulerSequence:
    case RuntimeRejection::kProviderFailure:
    case RuntimeRejection::kAssemblerFailure:
    case RuntimeRejection::kEventBridgeFailure:
    case RuntimeRejection::kExternalSinkControl:
    case RuntimeRejection::kBundleValidation:
    case RuntimeRejection::kPublicationFailure:
    case RuntimeRejection::kCancelled:
      return ArmAttemptDisposition::kTerminal;
  }
  return ArmAttemptDisposition::kTerminal;
}

std::string JoinDiagnostics(std::string message, std::string appended) {
  if (appended.empty()) {
    return message;
  }
  if (!message.empty()) {
    message += "; ";
  }
  message += appended;
  return message;
}

RuntimeRejection MapAsyncFailure(AsyncFailure failure) {
  switch (failure) {
    case AsyncFailure::kQueueOverflow:
      return RuntimeRejection::kQueueOverflow;
    case AsyncFailure::kSchedulerSequence:
      return RuntimeRejection::kSchedulerSequence;
    case AsyncFailure::kHostCallRoster:
    case AsyncFailure::kInstructionCounter:
    case AsyncFailure::kUnexpectedJitSafepoint:
    case AsyncFailure::kSourceAfterSeal:
      return RuntimeRejection::kSourceRejected;
    case AsyncFailure::kNone:
      break;
  }
  return RuntimeRejection::kSourceRejected;
}

const char* AsyncFailureMessage(AsyncFailure failure) {
  switch (failure) {
    case AsyncFailure::kQueueOverflow:
      return "capture runtime callback queue overflowed";
    case AsyncFailure::kSchedulerSequence:
      return "capture runtime scheduler event sequence is discontinuous";
    case AsyncFailure::kHostCallRoster:
      return "capture runtime host-call roster rejected an observation";
    case AsyncFailure::kInstructionCounter:
      return "capture runtime participant instruction counter ownership is "
             "invalid";
    case AsyncFailure::kUnexpectedJitSafepoint:
      return "capture runtime received an unsupported Processor JIT safepoint";
    case AsyncFailure::kSourceAfterSeal:
      return "capture runtime observed a source mutation while a checkpoint "
             "boundary was sealed";
    case AsyncFailure::kNone:
      break;
  }
  return "capture runtime source rejected the session";
}

}  // namespace

bool IsGuestExecutionSessionWokenInWaitCheckpointParticipant(
    const kernel::GuestSchedulerCheckpointParticipant& participant) {
  // A fiber re-readied out of its wait is published exactly as the passive
  // class is, because the scheduler cleared its exact-PC route and no frame of
  // the wait has unwound: no durable block-head PC, so no link register and
  // nothing restorable. Only a participant that has run can be one, which is
  // what separates it from kNotYetRun.
  if ((participant.state !=
           kernel::GuestSchedulerCheckpointParticipantState::kReady &&
       participant.state !=
           kernel::GuestSchedulerCheckpointParticipantState::kSuspended) ||
      participant.resume_kind !=
          kernel::GuestSchedulerCheckpointResumeKind::kNativeContinuation ||
      participant.restorable || participant.guest_pc) {
    return false;
  }
  // Already required of every roster participant, restated so the predicate is
  // self-contained.
  if (participant.preempt_defers_irql || participant.preempt_defers_lock ||
      participant.capture_declined_safepoints) {
    return false;
  }
  return IsWokenInWaitAllowlist(participant.blocked_wait_kind,
                                participant.blocked_wait);
}

bool IsGuestExecutionSessionBlockedParityCheckpointParticipant(
    const kernel::GuestSchedulerCheckpointParticipant& participant) {
  if (participant.state !=
          kernel::GuestSchedulerCheckpointParticipantState::kBlocked ||
      participant.resume_kind !=
          kernel::GuestSchedulerCheckpointResumeKind::kAfterBlockingExport ||
      participant.restorable || !participant.guest_pc ||
      (participant.guest_pc & 3)) {
    return false;
  }
  if (participant.preempt_defers_irql || participant.preempt_defers_lock ||
      participant.capture_declined_safepoints) {
    return false;
  }
  return IsBlockedParityWait(participant.blocked_wait_kind,
                             participant.blocked_wait);
}

struct GuestExecutionSessionCaptureRuntimePm4Wiring::MarkerBridge final
    : GuestExecutionMarkerBoundarySink {
  bool OnMarkerBoundary(
      const GuestExecutionMarkerBoundary& boundary) noexcept override {
    GuestExecutionSessionCaptureRuntime* target =
        runtime.load(std::memory_order_acquire);
    if (!target ||
        boundary.marker_source != GuestExecutionSessionMarkerSource::kPm4Swap ||
        boundary.marker_identity != gpu::kPm4SwapMarkerOpcode) {
      return false;
    }
    switch (boundary.kind) {
      case GuestExecutionMarkerBoundaryKind::kArm:
        return target->RequestStart();
      case GuestExecutionMarkerBoundaryKind::kStop:
        // OnArmedMarker queued the same PM4 marker first. The validated marker
        // count policy turns that event into the runtime stop request.
        return true;
    }
    return false;
  }

  bool OnArmedMarker(const gpu::Pm4MarkerEvent& event) noexcept override {
    GuestExecutionSessionCaptureRuntime* target =
        runtime.load(std::memory_order_acquire);
    if (!target || event.source != gpu::Pm4MarkerSource::kPm4Swap ||
        event.opcode != gpu::kPm4SwapMarkerOpcode) {
      return false;
    }
    return target->OnGuestMarker(GuestExecutionSessionMarkerSource::kPm4Swap,
                                 event.opcode);
  }

  std::atomic<GuestExecutionSessionCaptureRuntime*> runtime{nullptr};
};

struct GuestExecutionSessionCaptureRuntimePm4Wiring::Lifetime {
  std::unique_ptr<MarkerBridge> marker_bridge;
  std::shared_ptr<GuestExecutionMarkerController> marker_controller;
  std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4ExternalSink>
      external_sink;
  std::shared_ptr<GuestExecutionSessionCaptureRuntime> runtime;
};

GuestExecutionSessionCaptureRuntimePm4ExternalSink::
    GuestExecutionSessionCaptureRuntimePm4ExternalSink(
        gpu::CommandProcessor& command_processor,
        std::shared_ptr<GuestExecutionMarkerController> marker_controller)
    : command_processor_(command_processor),
      marker_controller_(std::move(marker_controller)) {}

GuestExecutionSessionCaptureRuntimePm4ExternalSink::
    ~GuestExecutionSessionCaptureRuntimePm4ExternalSink() {
  std::string ignored;
  AbortAndDetach(&ignored);
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::Activate(
    std::string* error) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (attached_ || terminally_detached_) {
    return Fail(error, "PM4 runtime external sink activation is not valid");
  }
  if (!marker_controller_->Begin()) {
    return Fail(error, "PM4 runtime external sink controller could not begin");
  }
  if (!command_processor_.AttachPm4MarkerSink(marker_controller_)) {
    return Fail(error,
                "PM4 runtime external sink could not attach its controller");
  }
  attached_ = true;
  return IsSourceHealthyLocked(error);
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::IsSourceHealthyLocked(
    std::string* error) const noexcept {
  if (terminally_detached_) {
    return true;
  }
  if (!attached_) {
    return Fail(error, "PM4 runtime external sink is not attached");
  }
  const gpu::Pm4MarkerDispatcherStatus source =
      command_processor_.pm4_marker_dispatcher_status();
  if (source.shut_down || source.sink_failed ||
      source.source_advanced_while_held ||
      (!source.sink_attached && !source.sink_held)) {
    return Fail(error, "PM4 runtime external sink source integrity was lost");
  }
  const GuestExecutionMarkerControllerStatus controller =
      marker_controller_->status();
  if (controller.state == GuestExecutionMarkerControllerState::kFailed) {
    return Fail(
        error, "PM4 runtime marker controller rejected with code " +
                   std::to_string(static_cast<uint32_t>(controller.rejection)));
  }
  return true;
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::Hold(
    gpu::Pm4MarkerHoldToken* token, std::string* error) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!token || terminally_detached_ || !attached_) {
    return Fail(error, "PM4 runtime external sink cannot be held");
  }
  if (!command_processor_.HoldPm4MarkerSink(marker_controller_, token) ||
      !*token) {
    return Fail(error, "PM4 runtime external sink hold failed");
  }
  return true;
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::
    AcknowledgeArmAndResumeAfterStart(const gpu::Pm4MarkerHoldToken& token,
                                      std::string* error) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!token || terminally_detached_ || !attached_ ||
      !IsSourceHealthyLocked(error)) {
    return false;
  }
  const GuestExecutionMarkerControllerStatus controller =
      marker_controller_->status();
  if (controller.state != GuestExecutionMarkerControllerState::kArmed ||
      controller.emitted_boundary_count != 1 ||
      controller.acknowledged_boundary_count != 0 ||
      !marker_controller_->AcknowledgeBoundary(1)) {
    return Fail(error,
                "PM4 runtime external sink arm boundary is not canonical");
  }
  if (!command_processor_.ResumePm4MarkerSink(marker_controller_, token)) {
    return Fail(error, "PM4 runtime external sink resume failed");
  }
  return true;
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::
    RearmAfterRejectedStart(const gpu::Pm4MarkerHoldToken& token,
                            std::string* error) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!token || terminally_detached_ || !attached_ ||
      !IsSourceHealthyLocked(error)) {
    return false;
  }
  const GuestExecutionMarkerControllerStatus controller =
      marker_controller_->status();
  // Retract before admission reopens; a marker delivered against a retained
  // unacknowledged arm boundary would fail the controller closed.
  if (controller.state != GuestExecutionMarkerControllerState::kArmed ||
      controller.emitted_boundary_count != 1 ||
      controller.acknowledged_boundary_count != 0 ||
      !marker_controller_->RetractArmBoundaryAndRearm()) {
    return Fail(error,
                "PM4 runtime external sink arm boundary is not retractable");
  }
  if (!command_processor_.ResumePm4MarkerSink(marker_controller_, token)) {
    return Fail(error, "PM4 runtime external sink rearm resume failed");
  }
  return true;
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::IsSourceHealthy(
    std::string* error) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return IsSourceHealthyLocked(error);
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::SealAndDetach(
    const gpu::Pm4MarkerHoldToken& token, std::string* error) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!token || terminally_detached_ || !attached_ ||
      !IsSourceHealthyLocked(error)) {
    return false;
  }
  const GuestExecutionMarkerControllerStatus controller =
      marker_controller_->status();
  bool acknowledge_stop = false;
  if (controller.state == GuestExecutionMarkerControllerState::kStopped) {
    if (controller.emitted_boundary_count != 2 ||
        controller.acknowledged_boundary_count != 1 ||
        !controller.stop_marker_ordinal ||
        controller.stop_marker_ordinal != token.last_ordinal) {
      return Fail(error,
                  "PM4 runtime stop boundary does not match the source seal");
    }
    acknowledge_stop = true;
  } else if (controller.state != GuestExecutionMarkerControllerState::kArmed ||
             controller.emitted_boundary_count != 1 ||
             controller.acknowledged_boundary_count != 1) {
    return Fail(error,
                "PM4 runtime controller is not at a terminal rendezvous");
  }
  if (!command_processor_.SealAndDetachHeldPm4MarkerSink(marker_controller_,
                                                         token)) {
    return Fail(error, "PM4 runtime external sink terminal attestation failed");
  }
  attached_ = false;
  terminally_detached_ = true;
  if (acknowledge_stop && !marker_controller_->AcknowledgeBoundary(2)) {
    return Fail(error, "PM4 runtime stop boundary acknowledgement failed");
  }
  return true;
}

bool GuestExecutionSessionCaptureRuntimePm4ExternalSink::AbortAndDetach(
    std::string* error) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminally_detached_ || !attached_) {
    return true;
  }
  if (command_processor_.DetachPm4MarkerSink(marker_controller_)) {
    attached_ = false;
    terminally_detached_ = true;
    return true;
  }
  const gpu::Pm4MarkerDispatcherStatus source =
      command_processor_.pm4_marker_dispatcher_status();
  if (source.shut_down && !source.sink_attached && !source.sink_held) {
    attached_ = false;
    terminally_detached_ = true;
    return true;
  }
  return Fail(error, "PM4 runtime external sink abort detach failed");
}

GuestExecutionSessionCaptureRuntimePm4Wiring::
    GuestExecutionSessionCaptureRuntimePm4Wiring(
        std::unique_ptr<Lifetime> lifetime) noexcept
    : lifetime_(std::move(lifetime)) {}

GuestExecutionSessionCaptureRuntimePm4Wiring::
    ~GuestExecutionSessionCaptureRuntimePm4Wiring() {
  Lifetime& lifetime = *lifetime_;
  if (lifetime.runtime && lifetime.runtime->IsControlWorkerThread()) {
    std::terminate();
  }
  if (lifetime.runtime) {
    lifetime.runtime->Shutdown();
  }
  const bool detached = !lifetime.external_sink ||
                        lifetime.external_sink->AbortAndDetach(nullptr);
  if (lifetime.marker_bridge) {
    lifetime.marker_bridge->runtime.store(nullptr, std::memory_order_release);
  }
  if (!detached) {
    // The command processor still owns a controller with this raw callback.
    // Preserve callback storage rather than allowing a teardown UAF.
    lifetime_.release();
  }
}

GuestExecutionSessionCaptureRuntime&
GuestExecutionSessionCaptureRuntimePm4Wiring::runtime() noexcept {
  return *lifetime_->runtime;
}

const GuestExecutionSessionCaptureRuntime&
GuestExecutionSessionCaptureRuntimePm4Wiring::runtime() const noexcept {
  return *lifetime_->runtime;
}

const std::shared_ptr<GuestExecutionMarkerController>&
GuestExecutionSessionCaptureRuntimePm4Wiring::marker_controller()
    const noexcept {
  return lifetime_->marker_controller;
}

std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4Wiring>
GuestExecutionSessionCaptureRuntimePm4Wiring::CreateAndAttach(
    Processor& processor, kernel::GuestScheduler& scheduler,
    gpu::CommandProcessor& command_processor,
    const GuestExecutionSessionCaptureRuntimeConfig& runtime_config,
    const GuestExecutionSessionCaptureRuntimeDependencies& dependencies,
    const GuestExecutionMarkerControllerConfig& marker_config,
    const GuestExecutionMarkerClock& marker_clock, std::string* error) {
  if (error) {
    error->clear();
  }
  const GuestExecutionSessionBoundaryPolicy& boundary =
      runtime_config.assembler.boundary;
  if (dependencies.pm4_external_sink ||
      runtime_config.assembler.pm4_marker_sink_ordinal ==
          GuestExecutionSessionAssembler::kNoExternalSink ||
      boundary.kind != GuestExecutionSessionBoundaryKind::kGuestMarkerCount ||
      boundary.value != marker_config.stop_marker_count ||
      boundary.marker_source != GuestExecutionSessionMarkerSource::kPm4Swap ||
      marker_config.marker_source !=
          GuestExecutionSessionMarkerSource::kPm4Swap ||
      boundary.marker_identity != gpu::kPm4SwapMarkerOpcode ||
      marker_config.marker_identity != gpu::kPm4SwapMarkerOpcode) {
    Fail(error,
         "PM4 runtime wiring requires one matching PM4 marker-count policy");
    return nullptr;
  }

  std::unique_ptr<MarkerBridge> marker_bridge;
  std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4ExternalSink>
      external_sink;
  std::unique_ptr<Lifetime> lifetime;
  std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4Wiring> wiring;
  try {
    marker_bridge = std::make_unique<MarkerBridge>();
  } catch (...) {
    Fail(error, "PM4 runtime wiring could not allocate its marker bridge");
    return nullptr;
  }
  std::shared_ptr<GuestExecutionMarkerController> marker_controller =
      GuestExecutionMarkerController::Create(marker_config, marker_clock,
                                             *marker_bridge, error);
  if (!marker_controller) {
    return nullptr;
  }
  try {
    external_sink =
        std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4ExternalSink>(
            new GuestExecutionSessionCaptureRuntimePm4ExternalSink(
                command_processor, marker_controller));
  } catch (...) {
    Fail(error, "PM4 runtime wiring could not allocate its source adapter");
    return nullptr;
  }

  // Complete every potentially throwing owner allocation before creating the
  // observer-retained runtime. No failure after this point can orphan a live
  // runtime with raw pointers to already-destroyed composition objects.
  try {
    lifetime = std::make_unique<Lifetime>();
    wiring = std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4Wiring>(
        new GuestExecutionSessionCaptureRuntimePm4Wiring(std::move(lifetime)));
  } catch (...) {
    Fail(error, "PM4 runtime wiring could not allocate its owner");
    return nullptr;
  }
  wiring->lifetime_->marker_bridge = std::move(marker_bridge);
  wiring->lifetime_->marker_controller = std::move(marker_controller);
  wiring->lifetime_->external_sink = std::move(external_sink);

  GuestExecutionSessionCaptureRuntimeDependencies runtime_dependencies =
      dependencies;
  runtime_dependencies.pm4_external_sink =
      wiring->lifetime_->external_sink.get();
  std::shared_ptr<GuestExecutionSessionCaptureRuntime> runtime =
      GuestExecutionSessionCaptureRuntime::CreateAndAttach(
          processor, scheduler, runtime_config, runtime_dependencies, error);
  if (!runtime) {
    return nullptr;
  }
  wiring->lifetime_->runtime = std::move(runtime);
  wiring->lifetime_->marker_bridge->runtime.store(
      wiring->lifetime_->runtime.get(), std::memory_order_release);
  if (!wiring->lifetime_->external_sink->Activate(error)) {
    return nullptr;
  }
  return wiring;
}

struct GuestExecutionSessionCaptureRuntime::Impl {
  Impl(Processor& processor, kernel::GuestScheduler& scheduler,
       const GuestExecutionSessionCaptureRuntimeConfig& config,
       const GuestExecutionSessionCaptureRuntimeDependencies& dependencies)
      : processor(processor),
        scheduler(scheduler),
        config(config),
        dependencies(dependencies),
        queue(config.event_queue_capacity) {
    if (dependencies.checkpoint_controller) {
      checkpoint_controller = dependencies.checkpoint_controller;
    } else {
      direct_checkpoint_controller =
          std::make_unique<DirectCheckpointController>(scheduler);
      checkpoint_controller = direct_checkpoint_controller.get();
    }
  }

  ~Impl() { StopWorker(); }

  bool InitializeAssembler(std::string* error) {
    GuestExecutionSessionAssemblerDependencies assembler_dependencies;
    assembler_dependencies.clock = dependencies.clock;
    assembler_dependencies.state_provider = dependencies.provider;
    assembler_dependencies.content_provider = dependencies.provider;
    assembler_dependencies.publisher = &deferred_publisher;
    GuestExecutionSessionAssemblerConfig assembler_config = config.assembler;
    assembler_config.defer_duration_boundaries = true;
    assembler = GuestExecutionSessionAssembler::Create(
        assembler_config, assembler_dependencies, error);
    if (!assembler) {
      return false;
    }
    if (!dependencies.pm4_external_sink) {
      return true;
    }
    uint32_t ordinal = GuestExecutionSessionAssembler::kNoExternalSink;
    if (!assembler->RegisterExternalSink("pm4-swap-marker-source", &ordinal)) {
      return Fail(error,
                  "capture runtime could not register its PM4 external sink");
    }
    if (ordinal != config.assembler.pm4_marker_sink_ordinal) {
      return Fail(error,
                  "capture runtime PM4 external sink ordinal differs from "
                  "the assembler configuration");
    }
    external_sink_registered.store(true, std::memory_order_release);
    return true;
  }

  void StartWorker() {
    worker = std::thread([this]() { WorkerMain(); });
    worker_id = worker.get_id();
  }

  void RequestShutdownFence() noexcept {
    std::lock_guard<std::mutex> lock(control_mutex);
    shutdown_requested.store(true, std::memory_order_release);
  }

  bool AdmitPublication() noexcept {
    // Admission and shutdown are totally ordered, but the external publisher
    // is never called while this internal gate is held. A publication admitted
    // first completes on the worker before an external Shutdown join returns.
    std::lock_guard<std::mutex> lock(control_mutex);
    return !shutdown_requested.load(std::memory_order_acquire);
  }

  bool IsWorkerThread() const noexcept {
    return std::this_thread::get_id() == worker_id;
  }

  void StopWorker() noexcept {
    shutdown_pending.store(true, std::memory_order_release);
    RequestShutdownFence();
    wake_condition.notify_all();
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      worker.join();
    }
  }

  void LatchAsyncFailure(AsyncFailure failure) noexcept {
    AsyncFailure expected = AsyncFailure::kNone;
    async_failure.compare_exchange_strong(expected, failure,
                                          std::memory_order_release,
                                          std::memory_order_relaxed);
    wake_condition.notify_one();
  }

  bool Push(const RuntimeEvent& event) noexcept {
    if (!queue.TryPush(event)) {
      LatchAsyncFailure(AsyncFailure::kQueueOverflow);
      return false;
    }
    queued_event_count.fetch_add(1, std::memory_order_relaxed);
    wake_condition.notify_one();
    return true;
  }

  bool ForwardIfActive(const RuntimeEvent& event) noexcept {
    if (!session_active.load(std::memory_order_acquire)) {
      return true;
    }
    callback_count.fetch_add(1, std::memory_order_acq_rel);
    const bool accepted = ForwardInsideCallback(
        event, session_active.load(std::memory_order_acquire),
        capture_gate.load(std::memory_order_acquire));
    callback_count.fetch_sub(1, std::memory_order_acq_rel);
    return accepted;
  }

  bool ForwardInsideCallback(const RuntimeEvent& event, bool active,
                             bool accepting) noexcept {
    bool accepted = true;
    if (active && accepting) {
      accepted = Push(event);
    } else if (active) {
      LatchAsyncFailure(AsyncFailure::kSourceAfterSeal);
      accepted = false;
    }
    return accepted;
  }

  void WaitForCallbacks() const noexcept {
    while (callback_count.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  void SetState(RuntimeState new_state) {
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      state = new_state;
    }
    state_atomic.store(new_state, std::memory_order_release);
    status_condition.notify_all();
  }

  void SetAttachmentStatus(bool processor_value, bool scheduler_value) {
    std::lock_guard<std::mutex> lock(status_mutex);
    processor_attached = processor_value;
    scheduler_attached = scheduler_value;
  }

  // The modeled export dispatches this session's provider binds blocked
  // participants to are recorded on the Processor-owned log installed here, so
  // there is exactly one log and both sides read the same open calls.
  bool AttachExternalEventLog() {
    std::shared_ptr<GuestExecutionCaptureExternalEventLog> log;
    try {
      log = std::make_shared<GuestExecutionCaptureExternalEventLog>();
    } catch (...) {
      return false;
    }
    if (!processor.AttachGuestExecutionCaptureExternalEventLog(log)) {
      return false;
    }
    external_event_log = std::move(log);
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      external_event_log_attached = true;
    }
    return true;
  }

  bool DetachExternalEventLog() {
    if (!external_event_log) {
      return true;
    }
    if (!processor.DetachGuestExecutionCaptureExternalEventLog(
            external_event_log)) {
      return false;
    }
    external_event_log.reset();
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      external_event_log_attached = false;
    }
    return true;
  }

  // A refused detach means a guest thread is still parked inside an export the
  // log promised to record. The log stays installed rather than dropping that
  // promise, and the session is terminally rejected instead of reporting a
  // result whose tape is missing an event that is still being produced.
  void RejectRetainedExternalEventLog() {
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      if (rejection == RuntimeRejection::kNone) {
        rejection = RuntimeRejection::kSourceAttachment;
      }
      if (!status_message.empty()) {
        status_message += "; ";
      }
      status_message +=
          "capture runtime retained its modeled export event log because a "
          "dispatch is still open";
    }
    SetState(RuntimeState::kRejected);
  }

  bool ValidateCheckpoint(
      const CheckpointSnapshot& checkpoint,
      const GuestExecutionCaptureThreadStateRegistrySnapshot& registry,
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) {
    if (!checkpoint.generation ||
        checkpoint.rejection != CheckpointRejection::kNone ||
        !checkpoint.active || !checkpoint.quiesced ||
        checkpoint.roster_scope !=
            kernel::GuestSchedulerCheckpointRosterScope::kSchedulerOwned ||
        checkpoint.release_policy !=
            kernel::GuestSchedulerCheckpointReleasePolicy::
                kRunningSafepointsRequeueAtHead) {
      return Fail(error,
                  "capture runtime checkpoint is not a held, complete "
                  "scheduler-owned roster");
    }
    if (!registry.all_ready() || registry.participants.empty()) {
      return Fail(error,
                  "capture runtime Processor participant roster is not ready");
    }
    if (checkpoint.participants.size() > registry.participants.size()) {
      return Fail(error,
                  "capture runtime scheduler checkpoint exceeds the "
                  "Processor participant roster");
    }
    if (host_calls.rejection !=
        GuestExecutionCaptureHostCallRosterRejection::kNone) {
      return Fail(error, "capture runtime host-call roster is rejected");
    }
    size_t scheduler_owned_count = 0;
    size_t unsupported_count = 0;
    std::string unsupported_diagnostic;
    auto fail_unsupported = [&]() {
      return Fail(error,
                  unsupported_diagnostic +
                      fmt::format("; unsupported={}/{}", unsupported_count,
                                  checkpoint.participants.size()));
    };
    auto fail_participant = [&](std::string message) {
      return unsupported_count ? fail_unsupported()
                               : Fail(error, std::move(message));
    };
    for (size_t index = 0; index < registry.participants.size(); ++index) {
      const auto& lifecycle = registry.participants[index];
      if (lifecycle.state !=
              GuestExecutionCaptureThreadStateLifecycleState::kReady ||
          !lifecycle.participant.capture_instance_id ||
          !lifecycle.participant.guest_thread_id) {
        return fail_participant(
            "capture runtime participant identity is not publishable");
      }
      for (size_t prior = 0; prior < index; ++prior) {
        if (registry.participants[prior].participant.capture_instance_id ==
                lifecycle.participant.capture_instance_id ||
            registry.participants[prior].participant.guest_thread_id ==
                lifecycle.participant.guest_thread_id) {
          return fail_participant(
              "capture runtime Processor roster contains a duplicate");
        }
      }
      const CheckpointParticipant* checkpoint_participant =
          FindCheckpointParticipant(checkpoint,
                                    lifecycle.participant.guest_thread_id);
      if (!checkpoint_participant) {
        continue;
      }
      if (checkpoint_participant->capture_instance_id !=
          lifecycle.participant.capture_instance_id) {
        return fail_participant(
            "capture runtime checkpoint and Processor identities differ");
      }
      ++scheduler_owned_count;
      if (checkpoint_participant->preempt_defers_irql ||
          checkpoint_participant->preempt_defers_lock ||
          checkpoint_participant->capture_declined_safepoints) {
        return fail_participant(
            "capture runtime checkpoint intersects an in-flight scheduler "
            "preemption episode");
      }
      if (IsPassiveOutsideGuestParticipant(*checkpoint_participant) &&
          FindOutermostActiveHostCall(host_calls, lifecycle.participant)) {
        return fail_participant(CensusPassiveParticipantHostCalls(
            checkpoint, registry, host_calls));
      }
      std::string capability_error;
      if (!dependencies.provider->SupportsCheckpointParticipant(
              *checkpoint_participant, &capability_error)) {
        if (!unsupported_count) {
          unsupported_diagnostic =
              capability_error.empty()
                  ? "capture runtime provider cannot encode a durable "
                    "continuation for a checkpoint participant"
                  : std::move(capability_error);
        } else if (unsupported_count < kMaxReportedUnsupportedParticipants) {
          unsupported_diagnostic.append("; also: ");
          unsupported_diagnostic.append(
              DescribeCheckpointParticipant(*checkpoint_participant));
        }
        ++unsupported_count;
      }
    }
    if (unsupported_count) {
      return fail_unsupported();
    }
    if (scheduler_owned_count != checkpoint.participants.size()) {
      return Fail(error,
                  "capture runtime checkpoint contains a non-Processor "
                  "participant");
    }
    for (const auto& call : host_calls.active_calls) {
      const CheckpointParticipant* checkpoint_participant =
          FindCheckpointParticipant(checkpoint,
                                    call.participant.guest_thread_id);
      // A thread parked inside a blocking export is always below the host-call
      // dispatch token its GuestFunction::Call opened, so its outer call is
      // expected exactly like a safepoint's. The provider has already refused
      // any blocked participant it could not bind to a modeled export
      // dispatch, so reaching here means this one carries a typed route.
      if (!ActiveHostCallHasDurableContinuation(checkpoint_participant)) {
        return Fail(error, CensusUnrepresentableActiveHostCalls(checkpoint,
                                                                host_calls));
      }
    }
    return true;
  }

  bool ValidateFinalizedCheckpoint(const CheckpointSnapshot& provisional,
                                   const CheckpointSnapshot& final,
                                   std::string* error) {
    if (final.generation != provisional.generation ||
        final.rejection != CheckpointRejection::kNone || final.active ||
        !final.quiesced || final.roster_scope != provisional.roster_scope ||
        final.release_policy != provisional.release_policy ||
        final.participants != provisional.participants) {
      return Fail(error,
                  "capture runtime checkpoint changed before atomic release");
    }
    return true;
  }

  bool ValidateAssemblerHostDepths(
      const GuestExecutionCaptureHostCallRosterSnapshot& host_calls,
      std::string* error) {
    const auto assembler_status = assembler->status();
    for (const auto& participant : assembler_status.participants) {
      uint32_t depth = 0;
      for (const auto& call : host_calls.active_calls) {
        if (call.participant == participant.identity) {
          depth = std::max(depth, call.participant_depth);
        }
      }
      if (depth != participant.host_call_depth) {
        return Fail(error,
                    "capture runtime host-call queue and roster depths differ");
      }
    }
    return true;
  }

  bool ArriveActiveParticipants(const CheckpointSnapshot& checkpoint,
                                std::string* error) {
    for (const auto& participant : assembler->status().participants) {
      if (!participant.host_call_depth || participant.arrived ||
          participant.held) {
        continue;
      }
      const CheckpointParticipant* checkpoint_participant =
          FindCheckpointParticipant(checkpoint,
                                    participant.identity.guest_thread_id);
      if (!checkpoint_participant ||
          checkpoint_participant->resume_kind !=
              CheckpointResumeKind::kJitSafepoint ||
          !checkpoint_participant->restorable) {
        return Fail(error,
                    "capture runtime cannot represent a non-safepoint active "
                    "outer call");
      }
      if (assembler->ArriveAtSafepoint(participant.identity) ==
          AssemblerAction::kReject) {
        return Fail(error, assembler->status().message);
      }
    }
    return true;
  }

  bool HoldExternalSink(std::string* error) {
    if (!dependencies.pm4_external_sink) {
      return true;
    }
    if (external_sink_terminally_detached.load(std::memory_order_acquire)) {
      return true;
    }
    if (external_sink_hold_token) {
      return true;
    }
    gpu::Pm4MarkerHoldToken token;
    std::string source_error;
    if (!dependencies.pm4_external_sink->Hold(&token, &source_error) ||
        !token) {
      external_sink_control_failed.store(true, std::memory_order_release);
      std::string message =
          "capture runtime could not hold its PM4 external sink; the owner "
          "must detach it before dependency teardown";
      if (!source_error.empty()) {
        message += ": " + source_error;
      }
      return Fail(error, std::move(message));
    }
    external_sink_hold_token = token;
    external_sink_attested_generation.store(token.sink_generation,
                                            std::memory_order_release);
    external_sink_hold_epoch.store(token.hold_epoch, std::memory_order_release);
    external_sink_last_ordinal.store(token.last_ordinal,
                                     std::memory_order_release);
    external_sink_held.store(true, std::memory_order_release);
    if (!dependencies.pm4_external_sink->IsSourceHealthy(&source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error, source_error.empty()
                             ? "capture runtime PM4 external sink is unhealthy"
                             : std::move(source_error));
    }
    return true;
  }

  bool ResumeExternalSinkAfterStart(std::string* error) {
    if (!dependencies.pm4_external_sink) {
      return true;
    }
    if (!external_sink_hold_token) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error,
                  "capture runtime cannot resume an unheld PM4 external sink");
    }
    std::string source_error;
    if (!dependencies.pm4_external_sink->AcknowledgeArmAndResumeAfterStart(
            *external_sink_hold_token, &source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error,
                  source_error.empty()
                      ? "capture runtime could not resume its PM4 external sink"
                      : std::move(source_error));
    }
    external_sink_hold_token.reset();
    external_sink_held.store(false, std::memory_order_release);
    if (!dependencies.pm4_external_sink->IsSourceHealthy(&source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error, source_error.empty()
                             ? "capture runtime PM4 source failed after resume"
                             : std::move(source_error));
    }
    return true;
  }

  bool RearmExternalSink(std::string* error) {
    if (!dependencies.pm4_external_sink) {
      return true;
    }
    if (!external_sink_hold_token) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error,
                  "capture runtime cannot rearm an unheld PM4 external sink");
    }
    std::string source_error;
    if (!dependencies.pm4_external_sink->RearmAfterRejectedStart(
            *external_sink_hold_token, &source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error,
                  source_error.empty()
                      ? "capture runtime could not rearm its PM4 external sink"
                      : std::move(source_error));
    }
    external_sink_hold_token.reset();
    external_sink_held.store(false, std::memory_order_release);
    if (!dependencies.pm4_external_sink->IsSourceHealthy(&source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error, source_error.empty()
                             ? "capture runtime PM4 source failed after rearm"
                             : std::move(source_error));
    }
    return true;
  }

  bool CheckExternalSinkHealth(std::string* error) {
    if (!dependencies.pm4_external_sink ||
        external_sink_terminally_detached.load(std::memory_order_acquire)) {
      return true;
    }
    std::string source_error;
    if (dependencies.pm4_external_sink->IsSourceHealthy(&source_error)) {
      return true;
    }
    external_sink_control_failed.store(true, std::memory_order_release);
    return Fail(error, source_error.empty()
                           ? "capture runtime PM4 external source was lost"
                           : std::move(source_error));
  }

  bool SealAndDetachExternalSink(std::string* error) {
    if (!dependencies.pm4_external_sink ||
        external_sink_terminally_detached.load(std::memory_order_acquire)) {
      return true;
    }
    if (!external_sink_hold_token) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error,
                  "capture runtime cannot seal an unheld PM4 external sink");
    }
    std::string source_error;
    if (!dependencies.pm4_external_sink->SealAndDetach(
            *external_sink_hold_token, &source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error, source_error.empty()
                             ? "capture runtime PM4 terminal source seal failed"
                             : std::move(source_error));
    }
    external_sink_hold_token.reset();
    external_sink_held.store(false, std::memory_order_release);
    external_sink_terminally_detached.store(true, std::memory_order_release);
    return true;
  }

  bool AbortAndDetachExternalSink(std::string* error) noexcept {
    if (!dependencies.pm4_external_sink ||
        external_sink_terminally_detached.load(std::memory_order_acquire)) {
      return true;
    }
    std::string source_error;
    if (!dependencies.pm4_external_sink->AbortAndDetach(&source_error)) {
      external_sink_control_failed.store(true, std::memory_order_release);
      return Fail(error, source_error.empty()
                             ? "capture runtime PM4 abort detach failed"
                             : std::move(source_error));
    }
    external_sink_hold_token.reset();
    external_sink_held.store(false, std::memory_order_release);
    external_sink_terminally_detached.store(true, std::memory_order_release);
    return true;
  }

  void EndProvider(bool accepted) noexcept {
    if (!provider_armed.load(std::memory_order_acquire)) {
      return;
    }
    dependencies.provider->EndCapture(accepted);
    provider_armed.store(false, std::memory_order_release);
  }

  bool CancelActiveBarrier(std::string* error = nullptr) noexcept {
    const uint64_t generation =
        checkpoint_generation.load(std::memory_order_acquire);
    if (!generation) {
      return true;
    }
    CheckpointRejection last_result = CheckpointRejection::kNone;
    for (size_t attempt = 0; attempt < config.checkpoint_release_attempts;
         ++attempt) {
      CheckpointSnapshot final;
      try {
        last_result = checkpoint_controller->Cancel(generation, &final);
      } catch (...) {
        last_result = CheckpointRejection::kInvalidTopology;
      }
      if (final.generation == generation && !final.active) {
        uint64_t expected = generation;
        checkpoint_generation.compare_exchange_strong(
            expected, 0, std::memory_order_release, std::memory_order_relaxed);
        return true;
      }
    }
    if (error) {
      *error = "capture runtime retained checkpoint generation " +
               std::to_string(generation) + " after " +
               std::to_string(config.checkpoint_release_attempts) +
               " failed cancellation attempts; last code " +
               std::to_string(static_cast<uint32_t>(last_result));
    }
    return false;
  }

  void Reject(RuntimeRejection rejection_value, std::string message) {
    std::string sink_error;
    if (!HoldExternalSink(&sink_error)) {
      rejection_value = RuntimeRejection::kExternalSinkControl;
      if (!message.empty()) {
        message += "; ";
      }
      message += sink_error;
    }
    capture_gate.store(false, std::memory_order_release);
    session_active.store(false, std::memory_order_release);
    WaitForCallbacks();
    std::string detach_error;
    const bool source_detached = AbortAndDetachExternalSink(&detach_error);
    std::string barrier_error;
    if (source_detached) {
      CancelActiveBarrier(&barrier_error);
    } else {
      barrier_error =
          "capture runtime retained its checkpoint because the PM4 source "
          "could not be detached";
    }
    EndProvider(false);
    deferred_publisher.Clear();
    if (assembler) {
      assembler->Cancel(message);
    }
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      rejection = rejection_value;
      if (!barrier_error.empty()) {
        if (!message.empty()) {
          message += "; ";
        }
        message += barrier_error;
      }
      if (!detach_error.empty()) {
        if (!message.empty()) {
          message += "; ";
        }
        message += detach_error;
      }
      status_message = std::move(message);
    }
    SetState(RuntimeState::kRejected);
  }

  // A retry only exists where a later arm boundary can arrive, and only for an
  // attempt that has not yet touched the assembler, provider or event bridge.
  bool ShouldRearmArmAttempt(RuntimeRejection rejection_value,
                             CheckpointRejection barrier_rejection) const {
    return dependencies.pm4_external_sink && !arm_attempt_committed &&
           arm_attempt_count.load(std::memory_order_relaxed) <=
               config.arm_retry_limit &&
           !shutdown_requested.load(std::memory_order_acquire) &&
           !external_sink_control_failed.load(std::memory_order_acquire) &&
           !external_sink_terminally_detached.load(std::memory_order_acquire) &&
           async_failure.load(std::memory_order_acquire) ==
               AsyncFailure::kNone &&
           ClassifyArmRejection(rejection_value, barrier_rejection) ==
               ArmAttemptDisposition::kRetryable;
  }

  // Tears the attempt down exactly as Reject does, minus the terminal source
  // detach, and hands the source back so the next marker arms a clean attempt.
  void DiscardArmAttemptAndRearm(RuntimeRejection rejection_value,
                                 std::string message) {
    std::string sink_error;
    if (!HoldExternalSink(&sink_error)) {
      Reject(RuntimeRejection::kExternalSinkControl,
             JoinDiagnostics(std::move(message), std::move(sink_error)));
      return;
    }
    capture_gate.store(false, std::memory_order_release);
    session_active.store(false, std::memory_order_release);
    WaitForCallbacks();
    std::string barrier_error;
    if (!CancelActiveBarrier(&barrier_error)) {
      Reject(RuntimeRejection::kCheckpointBarrier,
             JoinDiagnostics(std::move(message), std::move(barrier_error)));
      return;
    }
    EndProvider(false);
    deferred_publisher.Clear();
    // The attempt never seeded this assembler, and replacing it rather than
    // cancelling it makes the absence of carried state structural.
    assembler.reset();
    std::string assembler_error;
    if (!InitializeAssembler(&assembler_error)) {
      Reject(RuntimeRejection::kAssemblerFailure,
             JoinDiagnostics(std::move(message), std::move(assembler_error)));
      return;
    }
    initial_scheduler_checkpoint.reset();
    instruction_coverage_deltas.clear();
    XELOGW(
        "Guest execution session capture discarded arm attempt {} at marker "
        "ordinal {}: rejection={} diagnostic={}",
        arm_attempt_count.load(std::memory_order_relaxed),
        external_sink_last_ordinal.load(std::memory_order_relaxed),
        static_cast<uint32_t>(rejection_value), message);
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      status_message = message;
    }
    {
      std::lock_guard<std::mutex> lock(control_mutex);
      start_requested = false;
    }
    SetState(RuntimeState::kIdle);
    // Admission reopens last, so no marker can reach a half-discarded attempt.
    std::string rearm_error;
    if (!RearmExternalSink(&rearm_error)) {
      Reject(RuntimeRejection::kExternalSinkControl,
             JoinDiagnostics(std::move(message), std::move(rearm_error)));
    }
  }

  void RejectArmAttempt(RuntimeRejection rejection_value,
                        CheckpointRejection barrier_rejection,
                        std::string message) {
    if (ShouldRearmArmAttempt(rejection_value, barrier_rejection)) {
      DiscardArmAttemptAndRearm(rejection_value, std::move(message));
      return;
    }
    Reject(rejection_value, std::move(message));
  }

  bool RejectAsyncFailureIfAny() {
    const AsyncFailure failure = async_failure.load(std::memory_order_acquire);
    if (failure == AsyncFailure::kNone) {
      return false;
    }
    Reject(MapAsyncFailure(failure), AsyncFailureMessage(failure));
    return true;
  }

  void HandleStart() {
    if (state_atomic.load(std::memory_order_acquire) != RuntimeState::kIdle) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime start was requested outside idle");
      return;
    }
    SetState(RuntimeState::kStarting);
    arm_attempt_count.fetch_add(1, std::memory_order_relaxed);
    arm_attempt_committed = false;

    CheckpointSnapshot provisional;
    const CheckpointRejection pause_result =
        checkpoint_controller->Pause(config.checkpoint_timeout, &provisional);
    if (pause_result != CheckpointRejection::kNone) {
      if (provisional.generation && provisional.active) {
        checkpoint_generation.store(provisional.generation,
                                    std::memory_order_release);
      }
      RejectArmAttempt(
          RuntimeRejection::kCheckpointBarrier, pause_result,
          "capture runtime start checkpoint barrier failed with code " +
              std::to_string(static_cast<uint32_t>(pause_result)));
      return;
    }
    checkpoint_generation.store(provisional.generation,
                                std::memory_order_release);
    std::string error;
    // Hold as soon as the scheduler is quiescent. Ordinal assignment and sink
    // generation admission are now closed before roster/provider setup, so no
    // PM4 callback can straddle that comparatively expensive work.
    if (!HoldExternalSink(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }

    // The provisional barrier is now the only legal topology. Keep the event
    // gate sealed while the roster, providers and assembler are initialized,
    // but reject any source transition that races that initialization.
    session_active.store(true, std::memory_order_release);
    const auto registry = processor.QueryGuestExecutionCaptureParticipants();
    const auto host_calls = host_call_roster.snapshot();
    if (!ValidateCheckpoint(provisional, registry, host_calls, &error)) {
      RejectArmAttempt(RuntimeRejection::kCheckpointRoster,
                       CheckpointRejection::kNone, std::move(error));
      return;
    }
    // Past this point the attempt owns session state that no retry may inherit.
    arm_attempt_committed = true;
    if (!assembler->SeedParticipants(registry.participants, host_calls)) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime assembler rejected the initial roster");
      return;
    }
    if (!dependencies.provider->BeginCapture(provisional, registry.participants,
                                             host_calls, &error)) {
      Reject(RuntimeRejection::kProviderFailure,
             error.empty() ? "capture runtime provider failed to arm"
                           : std::move(error));
      return;
    }
    provider_armed.store(true, std::memory_order_release);
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!dependencies.event_bridge->BeginSession(
            *assembler, provisional, registry.participants, &error)) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             error.empty() ? "capture runtime event bridge failed to begin"
                           : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!assembler->Arm(&error) || !assembler->RequestStart(&error)) {
      Reject(RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    if (dependencies.pm4_external_sink &&
        assembler->OnExternalSinkHeld(
            config.assembler.pm4_marker_sink_ordinal) ==
            AssemblerAction::kReject) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
      return;
    }
    if (!ArriveActiveParticipants(provisional, &error) ||
        assembler->status().state != AssemblerState::kRecording) {
      Reject(RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }

    capture_gate.store(true, std::memory_order_release);
    // Open the marker source while the guest checkpoint is still held. Guest
    // execution cannot otherwise outrun Resume and produce a swap in the held
    // generation.
    if (!ResumeExternalSinkAfterStart(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }
    CheckpointSnapshot final;
    const CheckpointRejection finalize_result =
        checkpoint_controller->Finalize(provisional.generation, &final);
    const bool generation_released =
        final.generation == provisional.generation && !final.active;
    if (generation_released) {
      uint64_t expected = provisional.generation;
      checkpoint_generation.compare_exchange_strong(
          expected, 0, std::memory_order_release, std::memory_order_relaxed);
    }
    if (finalize_result != CheckpointRejection::kNone || !generation_released ||
        !ValidateFinalizedCheckpoint(provisional, final, &error)) {
      Reject(
          RuntimeRejection::kCheckpointBarrier,
          error.empty()
              ? "capture runtime start checkpoint release failed with code " +
                    std::to_string(static_cast<uint32_t>(finalize_result))
              : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }
    initial_scheduler_checkpoint = std::move(final);
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    SetState(RuntimeState::kRecording);
  }

  AssemblerAction ProcessSourceEvent(const RuntimeEvent& event,
                                     std::string* error) {
    switch (event.kind) {
      case RuntimeEventKind::kLifecycle:
        if (event.guest_instruction_delta &&
            assembler->OnInstructionCoverage(event.lifecycle.participant,
                                             event.guest_instruction_delta) !=
                AssemblerAction::kContinue) {
          if (error && error->empty()) {
            *error = assembler->status().message;
          }
          return AssemblerAction::kReject;
        }
        return assembler->OnParticipantLifecycle(event.lifecycle);
      case RuntimeEventKind::kHostCallBegin:
        return assembler->OnOuterHostCallBegin(
            event.participant, event.function_address,
            event.function_end_address, event.return_address);
      case RuntimeEventKind::kHostCallEnd:
        if (event.guest_instruction_delta &&
            assembler->OnInstructionCoverage(event.participant,
                                             event.guest_instruction_delta) !=
                AssemblerAction::kContinue) {
          if (error && error->empty()) {
            *error = assembler->status().message;
          }
          return AssemblerAction::kReject;
        }
        return assembler->OnOuterHostCallEnd(event.participant,
                                             event.host_call_outcome);
      case RuntimeEventKind::kScheduler: {
        const AssemblerAction action =
            dependencies.event_bridge->OnSchedulerEvent(*assembler,
                                                        event.scheduler, error);
        scheduler_event_count.fetch_add(1, std::memory_order_relaxed);
        return action;
      }
      case RuntimeEventKind::kGuestMarker:
        return assembler->OnGuestMarker(std::nullopt, event.marker_source,
                                        event.marker_identity);
      case RuntimeEventKind::kStart:
      case RuntimeEventKind::kStop:
        break;
    }
    return AssemblerAction::kContinue;
  }

  bool FlushQuiescedInstructionCoverage() {
    std::string error;
    instruction_coverage_deltas.clear();
    if (!dependencies.provider->CollectInstructionCoverageDeltas(
            &instruction_coverage_deltas, &error)) {
      Reject(RuntimeRejection::kProviderFailure,
             error.empty()
                 ? "capture runtime could not collect instruction coverage"
                 : std::move(error));
      return false;
    }
    if (!instruction_coverage_deltas.empty()) {
      Reject(RuntimeRejection::kSourceRejected,
             "capture runtime checkpoint found unordered residual instruction "
             "coverage");
      return false;
    }
    return true;
  }

  bool DrainSourceEventsDuringStop(std::string* error) {
    RuntimeEvent event;
    while (queue.TryPop(&event)) {
      processed_event_count.fetch_add(1, std::memory_order_relaxed);
      if (event.kind == RuntimeEventKind::kStart ||
          event.kind == RuntimeEventKind::kStop) {
        continue;
      }
      if (ProcessSourceEvent(event, error) == AssemblerAction::kReject) {
        if (error->empty()) {
          *error = assembler->status().message;
        }
        return false;
      }
    }
    return true;
  }

  void HandleStop(bool manual_request, bool deferred_duration_request = false) {
    if (state_atomic.load(std::memory_order_acquire) !=
        RuntimeState::kRecording) {
      return;
    }
    SetState(RuntimeState::kStopping);

    CheckpointSnapshot provisional;
    const CheckpointRejection pause_result =
        checkpoint_controller->Pause(config.checkpoint_timeout, &provisional);
    if (pause_result != CheckpointRejection::kNone) {
      if (provisional.generation && provisional.active) {
        checkpoint_generation.store(provisional.generation,
                                    std::memory_order_release);
      }
      Reject(RuntimeRejection::kCheckpointBarrier,
             "capture runtime stop checkpoint barrier failed with code " +
                 std::to_string(static_cast<uint32_t>(pause_result)));
      return;
    }
    checkpoint_generation.store(provisional.generation,
                                std::memory_order_release);

    std::string error;
    if (!HoldExternalSink(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }
    capture_gate.store(false, std::memory_order_release);
    WaitForCallbacks();

    if (scheduler.capture_rejected()) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             "capture runtime scheduler source rejected delivery");
      return;
    }
    if (RejectAsyncFailureIfAny()) {
      return;
    }
    if (!DrainSourceEventsDuringStop(&error)) {
      Reject(RuntimeRejection::kAssemblerFailure, std::move(error));
      return;
    }
    if (!FlushQuiescedInstructionCoverage()) {
      return;
    }
    // A manual stop becomes an assembler boundary only after the checkpoint
    // has closed the source gate and every callback admitted before that close
    // has drained. This keeps a host call that completed before the gate close
    // in the recording prefix instead of holding its begin and then treating
    // its end as unbalanced. A drained event may itself have triggered an
    // automatic reel boundary, in which case that earlier boundary wins.
    AssemblerState assembler_state = assembler->status().state;
    if ((manual_request || deferred_duration_request) &&
        assembler_state == AssemblerState::kRecording) {
      const AssemblerAction request_action =
          deferred_duration_request ? assembler->RequestDeferredDurationStop()
                                    : assembler->RequestStop();
      if (request_action == AssemblerAction::kReject) {
        Reject(RuntimeRejection::kAssemblerFailure,
               assembler->status().message);
        return;
      }
      assembler_state = assembler->status().state;
    }
    if (assembler_state == AssemblerState::kRejected) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
      return;
    }
    if (assembler_state != AssemblerState::kStopRequested &&
        assembler_state != AssemblerState::kStopRendezvous &&
        assembler_state != AssemblerState::kPublishing) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime stop did not establish a boundary");
      return;
    }
    if (dependencies.pm4_external_sink &&
        assembler->OnExternalSinkHeld(
            config.assembler.pm4_marker_sink_ordinal) ==
            AssemblerAction::kReject) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
      return;
    }
    // These are the last source snapshots inside the capture. Processor closes
    // session_active under its lifecycle-registry lock; that release store is
    // the topology cutoff. Host callbacks classify at entry and count their
    // entire roster mutation, so either source is wholly before the cutoff and
    // captured or rejected, or wholly after it and outside the session.
    const auto host_calls = host_call_roster.snapshot();
    const auto registry =
        processor.QueryGuestExecutionCaptureParticipantsAtCutoff(
            session_active);
    WaitForCallbacks();
    if (RejectAsyncFailureIfAny()) {
      return;
    }
    if (!ValidateCheckpoint(provisional, registry, host_calls, &error) ||
        !ValidateAssemblerHostDepths(host_calls, &error)) {
      Reject(RuntimeRejection::kCheckpointRoster, std::move(error));
      return;
    }
    if (!ArriveActiveParticipants(provisional, &error)) {
      Reject(RuntimeRejection::kAssemblerFailure, std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!dependencies.provider->SealCapture(provisional, host_calls, &error)) {
      Reject(RuntimeRejection::kProviderFailure,
             error.empty() ? "capture runtime provider failed to seal"
                           : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!dependencies.event_bridge->SealSession(*assembler, provisional,
                                                &error)) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             error.empty() ? "capture runtime event bridge failed to seal"
                           : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (assembler->status().state != AssemblerState::kPublishing) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime assembler did not reach publication rendezvous");
      return;
    }

    SetState(RuntimeState::kPublishing);
    if (!assembler->Publish(&error)) {
      Reject(RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    std::optional<GuestExecutionSessionBundle> staged =
        deferred_publisher.Take();
    if (!staged) {
      Reject(RuntimeRejection::kAssemblerFailure,
             "capture runtime assembler produced no staged bundle");
      return;
    }

    // This ticketed handoff is the final authority over the PM4 source. It
    // drains and removes the held controller before guest execution resumes;
    // source Shutdown or later swaps are therefore either observed before the
    // seal (and reject it) or wholly outside the captured session.
    if (!SealAndDetachExternalSink(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }

    CheckpointSnapshot final;
    const CheckpointRejection finalize_result =
        checkpoint_controller->Finalize(provisional.generation, &final);
    const bool generation_released =
        final.generation == provisional.generation && !final.active;
    if (generation_released) {
      uint64_t expected = provisional.generation;
      checkpoint_generation.compare_exchange_strong(
          expected, 0, std::memory_order_release, std::memory_order_relaxed);
    }
    if (finalize_result != CheckpointRejection::kNone || !generation_released ||
        !ValidateFinalizedCheckpoint(provisional, final, &error)) {
      Reject(RuntimeRejection::kCheckpointBarrier,
             error.empty()
                 ? "capture runtime stop checkpoint release failed with code " +
                       std::to_string(static_cast<uint32_t>(finalize_result))
                 : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    EndProvider(true);
    if (!dependencies.event_bridge->FinalizeBundle(
            &*staged, scheduler_event_count.load(std::memory_order_relaxed),
            &error)) {
      Reject(RuntimeRejection::kEventBridgeFailure,
             error.empty()
                 ? "capture runtime event bridge did not close the bundle"
                 : std::move(error));
      return;
    }
    if (shutdown_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!initial_scheduler_checkpoint ||
        !ValidateRuntimePublicationBundle(
            *staged, *initial_scheduler_checkpoint, final,
            config.assembler.bundle_limits, &error)) {
      Reject(RuntimeRejection::kBundleValidation,
             error.empty() ? "capture runtime lacks the held start checkpoint"
                           : std::move(error));
      return;
    }
    if (!CheckExternalSinkHealth(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }
    if (!AdmitPublication()) {
      return;
    }
    if (!dependencies.publisher->Publish(*staged, &error)) {
      Reject(RuntimeRejection::kPublicationFailure,
             error.empty() ? "capture runtime canonical publication failed"
                           : std::move(error));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      canonical_output_published = true;
      status_message.clear();
    }
    SetState(RuntimeState::kComplete);
  }

  void ProcessEvent(const RuntimeEvent& event) {
    const RuntimeState current = state_atomic.load(std::memory_order_acquire);
    if (event.kind == RuntimeEventKind::kStart) {
      HandleStart();
      return;
    }
    if (event.kind == RuntimeEventKind::kStop) {
      HandleStop(true);
      return;
    }
    if (current != RuntimeState::kRecording) {
      return;
    }
    std::string error;
    if (!CheckExternalSinkHealth(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }
    if (ProcessSourceEvent(event, &error) == AssemblerAction::kReject) {
      Reject(event.kind == RuntimeEventKind::kScheduler
                 ? RuntimeRejection::kEventBridgeFailure
                 : RuntimeRejection::kAssemblerFailure,
             error.empty() ? assembler->status().message : std::move(error));
      return;
    }
    const AssemblerState assembler_state = assembler->status().state;
    if (assembler_state == AssemblerState::kRejected) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
    } else if (assembler_state == AssemblerState::kStopRequested ||
               assembler_state == AssemblerState::kStopRendezvous ||
               assembler_state == AssemblerState::kPublishing) {
      HandleStop(false);
    }
  }

  void PollAssembler() {
    if (state_atomic.load(std::memory_order_acquire) !=
        RuntimeState::kRecording) {
      return;
    }
    std::string error;
    if (!CheckExternalSinkHealth(&error)) {
      Reject(RuntimeRejection::kExternalSinkControl, std::move(error));
      return;
    }
    const AssemblerState coverage_state = assembler->status().state;
    if (coverage_state == AssemblerState::kStopRequested ||
        coverage_state == AssemblerState::kStopRendezvous ||
        coverage_state == AssemblerState::kPublishing) {
      HandleStop(false);
      return;
    }
    const AssemblerAction action = assembler->Poll();
    if (action == AssemblerAction::kReject) {
      Reject(RuntimeRejection::kAssemblerFailure, assembler->status().message);
      return;
    }
    const AssemblerState assembler_state = assembler->status().state;
    if (action == AssemblerAction::kHold &&
        assembler_state == AssemblerState::kRecording) {
      HandleStop(false, true);
      return;
    }
    if (assembler_state == AssemblerState::kStopRequested ||
        assembler_state == AssemblerState::kStopRendezvous ||
        assembler_state == AssemblerState::kPublishing) {
      HandleStop(false);
    }
  }

  void WorkerMain() noexcept {
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      worker_running = true;
    }
    status_condition.notify_all();
    try {
      while (!shutdown_requested.load(std::memory_order_acquire)) {
        if (RejectAsyncFailureIfAny()) {
          break;
        }
        RuntimeEvent event;
        bool processed = false;
        while (queue.TryPop(&event)) {
          processed = true;
          processed_event_count.fetch_add(1, std::memory_order_relaxed);
          ProcessEvent(event);
          if (shutdown_requested.load(std::memory_order_acquire) ||
              IsTerminal(state_atomic.load(std::memory_order_acquire))) {
            break;
          }
          PollAssembler();
          if (IsTerminal(state_atomic.load(std::memory_order_acquire))) {
            break;
          }
        }
        if (shutdown_requested.load(std::memory_order_acquire)) {
          break;
        }
        if (IsTerminal(state_atomic.load(std::memory_order_acquire))) {
          break;
        }
        PollAssembler();
        if (IsTerminal(state_atomic.load(std::memory_order_acquire))) {
          break;
        }
        if (!processed) {
          std::unique_lock<std::mutex> lock(wake_mutex);
          wake_condition.wait_for(lock, config.control_poll_interval, [this]() {
            return shutdown_requested.load(std::memory_order_acquire) ||
                   queue.MaybeNotEmpty() ||
                   async_failure.load(std::memory_order_acquire) !=
                       AsyncFailure::kNone;
          });
        }
      }
    } catch (const std::bad_alloc&) {
      Reject(RuntimeRejection::kProviderFailure,
             "capture runtime control worker allocation failed");
    } catch (...) {
      Reject(RuntimeRejection::kSourceRejected,
             "capture runtime control worker raised an exception");
    }
    if (shutdown_requested.load(std::memory_order_acquire) &&
        !IsTerminal(state_atomic.load(std::memory_order_acquire))) {
      std::string sink_error;
      const bool sink_held = HoldExternalSink(&sink_error);
      capture_gate.store(false, std::memory_order_release);
      session_active.store(false, std::memory_order_release);
      WaitForCallbacks();
      std::string detach_error;
      const bool source_detached = AbortAndDetachExternalSink(&detach_error);
      std::string barrier_error;
      if (source_detached) {
        CancelActiveBarrier(&barrier_error);
      } else {
        barrier_error =
            "capture runtime retained its checkpoint because the PM4 source "
            "could not be detached";
      }
      EndProvider(false);
      deferred_publisher.Clear();
      if (assembler) {
        assembler->Cancel("capture runtime shut down");
      }
      {
        std::lock_guard<std::mutex> lock(status_mutex);
        rejection = sink_held ? RuntimeRejection::kCancelled
                              : RuntimeRejection::kExternalSinkControl;
        status_message = "capture runtime shut down";
        if (!sink_error.empty()) {
          status_message += "; " + sink_error;
        }
        if (!barrier_error.empty()) {
          status_message += "; " + barrier_error;
        }
        if (!detach_error.empty()) {
          status_message += "; " + detach_error;
        }
      }
      SetState(RuntimeState::kShutdown);
    }
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      worker_running = false;
    }
    status_condition.notify_all();
  }

  Processor& processor;
  kernel::GuestScheduler& scheduler;
  const GuestExecutionSessionCaptureRuntimeConfig config;
  const GuestExecutionSessionCaptureRuntimeDependencies dependencies;
  RuntimeEventQueue queue;
  GuestExecutionCaptureHostCallRoster host_call_roster;
  DeferredPublisher deferred_publisher;
  std::unique_ptr<DirectCheckpointController> direct_checkpoint_controller;
  GuestExecutionSessionCaptureRuntimeCheckpointController*
      checkpoint_controller = nullptr;
  std::unique_ptr<GuestExecutionSessionAssembler> assembler;
  std::optional<CheckpointSnapshot> initial_scheduler_checkpoint;
  std::vector<GuestExecutionSessionInstructionCoverageDelta>
      instruction_coverage_deltas;

  std::thread worker;
  std::thread::id worker_id;
  mutable std::mutex wake_mutex;
  std::condition_variable wake_condition;
  std::mutex control_mutex;
  std::mutex shutdown_operation_mutex;
  std::atomic<bool> shutdown_requested{false};
  std::atomic<bool> shutdown_pending{false};
  std::atomic<bool> session_active{false};
  std::atomic<bool> capture_gate{false};
  mutable std::atomic<uint64_t> callback_count{0};
  std::atomic<AsyncFailure> async_failure{AsyncFailure::kNone};
  std::atomic<uint64_t> last_scheduler_sequence{0};
  std::atomic<uint64_t> queued_event_count{0};
  std::atomic<uint64_t> processed_event_count{0};
  std::atomic<RuntimeState> state_atomic{RuntimeState::kIdle};
  std::atomic<uint64_t> arm_attempt_count{0};
  bool arm_attempt_committed = false;
  bool start_requested = false;
  bool stop_requested = false;

  mutable std::mutex status_mutex;
  mutable std::condition_variable status_condition;
  RuntimeState state = RuntimeState::kIdle;
  RuntimeRejection rejection = RuntimeRejection::kNone;
  std::string status_message;
  std::atomic<uint64_t> checkpoint_generation{0};
  std::atomic<uint64_t> scheduler_event_count{0};
  bool processor_attached = false;
  bool scheduler_attached = false;
  bool external_event_log_attached = false;
  std::shared_ptr<GuestExecutionCaptureExternalEventLog> external_event_log;
  std::atomic<bool> provider_armed{false};
  // Rearming rebuilds the assembler on the worker, so this outlives one
  // attachment and is read from every status caller.
  std::atomic<bool> external_sink_registered{false};
  std::atomic<bool> external_sink_held{false};
  std::atomic<bool> external_sink_terminally_detached{false};
  std::atomic<bool> external_sink_control_failed{false};
  std::optional<gpu::Pm4MarkerHoldToken> external_sink_hold_token;
  std::atomic<uint64_t> external_sink_attested_generation{0};
  std::atomic<uint64_t> external_sink_hold_epoch{0};
  std::atomic<uint64_t> external_sink_last_ordinal{0};
  std::atomic<void (*)(void*)> request_start_prequeue_test_hook{nullptr};
  std::atomic<void*> request_start_prequeue_test_context{nullptr};
  bool canonical_output_published = false;
  bool worker_running = false;
  std::weak_ptr<GuestExecutionSessionCaptureRuntime> owner;
};

std::shared_ptr<GuestExecutionSessionCaptureRuntime>
GuestExecutionSessionCaptureRuntime::CreateAndAttach(
    Processor& processor, kernel::GuestScheduler& scheduler,
    const GuestExecutionSessionCaptureRuntimeConfig& config,
    const GuestExecutionSessionCaptureRuntimeDependencies& dependencies,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!dependencies.clock || !dependencies.provider ||
      !dependencies.event_bridge || !dependencies.publisher) {
    Fail(error, "capture runtime dependencies are missing");
    return nullptr;
  }
  const bool pm4_sink_configured =
      config.assembler.pm4_marker_sink_ordinal !=
      GuestExecutionSessionAssembler::kNoExternalSink;
  if (pm4_sink_configured != (dependencies.pm4_external_sink != nullptr)) {
    Fail(error,
         "capture runtime PM4 external sink configuration and dependency "
         "differ");
    return nullptr;
  }
  if (config.assembler.coverage_mode !=
      GuestExecutionReelCoverageMode::kContinuousInstructions) {
    Fail(error,
         "capture runtime requires continuous instruction coverage mode");
    return nullptr;
  }
  if (config.assembler.boundary.kind ==
      GuestExecutionSessionBoundaryKind::kGuestInstructionCount) {
    Fail(error,
         "capture runtime does not support exact instruction-count boundaries");
    return nullptr;
  }
  if (config.event_queue_capacity < 2 ||
      !IsPowerOfTwo(config.event_queue_capacity)) {
    Fail(error,
         "capture runtime event queue capacity must be a power of two and at "
         "least two");
    return nullptr;
  }
  if (config.checkpoint_timeout.count() <= 0 ||
      config.control_poll_interval.count() <= 0 ||
      !config.checkpoint_release_attempts ||
      config.checkpoint_release_attempts > 64) {
    Fail(error, "capture runtime timing configuration is invalid");
    return nullptr;
  }

  std::unique_ptr<Impl> impl;
  try {
    impl = std::make_unique<Impl>(processor, scheduler, config, dependencies);
  } catch (...) {
    Fail(error, "capture runtime could not allocate its bounded queue");
    return nullptr;
  }
  if (!impl->InitializeAssembler(error)) {
    return nullptr;
  }
  auto runtime = std::shared_ptr<GuestExecutionSessionCaptureRuntime>(
      new GuestExecutionSessionCaptureRuntime(std::move(impl)));
  runtime->impl_->owner = runtime;
  runtime->impl_->StartWorker();
  if (!runtime->Attach(error)) {
    runtime->Shutdown();
    return nullptr;
  }
  return runtime;
}

GuestExecutionSessionCaptureRuntime::GuestExecutionSessionCaptureRuntime(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GuestExecutionSessionCaptureRuntime::~GuestExecutionSessionCaptureRuntime() {
  Shutdown();
}

bool GuestExecutionSessionCaptureRuntime::Attach(std::string* error) {
  const auto owner = impl_->owner.lock();
  if (!owner) {
    return Fail(error, "capture runtime lost ownership before attachment");
  }
  const auto scheduler_observer =
      std::static_pointer_cast<kernel::GuestSchedulerCaptureObserver>(owner);
  const auto processor_observer =
      std::static_pointer_cast<GuestExecutionCaptureHostCallObserver>(owner);
  // Installed before the observers so no dispatch a checkpoint may have to bind
  // to can open outside this session's log.
  if (!impl_->AttachExternalEventLog()) {
    return Fail(error,
                "capture runtime could not install its modeled export event "
                "log");
  }
  if (!impl_->scheduler.AttachCaptureObserverTransactionally(
          scheduler_observer, [this, &processor_observer]() {
            return impl_->processor.AttachGuestExecutionCaptureHostCallObserver(
                processor_observer);
          })) {
    impl_->DetachExternalEventLog();
    return Fail(error, "capture runtime observer transaction was rejected");
  }
  impl_->SetAttachmentStatus(true, true);
  return true;
}

bool GuestExecutionSessionCaptureRuntime::IsControlWorkerThread()
    const noexcept {
  return impl_->IsWorkerThread();
}

bool GuestExecutionSessionCaptureRuntime::RequestStart() noexcept {
  std::unique_lock<std::mutex> lock(impl_->control_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return false;
  }
  if (impl_->start_requested ||
      impl_->shutdown_requested.load(std::memory_order_acquire) ||
      IsTerminal(impl_->state_atomic.load(std::memory_order_acquire))) {
    return false;
  }
  if (auto hook = impl_->request_start_prequeue_test_hook.load(
          std::memory_order_acquire)) {
    hook(impl_->request_start_prequeue_test_context.load(
        std::memory_order_acquire));
  }
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kStart;
  if (!impl_->Push(event)) {
    return false;
  }
  impl_->start_requested = true;
  return true;
}

void GuestExecutionSessionCaptureRuntime::SetRequestStartPrequeueTestHook(
    void (*hook)(void*), void* context) noexcept {
  impl_->request_start_prequeue_test_context.store(context,
                                                   std::memory_order_release);
  impl_->request_start_prequeue_test_hook.store(hook,
                                                std::memory_order_release);
}

bool GuestExecutionSessionCaptureRuntime::RequestStop() noexcept {
  std::lock_guard<std::mutex> lock(impl_->control_mutex);
  if (!impl_->start_requested || impl_->stop_requested ||
      impl_->shutdown_requested.load(std::memory_order_acquire) ||
      IsTerminal(impl_->state_atomic.load(std::memory_order_acquire))) {
    return false;
  }
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kStop;
  if (!impl_->Push(event)) {
    return false;
  }
  impl_->stop_requested = true;
  return true;
}

bool GuestExecutionSessionCaptureRuntime::OnGuestMarker(
    GuestExecutionSessionMarkerSource source,
    uint64_t marker_identity) noexcept {
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kGuestMarker;
  event.marker_source = source;
  event.marker_identity = marker_identity;
  return impl_->ForwardIfActive(event);
}

void GuestExecutionSessionCaptureRuntime::Shutdown() noexcept {
  if (impl_->IsWorkerThread()) {
    // External callbacks may reenter Shutdown from the control worker. The
    // worker can establish the no-new-publication fence, but a non-worker call
    // owns joining and detaching the two permanent observers.
    impl_->shutdown_pending.store(true, std::memory_order_release);
    impl_->RequestShutdownFence();
    impl_->wake_condition.notify_all();
    return;
  }
  std::lock_guard<std::mutex> shutdown_lock(impl_->shutdown_operation_mutex);
  impl_->StopWorker();
  // Released first so no later dispatch can open a call this session would
  // have to account for.
  const bool log_released = impl_->DetachExternalEventLog();
  const auto owner = impl_->owner.lock();
  if (!owner) {
    if (!log_released) {
      impl_->RejectRetainedExternalEventLog();
    }
    return;
  }
  bool scheduler_attached = false;
  bool processor_attached = false;
  {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    scheduler_attached = impl_->scheduler_attached;
    processor_attached = impl_->processor_attached;
  }
  if (scheduler_attached) {
    const auto observer =
        std::static_pointer_cast<kernel::GuestSchedulerCaptureObserver>(owner);
    if (impl_->scheduler.DetachCaptureObserver(observer)) {
      scheduler_attached = false;
    }
  }
  if (processor_attached) {
    const auto observer =
        std::static_pointer_cast<GuestExecutionCaptureHostCallObserver>(owner);
    if (impl_->processor.DetachGuestExecutionCaptureHostCallObserver(
            observer)) {
      processor_attached = false;
    }
  }
  impl_->SetAttachmentStatus(processor_attached, scheduler_attached);
  if (!log_released) {
    impl_->RejectRetainedExternalEventLog();
  }
}

bool GuestExecutionSessionCaptureRuntime::WaitForTerminal(
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->status_mutex);
  return impl_->status_condition.wait_for(
      lock, timeout, [this]() { return IsTerminal(impl_->state); });
}

GuestExecutionSessionCaptureRuntimeStatus
GuestExecutionSessionCaptureRuntime::status() const {
  std::lock_guard<std::mutex> lock(impl_->status_mutex);
  GuestExecutionSessionCaptureRuntimeStatus result;
  result.state = impl_->state;
  result.rejection = impl_->rejection;
  result.checkpoint_generation =
      impl_->checkpoint_generation.load(std::memory_order_relaxed);
  result.arm_attempt_count =
      impl_->arm_attempt_count.load(std::memory_order_relaxed);
  result.queued_event_count =
      impl_->queued_event_count.load(std::memory_order_relaxed);
  result.processed_event_count =
      impl_->processed_event_count.load(std::memory_order_relaxed);
  result.scheduler_event_count =
      impl_->scheduler_event_count.load(std::memory_order_relaxed);
  result.last_scheduler_sequence =
      impl_->last_scheduler_sequence.load(std::memory_order_relaxed);
  result.processor_attached = impl_->processor_attached;
  result.external_event_log_attached = impl_->external_event_log_attached;
  result.scheduler_attached = impl_->scheduler_attached;
  result.provider_armed = impl_->provider_armed.load(std::memory_order_relaxed);
  result.external_sink_registered =
      impl_->external_sink_registered.load(std::memory_order_relaxed);
  result.external_sink_held =
      impl_->external_sink_held.load(std::memory_order_relaxed);
  result.external_sink_terminally_detached =
      impl_->external_sink_terminally_detached.load(std::memory_order_relaxed);
  result.external_sink_control_failed =
      impl_->external_sink_control_failed.load(std::memory_order_relaxed);
  result.external_sink_attested_generation =
      impl_->external_sink_attested_generation.load(std::memory_order_relaxed);
  result.external_sink_hold_epoch =
      impl_->external_sink_hold_epoch.load(std::memory_order_relaxed);
  result.external_sink_last_ordinal =
      impl_->external_sink_last_ordinal.load(std::memory_order_relaxed);
  result.canonical_output_published = impl_->canonical_output_published;
  result.worker_running = impl_->worker_running;
  result.shutdown_pending =
      impl_->shutdown_pending.load(std::memory_order_relaxed);
  result.message = impl_->status_message;
  return result;
}

GuestExecutionCaptureThreadStateLifecycleDisposition
GuestExecutionSessionCaptureRuntime::OnThreadStateSeed(
    std::span<const GuestExecutionCaptureThreadStateLifecycleEvent>
        events) noexcept {
  if (events.size() >
      impl_->config.assembler.bundle_limits.session.maximum_participants) {
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
  }
  for (const auto& event : events) {
    if (event.state != GuestExecutionCaptureThreadStateLifecycleState::kReady ||
        event.guest_instruction_delta ||
        !event.participant.capture_instance_id ||
        !event.participant.guest_thread_id) {
      return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
    }
  }
  return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
}

GuestExecutionCaptureThreadStateLifecycleDisposition
GuestExecutionSessionCaptureRuntime::OnThreadStateLifecycle(
    GuestExecutionCaptureThreadStateLifecycleEvent event) noexcept {
  if (event.guest_instruction_delta &&
      event.state !=
          GuestExecutionCaptureThreadStateLifecycleState::kDestroying) {
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
  }
  RuntimeEvent runtime_event;
  runtime_event.kind = RuntimeEventKind::kLifecycle;
  runtime_event.guest_instruction_delta = event.guest_instruction_delta;
  event.guest_instruction_delta = 0;
  runtime_event.lifecycle = event;
  return impl_->ForwardIfActive(runtime_event)
             ? GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept
             : GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
}

GuestExecutionCaptureJitSafepointDisposition
GuestExecutionSessionCaptureRuntime::OnJitSafepoint(const ThreadState&,
                                                    uint32_t) noexcept {
  if (impl_->session_active.load(std::memory_order_acquire)) {
    impl_->LatchAsyncFailure(AsyncFailure::kUnexpectedJitSafepoint);
    return GuestExecutionCaptureJitSafepointDisposition::kReject;
  }
  return GuestExecutionCaptureJitSafepointDisposition::kAccept;
}

GuestExecutionCaptureHostCallToken
GuestExecutionSessionCaptureRuntime::OnHostGuestCallBegin(
    const ThreadState& thread_state, const GuestFunction& function,
    uint32_t return_address) noexcept {
  // The callback count covers the roster mutation as well as classification,
  // making the stop cutoff indivisible with respect to final roster snapshots.
  impl_->callback_count.fetch_add(1, std::memory_order_acq_rel);
  const bool active = impl_->session_active.load(std::memory_order_acquire);
  const bool accepting = impl_->capture_gate.load(std::memory_order_acquire);
  const GuestExecutionCaptureHostCallToken token =
      impl_->host_call_roster.OnHostGuestCallBegin(thread_state, function,
                                                   return_address);
  if (!token) {
    impl_->LatchAsyncFailure(AsyncFailure::kHostCallRoster);
    impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
    return {};
  }
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kHostCallBegin;
  event.participant = {thread_state.guest_execution_capture_instance_id(),
                       thread_state.thread_id()};
  event.host_call_token = token;
  event.function_address = function.address();
  event.function_end_address = function.end_address();
  event.return_address = return_address;
  impl_->ForwardInsideCallback(event, active, accepting);
  impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
  return token;
}

bool GuestExecutionSessionCaptureRuntime::OnHostGuestCallEnd(
    GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
    const GuestFunction& function,
    GuestExecutionCaptureHostCallOutcome outcome) noexcept {
  impl_->callback_count.fetch_add(1, std::memory_order_acq_rel);
  const bool active = impl_->session_active.load(std::memory_order_acquire);
  const bool accepting = impl_->capture_gate.load(std::memory_order_acquire);
  RuntimeEvent event;
  event.kind = RuntimeEventKind::kHostCallEnd;
  event.participant = {thread_state.guest_execution_capture_instance_id(),
                       thread_state.thread_id()};
  event.host_call_token = token;
  event.host_call_outcome = outcome;
  event.function_address = function.address();
  event.function_end_address = function.end_address();
  const bool roster_accepted = impl_->host_call_roster.OnHostGuestCallEnd(
      token, thread_state, function, outcome);
  if (!roster_accepted) {
    impl_->LatchAsyncFailure(AsyncFailure::kHostCallRoster);
    impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }
  if (active && accepting) {
    ppc::PPCContext* context = thread_state.context();
    uint64_t* counter =
        std::atomic_ref<uint64_t*>(
            context->guest_execution_session_instruction_counter)
            .load(std::memory_order_acquire);
    if (counter) {
      if (counter != &context->guest_execution_session_instruction_count) {
        impl_->LatchAsyncFailure(AsyncFailure::kInstructionCounter);
        impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
        return false;
      }
      event.guest_instruction_delta =
          std::atomic_ref<uint64_t>(*counter).exchange(
              0, std::memory_order_acq_rel);
    }
  }
  const bool forwarded = impl_->ForwardInsideCallback(event, active, accepting);
  impl_->callback_count.fetch_sub(1, std::memory_order_acq_rel);
  return forwarded;
}

bool GuestExecutionSessionCaptureRuntime::OnSchedulerEvent(
    const kernel::GuestSchedulerCaptureEvent& event) noexcept {
  // GuestScheduler serializes observer delivery under its mutation lock, so a
  // single load/store validates continuity without a compare-exchange loop on
  // the permanent pre-arm path.
  const uint64_t previous =
      impl_->last_scheduler_sequence.load(std::memory_order_relaxed);
  if (!event.sequence || previous == std::numeric_limits<uint64_t>::max() ||
      event.sequence != previous + 1) {
    impl_->LatchAsyncFailure(AsyncFailure::kSchedulerSequence);
    return false;
  }
  impl_->last_scheduler_sequence.store(event.sequence,
                                       std::memory_order_release);
  RuntimeEvent runtime_event;
  runtime_event.kind = RuntimeEventKind::kScheduler;
  runtime_event.scheduler = event;
  return impl_->ForwardIfActive(runtime_event);
}

bool GuestExecutionSessionCaptureRuntime::CanDetach() const noexcept {
  return !impl_->session_active.load(std::memory_order_acquire) &&
         impl_->host_call_roster.CanDetach();
}

struct GuestExecutionSessionTitleCaptureRuntime::Impl {
  class HostClock final : public ppc::GuestInvocationRecorderClock,
                          public GuestExecutionMarkerClock {
   public:
    uint64_t NowTicks() const noexcept override {
      return Clock::QueryHostTickCount();
    }
  };

  Impl(Processor& processor, GuestExecutionSessionTitleCaptureConfig config,
       GuestExecutionSessionSha256 capture_build_sha256,
       GuestExecutionSessionSha256 replay_config_sha256)
      : processor(processor),
        config(std::move(config)),
        capture_build_sha256(capture_build_sha256),
        replay_config_sha256(replay_config_sha256),
        bundle_limits(
            MakeTitleCaptureBundleLimits(this->config.maximum_bundle_bytes)),
        publisher(this->config.output_directory, bundle_limits) {}

  bool BuildRuntimeConfig(
      std::string_view title_identity, std::string_view module_identity,
      GuestExecutionSessionCaptureRuntimeConfig* runtime_config,
      GuestExecutionMarkerControllerConfig* marker_config,
      std::string* error) const noexcept {
    if (!runtime_config || !marker_config || title_identity.empty() ||
        module_identity.empty()) {
      return Fail(error, "session title capture identity is missing");
    }
    const uint64_t tick_frequency = Clock::QueryHostTickFrequency();
    uint64_t warmup_ticks = 0;
    uint64_t rendezvous_ticks = 0;
    uint64_t maximum_duration_ticks = 0;
    if (!TicksFromMilliseconds(config.warmup_milliseconds, tick_frequency,
                               &warmup_ticks) ||
        !TicksFromMilliseconds(5000, tick_frequency, &rendezvous_ticks) ||
        !TicksFromMilliseconds(15 * 60 * 1000, tick_frequency,
                               &maximum_duration_ticks)) {
      return Fail(error, "session title capture timing overflows host ticks");
    }

    GuestExecutionSessionCaptureRuntimeConfig prepared;
    GuestExecutionSessionAssemblerConfig& assembler = prepared.assembler;
    assembler.session_epoch = Clock::QueryHostTickCount();
    if (!assembler.session_epoch) {
      assembler.session_epoch = 1;
    }
    assembler.boundary.kind =
        GuestExecutionSessionBoundaryKind::kGuestMarkerCount;
    assembler.boundary.value = config.stop_marker_count;
    assembler.boundary.marker_source =
        GuestExecutionSessionMarkerSource::kPm4Swap;
    assembler.boundary.marker_identity = gpu::kPm4SwapMarkerOpcode;
    assembler.coverage_mode =
        GuestExecutionReelCoverageMode::kContinuousInstructions;
    assembler.limits.maximum_segment_count = 1u << 20;
    assembler.limits.maximum_event_count = 1u << 20;
    assembler.limits.maximum_guest_instruction_count = 1ull << 32;
    assembler.limits.maximum_guest_marker_count = config.stop_marker_count;
    assembler.limits.maximum_duration_ticks = maximum_duration_ticks;
    assembler.maximum_stop_tail_event_count = 1u << 16;
    assembler.maximum_stop_tail_guest_instruction_count = 1u << 20;
    assembler.maximum_stop_tail_ticks = rendezvous_ticks;
    assembler.maximum_start_rendezvous_ticks = rendezvous_ticks;
    assembler.capture_tick_frequency = tick_frequency;
    assembler.maximum_events_per_chunk = 4096;
    assembler.pm4_marker_sink_ordinal = 0;
    assembler.capture_build_sha256 = capture_build_sha256;
    assembler.replay_config_sha256 = replay_config_sha256;
    try {
      const std::string title_source =
          std::string("xenia-title-identity-v1\0", 24) +
          std::string(title_identity);
      const std::string module_source =
          std::string("xenia-module-identity-v1\0", 25) +
          std::string(module_identity);
      assembler.title_identity_sha256 = GuestExecutionSessionCodec::HashBytes(
          reinterpret_cast<const uint8_t*>(title_source.data()),
          title_source.size());
      assembler.module_identity_sha256 = GuestExecutionSessionCodec::HashBytes(
          reinterpret_cast<const uint8_t*>(module_source.data()),
          module_source.size());
    } catch (...) {
      return Fail(error, "session title capture identity allocation failed");
    }
    assembler.bundle_limits = bundle_limits;
    prepared.arm_retry_limit = static_cast<size_t>(config.arm_retry_limit);

    GuestExecutionMarkerControllerConfig marker;
    marker.marker_source = GuestExecutionSessionMarkerSource::kPm4Swap;
    marker.marker_identity = gpu::kPm4SwapMarkerOpcode;
    marker.warmup_ticks = warmup_ticks;
    marker.stop_marker_count = config.stop_marker_count;
    marker.max_outstanding_boundaries = 1;
    *runtime_config = std::move(prepared);
    *marker_config = marker;
    return true;
  }

  Processor& processor;
  const GuestExecutionSessionTitleCaptureConfig config;
  const GuestExecutionSessionSha256 capture_build_sha256;
  const GuestExecutionSessionSha256 replay_config_sha256;
  const GuestExecutionSessionBundleLimits bundle_limits;
  HostClock clock;
  std::unique_ptr<GuestExecutionSessionCaptureProvider> provider;
  GuestExecutionSessionCaptureSchedulerEventBridge event_bridge;
  GuestExecutionSessionDirectoryPublisher publisher;
  std::unique_ptr<GuestExecutionSessionCaptureRuntimePm4Wiring> wiring;
};

bool GuestExecutionSessionTitleCaptureRuntime::IsRequested() noexcept {
  return !cvars::guest_execution_capture_output.empty();
}

std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>
GuestExecutionSessionTitleCaptureRuntime::CreateAndAttachProvider(
    Memory& memory, Processor& processor, bool guest_scheduler_enabled,
    std::string* error) {
  GuestExecutionSessionTitleCaptureConfig config;
  if (!CurrentTitleCaptureConfig(&config, error)) {
    return nullptr;
  }
  return CreateAndAttachProvider(memory, processor, config,
                                 guest_scheduler_enabled, error);
}

std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>
GuestExecutionSessionTitleCaptureRuntime::CreateAndAttachProvider(
    Memory& memory, Processor& processor,
    const GuestExecutionSessionTitleCaptureConfig& config,
    bool guest_scheduler_enabled, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!guest_scheduler_enabled) {
    Fail(error, "session title capture requires guest_scheduler=true");
    return nullptr;
  }
  if (!processor.backend()) {
    Fail(error, "session title capture requires an initialized CPU backend");
    return nullptr;
  }
  if (!ValidateTitleCaptureConfig(config, error)) {
    return nullptr;
  }

  GuestInvocationReplayConfig replay_config;
  GuestExecutionSessionSha256 capture_build_sha256 = {};
  GuestExecutionSessionSha256 replay_config_sha256 = {};
  if (!CaptureCurrentGuestInvocationReplayConfig(*processor.backend(),
                                                 &replay_config, error) ||
      !ValidateGuestInvocationReplayBenchmarkConfig(replay_config, error) ||
      !HashGuestInvocationReplayConfig(replay_config, &replay_config_sha256,
                                       error) ||
      !HashGuestInvocationReplayFile(filesystem::GetExecutablePath(),
                                     &capture_build_sha256, error)) {
    return nullptr;
  }

  return CreateAndAttachProviderWithProvenance(
      memory, processor, config, guest_scheduler_enabled, capture_build_sha256,
      replay_config_sha256, error);
}

std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>
GuestExecutionSessionTitleCaptureRuntime::CreateAndAttachProviderWithProvenance(
    Memory& memory, Processor& processor,
    const GuestExecutionSessionTitleCaptureConfig& config,
    bool guest_scheduler_enabled,
    const GuestExecutionSessionSha256& capture_build_sha256,
    const GuestExecutionSessionSha256& replay_config_sha256,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!guest_scheduler_enabled || !processor.backend() ||
      !ValidateTitleCaptureConfig(config, error) ||
      std::none_of(capture_build_sha256.cbegin(), capture_build_sha256.cend(),
                   [](uint8_t byte) { return byte != 0; }) ||
      std::none_of(replay_config_sha256.cbegin(), replay_config_sha256.cend(),
                   [](uint8_t byte) { return byte != 0; })) {
    if (error && error->empty()) {
      *error = "session title capture provenance is invalid";
    }
    return nullptr;
  }
  std::unique_ptr<Impl> impl;
  try {
    impl = std::make_unique<Impl>(processor, config, capture_build_sha256,
                                  replay_config_sha256);
  } catch (...) {
    Fail(error, "session title capture owner allocation failed");
    return nullptr;
  }
  GuestExecutionSessionCaptureProviderConfig provider_config;
  provider_config.jit_corpus_config_flags = JitCorpus::kConfigGuestScheduler;
  impl->provider = GuestExecutionSessionCaptureProvider::CreateAndAttach(
      memory, processor, provider_config, error);
  if (!impl->provider) {
    return nullptr;
  }
  return std::unique_ptr<GuestExecutionSessionTitleCaptureRuntime>(
      new GuestExecutionSessionTitleCaptureRuntime(std::move(impl)));
}

GuestExecutionSessionTitleCaptureRuntime::
    GuestExecutionSessionTitleCaptureRuntime(
        std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GuestExecutionSessionTitleCaptureRuntime::
    ~GuestExecutionSessionTitleCaptureRuntime() {
  Shutdown();
}

bool GuestExecutionSessionTitleCaptureRuntime::AttachRuntime(
    kernel::GuestScheduler& scheduler, gpu::CommandProcessor& command_processor,
    std::string_view title_identity, std::string_view module_identity,
    std::string* error) {
  if (!impl_ || impl_->wiring) {
    return Fail(error, "session title capture runtime attachment is invalid");
  }
  GuestExecutionSessionCaptureRuntimeConfig runtime_config;
  GuestExecutionMarkerControllerConfig marker_config;
  if (!impl_->BuildRuntimeConfig(title_identity, module_identity,
                                 &runtime_config, &marker_config, error)) {
    return false;
  }
  GuestExecutionSessionCaptureRuntimeDependencies dependencies;
  dependencies.clock = &impl_->clock;
  dependencies.provider = impl_->provider.get();
  dependencies.event_bridge = &impl_->event_bridge;
  dependencies.publisher = &impl_->publisher;
  impl_->wiring = GuestExecutionSessionCaptureRuntimePm4Wiring::CreateAndAttach(
      impl_->processor, scheduler, command_processor, runtime_config,
      dependencies, marker_config, impl_->clock, error);
  return impl_->wiring != nullptr;
}

void GuestExecutionSessionTitleCaptureRuntime::Shutdown() noexcept {
  if (!impl_) {
    return;
  }
  if (impl_->wiring) {
    const GuestExecutionSessionCaptureRuntimeStatus runtime_status =
        impl_->wiring->runtime().status();
    const GuestExecutionMarkerControllerStatus marker_status =
        impl_->wiring->marker_controller()->status();
    XELOGI(
        "Guest execution session capture status before detach: "
        "runtime_state={} runtime_rejection={} marker_state={} "
        "marker_rejection={} matching_markers={} warmup_markers={} "
        "ignored_markers={} arm_ordinal={} stop_ordinal={} "
        "emitted_boundaries={} acknowledged_boundaries={} arm_attempts={} "
        "queued_events={} processed_events={} published={} diagnostic={}",
        static_cast<uint32_t>(runtime_status.state),
        static_cast<uint32_t>(runtime_status.rejection),
        static_cast<uint32_t>(marker_status.state),
        static_cast<uint32_t>(marker_status.rejection),
        marker_status.matching_marker_count, marker_status.warmup_marker_count,
        marker_status.ignored_marker_count, marker_status.arm_marker_ordinal,
        marker_status.stop_marker_ordinal, marker_status.emitted_boundary_count,
        marker_status.acknowledged_boundary_count,
        runtime_status.arm_attempt_count, runtime_status.queued_event_count,
        runtime_status.processed_event_count,
        runtime_status.canonical_output_published,
        runtime_status.message.empty() ? "none" : runtime_status.message);
  }
  impl_->wiring.reset();
  impl_->provider.reset();
  impl_.reset();
}

bool GuestExecutionSessionTitleCaptureRuntime::runtime_attached()
    const noexcept {
  return impl_ && impl_->wiring;
}

const std::filesystem::path&
GuestExecutionSessionTitleCaptureRuntime::output_directory() const noexcept {
  static const std::filesystem::path kEmptyPath;
  return impl_ ? impl_->config.output_directory : kEmptyPath;
}

}  // namespace cpu
}  // namespace xe

#endif
