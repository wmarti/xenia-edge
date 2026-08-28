/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_EXPORT_EVENT_BRIDGE_H_
#define XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_EXPORT_EVENT_BRIDGE_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <map>
#include <string>

#include "xenia/cpu/guest_execution_external_event.h"
#include "xenia/cpu/guest_execution_session_assembler.h"

namespace xe {
namespace cpu {

// Resolves a modeled export dispatch's runtime-local pairing token to the
// durable global sequence its canonical session event was assigned. A
// checkpoint taken while the dispatch is still open cannot name that sequence
// yet, so the producer holds the token and asks for the sequence when the
// session encodes its durable state.
class GuestExecutionSessionCaptureExportSequenceResolver {
 public:
  virtual ~GuestExecutionSessionCaptureExportSequenceResolver() = default;

  // False when the token never reached the tape, was recorded for a different
  // participant, or the resolver is no longer usable.
  virtual bool ResolveModeledExportSequence(
      GuestExecutionCaptureExternalEventToken token,
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint64_t* global_sequence) const noexcept = 0;
};

// Canonicalizes one completed modeled export dispatch into the session
// envelope: the export itself as a kKernelExport carrying its little-endian
// returned value, then one kMemoryMutation per declared effect range carrying
// that range's postimage. Both are version-2 session formats, so no structured
// record is smuggled through an event payload.
//
// The recorded preimage and post-return volatile register set have no canonical
// carrier yet and are deliberately not published here; the offline runner
// refuses this class until they land together with its resume support.
//
// Every method runs on the session's control worker. Once anything rejects, the
// bridge stays rejected so a partially canonicalized dispatch can never be
// resolved.
class GuestExecutionSessionCaptureExportEventBridge final
    : public GuestExecutionSessionCaptureExportSequenceResolver {
 public:
  GuestExecutionSessionCaptureExportEventBridge() = default;
  ~GuestExecutionSessionCaptureExportEventBridge() override = default;
  GuestExecutionSessionCaptureExportEventBridge(
      const GuestExecutionSessionCaptureExportEventBridge&) = delete;
  GuestExecutionSessionCaptureExportEventBridge& operator=(
      const GuestExecutionSessionCaptureExportEventBridge&) = delete;

  GuestExecutionSessionAssemblerAction OnModeledExportRecord(
      GuestExecutionSessionAssembler& assembler,
      GuestExecutionCaptureExternalEventToken token,
      const GuestExecutionCaptureExternalEventRecord& record,
      std::string* error) noexcept;

  bool ResolveModeledExportSequence(
      GuestExecutionCaptureExternalEventToken token,
      const GuestExecutionCaptureParticipantIdentity& participant,
      uint64_t* global_sequence) const noexcept override;

  uint64_t canonicalized_export_count() const noexcept {
    return static_cast<uint64_t>(resolutions_.size());
  }
  bool rejected() const noexcept { return rejected_; }

 private:
  struct Resolution {
    GuestExecutionCaptureParticipantIdentity participant;
    uint64_t global_sequence = 0;
  };

  std::map<uint64_t, Resolution> resolutions_;
  bool rejected_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_EXECUTION_SESSION_CAPTURE_EXPORT_EVENT_BRIDGE_H_
