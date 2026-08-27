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

  bool operator==(const Pm4MarkerEvent&) const = default;
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
  bool DetachSink(const std::shared_ptr<Pm4MarkerSink>& sink);

  uint64_t marker_count() const noexcept {
    return marker_count_.load(std::memory_order_acquire);
  }
  bool sink_failed() const;

  void NotifyPm4Swap(uint64_t host_tick) noexcept;
  void Shutdown() noexcept;

 private:
  mutable std::mutex mutex_;
  std::condition_variable dispatch_condition_;
  std::shared_ptr<Pm4MarkerSink> sink_;
  std::thread::id dispatch_thread_;
  bool dispatching_ = false;
  bool sink_failed_ = false;
  bool shut_down_ = false;
  std::atomic<uint64_t> marker_count_{0};
};

}  // namespace gpu
}  // namespace xe

#endif

#endif  // XENIA_GPU_PM4_MARKER_SINK_H_
