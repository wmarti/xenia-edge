/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_REPLAY_TAPE_H_
#define XENIA_CPU_GUEST_EXECUTION_REPLAY_TAPE_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_execution_session_bundle.h"

namespace xe {
namespace cpu {

enum class GuestExecutionReplayTapeState : uint8_t {
  kReady,
  kRunning,
  kComplete,
  kRejected,
};

enum class GuestExecutionReplayTapeRejection : uint8_t {
  kNone,
  kInvalidCall,
  kRejectedCapture,
  kDecodeFailure,
  kEventOwnership,
  kLeaseMismatch,
  kDispositionMismatch,
  kDeterministicMismatch,
  kTimeout,
  kCancelled,
};

enum class GuestExecutionReplayAcquireResult : uint8_t {
  kAcquired,
  kComplete,
  kRejected,
};

struct GuestExecutionReplayTurn {
  uint64_t lease_id = 0;
  GuestExecutionSessionEvent event;
  std::vector<uint8_t> payload;

  explicit operator bool() const { return lease_id != 0; }
};

struct GuestExecutionReplayTapeStatus {
  GuestExecutionReplayTapeState state = GuestExecutionReplayTapeState::kReady;
  GuestExecutionReplayTapeRejection rejection =
      GuestExecutionReplayTapeRejection::kNone;
  uint64_t consumed_event_count = 0;
  uint64_t total_event_count = 0;
  uint64_t next_event_sequence = 0;
  bool has_active_lease = false;
  uint32_t active_owner = kGuestExecutionSessionNoThread;
  std::string message;
};

// A strict global event cursor shared by persistent replay workers. At most one
// event is leased at a time. A participant worker can acquire only its next
// event, while the coordinator uses kGuestExecutionSessionNoThread. Captured
// events expose their verified payload for injection; deterministic events
// advance only after the worker supplies the exact observed event record.
class GuestExecutionReplayTape final {
 public:
  static std::unique_ptr<GuestExecutionReplayTape> Create(
      const GuestExecutionSessionBundle& bundle, std::string* error = nullptr,
      GuestExecutionSessionBundleLimits limits = {});

  ~GuestExecutionReplayTape();
  GuestExecutionReplayTape(const GuestExecutionReplayTape&) = delete;
  GuestExecutionReplayTape& operator=(const GuestExecutionReplayTape&) = delete;

  bool Start(std::string* error = nullptr);

  GuestExecutionReplayAcquireResult Acquire(uint32_t owner,
                                            std::chrono::milliseconds timeout,
                                            GuestExecutionReplayTurn* turn,
                                            std::string* error = nullptr);

  // Advances a kReplayCaptured event after its payload has been applied.
  bool CommitCaptured(const GuestExecutionReplayTurn& turn,
                      std::string* error = nullptr);

  // Advances a kValidateDeterministic event only if every canonical field of
  // the observed record matches the capture.
  bool CommitDeterministic(const GuestExecutionReplayTurn& turn,
                           const GuestExecutionSessionEvent& observed,
                           std::string* error = nullptr);

  // Fails closed if a worker cannot finish an acquired turn.
  void Abandon(const GuestExecutionReplayTurn& turn, std::string message);
  void Cancel(std::string message);

  GuestExecutionReplayTapeStatus status() const;

 private:
  struct Impl;
  explicit GuestExecutionReplayTape(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_EXECUTION_REPLAY_TAPE_H_
