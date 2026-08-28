/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_runner.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/exact_jit_corpus_module.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/guest_execution_replay_tape.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <time.h>
#endif  // XE_PLATFORM_MAC

namespace xe {
namespace cpu {

namespace {

constexpr uint32_t kGuestPageSize = GuestExecutionSessionCodec::kGuestPageSize;
constexpr uint32_t kMaximumSupportedHostPageSize = 64 * 1024;
constexpr uint64_t kGuestAddressSpaceSize = 1ull << 32;

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool FailPlan(GuestExecutionSessionReplayPlan* output, std::string* error,
              std::string_view message) {
  if (output) {
    *output = {};
  }
  return Fail(error, message);
}

bool IsPowerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }

bool IsSupportedDataPageAddress(uint32_t address) {
  return (address >= 0x00001000u && address <= 0x7EFFF000u) ||
         (address >= 0x80000000u && address <= 0x9FFFF000u);
}

uint32_t XexBackingPageAddress(uint32_t address) {
  return address >= 0x90000000u && address < 0xA0000000u ? address - 0x10000000u
                                                         : address;
}

const std::vector<uint8_t>* FindBlob(
    const std::map<GuestExecutionSessionSha256, const std::vector<uint8_t>*>&
        blobs,
    const GuestExecutionSessionSha256& digest) {
  const auto it = blobs.find(digest);
  return it == blobs.end() ? nullptr : it->second;
}

const ppc::GuestInvocationPage* FindInvocationPage(
    const std::vector<ppc::GuestInvocationPage>& pages, uint32_t address) {
  const auto it = std::lower_bound(
      pages.cbegin(), pages.cend(), address,
      [](const ppc::GuestInvocationPage& page, uint32_t candidate_address) {
        return page.guest_address < candidate_address;
      });
  return it != pages.cend() && it->guest_address == address ? &*it : nullptr;
}

// The corpus supplies code bytes; checkpoint code references may only restate
// them. Returns the exact corpus bytes for one aligned range or null.
const uint8_t* CorpusCodeRange(const ExecutionJitCorpus& corpus,
                               uint64_t guest_address, uint64_t byte_size) {
  if (!byte_size || guest_address + byte_size > kGuestAddressSpaceSize) {
    return nullptr;
  }
  const uint32_t first_page =
      static_cast<uint32_t>(guest_address & ~uint64_t(kGuestPageSize - 1));
  const uint64_t end = guest_address + byte_size;
  for (uint64_t page = first_page; page < end; page += kGuestPageSize) {
    const uint32_t page_address = static_cast<uint32_t>(page);
    const uint8_t* data = corpus.FindPageData(page_address);
    if (!data) {
      return nullptr;
    }
    // Pages are stored contiguously in address order; a contiguous run of
    // supplied pages therefore has contiguous storage.
    if (page != first_page &&
        data != corpus.FindPageData(page_address - kGuestPageSize) +
                    kGuestPageSize) {
      return nullptr;
    }
  }
  return corpus.FindPageData(first_page) + (guest_address - first_page);
}

std::string EventKindMessage(std::string_view prefix,
                             GuestExecutionSessionEventKind kind) {
  std::string message(prefix);
  message.append(std::to_string(static_cast<uint32_t>(kind)));
  message.append(" has no replay hook in this runner");
  return message;
}

// Nested as wall start, CPU start, work, CPU end, wall end: the primary CPU
// interval excludes the wall-clock reads and the wall interval is diagnostic.
class ThreadCpuInterval {
 public:
  ThreadCpuInterval() = default;
  ~ThreadCpuInterval() { Release(); }
  ThreadCpuInterval(const ThreadCpuInterval&) = delete;
  ThreadCpuInterval& operator=(const ThreadCpuInterval&) = delete;

  bool Begin(std::string* error) {
#if XE_PLATFORM_MAC
    port_ = mach_thread_self();
    if (port_ == MACH_PORT_NULL) {
      return Fail(error, "failed to acquire the current Mach thread port");
    }
    has_port_ = true;
    wall_start_ = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!ReadGuestInvocationReplayThreadCpuNanoseconds(port_, &cpu_start_)) {
      return Fail(error, "failed to read current-thread CPU time");
    }
    return true;
#else
    return Fail(error, "timed guest execution replay is macOS-only");
#endif  // XE_PLATFORM_MAC
  }

  bool End(std::string* error) {
#if XE_PLATFORM_MAC
    const bool cpu_read_succeeded =
        has_port_ &&
        ReadGuestInvocationReplayThreadCpuNanoseconds(port_, &cpu_end_);
    wall_end_ = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    Release();
    if (!cpu_read_succeeded || cpu_end_ < cpu_start_) {
      return Fail(error, "current-thread CPU interval is invalid");
    }
    if (wall_end_ <= wall_start_) {
      return Fail(error, "uptime raw interval is missing or zero");
    }
    return true;
#else
    return Fail(error, "timed guest execution replay is macOS-only");
#endif  // XE_PLATFORM_MAC
  }

  uint64_t cpu_nanoseconds() const { return cpu_end_ - cpu_start_; }
  uint64_t uptime_raw_nanoseconds() const { return wall_end_ - wall_start_; }

 private:
  void Release() {
#if XE_PLATFORM_MAC
    if (has_port_) {
      mach_port_deallocate(mach_task_self(), port_);
      has_port_ = false;
    }
#endif  // XE_PLATFORM_MAC
  }

#if XE_PLATFORM_MAC
  thread_t port_ = MACH_PORT_NULL;
  bool has_port_ = false;
#endif  // XE_PLATFORM_MAC
  uint64_t wall_start_ = 0;
  uint64_t wall_end_ = 0;
  uint64_t cpu_start_ = 0;
  uint64_t cpu_end_ = 0;
};

bool CheckedAccumulate(uint64_t* total, uint64_t value, std::string* error) {
  if (value > std::numeric_limits<uint64_t>::max() - *total) {
    return Fail(error, "replay metric accumulator overflowed");
  }
  *total += value;
  return true;
}

// Guest JIT frames live on the host stack, so workers get the guest thread
// stack size rather than the platform's smaller default.
constexpr size_t kWorkerStackSize = 8ull * 1024ull * 1024ull;

bool SampleCurrentThreadCpuNanoseconds(uint64_t* cpu_nanoseconds) {
#if XE_PLATFORM_MAC
  const thread_t port = mach_thread_self();
  if (port == MACH_PORT_NULL) {
    return false;
  }
  const bool read_succeeded =
      ReadGuestInvocationReplayThreadCpuNanoseconds(port, cpu_nanoseconds);
  mach_port_deallocate(mach_task_self(), port);
  return read_succeeded;
#else
  return false;
#endif  // XE_PLATFORM_MAC
}

// Runs one in-interval verification and attributes its sampled thread CPU
// separately. Used only by full-verification repetitions.
bool RunInIntervalVerification(const std::function<bool(std::string*)>& verify,
                               uint64_t* verification_cpu_nanoseconds,
                               uint64_t* verification_count,
                               std::string* error) {
  uint64_t before = 0;
  if (!SampleCurrentThreadCpuNanoseconds(&before)) {
    return Fail(error, "failed to sample current-thread CPU time");
  }
  const bool verified = verify(error);
  uint64_t after = 0;
  if (!SampleCurrentThreadCpuNanoseconds(&after) || after < before) {
    return verified ? Fail(error, "failed to sample current-thread CPU time")
                    : false;
  }
  if (!CheckedAccumulate(verification_cpu_nanoseconds, after - before, error) ||
      !CheckedAccumulate(verification_count, 1, error)) {
    return false;
  }
  return verified;
}

bool AbandonTurn(GuestExecutionReplayTape* tape,
                 const GuestExecutionReplayTurn& turn, std::string* error,
                 std::string_view message) {
  tape->Abandon(turn, std::string(message));
  return Fail(error, message);
}

enum class ParticipantPhase : uint32_t {
  kOutside,
  kOuterPending,
  kInSegment,
  kInSegmentOuter,
  kOuterEndPending,
};

}  // namespace

bool BuildGuestExecutionSessionReplayPlan(
    const GuestExecutionSessionBundle& bundle, uint32_t host_page_size,
    GuestExecutionSessionReplayPlan* output, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return FailPlan(nullptr, error, "session replay plan output is null");
  }
  *output = {};
  if (!IsPowerOfTwo(host_page_size) || host_page_size < kGuestPageSize ||
      host_page_size > kMaximumSupportedHostPageSize) {
    return FailPlan(output, error,
                    "host page size is unsupported for session replay");
  }
  if (!ValidateGuestExecutionSessionBundle(bundle, error)) {
    return false;
  }
  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  if (manifest.rejected_event_count || manifest.unsupported_event_count) {
    return FailPlan(output, error,
                    "session contains rejected or unsupported events");
  }

  GuestExecutionSessionReplayPlan plan;
  plan.host_page_size = host_page_size;
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    plan.blobs.emplace(blob.sha256, &blob.bytes);
  }

  plan.participants.reserve(manifest.participants.size());
  for (const GuestExecutionSessionParticipant& participant :
       manifest.participants) {
    if (participant.initial_outer_call_state !=
        GuestExecutionSessionInitialOuterCallState::kOutside) {
      return FailPlan(output, error,
                      "participant begins inside an outer host call; "
                      "mid-function resumption is not implemented");
    }
    if (participant.boundary_arrival_kind ==
        GuestExecutionSessionBoundaryArrivalKind::kJitSafepoint) {
      return FailPlan(output, error,
                      "participant arrives at a JIT safepoint; safepoint "
                      "delivery is not implemented");
    }
    if (participant.guest_thread_id == UINT32_MAX) {
      return FailPlan(output, error, "participant guest thread id is reserved");
    }
    const std::vector<uint8_t>* state_blob =
        FindBlob(plan.blobs, participant.initial_state_sha256);
    if (!state_blob) {
      return FailPlan(output, error,
                      "participant initial state blob is missing");
    }
    GuestExecutionSessionReplayParticipant planned;
    planned.guest_thread_id = participant.guest_thread_id;
    planned.initial_state_blob = state_blob;
    std::vector<uint8_t> reencoded;
    if (!ppc::GuestPPCRegisterStateCodec::Decode(
            state_blob->data(), state_blob->size(), &planned.initial_state,
            error) ||
        !ppc::GuestPPCRegisterStateCodec::Encode(planned.initial_state,
                                                 &reencoded, error)) {
      *output = {};
      return false;
    }
    if (reencoded != *state_blob) {
      return FailPlan(output, error,
                      "participant initial state blob is not canonical");
    }
    plan.participants.push_back(std::move(planned));
  }

  const GuestExecutionSessionSha256* corpus_digest = nullptr;
  plan.segments.reserve(manifest.segments.size());
  for (const GuestExecutionSessionSegmentReference& segment :
       manifest.segments) {
    if (!corpus_digest) {
      const std::vector<uint8_t>* corpus_blob =
          FindBlob(plan.blobs, segment.code_corpus_sha256);
      if (!corpus_blob ||
          !ExecutionJitCorpus::Decode(*corpus_blob, &plan.corpus, error)) {
        return FailPlan(output, error, "segment code corpus failed to decode");
      }
      corpus_digest = &segment.code_corpus_sha256;
    } else if (segment.code_corpus_sha256 != *corpus_digest) {
      return FailPlan(output, error,
                      "session segments reference more than one code corpus; "
                      "multi-corpus replay is not implemented");
    }
    const std::vector<uint8_t>* segment_blob =
        FindBlob(plan.blobs, segment.segment_sha256);
    ppc::GuestInvocationArtifact artifact;
    if (!segment_blob || !ppc::GuestInvocationArtifactCodec::Decode(
                             *segment_blob, &artifact, error)) {
      return FailPlan(output, error, "segment artifact failed to decode");
    }
    if (artifact.invocations.size() != 1) {
      return FailPlan(output, error,
                      "segment artifact must contain exactly one invocation");
    }
    if (artifact.code_corpus_sha256 != segment.code_corpus_sha256) {
      return FailPlan(output, error,
                      "segment artifact names a different code corpus");
    }
    if (artifact.replay_config_sha256 != manifest.replay_config_sha256) {
      return FailPlan(
          output, error,
          "segment artifact replay configuration differs from the session");
    }
    if (artifact.capture_build_sha256 != manifest.capture_build_sha256) {
      return FailPlan(
          output, error,
          "segment artifact capture build differs from the session");
    }
    ppc::GuestFunctionInvocation& invocation = artifact.invocations.front();
    if (invocation.function_address != segment.entry_guest_address ||
        invocation.function_end_address != segment.end_guest_address) {
      return FailPlan(output, error,
                      "segment artifact extent differs from the manifest");
    }
    GuestInvocationReplayPlan segment_plan;
    if (!BuildGuestInvocationReplayPlan(invocation, plan.corpus, host_page_size,
                                        &segment_plan, error)) {
      *output = {};
      return false;
    }
    if (plan.segments.empty()) {
      plan.eager_function_count = segment_plan.eager_function_count;
      plan.eager_guest_code_bytes = segment_plan.eager_guest_code_bytes;
      plan.captured_host_code_bytes = segment_plan.captured_host_code_bytes;
    } else if (plan.eager_function_count != segment_plan.eager_function_count ||
               plan.eager_guest_code_bytes !=
                   segment_plan.eager_guest_code_bytes ||
               plan.captured_host_code_bytes !=
                   segment_plan.captured_host_code_bytes) {
      return FailPlan(
          output, error,
          "segment plans disagree about the shared corpus workload");
    }
    GuestExecutionSessionReplaySegment planned;
    planned.thread_ordinal = segment.thread_ordinal;
    planned.begin_sequence = segment.first_event_sequence;
    planned.end_sequence = segment.last_event_sequence;
    planned.invocation = std::move(invocation);
    plan.segments.push_back(std::move(planned));
    ++plan.participants[segment.thread_ordinal].segment_count;
  }

  for (size_t i = 0; i < bundle.chunks.size(); ++i) {
    if (manifest.chunks[i].kind !=
        GuestExecutionSessionChunkKind::kCheckpoint) {
      continue;
    }
    GuestExecutionSessionCheckpointChunk chunk;
    if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(bundle.chunks[i],
                                                           &chunk, error)) {
      *output = {};
      return false;
    }
    plan.checkpoints.push_back(std::move(chunk.checkpoint));
  }
  if (plan.checkpoints.size() < 2 ||
      plan.checkpoints.front().global_sequence != 0 ||
      plan.checkpoints.back().global_sequence != manifest.last_event_sequence) {
    return FailPlan(output, error,
                    "session lacks initial and final checkpoints");
  }

  std::map<uint32_t, GuestExecutionSessionReplayPage> pages;
  for (uint32_t address : plan.corpus.page_addresses()) {
    pages[address] = GuestExecutionSessionReplayPage{
        address, true, plan.corpus.FindPageData(address)};
  }
  for (size_t checkpoint_index = 0; checkpoint_index < plan.checkpoints.size();
       ++checkpoint_index) {
    const bool initial = checkpoint_index == 0;
    for (const GuestExecutionSessionContentReference& content :
         plan.checkpoints[checkpoint_index].content) {
      if (content.kind == GuestExecutionSessionContentKind::kGuestCode) {
        const std::vector<uint8_t>* blob = FindBlob(plan.blobs, content.sha256);
        const uint8_t* corpus_bytes = CorpusCodeRange(
            plan.corpus, content.guest_address, content.byte_size);
        if (!blob || !corpus_bytes || blob->size() != content.byte_size ||
            std::memcmp(blob->data(), corpus_bytes, blob->size())) {
          return FailPlan(output, error,
                          "checkpoint code differs from the exact corpus");
        }
        continue;
      }
      if (content.guest_address >= kGuestAddressSpaceSize ||
          content.byte_size != kGuestPageSize) {
        return FailPlan(output, error, "checkpoint page reference is invalid");
      }
      const uint32_t address = static_cast<uint32_t>(content.guest_address);
      const auto existing = pages.find(address);
      if (initial) {
        if (!IsSupportedDataPageAddress(address)) {
          return FailPlan(output, error,
                          "initial checkpoint page address is unsupported");
        }
        if (existing != pages.end()) {
          return FailPlan(output, error,
                          "initial checkpoint page overlaps exact corpus code "
                          "or repeats a page");
        }
        const std::vector<uint8_t>* blob = FindBlob(plan.blobs, content.sha256);
        if (!blob || blob->size() != kGuestPageSize) {
          return FailPlan(output, error,
                          "initial checkpoint page blob is missing");
        }
        pages[address] =
            GuestExecutionSessionReplayPage{address, false, blob->data()};
      } else if (existing == pages.end() || existing->second.code) {
        return FailPlan(
            output, error,
            "checkpoint page is absent from the initial checkpoint");
      }
    }
  }
  for (const GuestExecutionSessionReplaySegment& segment : plan.segments) {
    for (const ppc::GuestInvocationPage& page :
         segment.invocation.input_data_pages) {
      const auto it = pages.find(page.guest_address);
      if (it == pages.end() || it->second.code) {
        return FailPlan(
            output, error,
            "segment input page is absent from the initial checkpoint");
      }
    }
  }

  std::map<uint32_t, uint32_t> xex_backing_owners;
  for (const auto& page : pages) {
    const uint32_t backing_address = XexBackingPageAddress(page.first);
    const auto [it, inserted] =
        xex_backing_owners.emplace(backing_address, page.first);
    if (!inserted && it->second != page.first) {
      return FailPlan(output, error,
                      "supplied 0x8 and 0x9 pages alias the same backing page");
    }
  }
  std::map<uint32_t, bool> granules;
  for (const auto& page : pages) {
    const uint32_t granule_address = page.first & ~(host_page_size - 1);
    bool& writable = granules[granule_address];
    if (!page.second.code) {
      writable = true;
    }
  }
  for (const auto& [granule_address, writable] : granules) {
    const uint64_t granule_end = uint64_t(granule_address) + host_page_size;
    if (granule_end > kGuestAddressSpaceSize) {
      return FailPlan(output, error,
                      "host protection granule wraps the guest address space");
    }
    for (uint64_t page_address = granule_address; page_address < granule_end;
         page_address += kGuestPageSize) {
      if (!pages.contains(static_cast<uint32_t>(page_address))) {
        return FailPlan(
            output, error,
            "supplied pages do not close a host protection granule");
      }
    }
    plan.protection_granules.push_back(GuestInvocationReplayProtectionGranule{
        granule_address, host_page_size, writable});
  }

  std::map<uint64_t, uint32_t> segment_begins;
  std::map<uint64_t, uint32_t> segment_ends;
  for (size_t i = 0; i < plan.segments.size(); ++i) {
    segment_begins.emplace(plan.segments[i].begin_sequence,
                           static_cast<uint32_t>(i));
    segment_ends.emplace(plan.segments[i].end_sequence,
                         static_cast<uint32_t>(i));
  }
  std::map<uint64_t, uint32_t> intermediate_checkpoints;
  for (size_t i = 1; i + 1 < plan.checkpoints.size(); ++i) {
    intermediate_checkpoints.emplace(plan.checkpoints[i].global_sequence,
                                     static_cast<uint32_t>(i));
  }

  std::vector<ParticipantPhase> phases(plan.participants.size(),
                                       ParticipantPhase::kOutside);
  std::set<uint32_t> open_segments;
  std::set<uint32_t> reset_pages;
  for (size_t i = 0; i < bundle.chunks.size(); ++i) {
    if (manifest.chunks[i].kind != GuestExecutionSessionChunkKind::kEvents) {
      continue;
    }
    GuestExecutionSessionEventChunk chunk;
    if (!GuestExecutionSessionCodec::DecodeEventChunk(bundle.chunks[i], &chunk,
                                                      error)) {
      *output = {};
      return false;
    }
    for (const GuestExecutionSessionEvent& event : chunk.events) {
      if (plan.events.size() >= kGuestExecutionSessionReplayNoIndex) {
        return FailPlan(output, error, "session event count is unsupported");
      }
      const uint32_t event_index = static_cast<uint32_t>(plan.events.size());
      const uint64_t sequence = event.global_sequence;
      GuestExecutionSessionReplayEvent planned;
      planned.event = event;

      const auto ending = segment_ends.find(sequence);
      if (ending != segment_ends.end()) {
        open_segments.erase(ending->second);
      }
      const bool quiescent = open_segments.empty();

      if (event.thread_ordinal == kGuestExecutionSessionNoThread) {
        if (event.disposition !=
            GuestExecutionSessionEventDisposition::kReplayCaptured) {
          return FailPlan(output, error,
                          "coordinator event is not replay-captured");
        }
        switch (event.kind) {
          case GuestExecutionSessionEventKind::kBoundaryRequest:
          case GuestExecutionSessionEventKind::kBoundaryHeld:
            planned.role =
                GuestExecutionSessionReplayEventRole::kBoundaryControl;
            break;
          case GuestExecutionSessionEventKind::kGuestMarker:
            planned.role = GuestExecutionSessionReplayEventRole::kGuestMarker;
            break;
          case GuestExecutionSessionEventKind::kMemoryMutation: {
            if (!quiescent) {
              return FailPlan(
                  output, error,
                  "asynchronous mutation overlaps a running segment");
            }
            if (event.payload_kind !=
                    GuestExecutionSessionPayloadKind::kGuestBytes ||
                event.payload_size != event.byte_count || !event.byte_count ||
                event.guest_address + event.byte_count >
                    kGuestAddressSpaceSize) {
              return FailPlan(output, error,
                              "asynchronous mutation payload is invalid");
            }
            const uint64_t first_page =
                event.guest_address & ~uint64_t(kGuestPageSize - 1);
            const uint64_t end = event.guest_address + event.byte_count;
            for (uint64_t page = first_page; page < end;
                 page += kGuestPageSize) {
              const auto it = pages.find(static_cast<uint32_t>(page));
              if (it == pages.end() || it->second.code) {
                return FailPlan(output, error,
                                "asynchronous mutation targets a page absent "
                                "from the initial checkpoint");
              }
              reset_pages.insert(static_cast<uint32_t>(page));
            }
            planned.role =
                GuestExecutionSessionReplayEventRole::kAsynchronousMutation;
            break;
          }
          default:
            return FailPlan(
                output, error,
                EventKindMessage("coordinator event kind ", event.kind));
        }
        plan.coordinator_event_indices.push_back(event_index);
      } else {
        if (event.thread_ordinal >= plan.participants.size()) {
          return FailPlan(output, error, "event names an unknown participant");
        }
        if (event.kind ==
            GuestExecutionSessionEventKind::kInstructionCoverage) {
          return FailPlan(output, error,
                          "instruction coverage events require guest "
                          "instruction counters that timed replay does not "
                          "provide");
        }
        ParticipantPhase& phase = phases[event.thread_ordinal];
        switch (event.kind) {
          case GuestExecutionSessionEventKind::kOuterHostCallBegin:
            if (phase != ParticipantPhase::kOutside) {
              return FailPlan(output, error,
                              "participant outer host call does not bracket "
                              "exactly one segment");
            }
            phase = ParticipantPhase::kOuterPending;
            planned.role =
                GuestExecutionSessionReplayEventRole::kOuterHostCallBegin;
            break;
          case GuestExecutionSessionEventKind::kSegmentBegin: {
            if (phase == ParticipantPhase::kOutside) {
              phase = ParticipantPhase::kInSegment;
            } else if (phase == ParticipantPhase::kOuterPending) {
              phase = ParticipantPhase::kInSegmentOuter;
            } else {
              return FailPlan(output, error,
                              "participant begins a nested segment");
            }
            // Without recorded scheduling, two open segments would replay an
            // unobserved race and their page checks would read racing memory.
            if (!quiescent) {
              return FailPlan(
                  output, error,
                  "segment begin at sequence " + std::to_string(sequence) +
                      " overlaps an open segment; concurrent segments are "
                      "not replayable without recorded scheduling");
            }
            const auto beginning = segment_begins.find(sequence);
            if (beginning == segment_begins.end() ||
                plan.segments[beginning->second].thread_ordinal !=
                    event.thread_ordinal) {
              return FailPlan(output, error,
                              "segment begin has no matching segment");
            }
            planned.segment_index = beginning->second;
            planned.role = GuestExecutionSessionReplayEventRole::kSegmentBegin;
            break;
          }
          case GuestExecutionSessionEventKind::kSegmentEnd: {
            if (phase == ParticipantPhase::kInSegment) {
              phase = ParticipantPhase::kOutside;
            } else if (phase == ParticipantPhase::kInSegmentOuter) {
              phase = ParticipantPhase::kOuterEndPending;
            } else {
              return FailPlan(output, error,
                              "participant ends a segment it did not begin");
            }
            if (ending == segment_ends.end() ||
                plan.segments[ending->second].thread_ordinal !=
                    event.thread_ordinal) {
              return FailPlan(output, error,
                              "segment end has no matching segment");
            }
            planned.segment_index = ending->second;
            planned.role = GuestExecutionSessionReplayEventRole::kSegmentEnd;
            break;
          }
          case GuestExecutionSessionEventKind::kOuterHostCallEnd:
            if (phase != ParticipantPhase::kOuterEndPending) {
              return FailPlan(output, error,
                              "participant outer host call does not bracket "
                              "exactly one segment");
            }
            phase = ParticipantPhase::kOutside;
            planned.role =
                GuestExecutionSessionReplayEventRole::kOuterHostCallEnd;
            break;
          default:
            return FailPlan(
                output, error,
                EventKindMessage("participant event kind ", event.kind));
        }
        if (event.disposition !=
            GuestExecutionSessionEventDisposition::kValidateDeterministic) {
          return FailPlan(output, error,
                          "participant control event is not deterministic");
        }
        plan.participants[event.thread_ordinal].event_indices.push_back(
            event_index);
      }

      const auto checkpoint = intermediate_checkpoints.find(sequence);
      if (checkpoint != intermediate_checkpoints.end()) {
        if (!quiescent) {
          return FailPlan(
              output, error,
              "intermediate checkpoint is not at a quiescent point");
        }
        planned.checkpoint_index = checkpoint->second;
      }
      const auto beginning = segment_begins.find(sequence);
      if (beginning != segment_begins.end()) {
        open_segments.insert(beginning->second);
      }
      plan.events.push_back(std::move(planned));
    }
  }
  for (ParticipantPhase phase : phases) {
    if (phase != ParticipantPhase::kOutside) {
      return FailPlan(output, error,
                      "participant ends inside a segment or outer host call");
    }
  }
  if (plan.events.size() != manifest.accepted_event_count) {
    return FailPlan(output, error,
                    "planned events do not cover the accepted event count");
  }

  for (const GuestExecutionSessionReplaySegment& segment : plan.segments) {
    for (const ppc::GuestInvocationPage& page :
         segment.invocation.expected_dirty_pages) {
      reset_pages.insert(page.guest_address);
    }
  }
  for (size_t checkpoint_index = 1; checkpoint_index < plan.checkpoints.size();
       ++checkpoint_index) {
    for (const GuestExecutionSessionContentReference& content :
         plan.checkpoints[checkpoint_index].content) {
      if (content.kind != GuestExecutionSessionContentKind::kGuestPage) {
        continue;
      }
      const uint32_t address = static_cast<uint32_t>(content.guest_address);
      const std::vector<uint8_t>* blob = FindBlob(plan.blobs, content.sha256);
      if (!blob || blob->size() != kGuestPageSize) {
        return FailPlan(output, error, "checkpoint page blob is missing");
      }
      if (std::memcmp(blob->data(), pages[address].initial_data,
                      kGuestPageSize)) {
        reset_pages.insert(address);
      }
    }
  }

  plan.pages.reserve(pages.size());
  for (const auto& page : pages) {
    plan.pages.push_back(page.second);
  }
  plan.reset_page_addresses.assign(reset_pages.cbegin(), reset_pages.cend());
  *output = std::move(plan);
  return true;
}

bool BuildGuestExecutionContinuousReplayPlan(
    const GuestExecutionSessionBundle& bundle, uint32_t host_page_size,
    GuestExecutionContinuousReplayPlan* output, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "continuous replay plan output is null");
  }
  *output = {};
  const auto fail = [&](std::string_view message) {
    *output = {};
    return Fail(error, message);
  };
  if (!IsPowerOfTwo(host_page_size) || host_page_size < kGuestPageSize ||
      host_page_size > kMaximumSupportedHostPageSize) {
    return fail("host page size is unsupported for continuous replay");
  }
  if (!ValidateGuestExecutionSessionBundle(bundle, error)) {
    return false;
  }
  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  if (!manifest.segments.empty() || manifest.accepted_segment_count ||
      manifest.rejected_segment_count) {
    return fail("continuous replay requires a zero-segment session");
  }
  if (manifest.rejected_event_count || manifest.unsupported_event_count) {
    return fail("continuous session contains rejected or unsupported events");
  }
  if (manifest.chunks.size() < 7 ||
      manifest.chunks.front().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint ||
      manifest.chunks[1].kind != GuestExecutionSessionChunkKind::kCodeCorpus ||
      manifest.chunks.back().kind !=
          GuestExecutionSessionChunkKind::kCheckpoint) {
    return fail("continuous session chunk closure is missing");
  }

  GuestExecutionContinuousReplayPlan plan;
  plan.host_page_size = host_page_size;
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    if (!plan.blobs.emplace(blob.sha256, &blob.bytes).second) {
      return fail("continuous session content digest is duplicated");
    }
  }

  GuestExecutionSessionCheckpointChunk initial_chunk;
  GuestExecutionSessionCheckpointChunk final_chunk;
  GuestExecutionSessionCodeCorpusChunk corpus_chunk;
  if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
          bundle.chunks.front(), &initial_chunk, error) ||
      !GuestExecutionSessionCodec::DecodeCodeCorpusChunk(
          bundle.chunks[1], &corpus_chunk, error) ||
      !GuestExecutionSessionCodec::DecodeCheckpointChunk(bundle.chunks.back(),
                                                         &final_chunk, error)) {
    *output = {};
    return false;
  }
  const std::vector<uint8_t>* corpus_blob =
      FindBlob(plan.blobs, corpus_chunk.code_corpus_sha256);
  if (!corpus_blob ||
      !ExecutionJitCorpus::Decode(*corpus_blob, &plan.corpus, error)) {
    return fail("continuous session code corpus failed to decode");
  }
  plan.initial_session_checkpoint = std::move(initial_chunk.checkpoint);
  plan.final_session_checkpoint = std::move(final_chunk.checkpoint);

  std::vector<GuestExecutionSessionEvent> canonical_events;
  std::vector<GuestExecutionContinuousEvent> control_events;
  bool saw_start_scheduler_topology = false;
  bool saw_final_scheduler_topology = false;
  for (size_t i = 2; i + 1 < bundle.chunks.size(); ++i) {
    switch (manifest.chunks[i].kind) {
      case GuestExecutionSessionChunkKind::kEvents: {
        GuestExecutionSessionEventChunk chunk;
        if (!GuestExecutionSessionCodec::DecodeEventChunk(bundle.chunks[i],
                                                          &chunk, error)) {
          *output = {};
          return false;
        }
        canonical_events.insert(canonical_events.end(),
                                std::make_move_iterator(chunk.events.begin()),
                                std::make_move_iterator(chunk.events.end()));
        break;
      }
      case GuestExecutionSessionChunkKind::kContinuousEvents: {
        std::vector<GuestExecutionContinuousEvent> chunk;
        if (!GuestExecutionContinuousEventCodec::Decode(bundle.chunks[i],
                                                        &chunk, error)) {
          *output = {};
          return false;
        }
        control_events.insert(control_events.end(),
                              std::make_move_iterator(chunk.begin()),
                              std::make_move_iterator(chunk.end()));
        break;
      }
      case GuestExecutionSessionChunkKind::kSchedulerTopology: {
        GuestExecutionSessionSchedulerTopologyChunk topology;
        if (!GuestExecutionSessionCodec::DecodeSchedulerTopologyChunk(
                bundle.chunks[i], &topology, error)) {
          *output = {};
          return false;
        }
        if (topology.boundary ==
            GuestExecutionSessionSchedulerTopologyBoundary::kStart) {
          if (saw_start_scheduler_topology) {
            return fail("continuous start scheduler topology is duplicated");
          }
          saw_start_scheduler_topology = true;
          plan.initial_scheduler_topology = std::move(topology);
        } else if (topology.boundary ==
                   GuestExecutionSessionSchedulerTopologyBoundary::kFinal) {
          if (saw_final_scheduler_topology) {
            return fail("continuous final scheduler topology is duplicated");
          }
          saw_final_scheduler_topology = true;
          plan.final_scheduler_topology = std::move(topology);
        } else {
          return fail("continuous scheduler topology boundary is unknown");
        }
        break;
      }
      default:
        return fail("continuous session contains a misplaced chunk");
    }
  }
  if (!saw_start_scheduler_topology || !saw_final_scheduler_topology) {
    return fail("continuous scheduler topology closure is missing");
  }
  if (canonical_events.size() != control_events.size() ||
      canonical_events.size() != manifest.accepted_event_count) {
    return fail("continuous control overlay does not cover the canonical tape");
  }
  plan.events.reserve(canonical_events.size());
  for (size_t i = 0; i < canonical_events.size(); ++i) {
    if (canonical_events[i].global_sequence !=
            control_events[i].global_sequence ||
        canonical_events[i].kind != control_events[i].kind) {
      return fail("continuous control overlay differs from the canonical tape");
    }
    GuestExecutionContinuousReplayEvent event;
    event.canonical = std::move(canonical_events[i]);
    event.control = std::move(control_events[i]);
    if (event.canonical.payload_size) {
      event.payload = FindBlob(plan.blobs, event.canonical.payload_sha256);
      if (!event.payload ||
          event.payload->size() != event.canonical.payload_size) {
        return fail("continuous event payload blob is missing");
      }
    }
    plan.events.push_back(std::move(event));
  }

  if (plan.initial_session_checkpoint.thread_states.size() !=
          manifest.participants.size() ||
      plan.final_session_checkpoint.thread_states.size() !=
          manifest.participants.size()) {
    return fail("continuous checkpoints do not cover every participant");
  }
  std::vector<const GuestExecutionContinuousReplayEvent*> final_routes(
      manifest.participants.size());
  for (const GuestExecutionContinuousReplayEvent& event : plan.events) {
    if (event.control.checkpoint.kind ==
        GuestExecutionContinuousCheckpointReferenceKind::kNone) {
      continue;
    }
    const uint32_t ordinal = event.control.subject.participant_ordinal;
    if (ordinal >= manifest.participants.size()) {
      return fail("continuous checkpoint route names an unknown participant");
    }
    if (plan.final_scheduler_topology.participants[ordinal].state ==
        GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned) {
      return fail(
          "continuous scheduler-unowned participant has a checkpoint route");
    }
    const uint64_t checkpoint_sequence =
        event.control.checkpoint.checkpoint_global_sequence;
    const GuestExecutionSessionCheckpoint* checkpoint = nullptr;
    if (!checkpoint_sequence) {
      checkpoint = &plan.initial_session_checkpoint;
    } else if (checkpoint_sequence == manifest.last_event_sequence) {
      checkpoint = &plan.final_session_checkpoint;
      if (final_routes[ordinal]) {
        return fail("continuous final checkpoint route is duplicated");
      }
      final_routes[ordinal] = &event;
    } else {
      return fail("continuous event refers to an unavailable checkpoint");
    }
    const GuestExecutionSessionThreadStateReference& state =
        checkpoint->thread_states[ordinal];
    const std::vector<uint8_t>* state_blob = FindBlob(plan.blobs, state.sha256);
    ppc::GuestPPCThreadCheckpoint decoded;
    if (!state_blob || state_blob->size() != state.byte_size ||
        event.control.checkpoint.state_size != state.byte_size ||
        event.control.checkpoint.state_sha256 != state.sha256 ||
        !GuestExecutionContinuousEventCodec::DecodeAndValidateCheckpoint(
            event.control, *state_blob, event.control.checkpoint.binding,
            &decoded, error)) {
      if (error && error->empty()) {
        error->assign("continuous checkpoint route blob is invalid");
      }
      *output = {};
      return false;
    }
  }

  std::map<uint32_t, GuestExecutionContinuousReplayResumeEntry> resume_entries;
  plan.participants.reserve(manifest.participants.size());
  for (size_t i = 0; i < manifest.participants.size(); ++i) {
    const GuestExecutionSessionParticipant& participant =
        manifest.participants[i];
    const bool initial_scheduler_unowned =
        plan.initial_scheduler_topology.participants[i].state ==
        GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned;
    const bool final_scheduler_unowned =
        plan.final_scheduler_topology.participants[i].state ==
        GuestExecutionSessionSchedulerParticipantState::kSchedulerUnowned;
    if (initial_scheduler_unowned != final_scheduler_unowned) {
      return fail(
          "continuous participant scheduler ownership changes without a "
          "typed entry route");
    }
    const GuestExecutionSessionThreadStateReference& initial_state =
        plan.initial_session_checkpoint.thread_states[i];
    const GuestExecutionSessionThreadStateReference& final_state =
        plan.final_session_checkpoint.thread_states[i];
    const std::vector<uint8_t>* initial_blob =
        FindBlob(plan.blobs, initial_state.sha256);
    const std::vector<uint8_t>* final_blob =
        FindBlob(plan.blobs, final_state.sha256);
    GuestExecutionContinuousReplayParticipant planned;
    planned.ordinal = participant.ordinal;
    planned.guest_thread_id = participant.guest_thread_id;
    planned.initial_outer_call_state = participant.initial_outer_call_state;
    planned.boundary_arrival_kind = participant.boundary_arrival_kind;
    planned.held_after_event_sequence = participant.held_after_event_sequence;
    if (!initial_blob || initial_blob->size() != initial_state.byte_size ||
        !final_blob || final_blob->size() != final_state.byte_size ||
        !ppc::GuestPPCThreadCheckpointCodec::Decode(
            *initial_blob, &planned.initial_checkpoint, error) ||
        !ppc::GuestPPCThreadCheckpointCodec::Decode(
            *final_blob, &planned.final_checkpoint, error)) {
      if (error && error->empty()) {
        error->assign("continuous participant checkpoint blob is invalid");
      }
      *output = {};
      return false;
    }
    auto validate_route = [&](const ppc::GuestPPCThreadCheckpoint& route,
                              std::string_view boundary) {
      if (route.participant_ordinal != participant.ordinal ||
          route.guest_thread_id != participant.guest_thread_id) {
        return Fail(error, std::string("continuous ") + std::string(boundary) +
                               " checkpoint participant differs");
      }
      if (route.resume_kind == ppc::GuestPPCThreadResumeKind::kOutsideGuest) {
        return true;
      }
      const ExecutionJitCorpus::FunctionRecord* owner =
          plan.corpus.FindFunction(route.owning_function_address);
      if (!owner || owner->end_address != route.owning_function_end_address) {
        return Fail(error, std::string("continuous ") + std::string(boundary) +
                               " checkpoint owner differs from the corpus");
      }
      const ExecutionJitCorpus::FunctionRecord* resume_entry =
          plan.corpus.FindFunction(route.resume_pc);
      if (resume_entry && resume_entry->address != owner->address) {
        return Fail(error, std::string("continuous ") + std::string(boundary) +
                               " checkpoint resume entry is ambiguous");
      }
      return true;
    };
    if (!validate_route(planned.initial_checkpoint, "initial") ||
        !validate_route(planned.final_checkpoint, "final")) {
      *output = {};
      return false;
    }
    const bool initial_outside = planned.initial_checkpoint.resume_kind ==
                                 ppc::GuestPPCThreadResumeKind::kOutsideGuest;
    const bool final_outside = planned.final_checkpoint.resume_kind ==
                               ppc::GuestPPCThreadResumeKind::kOutsideGuest;
    if (initial_outside != final_outside) {
      return fail(
          "continuous participant continuation class changes without a "
          "typed entry route");
    }
    if (initial_outside) {
      const auto is_passive_resume = [](const auto& topology) {
        return !topology.restorable && !topology.guest_pc &&
               (topology.resume_kind ==
                    GuestExecutionSessionSchedulerResumeKind::
                        kNativeContinuation ||
                topology.resume_kind ==
                    GuestExecutionSessionSchedulerResumeKind::kNotYetRun);
      };
      const auto& initial_topology =
          plan.initial_scheduler_topology.participants[i];
      const auto& final_topology =
          plan.final_scheduler_topology.participants[i];
      const bool has_guest_execution_event = std::any_of(
          plan.events.cbegin(), plan.events.cend(), [&](const auto& event) {
            return event.canonical.thread_ordinal == participant.ordinal &&
                   event.canonical.kind !=
                       GuestExecutionSessionEventKind::kThreadDispatch &&
                   event.canonical.kind !=
                       GuestExecutionSessionEventKind::kSynchronization;
          });
      if (final_routes[i] || initial_state != final_state ||
          planned.initial_checkpoint != planned.final_checkpoint ||
          participant.initial_outer_call_state !=
              GuestExecutionSessionInitialOuterCallState::kOutside ||
          participant.boundary_arrival_kind !=
              GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside ||
          (!initial_scheduler_unowned &&
           (!is_passive_resume(initial_topology) ||
            !is_passive_resume(final_topology))) ||
          has_guest_execution_event) {
        return fail(
            "continuous outside-guest participant is not passive and "
            "byte-stable");
      }
      const char* topology_difference =
          GuestExecutionSessionSchedulerTopologyFirstDifference(
              initial_topology, final_topology);
      if (topology_difference) {
        return fail(std::string("continuous outside-guest participant "
                                "scheduler topology changes between "
                                "boundaries: ") +
                    topology_difference);
      }
    } else {
      if (final_scheduler_unowned || !final_routes[i]) {
        return fail("continuous executable participant route is missing");
      }
      const ppc::GuestPPCThreadCheckpointBinding& final_binding =
          final_routes[i]->control.checkpoint.binding;
      if (!ppc::GuestPPCThreadCheckpointCodec::ValidateBinding(
              planned.final_checkpoint, final_binding, error)) {
        *output = {};
        return false;
      }
    }
    if (!initial_outside &&
        planned.initial_checkpoint.resume_pc !=
            planned.initial_checkpoint.owning_function_address) {
      const GuestExecutionContinuousReplayResumeEntry resume = {
          planned.initial_checkpoint.resume_pc,
          planned.initial_checkpoint.owning_function_address,
          planned.initial_checkpoint.owning_function_end_address};
      const auto [it, inserted] =
          resume_entries.emplace(resume.resume_pc, resume);
      if (!inserted && it->second != resume) {
        return fail("continuous initial resume entry is ambiguous");
      }
    }
    plan.participants.push_back(std::move(planned));
  }
  for (const auto& entry : resume_entries) {
    plan.resume_entries.push_back(entry.second);
  }

  std::map<uint32_t, GuestExecutionSessionReplayPage> pages;
  for (uint32_t address : plan.corpus.page_addresses()) {
    pages.emplace(address,
                  GuestExecutionSessionReplayPage{
                      address, true, plan.corpus.FindPageData(address)});
  }
  for (const GuestExecutionSessionContentReference& content :
       plan.initial_session_checkpoint.content) {
    const std::vector<uint8_t>* blob = FindBlob(plan.blobs, content.sha256);
    if (content.kind == GuestExecutionSessionContentKind::kGuestCode) {
      const uint8_t* corpus_bytes = CorpusCodeRange(
          plan.corpus, content.guest_address, content.byte_size);
      if (!blob || !corpus_bytes || blob->size() != content.byte_size ||
          std::memcmp(blob->data(), corpus_bytes, blob->size())) {
        return fail("continuous checkpoint code differs from the corpus");
      }
      continue;
    }
    if (content.guest_address > UINT32_MAX ||
        content.byte_size != kGuestPageSize) {
      return fail("continuous initial checkpoint page is invalid");
    }
    const uint32_t address = static_cast<uint32_t>(content.guest_address);
    if (!IsSupportedDataPageAddress(address) || !blob ||
        blob->size() != kGuestPageSize || pages.contains(address)) {
      return fail("continuous initial checkpoint page closure is invalid");
    }
    pages.emplace(
        address, GuestExecutionSessionReplayPage{address, false, blob->data()});
  }
  for (const GuestExecutionSessionContentReference& content :
       plan.final_session_checkpoint.content) {
    const std::vector<uint8_t>* blob = FindBlob(plan.blobs, content.sha256);
    if (!blob || blob->size() != content.byte_size) {
      return fail("continuous final checkpoint content blob is missing");
    }
    if (content.kind == GuestExecutionSessionContentKind::kGuestCode) {
      const uint8_t* corpus_bytes = CorpusCodeRange(
          plan.corpus, content.guest_address, content.byte_size);
      if (!corpus_bytes ||
          std::memcmp(blob->data(), corpus_bytes, blob->size())) {
        return fail("continuous final checkpoint code differs from the corpus");
      }
      continue;
    }
    if (content.guest_address > UINT32_MAX ||
        !pages.contains(static_cast<uint32_t>(content.guest_address)) ||
        pages.at(static_cast<uint32_t>(content.guest_address)).code) {
      return fail(
          "continuous final checkpoint page was not captured initially");
    }
  }
  for (const GuestExecutionContinuousReplayEvent& event : plan.events) {
    if (event.canonical.kind !=
        GuestExecutionSessionEventKind::kMemoryMutation) {
      continue;
    }
    const uint64_t end =
        event.canonical.guest_address + event.canonical.byte_count;
    for (uint64_t address =
             event.canonical.guest_address & ~uint64_t(kGuestPageSize - 1);
         address < end; address += kGuestPageSize) {
      const auto page = pages.find(static_cast<uint32_t>(address));
      if (address > UINT32_MAX || page == pages.end() || page->second.code) {
        return fail("continuous memory mutation is outside captured data");
      }
    }
  }

  std::map<uint32_t, uint32_t> xex_backing_owners;
  std::map<uint32_t, bool> granules;
  for (const auto& [address, page] : pages) {
    const uint32_t backing_address = XexBackingPageAddress(address);
    const auto [owner, inserted] =
        xex_backing_owners.emplace(backing_address, address);
    if (!inserted && owner->second != address) {
      return fail("continuous supplied pages alias one XEX backing page");
    }
    bool& writable = granules[address & ~(host_page_size - 1)];
    writable |= !page.code;
    if (!page.code) {
      plan.reset_page_addresses.push_back(address);
    }
  }
  for (const auto& [granule_address, writable] : granules) {
    const uint64_t granule_end = uint64_t(granule_address) + host_page_size;
    if (granule_end > kGuestAddressSpaceSize) {
      return fail("continuous host protection granule wraps guest memory");
    }
    for (uint64_t address = granule_address; address < granule_end;
         address += kGuestPageSize) {
      if (!pages.contains(static_cast<uint32_t>(address))) {
        return fail("continuous pages do not close a host protection granule");
      }
    }
    plan.protection_granules.push_back(GuestInvocationReplayProtectionGranule{
        granule_address, host_page_size, writable});
  }
  plan.pages.reserve(pages.size());
  for (const auto& [address, page] : pages) {
    plan.pages.push_back(page);
  }
  *output = std::move(plan);
  return true;
}

// One persistent host worker owning one participant's real ThreadState. The
// coordinator hands over commands under the mutex and reads results after the
// worker reports completion.
struct GuestExecutionSessionRunner::Worker {
  uint32_t ordinal = 0;
  std::unique_ptr<ThreadState> thread_state;
  std::unique_ptr<xe::threading::Thread> thread;
  std::mutex mutex;
  std::condition_variable condition;
  WorkerCommand command = WorkerCommand::kIdle;
  bool completed = false;
  bool succeeded = false;
  std::string error;
  uint64_t restore_cpu_nanoseconds = 0;
  GuestExecutionSessionReplayParticipantMetrics metrics;
};

std::unique_ptr<GuestExecutionSessionRunner>
GuestExecutionSessionRunner::Create(const GuestExecutionSessionBundle& bundle,
                                    std::unique_ptr<backend::Backend> backend,
                                    Options options, std::string* error) {
  if (error) {
    error->clear();
  }
#if !XE_PLATFORM_MAC || !XE_ARCH_ARM64
  Fail(error, "guest execution session replay runner requires Apple A64");
  return nullptr;
#endif  // !XE_PLATFORM_MAC || !XE_ARCH_ARM64
  if (!backend) {
    Fail(error, "session replay requires a backend");
    return nullptr;
  }
  if (options.event_timeout <= std::chrono::milliseconds::zero()) {
    Fail(error, "session replay event timeout must be positive");
    return nullptr;
  }
  if (!options.minimum_participant_cpu_nanoseconds_per_repetition) {
    Fail(error, "session replay requires a positive participant CPU floor");
    return nullptr;
  }

  const size_t native_page_size = xe::memory::page_size();
  if (native_page_size > std::numeric_limits<uint32_t>::max()) {
    Fail(error, "native host page size does not fit the replay format");
    return nullptr;
  }
  GuestExecutionSessionReplayPlan plan;
  if (!BuildGuestExecutionSessionReplayPlan(
          bundle, static_cast<uint32_t>(native_page_size), &plan, error)) {
    return nullptr;
  }

  std::unique_ptr<GuestExecutionSessionRunner> runner(
      new GuestExecutionSessionRunner(bundle, std::move(plan), options));
  if (!runner->Initialize(std::move(backend), error)) {
    return nullptr;
  }
  return std::move(runner);
}

GuestExecutionSessionRunner::GuestExecutionSessionRunner(
    const GuestExecutionSessionBundle& bundle,
    GuestExecutionSessionReplayPlan plan, Options options)
    : bundle_(&bundle), plan_(std::move(plan)), options_(options) {}

GuestExecutionSessionRunner::~GuestExecutionSessionRunner() {
  StopWorkers();
  workers_.clear();
}

bool GuestExecutionSessionRunner::Initialize(
    std::unique_ptr<backend::Backend> backend, std::string* error) {
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    return Fail(error, "guest memory initialization failed");
  }

  processor_ = std::make_unique<Processor>(memory_.get(), nullptr);
  if (!processor_->Setup(std::move(backend))) {
    return Fail(error, "guest processor initialization failed");
  }
  if (!CommitAndLoadPages(error) || !CloseAndReopenGuestViews(error) ||
      !PrepareResetPageCopies(error)) {
    return false;
  }

  std::string module_error;
  std::unique_ptr<ExactJitCorpusModule> module = ExactJitCorpusModule::Create(
      processor_.get(), plan_.corpus, "guest_execution_session_replay",
      &module_error);
  if (!module) {
    if (error) {
      error->assign(module_error.empty() ? "exact corpus module creation failed"
                                         : module_error);
    }
    return false;
  }
  module_ = module.get();
  if (!processor_->AddModule(std::move(module))) {
    module_ = nullptr;
    return Fail(error, "exact corpus module registration failed");
  }

  workers_.reserve(plan_.participants.size());
  for (size_t i = 0; i < plan_.participants.size(); ++i) {
    auto worker = std::make_unique<Worker>();
    worker->ordinal = static_cast<uint32_t>(i);
    worker->thread_state = std::make_unique<ThreadState>(
        processor_.get(), plan_.participants[i].guest_thread_id, 0, 0);
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
    if (worker->thread_state->PublishGuestExecutionCaptureReady() !=
        GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept) {
      return Fail(error, "capture ThreadState publication failed");
    }
#endif
    workers_.push_back(std::move(worker));
  }
  return StartWorkers(error);
}

bool GuestExecutionSessionRunner::CommitAndLoadPages(std::string* error) {
  struct NativeHeapPage {
    BaseHeap* heap = nullptr;
    uint32_t address = 0;
    uint32_t size = 0;
  };
  std::map<std::pair<uint32_t, uint32_t>, NativeHeapPage> native_pages;

  for (const GuestExecutionSessionReplayPage& page : plan_.pages) {
    BaseHeap* heap = memory_->LookupHeap(page.guest_address);
    if (!heap || page.guest_address < heap->heap_base()) {
      return Fail(error, "supplied page has no ordinary virtual/XEX heap");
    }
    const uint32_t heap_page_size = heap->page_size();
    const uint32_t heap_page_address =
        heap->heap_base() +
        ((page.guest_address - heap->heap_base()) / heap_page_size) *
            heap_page_size;
    native_pages.emplace(
        std::make_pair(heap->heap_base(), heap_page_address),
        NativeHeapPage{heap, heap_page_address, heap_page_size});
  }

  for (const auto& native_page_entry : native_pages) {
    const NativeHeapPage& native_page = native_page_entry.second;
    if (!native_page.heap->AllocFixed(
            native_page.address, native_page.size, 0,
            kMemoryAllocationReserve | kMemoryAllocationCommit,
            kMemoryProtectRead | kMemoryProtectWrite)) {
      return Fail(error, "heap-native replay page commit failed");
    }
  }

  for (const GuestExecutionSessionReplayPage& page : plan_.pages) {
    if (!page.initial_data) {
      return Fail(error, "planned replay page has no initial contents");
    }
    std::memcpy(memory_->TranslateVirtual(page.guest_address),
                page.initial_data, kGuestPageSize);
  }
  return true;
}

bool GuestExecutionSessionRunner::CloseAndReopenGuestViews(std::string* error) {
  if (!xe::memory::Protect(memory_->virtual_membase(), 0x100000000ull,
                           xe::memory::PageAccess::kNoAccess)) {
    return Fail(error, "failed to close the full guest virtual view");
  }
  if (!xe::memory::Protect(memory_->physical_membase(), 0x20000000ull,
                           xe::memory::PageAccess::kNoAccess)) {
    return Fail(error, "failed to close the full raw physical view");
  }

  for (const GuestInvocationReplayProtectionGranule& granule :
       plan_.protection_granules) {
    const xe::memory::PageAccess access =
        granule.writable ? xe::memory::PageAccess::kReadWrite
                         : xe::memory::PageAccess::kReadOnly;
    if (!xe::memory::Protect(memory_->TranslateVirtual(granule.guest_address),
                             granule.size, access)) {
      return Fail(error, "failed to reopen a supplied protection granule");
    }
  }
  return true;
}

bool GuestExecutionSessionRunner::PrepareResetPageCopies(std::string* error) {
  reset_page_copies_.clear();
  reset_page_copies_.reserve(plan_.reset_page_addresses.size());
  for (uint32_t address : plan_.reset_page_addresses) {
    const GuestExecutionSessionReplayPage* page = FindPage(address);
    if (!page || page->code || !page->initial_data) {
      reset_page_copies_.clear();
      return Fail(error, "reset page has no initial contents");
    }
    reset_page_copies_.push_back(
        ResetPageCopy{memory_->TranslateVirtual(address), page->initial_data});
  }
  return true;
}

const GuestExecutionSessionReplayPage* GuestExecutionSessionRunner::FindPage(
    uint32_t guest_address) const {
  const auto it = std::lower_bound(
      plan_.pages.cbegin(), plan_.pages.cend(), guest_address,
      [](const GuestExecutionSessionReplayPage& page, uint32_t address) {
        return page.guest_address < address;
      });
  return it != plan_.pages.cend() && it->guest_address == guest_address
             ? &*it
             : nullptr;
}

bool GuestExecutionSessionRunner::StartWorkers(std::string* error) {
  for (std::unique_ptr<Worker>& worker : workers_) {
    Worker* raw_worker = worker.get();
    xe::threading::Thread::CreationParameters params;
    params.stack_size = kWorkerStackSize;
    worker->thread = xe::threading::Thread::Create(
        params, [this, raw_worker]() { WorkerMain(raw_worker); });
    if (!worker->thread) {
      return Fail(error, "failed to create a persistent replay worker");
    }
  }
  return true;
}

void GuestExecutionSessionRunner::StopWorkers() {
  if (tape_) {
    tape_->Cancel("session replay runner is shutting down");
  }
  for (std::unique_ptr<Worker>& worker : workers_) {
    if (!worker->thread) {
      continue;
    }
    {
      // An outstanding command finishes first so its idle transition cannot
      // overwrite the exit request.
      std::unique_lock<std::mutex> lock(worker->mutex);
      worker->condition.wait(
          lock, [&]() { return worker->command == WorkerCommand::kIdle; });
      worker->command = WorkerCommand::kExit;
    }
    worker->condition.notify_all();
    xe::threading::Wait(worker->thread.get(), false);
    worker->thread.reset();
  }
}

void GuestExecutionSessionRunner::DispatchWorkers(WorkerCommand command) {
  for (std::unique_ptr<Worker>& worker : workers_) {
    {
      std::lock_guard<std::mutex> lock(worker->mutex);
      worker->command = command;
      worker->completed = false;
      worker->succeeded = false;
      worker->error.clear();
    }
    worker->condition.notify_all();
  }
}

bool GuestExecutionSessionRunner::WaitForWorkers(std::string* error) {
  bool all_succeeded = true;
  std::string first_error;
  for (std::unique_ptr<Worker>& worker : workers_) {
    std::unique_lock<std::mutex> lock(worker->mutex);
    worker->condition.wait(lock, [&]() { return worker->completed; });
    if (!worker->succeeded && all_succeeded) {
      all_succeeded = false;
      first_error =
          worker->error.empty() ? "replay worker failed" : worker->error;
    }
  }
  if (!all_succeeded) {
    return Fail(error, first_error);
  }
  return true;
}

void GuestExecutionSessionRunner::WorkerMain(Worker* worker) {
  ThreadState::Bind(worker->thread_state.get());
  for (;;) {
    WorkerCommand command;
    {
      std::unique_lock<std::mutex> lock(worker->mutex);
      worker->condition.wait(
          lock, [&]() { return worker->command != WorkerCommand::kIdle; });
      command = worker->command;
    }
    if (command == WorkerCommand::kExit) {
      break;
    }
    std::string error;
    const bool succeeded = command == WorkerCommand::kRestore
                               ? RestoreParticipant(worker, &error)
                               : RunParticipant(worker, &error);
    {
      std::lock_guard<std::mutex> lock(worker->mutex);
      worker->command = WorkerCommand::kIdle;
      worker->completed = true;
      worker->succeeded = succeeded;
      worker->error = error;
    }
    worker->condition.notify_all();
  }
  ThreadState::Bind(nullptr);
}

bool GuestExecutionSessionRunner::RestoreParticipant(Worker* worker,
                                                     std::string* error) {
  const GuestExecutionSessionReplayParticipant& participant =
      plan_.participants[worker->ordinal];
  worker->restore_cpu_nanoseconds = 0;
  ThreadCpuInterval interval;
  if (!interval.Begin(error)) {
    return false;
  }
  // Host floating-point control state is per thread, so the backend reset must
  // run on the worker that will execute the participant.
  ppc::RestoreGuestPPCRegisterState(participant.initial_state,
                                    worker->thread_state->context());
  if (!processor_->backend()->ResetGuestInvocationReplayState(
          worker->thread_state->context())) {
    return Fail(error, "backend rejected deterministic participant reset");
  }
  // The live context must reproduce the checkpoint blob byte for byte, not
  // merely the decoded struct.
  std::vector<uint8_t> encoded;
  if (!ppc::GuestPPCRegisterStateCodec::Encode(
          ppc::CaptureGuestPPCRegisterState(*worker->thread_state->context()),
          &encoded, error)) {
    return false;
  }
  if (!participant.initial_state_blob ||
      encoded != *participant.initial_state_blob) {
    return Fail(error,
                "restored participant state does not reproduce its "
                "checkpoint blob");
  }
  if (!interval.End(error)) {
    return false;
  }
  worker->restore_cpu_nanoseconds = interval.cpu_nanoseconds();
  return true;
}

bool GuestExecutionSessionRunner::RunParticipant(Worker* worker,
                                                 std::string* error) {
  const GuestExecutionSessionReplayParticipant& participant =
      plan_.participants[worker->ordinal];
  worker->metrics = {};
  ThreadCpuInterval interval;
  std::string begin_error;
  if (!interval.Begin(&begin_error)) {
    tape_->Cancel(begin_error);
    return Fail(error, begin_error);
  }
  bool succeeded = true;
  for (uint32_t event_index : participant.event_indices) {
    if (!ReplayParticipantEvent(worker, plan_.events[event_index], error)) {
      succeeded = false;
      break;
    }
    ++worker->metrics.consumed_event_count;
  }
  std::string interval_error;
  const bool interval_succeeded = interval.End(&interval_error);
  if (!succeeded) {
    return false;
  }
  if (!interval_succeeded) {
    tape_->Cancel(interval_error);
    return Fail(error, interval_error);
  }
  worker->metrics.thread_cpu_nanoseconds = interval.cpu_nanoseconds();
  worker->metrics.uptime_raw_nanoseconds = interval.uptime_raw_nanoseconds();
  return true;
}

bool GuestExecutionSessionRunner::ReplayParticipantEvent(
    Worker* worker, const GuestExecutionSessionReplayEvent& planned,
    std::string* error) {
  GuestExecutionReplayTurn turn;
  const GuestExecutionReplayAcquireResult result =
      tape_->Acquire(worker->ordinal, options_.event_timeout, &turn, error);
  if (result == GuestExecutionReplayAcquireResult::kComplete) {
    return Fail(error,
                "replay tape completed before a participant consumed its "
                "recorded events");
  }
  if (result != GuestExecutionReplayAcquireResult::kAcquired) {
    return false;
  }
  if (turn.event != planned.event) {
    return AbandonTurn(tape_.get(), turn, error,
                       "replay tape cursor differs from the planned event");
  }
  std::string message;
  if (full_verification_ &&
      planned.checkpoint_index != kGuestExecutionSessionReplayNoIndex &&
      !RunInIntervalVerification(
          [&](std::string* verify_error) {
            return VerifyCheckpoint(plan_.checkpoints[planned.checkpoint_index],
                                    verify_error);
          },
          &worker->metrics.in_interval_verification_thread_cpu_nanoseconds,
          &worker->metrics.in_interval_verification_count, &message)) {
    return AbandonTurn(tape_.get(), turn, error, message);
  }

  // Control events carry no auxiliary fields, so the commit is the tape
  // record itself; ordering and the state comparisons are what execution
  // contributes.
  switch (planned.role) {
    case GuestExecutionSessionReplayEventRole::kOuterHostCallBegin:
    case GuestExecutionSessionReplayEventRole::kOuterHostCallEnd:
      return tape_->CommitDeterministic(turn, turn.event, error);
    case GuestExecutionSessionReplayEventRole::kSegmentBegin: {
      const GuestExecutionSessionReplaySegment& segment =
          plan_.segments[planned.segment_index];
      if (full_verification_ &&
          !RunInIntervalVerification(
              [&](std::string* verify_error) {
                return VerifySegmentEntry(*worker, segment, verify_error);
              },
              &worker->metrics.in_interval_verification_thread_cpu_nanoseconds,
              &worker->metrics.in_interval_verification_count, &message)) {
        return AbandonTurn(tape_.get(), turn, error, message);
      }
      if (!tape_->CommitDeterministic(turn, turn.event, error)) {
        return false;
      }
      Function* function = segment_functions_[planned.segment_index];
      if (!function ||
          !function->Call(worker->thread_state.get(),
                          segment.invocation.expected_return_address)) {
        message = "segment root has no executable code";
        tape_->Cancel(message);
        return Fail(error, message);
      }
      return true;
    }
    case GuestExecutionSessionReplayEventRole::kSegmentEnd: {
      const GuestExecutionSessionReplaySegment& segment =
          plan_.segments[planned.segment_index];
      if (full_verification_ &&
          !RunInIntervalVerification(
              [&](std::string* verify_error) {
                return VerifySegmentExit(*worker, segment, verify_error);
              },
              &worker->metrics.in_interval_verification_thread_cpu_nanoseconds,
              &worker->metrics.in_interval_verification_count, &message)) {
        return AbandonTurn(tape_.get(), turn, error, message);
      }
      return tape_->CommitDeterministic(turn, turn.event, error);
    }
    default:
      return AbandonTurn(tape_.get(), turn, error,
                         "participant leased a coordinator event");
  }
}

bool GuestExecutionSessionRunner::VerifyParticipantState(
    uint32_t ordinal, const ppc::GuestPPCRegisterState& expected,
    std::string_view message, std::string* error) const {
  if (ppc::CaptureGuestPPCRegisterState(
          *workers_[ordinal]->thread_state->context()) != expected) {
    return Fail(error, message);
  }
  return true;
}

bool GuestExecutionSessionRunner::VerifySegmentEntry(
    const Worker& worker, const GuestExecutionSessionReplaySegment& segment,
    std::string* error) const {
  if (!VerifyParticipantState(
          worker.ordinal, segment.invocation.input,
          "segment entry state differs from the captured input", error)) {
    return false;
  }
  for (const ppc::GuestInvocationPage& page :
       segment.invocation.input_data_pages) {
    if (std::memcmp(memory_->TranslateVirtual(page.guest_address),
                    page.data.data(), page.data.size())) {
      return Fail(error, "segment input page differs from live guest memory");
    }
  }
  return true;
}

bool GuestExecutionSessionRunner::VerifySegmentExit(
    const Worker& worker, const GuestExecutionSessionReplaySegment& segment,
    std::string* error) const {
  if (!VerifyParticipantState(
          worker.ordinal, segment.invocation.expected_output,
          "segment exit state differs from the captured output", error)) {
    return false;
  }
  for (const ppc::GuestInvocationPage& input_page :
       segment.invocation.input_data_pages) {
    const ppc::GuestInvocationPage* dirty_page = FindInvocationPage(
        segment.invocation.expected_dirty_pages, input_page.guest_address);
    const auto& expected_data = dirty_page ? dirty_page->data : input_page.data;
    if (std::memcmp(memory_->TranslateVirtual(input_page.guest_address),
                    expected_data.data(), expected_data.size())) {
      return Fail(error,
                  "segment output page differs from the captured memory");
    }
  }
  return true;
}

bool GuestExecutionSessionRunner::VerifyCheckpoint(
    const GuestExecutionSessionCheckpoint& checkpoint,
    std::string* error) const {
  for (const GuestExecutionSessionThreadStateReference& reference :
       checkpoint.thread_states) {
    if (reference.thread_ordinal >= workers_.size()) {
      return Fail(error, "checkpoint names an unknown participant");
    }
    std::vector<uint8_t> encoded;
    if (!ppc::GuestPPCRegisterStateCodec::Encode(
            ppc::CaptureGuestPPCRegisterState(
                *workers_[reference.thread_ordinal]->thread_state->context()),
            &encoded, error)) {
      return false;
    }
    if (encoded.size() != reference.byte_size ||
        GuestExecutionSessionCodec::HashBytes(encoded) != reference.sha256) {
      return Fail(error,
                  "checkpoint participant state differs from replayed state");
    }
  }
  for (const GuestExecutionSessionContentReference& content :
       checkpoint.content) {
    const std::vector<uint8_t>* blob = FindBlob(plan_.blobs, content.sha256);
    if (!blob || blob->size() != content.byte_size ||
        content.guest_address + content.byte_size > kGuestAddressSpaceSize) {
      return Fail(error, "checkpoint content blob is missing");
    }
    const uint32_t address = static_cast<uint32_t>(content.guest_address);
    if (content.kind == GuestExecutionSessionContentKind::kGuestPage) {
      const GuestExecutionSessionReplayPage* page = FindPage(address);
      if (!page || page->code) {
        return Fail(error, "checkpoint page is not a supplied data page");
      }
    }
    if (std::memcmp(memory_->TranslateVirtual(address), blob->data(),
                    blob->size())) {
      return Fail(error,
                  content.kind == GuestExecutionSessionContentKind::kGuestPage
                      ? "checkpoint page differs from live guest memory"
                      : "checkpoint code differs from live guest code");
    }
  }
  return true;
}

bool GuestExecutionSessionRunner::VerifyFinalState(std::string* error) const {
  const GuestExecutionSessionCheckpoint& final_checkpoint =
      plan_.checkpoints.back();
  if (final_checkpoint.thread_states.size() != workers_.size()) {
    return Fail(error, "final checkpoint does not cover every participant");
  }
  if (!VerifyCheckpoint(final_checkpoint, error)) {
    return false;
  }
  // Pages the final checkpoint leaves unreferenced must be unchanged, so an
  // unrecorded dirty page rejects rather than passing silently.
  std::set<uint32_t> referenced_pages;
  for (const GuestExecutionSessionContentReference& content :
       final_checkpoint.content) {
    if (content.kind == GuestExecutionSessionContentKind::kGuestPage) {
      referenced_pages.insert(static_cast<uint32_t>(content.guest_address));
    }
  }
  for (const GuestExecutionSessionReplayPage& page : plan_.pages) {
    if (!page.code && referenced_pages.contains(page.guest_address)) {
      continue;
    }
    if (std::memcmp(memory_->TranslateVirtual(page.guest_address),
                    page.initial_data, kGuestPageSize)) {
      return Fail(error, page.code
                             ? "session replay modified exact corpus code bytes"
                             : "session replay dirtied a page the final "
                               "checkpoint does not record");
    }
  }
  for (size_t i = 0; i < segment_functions_.size(); ++i) {
    if (!segment_functions_[i] ||
        !module_->HasExactExtent(*segment_functions_[i])) {
      return Fail(error, "segment root lost its captured exact extent");
    }
  }
  return true;
}

bool GuestExecutionSessionRunner::ResolveFunctionsInCaptureOrder(
    std::string* error) {
  resolved_functions_.clear();
  segment_functions_.assign(plan_.segments.size(), nullptr);
  std::map<uint32_t, Function*> functions_by_address;
  resolved_functions_.reserve(plan_.corpus.function_definition_order().size());
  for (uint32_t address : plan_.corpus.function_definition_order()) {
    Function* function = processor_->ResolveFunction(address);
    if (!function) {
      resolved_functions_.clear();
      return Fail(error,
                  "captured function failed to resolve during replay warmup");
    }
    if (!module_->HasExactExtent(*function)) {
      resolved_functions_.clear();
      return Fail(error,
                  "replayed function does not have its captured exact extent");
    }
    if (!function->is_guest()) {
      resolved_functions_.clear();
      return Fail(error, "replayed corpus function is not guest code");
    }
    resolved_functions_.push_back(static_cast<GuestFunction*>(function));
    functions_by_address.emplace(address, function);
  }
  for (size_t i = 0; i < plan_.segments.size(); ++i) {
    const auto it = functions_by_address.find(
        plan_.segments[i].invocation.function_address);
    if (it == functions_by_address.end()) {
      resolved_functions_.clear();
      segment_functions_.clear();
      return Fail(error, "segment root was not defined in captured order");
    }
    segment_functions_[i] = it->second;
  }
  return true;
}

bool GuestExecutionSessionRunner::CaptureWarmedCodeShape(
    GuestInvocationReplayCodeShape* output, std::string* error) const {
  std::vector<GuestInvocationReplayCodeShapeFunction> functions;
  functions.reserve(resolved_functions_.size());
  for (const GuestFunction* function : resolved_functions_) {
    functions.push_back(GuestInvocationReplayCodeShapeFunction{
        function->address(), function->end_address(), function->machine_code(),
        function->machine_code_length()});
  }
  return HashGuestInvocationReplayA64CodeShape(functions, output, error);
}

bool GuestExecutionSessionRunner::RestoreAll(
    GuestExecutionSessionReplayMetrics* metrics, std::string* error) {
  ThreadCpuInterval interval;
  if (!interval.Begin(error)) {
    return false;
  }
  for (const ResetPageCopy& copy : reset_page_copies_) {
    std::memcpy(copy.destination, copy.source, kGuestPageSize);
  }
  DispatchWorkers(WorkerCommand::kRestore);
  const bool workers_succeeded = WaitForWorkers(error);
  if (!interval.End(workers_succeeded ? error : nullptr) ||
      !workers_succeeded) {
    return false;
  }
  if (!CheckedAccumulate(&metrics->restore_thread_cpu_nanoseconds,
                         interval.cpu_nanoseconds(), error)) {
    return false;
  }
  for (size_t i = 0; i < workers_.size(); ++i) {
    if (!CheckedAccumulate(&metrics->restore_thread_cpu_nanoseconds,
                           workers_[i]->restore_cpu_nanoseconds, error) ||
        !CheckedAccumulate(
            &metrics->participants[i].restore_thread_cpu_nanoseconds,
            workers_[i]->restore_cpu_nanoseconds, error)) {
      return false;
    }
  }
  return true;
}

bool GuestExecutionSessionRunner::RunCoordinator(
    uint64_t* verification_cpu_nanoseconds, uint64_t* verification_count,
    std::string* error) {
  for (uint32_t event_index : plan_.coordinator_event_indices) {
    const GuestExecutionSessionReplayEvent& planned = plan_.events[event_index];
    GuestExecutionReplayTurn turn;
    const GuestExecutionReplayAcquireResult result = tape_->Acquire(
        kGuestExecutionSessionNoThread, options_.event_timeout, &turn, error);
    if (result == GuestExecutionReplayAcquireResult::kComplete) {
      return Fail(error,
                  "replay tape completed before the coordinator consumed its "
                  "recorded events");
    }
    if (result != GuestExecutionReplayAcquireResult::kAcquired) {
      return false;
    }
    if (turn.event != planned.event) {
      return AbandonTurn(tape_.get(), turn, error,
                         "replay tape cursor differs from the planned event");
    }
    std::string message;
    if (full_verification_ &&
        planned.checkpoint_index != kGuestExecutionSessionReplayNoIndex &&
        !RunInIntervalVerification(
            [&](std::string* verify_error) {
              return VerifyCheckpoint(
                  plan_.checkpoints[planned.checkpoint_index], verify_error);
            },
            verification_cpu_nanoseconds, verification_count, &message)) {
      return AbandonTurn(tape_.get(), turn, error, message);
    }
    switch (planned.role) {
      case GuestExecutionSessionReplayEventRole::kBoundaryControl:
      case GuestExecutionSessionReplayEventRole::kGuestMarker:
        break;
      case GuestExecutionSessionReplayEventRole::kAsynchronousMutation:
        if (turn.payload.size() != turn.event.byte_count ||
            turn.event.guest_address + turn.payload.size() >
                kGuestAddressSpaceSize) {
          return AbandonTurn(
              tape_.get(), turn, error,
              "asynchronous mutation payload differs from its event");
        }
        std::memcpy(memory_->TranslateVirtual(
                        static_cast<uint32_t>(turn.event.guest_address)),
                    turn.payload.data(), turn.payload.size());
        break;
      default:
        return AbandonTurn(tape_.get(), turn, error,
                           "coordinator leased a participant event");
    }
    if (!tape_->CommitCaptured(turn, error)) {
      return false;
    }
  }
  const GuestExecutionReplayTapeStatus status = tape_->status();
  if (status.state != GuestExecutionReplayTapeState::kComplete ||
      status.consumed_event_count != status.total_event_count ||
      status.has_active_lease) {
    const std::string message = status.message.empty()
                                    ? "replay tape has unconsumed events"
                                    : status.message;
    tape_->Cancel(message);
    return Fail(error, message);
  }
  return true;
}

bool GuestExecutionSessionRunner::RunInterval(
    GuestExecutionSessionReplayMetrics* metrics, std::string* error) {
  tape_ = GuestExecutionReplayTape::Create(*bundle_, error);
  if (!tape_ || !tape_->Start(error)) {
    tape_.reset();
    return false;
  }
  ThreadCpuInterval interval;
  if (!interval.Begin(error)) {
    tape_.reset();
    return false;
  }
  DispatchWorkers(WorkerCommand::kRun);
  std::string coordinator_error;
  uint64_t coordinator_verification_cpu = 0;
  uint64_t coordinator_verification_count = 0;
  const bool coordinator_succeeded =
      RunCoordinator(&coordinator_verification_cpu,
                     &coordinator_verification_count, &coordinator_error);
  if (!coordinator_succeeded) {
    tape_->Cancel(coordinator_error);
  }
  std::string worker_error;
  const bool workers_succeeded = WaitForWorkers(&worker_error);
  std::string interval_error;
  const bool interval_succeeded = interval.End(&interval_error);
  const GuestExecutionReplayTapeStatus status = tape_->status();
  tape_.reset();
  if (!coordinator_succeeded) {
    return Fail(error, coordinator_error);
  }
  if (!workers_succeeded) {
    return Fail(error, worker_error);
  }
  if (!interval_succeeded) {
    return Fail(error, interval_error);
  }
  if (status.state != GuestExecutionReplayTapeState::kComplete) {
    return Fail(error, status.message.empty() ? "replay tape did not complete"
                                              : status.message);
  }

  uint64_t participant_cpu = 0;
  uint64_t consumed_event_count = 0;
  uint64_t verification_cpu = coordinator_verification_cpu;
  uint64_t verification_count = coordinator_verification_count;
  for (size_t i = 0; i < workers_.size(); ++i) {
    const GuestExecutionSessionReplayParticipantMetrics& worker_metrics =
        workers_[i]->metrics;
    if (worker_metrics.consumed_event_count !=
        plan_.participants[i].event_indices.size()) {
      return Fail(error, "participant consumed an unexpected event count");
    }
    GuestExecutionSessionReplayParticipantMetrics& totals =
        metrics->participants[i];
    if (!CheckedAccumulate(&totals.thread_cpu_nanoseconds,
                           worker_metrics.thread_cpu_nanoseconds, error) ||
        !CheckedAccumulate(&totals.uptime_raw_nanoseconds,
                           worker_metrics.uptime_raw_nanoseconds, error) ||
        !CheckedAccumulate(&totals.consumed_event_count,
                           worker_metrics.consumed_event_count, error) ||
        !CheckedAccumulate(
            &totals.in_interval_verification_thread_cpu_nanoseconds,
            worker_metrics.in_interval_verification_thread_cpu_nanoseconds,
            error) ||
        !CheckedAccumulate(&totals.in_interval_verification_count,
                           worker_metrics.in_interval_verification_count,
                           error) ||
        !CheckedAccumulate(&participant_cpu,
                           worker_metrics.thread_cpu_nanoseconds, error) ||
        !CheckedAccumulate(&consumed_event_count,
                           worker_metrics.consumed_event_count, error) ||
        !CheckedAccumulate(
            &verification_cpu,
            worker_metrics.in_interval_verification_thread_cpu_nanoseconds,
            error) ||
        !CheckedAccumulate(&verification_count,
                           worker_metrics.in_interval_verification_count,
                           error)) {
      return false;
    }
  }
  if (!CheckedAccumulate(&consumed_event_count,
                         plan_.coordinator_event_indices.size(), error)) {
    return false;
  }
  if (consumed_event_count != status.consumed_event_count ||
      consumed_event_count != plan_.events.size()) {
    return Fail(error, "consumed events do not cover the complete tape");
  }
  if (!full_verification_ &&
      participant_cpu <
          options_.minimum_participant_cpu_nanoseconds_per_repetition) {
    return Fail(error,
                "participant CPU per repetition is below the configured "
                "guest-work floor");
  }
  metrics->consumed_event_count_per_repetition = consumed_event_count;
  return CheckedAccumulate(&metrics->participant_thread_cpu_nanoseconds,
                           participant_cpu, error) &&
         CheckedAccumulate(&metrics->coordinator_thread_cpu_nanoseconds,
                           interval.cpu_nanoseconds(), error) &&
         CheckedAccumulate(&metrics->uptime_raw_nanoseconds,
                           interval.uptime_raw_nanoseconds(), error) &&
         CheckedAccumulate(
             &metrics->in_interval_verification_thread_cpu_nanoseconds,
             verification_cpu, error) &&
         CheckedAccumulate(&metrics->in_interval_verification_count,
                           verification_count, error);
}

bool GuestExecutionSessionRunner::RunRepetition(
    GuestExecutionSessionReplayMetrics* metrics, bool full_verification,
    std::string* error) {
  if (metrics->participants.size() != workers_.size()) {
    metrics->participants.assign(
        workers_.size(), GuestExecutionSessionReplayParticipantMetrics{});
  }
  full_verification_ = full_verification;
  if (!RestoreAll(metrics, error)) {
    return false;
  }
  if (!RunInterval(metrics, error)) {
    return false;
  }
  ThreadCpuInterval verification;
  if (!verification.Begin(error)) {
    return false;
  }
  const bool verified = VerifyFinalState(error);
  if (!verification.End(verified ? error : nullptr) || !verified) {
    return false;
  }
  if (!CheckedAccumulate(&metrics->verification_thread_cpu_nanoseconds,
                         verification.cpu_nanoseconds(), error) ||
      !CheckedAccumulate(&metrics->repetition_count, 1, error)) {
    return false;
  }
  return true;
}

bool GuestExecutionSessionRunner::WarmAndVerify(std::string* error) {
  if (error) {
    error->clear();
  }
  if (warmed_) {
    return Fail(error, "session replay has already been warmed");
  }
  warmup_metrics_ = {};
  if (!ResolveFunctionsInCaptureOrder(error) ||
      !RunRepetition(&warmup_metrics_, true, error) ||
      !CaptureWarmedCodeShape(&warmed_code_shape_, error)) {
    return false;
  }
  uint64_t intermediate_checkpoint_count = 0;
  for (const GuestExecutionSessionReplayEvent& event : plan_.events) {
    if (event.checkpoint_index != kGuestExecutionSessionReplayNoIndex) {
      ++intermediate_checkpoint_count;
    }
  }
  warmup_metrics_.intermediate_checkpoint_count = intermediate_checkpoint_count;
  warmup_metrics_.reset_page_count_per_repetition =
      plan_.reset_page_addresses.size();
  warmup_metrics_.reset_bytes_per_repetition =
      uint64_t(plan_.reset_page_addresses.size()) * kGuestPageSize;
  warmup_metrics_.code_shape = warmed_code_shape_;
  warmed_ = true;
  return true;
}

bool GuestExecutionSessionRunner::RunTimed(
    uint64_t repetition_count, GuestExecutionSessionReplayMetrics* metrics,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!metrics) {
    return Fail(error, "session replay metrics output is null");
  }
  *metrics = {};
  if (!warmed_) {
    return Fail(error, "session replay must be warmed before timing");
  }
  if (!repetition_count || repetition_count > kMaxRepetitionCount) {
    return Fail(error, "timed repetition count is outside the bounded range");
  }
  backend::CodeCache* code_cache = processor_->backend()->code_cache();
  if (!code_cache) {
    return Fail(error, "replay backend has no code cache");
  }

  GuestExecutionSessionReplayMetrics accumulated;
  accumulated.placement_generation_before = code_cache->placement_generation();
  for (uint64_t i = 0; i < repetition_count; ++i) {
    if (!RunRepetition(&accumulated, false, error)) {
      return false;
    }
  }
  accumulated.placement_generation_after = code_cache->placement_generation();
  if (accumulated.placement_generation_after !=
      accumulated.placement_generation_before) {
    return Fail(error, "code placement changed inside the timed interval");
  }
  if (!accumulated.participant_thread_cpu_nanoseconds) {
    return Fail(error, "participant CPU interval is missing or zero");
  }
  if (!accumulated.uptime_raw_nanoseconds) {
    return Fail(error, "uptime raw interval is missing or zero");
  }

  GuestInvocationReplayCodeShape final_code_shape;
  if (!CaptureWarmedCodeShape(&final_code_shape, error)) {
    return false;
  }
  if (final_code_shape != warmed_code_shape_) {
    return Fail(error, "normalized warmed code shape changed during replay");
  }

  accumulated.intermediate_checkpoint_count =
      warmup_metrics_.intermediate_checkpoint_count;
  accumulated.reset_page_count_per_repetition =
      plan_.reset_page_addresses.size();
  accumulated.reset_bytes_per_repetition =
      uint64_t(plan_.reset_page_addresses.size()) * kGuestPageSize;
  accumulated.code_shape = warmed_code_shape_;
  *metrics = std::move(accumulated);
  return true;
}

}  // namespace cpu
}  // namespace xe
