/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_ARTIFACT_H_
#define XENIA_CPU_GUEST_INVOCATION_ARTIFACT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace cpu {
namespace ppc {

struct PPCContext_s;

// Dependencies that a single-function capture may discover. Version 1 replay
// artifacts are deliberately self-contained, so any of these flags makes an
// artifact invalid for encoding or replay. Keeping the reasons distinct lets a
// future capturer fail with a precise explanation instead of silently
// producing a misleading benchmark.
enum GuestInvocationDependency : uint32_t {
  kGuestInvocationDependencyMmio = 1u << 0,
  kGuestInvocationDependencyKernelExport = 1u << 1,
  kGuestInvocationDependencyExternOrBuiltin = 1u << 2,
  kGuestInvocationDependencyClockOrTimebase = 1u << 3,
  kGuestInvocationDependencyAtomicReservation = 1u << 4,
  kGuestInvocationDependencyCrossThreadMutation = 1u << 5,
  kGuestInvocationDependencyRecursiveInvocation = 1u << 6,
  kGuestInvocationDependencyUnbalancedReturn = 1u << 7,
  kGuestInvocationDependencyIncompletePageDiscovery = 1u << 8,
  kGuestInvocationDependencyPageDiscoveryOverflow = 1u << 9,
  kGuestInvocationDependencyAsyncInterrupt = 1u << 10,
  kGuestInvocationDependencySelfModifyingCode = 1u << 11,
  kGuestInvocationDependencyPhysicalAlias = 1u << 12,
  kGuestInvocationDependencyUnsupportedMappingOrProtection = 1u << 13,
};

constexpr uint32_t kGuestInvocationKnownDependencyMask =
    kGuestInvocationDependencyMmio | kGuestInvocationDependencyKernelExport |
    kGuestInvocationDependencyExternOrBuiltin |
    kGuestInvocationDependencyClockOrTimebase |
    kGuestInvocationDependencyAtomicReservation |
    kGuestInvocationDependencyCrossThreadMutation |
    kGuestInvocationDependencyRecursiveInvocation |
    kGuestInvocationDependencyUnbalancedReturn |
    kGuestInvocationDependencyIncompletePageDiscovery |
    kGuestInvocationDependencyPageDiscoveryOverflow |
    kGuestInvocationDependencyAsyncInterrupt |
    kGuestInvocationDependencySelfModifyingCode |
    kGuestInvocationDependencyPhysicalAlias |
    kGuestInvocationDependencyUnsupportedMappingOrProtection;

// Pointer-free, padding-free architectural state selected explicitly from
// PPCContext. Floating-point registers are stored as raw IEEE-754 bits, and
// vector registers (including the internal VSCR vector) as exact bytes, so NaN
// payloads and lane contents survive a round trip.
//
// PPCContext's preemption request, thread ID, scratch value, safepoint PC,
// coverage arena and host pointers are intentionally absent. They are replay
// scaffolding rather than PowerPC architectural state; in particular, the
// replay runner assigns its own thread ID.
struct GuestPPCRegisterState {
  std::array<uint64_t, 32> gpr = {};
  std::array<uint64_t, 32> fpr_bits = {};
  std::array<std::array<uint8_t, 16>, 128> vector_registers = {};
  std::array<std::array<uint8_t, 4>, 8> condition_register_fields = {};
  uint64_t link_register = 0;
  uint64_t count_register = 0;
  uint64_t machine_state_register = 0;
  uint32_t fpscr = 0;
  std::array<uint8_t, 16> vscr_vector = {};
  uint32_t vrsave = 0;
  uint8_t xer_ca = 0;
  uint8_t xer_ov = 0;
  uint8_t xer_so = 0;
  uint8_t vscr_sat = 0;

  bool operator==(const GuestPPCRegisterState&) const = default;
};

// Copies only the architectural fields above. RestoreGuestPPCRegisterState
// leaves all replay-assigned and host-specific PPCContext fields untouched.
GuestPPCRegisterState CaptureGuestPPCRegisterState(const PPCContext_s& context);
void RestoreGuestPPCRegisterState(const GuestPPCRegisterState& state,
                                  PPCContext_s* context);

// Canonical standalone encoding for one pointer-free architectural register
// state. The fixed little-endian header makes the content-addressed blob
// self-describing; decoders accept exactly one complete blob and reject every
// truncated or trailing byte sequence.
//
// The payload is the exact field-by-field register-state representation
// embedded by version 1 GuestInvocationArtifactCodec. The artifact keeps its
// existing bytes by embedding only that payload, while standalone state blobs
// add this codec's header. A payload layout change therefore requires a new
// version here and in every enclosing format that embeds it.
class GuestPPCRegisterStateCodec {
 public:
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kHeaderSize = 32;
  static constexpr uint32_t kPayloadSize = 2644;
  static constexpr uint32_t kEncodedSize = kHeaderSize + kPayloadSize;

  // On failure, output is cleared and error (when non-null) describes the
  // first rejected invariant.
  static bool Encode(const GuestPPCRegisterState& state,
                     std::vector<uint8_t>* output,
                     std::string* error = nullptr);

  // On failure, output is reset. The magic, version, size, flags and reserved
  // fields are all validated before the architectural payload is accepted.
  static bool Decode(const uint8_t* data, size_t data_size,
                     GuestPPCRegisterState* output,
                     std::string* error = nullptr);
  static bool Decode(const std::vector<uint8_t>& data,
                     GuestPPCRegisterState* output,
                     std::string* error = nullptr) {
    return Decode(data.data(), data.size(), output, error);
  }
};

// Portable re-entry mode for one captured guest thread. Version 1 restores at
// an exact catalog-bound guest block head. The pending-extern form has a
// canonical wire representation for a modeled blocking export, but replay
// support must still opt in by supplying the exact binding below.
enum class GuestPPCThreadResumeKind : uint32_t {
  kGuestBlockHead = 1,
  kPendingModeledBlockingExtern = 2,
  kOutsideGuest = 3,
};

// Pointer-free semantic state for one fixed session participant. Host stacks,
// backend contexts, native pointers, reservations and thread lifecycle state
// are deliberately absent. Version 1 reserves canonical-zero wire space for
// the latter two contracts rather than silently pretending they were saved.
struct GuestPPCThreadCheckpoint {
  // Exact session participant and guest thread whose state this blob holds.
  // This is the scheduler subject identity. A scheduler event's actor is a
  // separate event-schema concern and must not be inferred from this pair.
  uint32_t participant_ordinal = 0;
  uint32_t guest_thread_id = 0;
  GuestPPCThreadResumeKind resume_kind =
      GuestPPCThreadResumeKind::kGuestBlockHead;
  uint32_t resume_pc = 0;

  // Inclusive exact-corpus extent owning resume_pc. A consumer must bind both
  // endpoints to its code catalog before attaching the state to a ThreadState.
  uint32_t owning_function_address = 0;
  uint32_t owning_function_end_address = 0;
  uint32_t outer_guest_return_address = 0;

  // Both fields are zero at an ordinary block head. Both are nonzero for a
  // pending modeled blocking extern, and the export address is PPC-aligned.
  uint64_t pending_external_event_sequence = 0;
  uint32_t pending_export_guest_address = 0;

  GuestPPCRegisterState registers = {};

  bool operator==(const GuestPPCThreadCheckpoint&) const = default;
};

// Exact session/catalog identity that a decoded checkpoint must match before
// it is attached to a live or replay-created ThreadState. Keeping this
// pointer-free lets bundle validation reject a misplaced blob before any
// platform-specific restore is attempted.
struct GuestPPCThreadCheckpointBinding {
  uint32_t participant_ordinal = 0;
  uint32_t guest_thread_id = 0;
  GuestPPCThreadResumeKind resume_kind =
      GuestPPCThreadResumeKind::kGuestBlockHead;
  uint32_t resume_pc = 0;
  uint32_t owning_function_address = 0;
  uint32_t owning_function_end_address = 0;
  uint32_t outer_guest_return_address = 0;
  uint64_t pending_external_event_sequence = 0;
  uint32_t pending_export_guest_address = 0;

  bool operator==(const GuestPPCThreadCheckpointBinding&) const = default;
};

// Fixed-size canonical wrapper around the complete 2676-byte standalone
// register-state blob. All integers are little-endian and every field is
// emitted explicitly; C++ object padding is never serialized.
class GuestPPCThreadCheckpointCodec {
 public:
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kHeaderSize = 32;
  static constexpr uint32_t kMetadataSize = 72;
  static constexpr uint32_t kReservationReservedSize = 16;
  static constexpr uint32_t kLifecycleReservedSize = 16;
  static constexpr uint32_t kRegisterStateOffset = kHeaderSize + kMetadataSize;
  static constexpr uint32_t kEncodedSize =
      kRegisterStateOffset + GuestPPCRegisterStateCodec::kEncodedSize;

  // On failure, output is cleared and error (when non-null) describes the
  // first rejected invariant.
  static bool Encode(const GuestPPCThreadCheckpoint& checkpoint,
                     std::vector<uint8_t>* output,
                     std::string* error = nullptr);

  // Decodes and validates into a temporary. On failure, output is not
  // modified, including for truncation, trailing data and a malformed nested
  // register blob.
  static bool Decode(const uint8_t* data, size_t data_size,
                     GuestPPCThreadCheckpoint* output,
                     std::string* error = nullptr);
  static bool Decode(const std::vector<uint8_t>& data,
                     GuestPPCThreadCheckpoint* output,
                     std::string* error = nullptr) {
    return Decode(data.data(), data.size(), output, error);
  }

  // Validates the semantic state and requires every re-entry and participant
  // identity field to equal the caller's session/catalog binding.
  static bool ValidateBinding(const GuestPPCThreadCheckpoint& checkpoint,
                              const GuestPPCThreadCheckpointBinding& binding,
                              std::string* error = nullptr);
};

struct GuestInvocationPage {
  uint32_t guest_address = 0;
  std::array<uint8_t, 4096> data = {};

  bool operator==(const GuestInvocationPage&) const = default;
};

struct GuestFunctionInvocation {
  // Inclusive scanner extent of the function. Version 1 executes through
  // Function::Call, so entry_address must equal function_address. Keeping the
  // field explicit prevents a future format from silently changing the entry
  // contract if mid-function execution becomes available.
  uint32_t function_address = 0;
  uint32_t function_end_address = 0;
  uint32_t entry_address = 0;

  // The runner must stop when control returns here. The captured input LR must
  // match it; the expected output LR remains independently checked as part of
  // expected_output.
  uint32_t expected_return_address = 0;

  uint32_t dependency_flags = 0;
  GuestPPCRegisterState input = {};
  GuestPPCRegisterState expected_output = {};

  // Canonical, strictly increasing 4 KiB guest data pages required by this
  // invocation: every directly touched data page plus every non-code page
  // needed to complete each allowed host-protection-granule closure. Code pages
  // in those closures are supplied separately by the exact code corpus named
  // in the artifact header, avoiding duplicate and potentially conflicting
  // code bytes. Version 1 supports only ordinary virtual and XEX memory page
  // bases in 00001000-7EFFF000 and 80000000-9FFFF000.
  // expected_dirty_pages contains complete final-page snapshots; every dirty
  // page must exist in input_data_pages and differ from its initial bytes.
  //
  // This is intentionally a bounded capture of touched pages and their
  // protection-granule closures, not a full committed-memory checkpoint. On
  // hosts with protection granules larger than 4 KiB, an access to another
  // supplied page in the same granule cannot be trapped independently. A
  // capturer that cannot discover every required page, or exceeds the limit,
  // must set the corresponding dependency flag and the artifact will fail
  // closed. Allocation, protection and fault semantics are outside v1: capture
  // proves that each recorded access succeeded, while the runner must still
  // recreate and verify mappings and reject data/code page overlap before
  // execution.
  std::vector<GuestInvocationPage> input_data_pages;
  std::vector<GuestInvocationPage> expected_dirty_pages;

  bool operator==(const GuestFunctionInvocation&) const = default;
};

struct GuestInvocationArtifact {
  // SHA-256 provenance of the emulator build that performed capture. An A/B
  // replay candidate is expected to be a different build; this hash must be
  // reported, but must never be used as a candidate-build compatibility gate.
  std::array<uint8_t, 32> capture_build_sha256 = {};

  // SHA-256 of the exact external JIT code corpus and canonical
  // replay-affecting configuration. A runner must require exact matches for
  // both before executing. An all-zero value for any hash is invalid.
  std::array<uint8_t, 32> code_corpus_sha256 = {};
  std::array<uint8_t, 32> replay_config_sha256 = {};

  // Capture order is significant and preserved.
  std::vector<GuestFunctionInvocation> invocations;

  bool operator==(const GuestInvocationArtifact&) const = default;
};

// Portable little-endian version 1 wire format. Every integer and byte array
// is emitted field-by-field; raw C++ structs, host pointers and padding are
// never serialized. Decoder limits are part of the format's resource-safety
// contract rather than suggested capture targets.
class GuestInvocationArtifactCodec {
 public:
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint64_t kMaxArtifactSize = 512ull * 1024ull * 1024ull;
  static constexpr uint32_t kMaxInvocations = 65536;
  static constexpr uint32_t kMaxDataPagesPerInvocation = 65536;
  static constexpr uint32_t kMaxFunctionSize = 16u * 1024u * 1024u;

  // Public for format tests and independent readers.
  static constexpr uint32_t kHeaderSize = 136;
  static constexpr uint32_t kInvocationHeaderSize = 40;
  static constexpr uint32_t kArchitecturalStateSize =
      GuestPPCRegisterStateCodec::kPayloadSize;
  static constexpr uint32_t kPageRecordSize = 4 + kPageSize;

  // On failure, output is cleared and error (when non-null) describes the
  // first rejected invariant.
  static bool Encode(const GuestInvocationArtifact& artifact,
                     std::vector<uint8_t>* output,
                     std::string* error = nullptr);

  // On failure, output is reset. Truncation, trailing bytes, non-canonical page
  // order, unknown fields and unsafe dependency flags all fail closed.
  static bool Decode(const uint8_t* data, size_t data_size,
                     GuestInvocationArtifact* output,
                     std::string* error = nullptr);
  static bool Decode(const std::vector<uint8_t>& data,
                     GuestInvocationArtifact* output,
                     std::string* error = nullptr) {
    return Decode(data.data(), data.size(), output, error);
  }
};

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_ARTIFACT_H_
