/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture.h"

#include <string_view>
#include <utility>

namespace xe {
namespace cpu {

namespace {

constexpr std::string_view kMissingPublicationDiagnostic =
    "capture publication failed without a diagnostic";
constexpr std::string_view kStoppedDiagnostic =
    "capture stopped before the selected invocation completed";

}  // namespace

std::unique_ptr<GuestInvocationCaptureCoordinator>
GuestInvocationCaptureCoordinator::Create(
    uint64_t segment_ordinal,
    const ppc::GuestInvocationRecorderSelection& selection,
    const ppc::GuestInvocationRecorderLimits& limits,
    ppc::GuestInvocationRecorderPageReader& page_reader,
    const ppc::GuestInvocationRecorderClock& clock,
    SegmentHandler segment_handler, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!segment_handler) {
    if (error) {
      error->assign("capture segment handler is missing");
    }
    return nullptr;
  }
  const uint64_t segment_start_tick = clock.NowTicks();
  std::unique_ptr<ppc::GuestInvocationRecorder> recorder =
      ppc::GuestInvocationRecorder::Create(selection, limits, page_reader,
                                           clock, error);
  if (!recorder) {
    return nullptr;
  }
  return std::unique_ptr<GuestInvocationCaptureCoordinator>(
      new GuestInvocationCaptureCoordinator(segment_ordinal, segment_start_tick,
                                            std::move(recorder), clock,
                                            std::move(segment_handler)));
}

GuestInvocationCaptureCoordinator::GuestInvocationCaptureCoordinator(
    uint64_t segment_ordinal, uint64_t segment_start_tick,
    std::unique_ptr<ppc::GuestInvocationRecorder> recorder,
    const ppc::GuestInvocationRecorderClock& clock,
    SegmentHandler segment_handler)
    : segment_ordinal_(segment_ordinal),
      segment_start_tick_(segment_start_tick),
      recorder_(std::move(recorder)),
      clock_(clock),
      segment_handler_(std::move(segment_handler)) {}

GuestInvocationCaptureCoordinator::~GuestInvocationCaptureCoordinator() {
  Stop();
}

bool GuestInvocationCaptureCoordinator::IsTerminalLocked() const {
  return state_ != GuestInvocationCaptureState::kRecording;
}

bool GuestInvocationCaptureCoordinator::FinishCallbackLocked(
    bool callback_succeeded) {
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  if (recorder_->state() == ppc::GuestInvocationRecorderState::kRejected) {
    state_ = GuestInvocationCaptureState::kRejected;
    segment_end_tick_ = clock_.NowTicks();
    message_ = recorder_->rejection_message();
    if (message_.empty()) {
      message_ = "capture recorder rejected without a diagnostic";
    }
    segment_handler_ = {};
    return false;
  }
  if (recorder_->state() != ppc::GuestInvocationRecorderState::kComplete) {
    return callback_succeeded;
  }

  const ppc::GuestInvocationRecorderResult* result = recorder_->result();
  if (!result) {
    state_ = GuestInvocationCaptureState::kPublicationFailed;
    message_ = "completed capture has no recorder result";
    return false;
  }
  segment_end_tick_ = clock_.NowTicks();
  std::string publication_error;
  if (!segment_handler_(segment_ordinal_, segment_start_tick_,
                        segment_end_tick_, *result, &publication_error)) {
    state_ = GuestInvocationCaptureState::kPublicationFailed;
    message_ = publication_error.empty()
                   ? std::string(kMissingPublicationDiagnostic)
                   : std::move(publication_error);
    segment_handler_ = {};
    return false;
  }
  state_ = GuestInvocationCaptureState::kPublished;
  message_.clear();
  segment_handler_ = {};
  return true;
}

bool GuestInvocationCaptureCoordinator::Poll() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(recorder_->Poll());
}

bool GuestInvocationCaptureCoordinator::OnFunctionDependency(
    uint32_t source_address, uint32_t dependency_address) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnFunctionDependency(source_address, dependency_address));
}

bool GuestInvocationCaptureCoordinator::OnFunctionDefined(
    uint32_t address, uint32_t end_address) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnFunctionDefined(address, end_address));
}

bool GuestInvocationCaptureCoordinator::OnFunctionEntry(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t end_address, const ppc::GuestPPCRegisterState& state) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnFunctionEntry(identity, address, end_address, state));
}

bool GuestInvocationCaptureCoordinator::OnFunctionExit(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t return_address, const ppc::GuestPPCRegisterState& state) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnFunctionExit(identity, address, return_address, state));
}

bool GuestInvocationCaptureCoordinator::OnMemoryAccess(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
    uint32_t size, ppc::GuestInvocationRecorderMemoryAccess access) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnMemoryAccess(identity, address, size, access));
}

bool GuestInvocationCaptureCoordinator::OnUnsupportedDependency(
    const ppc::GuestInvocationRecorderIdentity& identity,
    uint32_t dependency_flags) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnUnsupportedDependency(identity, dependency_flags));
}

bool GuestInvocationCaptureCoordinator::OnTailCall(
    const ppc::GuestInvocationRecorderIdentity& identity, uint32_t from_address,
    uint32_t target_address) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(
      recorder_->OnTailCall(identity, from_address, target_address));
}

bool GuestInvocationCaptureCoordinator::OnUnwindOrLongjmp(
    const ppc::GuestInvocationRecorderIdentity& identity) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(recorder_->OnUnwindOrLongjmp(identity));
}

bool GuestInvocationCaptureCoordinator::OnAsyncReentry(
    const ppc::GuestInvocationRecorderIdentity& identity) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return state_ == GuestInvocationCaptureState::kPublished;
  }
  return FinishCallbackLocked(recorder_->OnAsyncReentry(identity));
}

void GuestInvocationCaptureCoordinator::Stop() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (IsTerminalLocked()) {
    return;
  }
  state_ = GuestInvocationCaptureState::kStopped;
  message_.assign(kStoppedDiagnostic);
  segment_end_tick_ = clock_.NowTicks();
  segment_handler_ = {};
}

GuestInvocationCaptureStatus GuestInvocationCaptureCoordinator::status() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  GuestInvocationCaptureStatus status;
  status.state = state_;
  status.recorder_state = recorder_->state();
  status.rejection = recorder_->rejection();
  status.rejected_dependency_flags = recorder_->rejected_dependency_flags();
  status.segment_ordinal = segment_ordinal_;
  status.accepted_segment_count =
      state_ == GuestInvocationCaptureState::kPublished ? 1 : 0;
  status.rejected_segment_count =
      state_ == GuestInvocationCaptureState::kRejected ||
              state_ == GuestInvocationCaptureState::kPublicationFailed ||
              state_ == GuestInvocationCaptureState::kStopped
          ? 1
          : 0;
  status.segment_start_tick = segment_start_tick_;
  status.segment_end_tick = segment_end_tick_;
  status.message = message_;
  return status;
}

}  // namespace cpu
}  // namespace xe
