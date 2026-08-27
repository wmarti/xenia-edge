/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/function.h"

#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/processor.h"
#endif
#include "xenia/cpu/symbol.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

#if XE_OPTION_PROFILING
// Diagnostic: per-guest-address CPU scope for guest function dispatch. Only
// catches host->guest entries and re-entrant callbacks, not direct guest->guest
// JIT calls.
namespace {
struct GuestFnScope {
  MicroProfileToken token;
  uint64_t tick;
  explicit GuestFnScope(uint32_t address)
      : token(GetGuestFunctionToken(address)) {
    tick = MicroProfileEnter(token);
  }
  ~GuestFnScope() { MicroProfileLeave(token, tick); }
};
}  // namespace
#define GUEST_FN_SCOPE(address) GuestFnScope guest_fn_scope_(address)
#else
#define GUEST_FN_SCOPE(address)
#endif

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
namespace {

class GuestExecutionCaptureHostCallGuard {
 public:
  GuestExecutionCaptureHostCallGuard(GuestFunction& function,
                                     ThreadState& thread_state,
                                     uint32_t return_address,
                                     ThreadState* original_thread_state)
      : function_(function),
        thread_state_(thread_state),
        original_thread_state_(original_thread_state),
        token_(thread_state.processor()->BeginGuestExecutionCaptureHostCall(
            thread_state, function, return_address)) {}

  ~GuestExecutionCaptureHostCallGuard() {
    if (token_) {
      thread_state_.processor()->EndGuestExecutionCaptureHostCall(
          token_, thread_state_, function_, outcome_);
    }
    if (original_thread_state_ != &thread_state_) {
      ThreadState::Bind(original_thread_state_);
    }
  }

  GuestExecutionCaptureHostCallGuard(
      const GuestExecutionCaptureHostCallGuard&) = delete;
  GuestExecutionCaptureHostCallGuard& operator=(
      const GuestExecutionCaptureHostCallGuard&) = delete;

  void SetResult(bool entered) {
    outcome_ = entered ? GuestExecutionCaptureHostCallOutcome::kReturnedToHost
                       : GuestExecutionCaptureHostCallOutcome::kFailedToEnter;
  }

 private:
  GuestFunction& function_;
  ThreadState& thread_state_;
  ThreadState* original_thread_state_ = nullptr;
  GuestExecutionCaptureHostCallToken token_;
  GuestExecutionCaptureHostCallOutcome outcome_ =
      GuestExecutionCaptureHostCallOutcome::kAbortedByHostUnwind;
};

}  // namespace
#endif

Function::Function(Module* module, uint32_t address)
    : Symbol(Symbol::Type::kFunction, module, address) {}

Function::~Function() = default;

BuiltinFunction::BuiltinFunction(Module* module, uint32_t address)
    : Function(module, address) {
  behavior_ = Behavior::kBuiltin;
}

BuiltinFunction::~BuiltinFunction() = default;

void BuiltinFunction::SetupBuiltin(Handler handler, void* arg0, void* arg1) {
  behavior_ = Behavior::kBuiltin;
  handler_ = handler;
  arg0_ = arg0;
  arg1_ = arg1;
}

bool BuiltinFunction::Call(ThreadState* thread_state, uint32_t return_address) {
  GUEST_FN_SCOPE(address());

  ThreadState* original_thread_state = ThreadState::Get();
  if (original_thread_state != thread_state) {
    ThreadState::Bind(thread_state);
  }

  assert_not_null(handler_);
  handler_(thread_state->context(), arg0_, arg1_);

  if (original_thread_state != thread_state) {
    ThreadState::Bind(original_thread_state);
  }

  return true;
}

GuestFunction::GuestFunction(Module* module, uint32_t address)
    : Function(module, address) {
  behavior_ = Behavior::kDefault;
}

GuestFunction::~GuestFunction() = default;

void GuestFunction::SetupExtern(ExternHandler handler, Export* export_data) {
  behavior_ = Behavior::kExtern;
  extern_handler_ = handler;
  export_data_ = export_data;
}

const SourceMapEntry* GuestFunction::LookupGuestAddress(
    uint32_t guest_address) const {
  // TODO(benvanik): binary search? We know the list is sorted by code order.
  for (size_t i = 0; i < source_map_.size(); ++i) {
    const auto& entry = source_map_[i];
    if (entry.guest_address == guest_address) {
      return &entry;
    }
  }
  return nullptr;
}

const SourceMapEntry* GuestFunction::LookupHIROffset(uint32_t offset) const {
  // TODO(benvanik): binary search? We know the list is sorted by code order.
  for (size_t i = 0; i < source_map_.size(); ++i) {
    const auto& entry = source_map_[i];
    if (entry.hir_offset >= offset) {
      return &entry;
    }
  }
  return nullptr;
}

const SourceMapEntry* GuestFunction::LookupMachineCodeOffset(
    uint32_t offset) const {
  // TODO(benvanik): binary search? We know the list is sorted by code order.
  for (int64_t i = source_map_.size() - 1; i >= 0; --i) {
    const auto& entry = source_map_[i];
    if (entry.code_offset <= offset) {
      return &entry;
    }
  }
  return source_map_.empty() ? nullptr : &source_map_[0];
}

uint32_t GuestFunction::MapGuestAddressToMachineCodeOffset(
    uint32_t guest_address) const {
  auto entry = LookupGuestAddress(guest_address);
  return entry ? entry->code_offset : 0;
}

uintptr_t GuestFunction::MapGuestAddressToMachineCode(
    uint32_t guest_address) const {
  auto entry = LookupGuestAddress(guest_address);

  if (entry) {
    return reinterpret_cast<uintptr_t>(machine_code()) + entry->code_offset;
  } else {
    return 0;
  }
}

uint32_t GuestFunction::MapMachineCodeToGuestAddress(
    uintptr_t host_address) const {
  void* code = machine_code();
  if (!code) {
    return address();
  }
  auto entry = LookupMachineCodeOffset(
      static_cast<uint32_t>(host_address - reinterpret_cast<uintptr_t>(code)));
  return entry ? entry->guest_address : address();
}

bool GuestFunction::Call(ThreadState* thread_state, uint32_t return_address) {
  GUEST_FN_SCOPE(address());

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  ThreadState* original_thread_state = ThreadState::Get();
  if (original_thread_state != thread_state) {
    ThreadState::Bind(thread_state);
  }
  GuestExecutionCaptureHostCallGuard capture_guard(
      *this, *thread_state, return_address, original_thread_state);
  const bool result = CallImpl(thread_state, return_address);
  capture_guard.SetResult(result);
  return result;
#else
  ThreadState* original_thread_state = ThreadState::Get();
  if (original_thread_state != thread_state) {
    ThreadState::Bind(thread_state);
  }

  bool result = CallImpl(thread_state, return_address);

  if (original_thread_state != thread_state) {
    ThreadState::Bind(original_thread_state);
  }

  return result;
#endif
}

}  // namespace cpu
}  // namespace xe
