/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/thread_state.h"

#if XE_ARCH_ARM64 && XE_COMPILER_MSVC
#include <intrin.h>
#endif

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::testing;

namespace {

#if XE_ARCH_ARM64

uint64_t ReadHostFpcr() {
#if XE_COMPILER_MSVC
  return _ReadStatusReg(0x5A20);
#else
  uint64_t value;
  __asm__ volatile("mrs %0, fpcr" : "=r"(value));
  return value;
#endif
}

uint64_t ReadHostFpsr() {
#if XE_COMPILER_MSVC
  return _ReadStatusReg(0x5A21);
#else
  uint64_t value;
  __asm__ volatile("mrs %0, fpsr" : "=r"(value));
  return value;
#endif
}

void WriteHostFpcr(uint64_t value) {
#if XE_COMPILER_MSVC
  _WriteStatusReg(0x5A20, value);
#else
  __asm__ volatile("msr fpcr, %0" : : "r"(value));
#endif
}

void WriteHostFpsr(uint64_t value) {
#if XE_COMPILER_MSVC
  _WriteStatusReg(0x5A21, value);
#else
  __asm__ volatile("msr fpsr, %0" : : "r"(value));
#endif
}

class ScopedHostFloatingPointState {
 public:
  ScopedHostFloatingPointState()
      : fpcr_(ReadHostFpcr()), fpsr_(ReadHostFpsr()) {}
  ~ScopedHostFloatingPointState() {
    WriteHostFpcr(fpcr_);
    WriteHostFpsr(fpsr_);
  }

 private:
  uint64_t fpcr_;
  uint64_t fpsr_;
};

constexpr std::array<uint32_t, 8> kExpectedScalarControls = {
    (0b00u << 22),
    (0b11u << 22),
    (0b01u << 22),
    (0b10u << 22),
    (0b00u << 22) | (1u << 24),
    (0b11u << 22) | (1u << 24),
    (0b01u << 22) | (1u << 24),
    (0b10u << 22) | (1u << 24),
};

#elif XE_ARCH_AMD64

class ScopedHostFloatingPointState {
 public:
  ScopedHostFloatingPointState() : mxcsr_(_mm_getcsr()) {}
  ~ScopedHostFloatingPointState() { _mm_setcsr(mxcsr_); }

 private:
  uint32_t mxcsr_;
};

constexpr std::array<uint32_t, 8> kExpectedScalarControls = {
    0x1F80, 0x7F80, 0x5F80, 0x3F80, 0x9F80, 0xFF80, 0xDF80, 0xBF80,
};

#endif

}  // namespace

TEST_CASE("GUEST_INVOCATION_UNSUPPORTED_BACKEND_FAILS_CLOSED",
          "[backend][replay]") {
  xe::cpu::backend::NullBackend backend;
  ppc::PPCContext context = {};
  REQUIRE_FALSE(backend.ResetGuestInvocationReplayState(nullptr));
  REQUIRE_FALSE(backend.ResetGuestInvocationReplayState(&context));
}

TEST_CASE("GUEST_INVOCATION_BACKEND_RESET", "[backend][replay]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend_owner = CreateBackend();
  REQUIRE(backend_owner);
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend_owner));

  constexpr uint32_t kStackSize = 64 * 1024;
  const uint32_t stack_address = memory->SystemHeapAlloc(kStackSize);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + kStackSize);
  auto* context = thread_state->context();
  ScopedHostFloatingPointState host_fp_guard;
  const uint32_t saved_thread_id = context->thread_id;
  auto* const saved_global_mutex = context->global_mutex;
  Processor* const saved_processor = context->processor;
  auto* const saved_kernel_state = context->kernel_state;
  uint8_t* const saved_physical_membase = context->physical_membase;
  ThreadState* const saved_thread_state = context->thread_state;
  uint8_t* const saved_virtual_membase = context->virtual_membase;
  uint8_t* const saved_trace_counts = context->trace_counts;

#if XE_ARCH_ARM64
  using namespace xe::cpu::backend::a64;
  auto* backend = static_cast<A64Backend*>(processor->backend());
  auto* backend_context = backend->BackendContextForGuestContext(context);

  std::array<uint8_t, sizeof(backend_context->est_consts)> saved_est_consts;
  std::memcpy(saved_est_consts.data(), backend_context->est_consts,
              saved_est_consts.size());
  ReserveHelper* const saved_reserve_helper = backend_context->reserve_helper_;
  uint64_t* const saved_guest_tick_count = backend_context->guest_tick_count;
  const uint64_t saved_indirection_table_bias =
      backend_context->indirection_table_bias;
  const uint64_t saved_code_execute_base = backend_context->code_execute_base;
  const uint64_t saved_external_indirection_table =
      backend_context->external_indirection_table;
  const uint64_t saved_guest_to_host_thunk =
      backend_context->guest_to_host_thunk_address;
  const uint64_t saved_guest_to_host_thunk_no_vec =
      backend_context->guest_to_host_thunk_no_vec_address;
  const uint32_t saved_ox1000 = backend_context->Ox1000;
  const uint32_t saved_fpcr_vmx_daz = backend_context->fpcr_vmx_daz;

  auto poison_and_reset = [&](uint32_t rn, bool non_ieee, bool njm) {
    std::memset(backend_context->helper_scratch_v128s, 0xA5,
                sizeof(backend_context->helper_scratch_v128s));
    std::memset(backend_context->helper_scratch_u64s, 0x5A,
                sizeof(backend_context->helper_scratch_u64s));
    backend_context->cached_reserve_value_ = 0x0123456789ABCDEFull;
    backend_context->stackpoint_head =
        reinterpret_cast<const A64StackpointNode*>(uintptr_t{0x1110});
    backend_context->reserve_address = 0x89ABCDEFu;
    backend_context->reserve_generation = 0x76543210u;
    backend_context->pending_stackpoint_sync_node =
        reinterpret_cast<const A64StackpointNode*>(uintptr_t{0x2220});
    backend_context->fpcr_fpu = 0xFFFFFFFFu;
    backend_context->fpcr_vmx = 0xFFFFFFFFu;
    backend_context->flags = 0xFFFFFFFFu;
    backend_context->db16cyc_spins = 0x11111111u;
    backend_context->db16cyc_last_tick = 0x2222222233333333ull;
    backend_context->spin_wait_spins = 0x44444444u;
    backend_context->spin_wait_site = 0x55555555u;
    backend_context->spin_wait_armed = 0x66666666u;
    backend_context->spin_wait_reset_tick = 0x7777777788888888ull;

    context->preempt_requested = 1;
    context->scratch = 0x99999999AAAAAAAAull;
    context->last_safepoint_pc = 0xBBBBBBBBu;
    context->fpscr.value = 0xA5A5A5A0u;
    context->fpscr.bits.rn = rn;
    context->fpscr.bits.ni = non_ieee ? 1 : 0;
    const uint32_t captured_fpscr = context->fpscr.value;
    std::memset(&context->vscr_vec, 0xC3, sizeof(context->vscr_vec));
    context->vscr_vec.u32[3] = njm ? 0x5A010000u : 0x5A000000u;
    const vec128_t captured_vscr = context->vscr_vec;

    WriteHostFpcr((0b11u << 22) | (1u << 24));
    WriteHostFpsr(0x0800001Fu);

    REQUIRE(backend->ResetGuestInvocationReplayState(context));
    const uint64_t reset_host_fpcr = ReadHostFpcr();
    const uint64_t reset_host_fpsr = ReadHostFpsr();
    REQUIRE(context->fpscr.value == captured_fpscr);
    REQUIRE(std::memcmp(&context->vscr_vec, &captured_vscr,
                        sizeof(captured_vscr)) == 0);
    REQUIRE(context->preempt_requested == 0);
    REQUIRE(context->scratch == 0);
    REQUIRE(context->last_safepoint_pc == 0);

    const std::array<uint8_t, sizeof(backend_context->helper_scratch_v128s)>
        zero_v128_scratch = {};
    const std::array<uint8_t, sizeof(backend_context->helper_scratch_u64s)>
        zero_integer_scratch = {};
    REQUIRE(std::memcmp(backend_context->helper_scratch_v128s,
                        zero_v128_scratch.data(),
                        zero_v128_scratch.size()) == 0);
    REQUIRE(std::memcmp(backend_context->helper_scratch_u64s,
                        zero_integer_scratch.data(),
                        zero_integer_scratch.size()) == 0);
    REQUIRE(backend_context->cached_reserve_value_ == 0);
    REQUIRE(backend_context->stackpoint_head == nullptr);
    REQUIRE(backend_context->reserve_address == 0);
    REQUIRE(backend_context->reserve_generation == 0);
    REQUIRE(backend_context->pending_stackpoint_sync_node == nullptr);
    REQUIRE(backend_context->db16cyc_spins == 0);
    REQUIRE(backend_context->db16cyc_last_tick == 0);
    REQUIRE(backend_context->spin_wait_spins == 0);
    REQUIRE(backend_context->spin_wait_site == 0);
    REQUIRE(backend_context->spin_wait_armed == 0);
    REQUIRE(backend_context->spin_wait_reset_tick == 0);

    const uint32_t control = rn | (non_ieee ? 4u : 0u);
    REQUIRE(backend_context->fpcr_fpu == kExpectedScalarControls[control]);
    REQUIRE(backend_context->fpcr_vmx ==
            (njm ? DEFAULT_VMX_FPCR : (DEFAULT_VMX_FPCR & ~(1u << 24))));
    REQUIRE(backend_context->flags ==
            ((njm ? (1u << kA64BackendNJMOn) : 0) |
             (non_ieee ? (1u << kA64BackendNonIEEEMode) : 0)));
    REQUIRE(reset_host_fpcr == kExpectedScalarControls[control]);
    REQUIRE(reset_host_fpsr == 0);
  };

  poison_and_reset(2, true, true);
  poison_and_reset(3, false, false);
  REQUIRE_FALSE(backend->ResetGuestInvocationReplayState(nullptr));

  REQUIRE(std::memcmp(saved_est_consts.data(), backend_context->est_consts,
                      saved_est_consts.size()) == 0);
  REQUIRE(backend_context->reserve_helper_ == saved_reserve_helper);
  REQUIRE(backend_context->guest_tick_count == saved_guest_tick_count);
  REQUIRE(backend_context->indirection_table_bias ==
          saved_indirection_table_bias);
  REQUIRE(backend_context->code_execute_base == saved_code_execute_base);
  REQUIRE(backend_context->external_indirection_table ==
          saved_external_indirection_table);
  REQUIRE(backend_context->guest_to_host_thunk_address ==
          saved_guest_to_host_thunk);
  REQUIRE(backend_context->guest_to_host_thunk_no_vec_address ==
          saved_guest_to_host_thunk_no_vec);
  REQUIRE(backend_context->Ox1000 == saved_ox1000);
  REQUIRE(backend_context->fpcr_vmx_daz == saved_fpcr_vmx_daz);

#elif XE_ARCH_AMD64
  using namespace xe::cpu::backend::x64;
  auto* backend = static_cast<X64Backend*>(processor->backend());
  auto* backend_context = backend->BackendContextForGuestContext(context);

  ReserveHelper* const saved_reserve_helper = backend_context->reserve_helper_;
  uint64_t* const saved_guest_tick_count = backend_context->guest_tick_count;
  X64BackendStackpoint* const saved_stackpoints = backend_context->stackpoints;
  const uint32_t saved_ox1000 = backend_context->Ox1000;
  const uint32_t saved_mxcsr_vmx_daz = backend_context->mxcsr_vmx_daz;

  auto poison_and_reset = [&](uint32_t rn, bool non_ieee, bool njm) {
    std::memset(backend_context->helper_scratch_u64s, 0xA5,
                sizeof(backend_context->helper_scratch_u64s));
    backend_context->cached_reserve_value_ = 0x0123456789ABCDEFull;
    backend_context->reserve_address = 0x89ABCDEFu;
    backend_context->reserve_generation = 0x76543210u;
    backend_context->current_stackpoint_depth = 0x11111111u;
    backend_context->mxcsr_fpu = 0xFFFFFFFFu;
    backend_context->mxcsr_vmx = 0xFFFFFFFFu;
    backend_context->flags = 0xFFFFFFFFu;

    context->preempt_requested = 1;
    context->scratch = 0x99999999AAAAAAAAull;
    context->last_safepoint_pc = 0xBBBBBBBBu;
    context->fpscr.value = 0xA5A5A5A0u;
    context->fpscr.bits.rn = rn;
    context->fpscr.bits.ni = non_ieee ? 1 : 0;
    const uint32_t captured_fpscr = context->fpscr.value;
    std::memset(&context->vscr_vec, 0xC3, sizeof(context->vscr_vec));
    context->vscr_vec.u32[3] = njm ? 0x5A010000u : 0x5A000000u;
    const vec128_t captured_vscr = context->vscr_vec;

    // Status flags, DAZ, FZ and a different rounding mode are all dirty.
    _mm_setcsr(0xFFFFu);

    REQUIRE(backend->ResetGuestInvocationReplayState(context));
    const uint32_t reset_host_mxcsr = _mm_getcsr();
    REQUIRE(context->fpscr.value == captured_fpscr);
    REQUIRE(std::memcmp(&context->vscr_vec, &captured_vscr,
                        sizeof(captured_vscr)) == 0);
    REQUIRE(context->preempt_requested == 0);
    REQUIRE(context->scratch == 0);
    REQUIRE(context->last_safepoint_pc == 0);

    const std::array<uint8_t, sizeof(backend_context->helper_scratch_u64s)>
        zero_scratch = {};
    REQUIRE(std::memcmp(backend_context->helper_scratch_u64s,
                        zero_scratch.data(), zero_scratch.size()) == 0);
    REQUIRE(backend_context->cached_reserve_value_ == 0);
    REQUIRE(backend_context->reserve_address == 0);
    REQUIRE(backend_context->reserve_generation == 0);
    REQUIRE(backend_context->current_stackpoint_depth == 0);

    const uint32_t control = rn | (non_ieee ? 4u : 0u);
    REQUIRE(backend_context->mxcsr_fpu == kExpectedScalarControls[control]);
    REQUIRE(backend_context->mxcsr_vmx ==
            (njm ? DEFAULT_VMX_MXCSR : _MM_MASK_MASK));
    REQUIRE(backend_context->flags ==
            ((njm ? (1u << kX64BackendNJMOn) : 0) |
             (non_ieee ? (1u << kX64BackendNonIEEEMode) : 0)));
    REQUIRE(reset_host_mxcsr == kExpectedScalarControls[control]);
  };

  poison_and_reset(2, true, true);
  poison_and_reset(3, false, false);
  REQUIRE_FALSE(backend->ResetGuestInvocationReplayState(nullptr));

  REQUIRE(backend_context->reserve_helper_ == saved_reserve_helper);
  REQUIRE(backend_context->guest_tick_count == saved_guest_tick_count);
  REQUIRE(backend_context->stackpoints == saved_stackpoints);
  REQUIRE(backend_context->Ox1000 == saved_ox1000);
  REQUIRE(backend_context->mxcsr_vmx_daz == saved_mxcsr_vmx_daz);
#else
  FAIL("Guest invocation replay reset is unsupported on this architecture");
#endif

  REQUIRE(context->thread_id == saved_thread_id);
  REQUIRE(context->global_mutex == saved_global_mutex);
  REQUIRE(context->processor == saved_processor);
  REQUIRE(context->kernel_state == saved_kernel_state);
  REQUIRE(context->physical_membase == saved_physical_membase);
  REQUIRE(context->thread_state == saved_thread_state);
  REQUIRE(context->virtual_membase == saved_virtual_membase);
  REQUIRE(context->trace_counts == saved_trace_counts);

  thread_state.reset();
  memory->SystemHeapFree(stack_address);
}
