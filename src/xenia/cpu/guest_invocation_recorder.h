/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_RECORDER_H_
#define XENIA_CPU_GUEST_INVOCATION_RECORDER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/cpu/guest_invocation_artifact.h"

namespace xe {
namespace cpu {
namespace ppc {

struct GuestInvocationRecorderIdentity {
  uint64_t context_id = 0;
  uint64_t thread_id = 0;

  bool operator==(const GuestInvocationRecorderIdentity&) const = default;
};

struct GuestInvocationRecorderSelection {
  uint32_t root_address = 0;
  uint32_t root_end_address = 0;
  uint32_t occurrence = 0;

  // Optional. If both fields are zero, the selected global root occurrence
  // claims its context and thread as the owner. If supplied, both fields must
  // be nonzero and the selected occurrence must match them.
  GuestInvocationRecorderIdentity owner = {};
};

struct GuestInvocationRecorderLimits {
  static constexpr uint32_t kMaximumCodePageCount = 65536;
  static constexpr uint32_t kMinimumHostProtectionPageSize = 4096;
  static constexpr uint32_t kMaximumHostProtectionPageSize = 64 * 1024;

  uint32_t max_attempts = 16;
  uint64_t max_duration_ticks = 1;
  // Aggregate bound for supplied data-granule pages and distinct cross-thread
  // write backing pages. Captured code granules use max_code_page_count
  // separately.
  uint32_t max_page_count = 32768;
  // Separate from invocation data pages because definitions are cataloged
  // before the selected translation closure is known.
  uint32_t max_code_page_count = kMaximumCodePageCount;
  uint64_t max_access_count = 64'000'000;
  uint32_t max_call_depth = 256;
  uint64_t max_event_count = 128'000'000;
  uint32_t max_function_count = 262144;
  uint32_t host_protection_page_size = kMinimumHostProtectionPageSize;
};

struct GuestInvocationRecorderFunction {
  uint32_t address = 0;
  uint32_t end_address = 0;

  bool operator==(const GuestInvocationRecorderFunction&) const = default;
};

enum class GuestInvocationRecorderMemoryAccess : uint8_t {
  kRead,
  kWrite,
  kReadWrite,
};

enum class GuestInvocationRecorderState : uint8_t {
  kWaitingForOccurrence,
  kRecordingDiscovery,
  kWaitingForDiscoveryAttempt,
  kWaitingForFinalAttempt,
  kRecordingFinalAttempt,
  kComplete,
  kRejected,
};

enum class GuestInvocationRecorderRejection : uint8_t {
  kNone,
  kDeadlineExceeded,
  kAttemptLimit,
  kPageLimit,
  kAccessLimit,
  kCallDepthLimit,
  kEventLimit,
  kFunctionLimit,
  kInvalidEvent,
  kUnsupportedDependency,
  kRecursiveInvocation,
  kUnbalancedReturn,
  kAsyncReentry,
  kSelfModifyingCode,
  kCrossThreadMutation,
  kIncompletePageDiscovery,
  kIncompleteTranslationClosure,
  kPageReadFailure,
};

class GuestInvocationRecorderPageReader {
 public:
  virtual ~GuestInvocationRecorderPageReader() = default;
  virtual bool ReadPage(uint32_t page_address,
                        std::array<uint8_t, 4096>* output) = 0;
  // Queried only immediately after ReadPage returns false on the recorder's
  // serialized callback stream. A retryable failure must not have modified
  // the output and may be retried by Poll without rejecting the capture.
  virtual bool last_read_was_retryable() const { return false; }
};

class GuestInvocationRecorderClock {
 public:
  virtual ~GuestInvocationRecorderClock() = default;
  virtual uint64_t NowTicks() const = 0;
};

struct GuestInvocationRecorderResult {
  GuestFunctionInvocation invocation;
  GuestInvocationRecorderIdentity owner;

  // The transitive closure of successfully defined functions in the exact
  // definition order reported to the registry. A demand JIT declares call
  // targets it never translates; those carry no emitted code and are excluded.
  // It may include functions that were never entered and remains separate from
  // the runtime call tree so an exact-corpus builder can reproduce code
  // placement.
  std::vector<GuestInvocationRecorderFunction> translation_dependencies;
  // Call targets a closure member declared but the registry never defined, in
  // address order. A declared-only target that is *called* is safe to drop,
  // because reaching it enters it and reseeds the closure from its own
  // definition. A declared-only target the backend *inlines* is not: it never
  // executes as a function, is never entered, and yet its identity and
  // metadata are still required to reproduce the inline. The recorder cannot
  // tell the two apart, so it reports the addresses and leaves identifying the
  // inlinable ones to the layer that can read their metadata.
  std::vector<uint32_t> declared_only_dependencies;
  // Immutable guest code pages sampled immediately after successful
  // translation and before backend publication. Only pages required by the
  // selected translation closure are retained, in address order.
  std::vector<GuestInvocationPage> code_pages;
  std::vector<GuestInvocationRecorderFunction> entered_functions;
  std::vector<uint32_t> touched_page_addresses;
  uint32_t attempt_count = 0;
};

// Platform-neutral state machine for a bounded single-root invocation capture.
// This class does not install execution or memory hooks and is intentionally
// not thread-safe. The hook/service layer must serialize every method call onto
// one coordinator stream. Identity fields describe the originating guest
// context/thread; they do not make concurrent host calls safe. Reentrant event
// callbacks on that stream are rejected. The coordinator must Poll even when
// it has no relevant event to report so ignored title traffic cannot suspend
// deadline enforcement.
//
// Create the recorder before title translation. Report every dependency edge
// before the successful definition of its source, then report that definition
// exactly once. OnFunctionDefined seals the source's dependency list and its
// callback order is the successful-definition order. Runtime callbacks must be
// in guest execution order. OnFunctionEntry and OnMemoryAccess must run before
// the named function body/access; OnFunctionExit must run after the normal
// return state is materialized. This lets the final root entry snapshot every
// discovered page before the body can mutate it.
class GuestInvocationRecorder {
 public:
  static std::unique_ptr<GuestInvocationRecorder> Create(
      const GuestInvocationRecorderSelection& selection,
      const GuestInvocationRecorderLimits& limits,
      GuestInvocationRecorderPageReader& page_reader,
      const GuestInvocationRecorderClock& clock, std::string* error = nullptr);

  ~GuestInvocationRecorder();
  GuestInvocationRecorder(const GuestInvocationRecorder&) = delete;
  GuestInvocationRecorder& operator=(const GuestInvocationRecorder&) = delete;

  bool Poll();
  bool OnFunctionDependency(uint32_t source_address,
                            uint32_t dependency_address);
  bool OnFunctionDefined(uint32_t address, uint32_t end_address);
  bool OnFunctionEntry(const GuestInvocationRecorderIdentity& identity,
                       uint32_t address, uint32_t end_address,
                       const GuestPPCRegisterState& state);
  bool OnFunctionExit(const GuestInvocationRecorderIdentity& identity,
                      uint32_t address, uint32_t return_address,
                      const GuestPPCRegisterState& state);
  bool OnMemoryAccess(const GuestInvocationRecorderIdentity& identity,
                      uint32_t address, uint32_t size,
                      GuestInvocationRecorderMemoryAccess access);
  bool OnUnsupportedDependency(const GuestInvocationRecorderIdentity& identity,
                               uint32_t dependency_flags);
  bool OnTailCall(const GuestInvocationRecorderIdentity& identity,
                  uint32_t from_address, uint32_t target_address);
  bool OnUnwindOrLongjmp(const GuestInvocationRecorderIdentity& identity);
  bool OnAsyncReentry(const GuestInvocationRecorderIdentity& identity);

  GuestInvocationRecorderState state() const;
  // A definition whose code pages were contended when it was registered is
  // snapshotted later by Poll. Entry rejects while any remain outstanding.
  bool has_pending_definition_snapshots() const;
  bool is_recording_attempt() const;
  bool is_recording_identity(
      const GuestInvocationRecorderIdentity& identity) const;
  GuestInvocationRecorderRejection rejection() const;
  uint32_t rejected_dependency_flags() const;
  const std::string& rejection_message() const;
  const GuestInvocationRecorderResult* result() const;

 private:
  struct Impl;
  explicit GuestInvocationRecorder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_INVOCATION_RECORDER_H_
