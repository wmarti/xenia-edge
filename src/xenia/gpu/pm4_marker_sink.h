/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_PM4_MARKER_SINK_H_
#define XENIA_GPU_PM4_MARKER_SINK_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace xe {
namespace gpu {

// Numerically equal to cpu::GuestExecutionSessionMarkerSource so capture code
// can record the source without a GPU dependency or a translation table.
enum class Pm4MarkerSource : uint32_t {
  kNone = 0,
  // An executed guest PM4_XE_SWAP packet. This is a guest-work boundary, not
  // evidence that a host drawable was presented.
  kPm4Swap = 2,
};

// xenos::PM4_XE_SWAP, restated so capture consumers need no GPU headers.
constexpr uint32_t kPm4SwapMarkerOpcode = 0x64;

struct Pm4MarkerEvent {
  Pm4MarkerSource source = Pm4MarkerSource::kNone;
  uint32_t opcode = 0;
  // Source-local 1-based count for one command processor. This is not a
  // session event sequence and must not be used to order other event sources.
  uint64_t ordinal = 0;
  // Clock::QueryHostTickCount() sampled after IssueSwap returns.
  uint64_t host_tick = 0;
  // Attachment generation under which this event was admitted. A hold drains
  // every event in its generation before returning, and resume starts a new
  // generation.
  uint64_t sink_generation = 0;

  bool operator==(const Pm4MarkerEvent&) const = default;
};

struct Pm4MarkerHoldToken {
  uint64_t sink_generation = 0;
  uint64_t hold_epoch = 0;
  uint64_t last_ordinal = 0;

  explicit operator bool() const noexcept {
    return sink_generation && hold_epoch;
  }
  bool operator==(const Pm4MarkerHoldToken&) const = default;
};

struct Pm4MarkerDispatcherStatus {
  uint64_t marker_count = 0;
  uint64_t sink_generation = 0;
  uint64_t hold_epoch = 0;
  bool sink_attached = false;
  bool sink_held = false;
  bool sink_failed = false;
  bool source_advanced_while_held = false;
  bool shut_down = false;
};

class Pm4MarkerDispatcher;

// One PM4 swap occurrence admitted immediately before IssueSwap. The ticket
// remains live through IssueSwap and marker delivery, so source control cannot
// overtake an occurrence that has already reached the swap boundary.
class Pm4MarkerDispatchLease {
 public:
  Pm4MarkerDispatchLease() = default;
  ~Pm4MarkerDispatchLease();
  Pm4MarkerDispatchLease(Pm4MarkerDispatchLease&& other) noexcept;
  Pm4MarkerDispatchLease& operator=(Pm4MarkerDispatchLease&& other) noexcept;
  Pm4MarkerDispatchLease(const Pm4MarkerDispatchLease&) = delete;
  Pm4MarkerDispatchLease& operator=(const Pm4MarkerDispatchLease&) = delete;

 private:
  friend class Pm4MarkerDispatcher;

  void Reset() noexcept;

  Pm4MarkerDispatcher* dispatcher_ = nullptr;
  std::shared_ptr<class Pm4MarkerSink> sink_;
  Pm4MarkerEvent event_;
  uint64_t ticket_ = 0;
  bool ticket_live_ = false;
};

class Pm4MarkerSink {
 public:
  virtual ~Pm4MarkerSink() = default;

  // Runs on the command processor thread and must not block, allocate, or call
  // back into the command processor. False fails delivery closed.
  virtual bool OnPm4Marker(const Pm4MarkerEvent& event) noexcept = 0;

  // Delivered at most once after marker production has stopped, if the sink is
  // still attached when the source shuts down.
  virtual void OnPm4MarkerSourceShutdown() noexcept = 0;
};

// Owns only PM4 marker delivery mechanics. Capture window sequencing, parking,
// acknowledgement, and stop policy belong to the session assembler.
class Pm4MarkerDispatcher {
 public:
  bool AttachSink(std::shared_ptr<Pm4MarkerSink> sink);
  // Synchronously removes sink from admission after draining every callback
  // admitted in the current generation. Repeating a healthy hold for the same
  // sink returns the same token. False leaves attachment state unchanged.
  bool HoldSink(const std::shared_ptr<Pm4MarkerSink>& sink,
                Pm4MarkerHoldToken* token);
  // Reattaches only the held sink named by the exact hold token and starts a
  // new generation. Any ordinal produced while held permanently prevents
  // resume and leaves the sink held.
  bool ResumeSink(const std::shared_ptr<Pm4MarkerSink>& sink,
                  const Pm4MarkerHoldToken& token);
  // Atomically validates the held-generation attestation and terminally
  // detaches the sink. Success proves that source shutdown and every admitted
  // marker are wholly before or after the publication cutoff.
  bool SealAndDetachHeldSink(const std::shared_ptr<Pm4MarkerSink>& sink,
                             const Pm4MarkerHoldToken& token);
  // Terminal removal of either an attached or held sink. This invalidates any
  // outstanding hold token.
  bool DetachSink(const std::shared_ptr<Pm4MarkerSink>& sink);

  uint64_t marker_count() const noexcept {
    return marker_count_.load(std::memory_order_acquire);
  }
  bool sink_failed() const;
  Pm4MarkerDispatcherStatus status() const;

  Pm4MarkerDispatchLease BeginPm4Swap() noexcept;
  void CompletePm4Swap(Pm4MarkerDispatchLease lease,
                       uint64_t host_tick) noexcept;
  void NotifyPm4Swap(uint64_t host_tick) noexcept;
  void Shutdown() noexcept;

 private:
  friend class Pm4MarkerDispatcherTestAccess;
  friend class Pm4MarkerDispatchLease;

  void SetPostOrdinalAssignmentHookForTesting(void (*hook)(void*),
                                              void* context);
  void SetPostTicketAssignmentHookForTesting(void (*hook)(void*, uint64_t),
                                             void* context);
  void RunPostTicketAssignmentHook(uint64_t ticket) const;
  void CompleteLease(Pm4MarkerDispatchLease* lease, uint64_t host_tick,
                     bool deliver) noexcept;

  mutable std::mutex mutex_;
  std::condition_variable dispatch_condition_;
  std::shared_ptr<Pm4MarkerSink> sink_;
  std::shared_ptr<Pm4MarkerSink> held_sink_;
  std::thread::id dispatch_thread_;
  Pm4MarkerHoldToken hold_token_;
  uint64_t sink_generation_ = 0;
  std::atomic<uint64_t> hold_epoch_{0};
  bool dispatching_ = false;
  bool sink_failed_ = false;
  bool source_advanced_while_held_ = false;
  bool shut_down_ = false;
  void (*post_ordinal_assignment_hook_)(void*) = nullptr;
  void* post_ordinal_assignment_context_ = nullptr;
  void (*post_ticket_assignment_hook_)(void*, uint64_t) = nullptr;
  void* post_ticket_assignment_context_ = nullptr;
  std::atomic<uint64_t> marker_count_{0};
  std::atomic<uint64_t> next_admission_ticket_{0};
  uint64_t serving_admission_ticket_ = 0;
  std::atomic<bool> admission_open_{false};
};

}  // namespace gpu
}  // namespace xe

#endif

#endif  // XENIA_GPU_PM4_MARKER_SINK_H_
