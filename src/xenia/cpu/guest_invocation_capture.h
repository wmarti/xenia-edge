/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_CAPTURE_H_
#define XENIA_CPU_GUEST_INVOCATION_CAPTURE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/cpu/guest_invocation_recorder.h"

namespace xe {
namespace cpu {

constexpr uint32_t kGuestInvocationCaptureRootEventBit = 0;
constexpr uint32_t kGuestInvocationCaptureOwnerEventBit = 1;
constexpr uint32_t kGuestInvocationCaptureWriteEventBit = 2;
constexpr uint8_t kGuestInvocationCaptureRootEvent =
    1u << kGuestInvocationCaptureRootEventBit;
constexpr uint8_t kGuestInvocationCaptureOwnerEvent =
    1u << kGuestInvocationCaptureOwnerEventBit;
constexpr uint8_t kGuestInvocationCaptureWriteEvent =
    1u << kGuestInvocationCaptureWriteEventBit;
constexpr uint8_t kGuestInvocationCaptureAllEvents =
    kGuestInvocationCaptureRootEvent | kGuestInvocationCaptureOwnerEvent |
    kGuestInvocationCaptureWriteEvent;

enum class GuestInvocationCaptureState : uint8_t {
  kRecording,
  kPublishing,
  kPublished,
  kRejected,
  kPublicationFailed,
  kStopped,
};

struct GuestInvocationCaptureStatus {
  GuestInvocationCaptureState state = GuestInvocationCaptureState::kRecording;
  ppc::GuestInvocationRecorderState recorder_state =
      ppc::GuestInvocationRecorderState::kWaitingForOccurrence;
  ppc::GuestInvocationRecorderRejection rejection =
      ppc::GuestInvocationRecorderRejection::kNone;
  uint32_t rejected_dependency_flags = 0;
  uint64_t segment_ordinal = 0;
  uint64_t accepted_segment_count = 0;
  uint64_t rejected_segment_count = 0;
  // Bounds the full recorder lifetime, including discovery attempts. These
  // are not the start and end of the accepted final invocation.
  uint64_t capture_start_tick = 0;
  uint64_t capture_end_tick = 0;
  std::string message;
};

// Stable hook surface for a capture build. A bounded execution-capture session
// may implement this interface by coordinating multiple independent recorder
// segments and aggregating their ordered results. Translation and A64
// execution hooks must depend only on this interface, not on the current
// one-segment primitive. A false callback result rejects the capture, not the
// title operation being observed.
class GuestInvocationCaptureEventSink {
 public:
  virtual ~GuestInvocationCaptureEventSink() = default;

  // Translation uses the selected root to keep only its discovery entry hook
  // live while no invocation is recording. A zero root conservatively treats
  // every function entry as a possible root for generic test sinks.
  virtual uint32_t root_address() const { return 0; }
  virtual uint8_t initial_event_mask() const {
    return kGuestInvocationCaptureAllEvents;
  }
  virtual uint8_t event_mask(
      const ppc::GuestInvocationRecorderIdentity& identity) const {
    return initial_event_mask();
  }

  virtual bool Poll() = 0;
  virtual bool OnFunctionDependency(uint32_t source_address,
                                    uint32_t dependency_address) = 0;
  virtual bool OnFunctionDefined(uint32_t address, uint32_t end_address) = 0;
  virtual bool OnFunctionEntry(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t end_address, const ppc::GuestPPCRegisterState& state) = 0;
  virtual bool OnFunctionExit(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t return_address, const ppc::GuestPPCRegisterState& state) = 0;
  virtual bool OnMemoryAccess(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t size, ppc::GuestInvocationRecorderMemoryAccess access) = 0;
  virtual bool OnUnsupportedDependency(
      const ppc::GuestInvocationRecorderIdentity& identity,
      uint32_t dependency_flags) = 0;
  virtual bool OnTailCall(const ppc::GuestInvocationRecorderIdentity& identity,
                          uint32_t from_address, uint32_t target_address) = 0;
  virtual bool OnUnwindOrLongjmp(
      const ppc::GuestInvocationRecorderIdentity& identity) = 0;
  virtual bool OnAsyncReentry(
      const ppc::GuestInvocationRecorderIdentity& identity) = 0;
};

// Serializes every recorder callback, including callbacks from different guest
// threads, and publishes a completed capture exactly once while holding the
// same coordinator lock. This is the only recorder interface that live
// translation and execution hooks may use.
//
// page_reader and clock must outlive the coordinator. segment_handler is
// invoked synchronously on the callback that completes the selected root
// invocation. It must publish atomically or return false with a diagnostic.
// Capture instrumentation is compiled out of normal builds, so the one-time
// publication cost is intentionally paid by the dedicated capture build. The
// segment ordinal and tick interval let a later session retain ordering and
// boundaries while sharing and deduplicating corpus and page storage.
class GuestInvocationCaptureCoordinator final
    : public GuestInvocationCaptureEventSink {
 public:
  using SegmentHandler = std::function<bool(
      uint64_t segment_ordinal, uint64_t capture_start_tick,
      uint64_t capture_end_tick,
      const ppc::GuestInvocationRecorderResult& result, std::string* error)>;

  static std::unique_ptr<GuestInvocationCaptureCoordinator> Create(
      uint64_t segment_ordinal,
      const ppc::GuestInvocationRecorderSelection& selection,
      const ppc::GuestInvocationRecorderLimits& limits,
      ppc::GuestInvocationRecorderPageReader& page_reader,
      const ppc::GuestInvocationRecorderClock& clock,
      SegmentHandler segment_handler, std::string* error = nullptr);

  ~GuestInvocationCaptureCoordinator();
  GuestInvocationCaptureCoordinator(const GuestInvocationCaptureCoordinator&) =
      delete;
  GuestInvocationCaptureCoordinator& operator=(
      const GuestInvocationCaptureCoordinator&) = delete;

  bool Poll() override;
  bool OnFunctionDependency(uint32_t source_address,
                            uint32_t dependency_address) override;
  bool OnFunctionDefined(uint32_t address, uint32_t end_address) override;
  bool OnFunctionEntry(const ppc::GuestInvocationRecorderIdentity& identity,
                       uint32_t address, uint32_t end_address,
                       const ppc::GuestPPCRegisterState& state) override;
  bool OnFunctionExit(const ppc::GuestInvocationRecorderIdentity& identity,
                      uint32_t address, uint32_t return_address,
                      const ppc::GuestPPCRegisterState& state) override;
  bool OnMemoryAccess(const ppc::GuestInvocationRecorderIdentity& identity,
                      uint32_t address, uint32_t size,
                      ppc::GuestInvocationRecorderMemoryAccess access) override;
  bool OnUnsupportedDependency(
      const ppc::GuestInvocationRecorderIdentity& identity,
      uint32_t dependency_flags) override;
  bool OnTailCall(const ppc::GuestInvocationRecorderIdentity& identity,
                  uint32_t from_address, uint32_t target_address) override;
  bool OnUnwindOrLongjmp(
      const ppc::GuestInvocationRecorderIdentity& identity) override;
  bool OnAsyncReentry(
      const ppc::GuestInvocationRecorderIdentity& identity) override;

  uint32_t root_address() const override { return root_address_; }
  uint8_t initial_event_mask() const override;
  uint8_t event_mask(
      const ppc::GuestInvocationRecorderIdentity& identity) const override;

  // Prevents any later callback or publication. An incomplete capture becomes
  // a deterministic terminal result instead of being mistaken for success.
  void Stop();
  GuestInvocationCaptureStatus status() const;

 private:
  GuestInvocationCaptureCoordinator(
      uint64_t segment_ordinal, uint64_t capture_start_tick,
      uint32_t root_address,
      std::unique_ptr<ppc::GuestInvocationRecorder> recorder,
      const ppc::GuestInvocationRecorderClock& clock,
      SegmentHandler segment_handler);

  bool FinishCallbackLocked(bool callback_succeeded);
  bool FinishIdentityCallbackLocked(
      bool callback_succeeded,
      const ppc::GuestInvocationRecorderIdentity& identity);
  void RefreshActiveIdentityLocked(
      const ppc::GuestInvocationRecorderIdentity* identity);
  void ClearFastStateLocked();
  bool IsTerminalLocked() const;

  // Recursive so a host callback that reenters capture reaches the recorder's
  // explicit reentry rejection rather than deadlocking this coordinator.
  mutable std::recursive_mutex mutex_;
  uint64_t segment_ordinal_ = 0;
  uint64_t capture_start_tick_ = 0;
  uint64_t capture_end_tick_ = 0;
  std::unique_ptr<ppc::GuestInvocationRecorder> recorder_;
  const ppc::GuestInvocationRecorderClock& clock_;
  SegmentHandler segment_handler_;
  GuestInvocationCaptureState state_ = GuestInvocationCaptureState::kRecording;
  std::string message_;
  uint32_t root_address_ = 0;
  std::atomic<uint8_t> common_event_mask_{kGuestInvocationCaptureRootEvent |
                                          kGuestInvocationCaptureWriteEvent};
  std::atomic<uint64_t> active_context_id_{0};
  std::atomic<uint64_t> active_thread_id_{0};
  std::atomic<bool> active_identity_{false};
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_CAPTURE_H_
