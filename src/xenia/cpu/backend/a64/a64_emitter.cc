/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_emitter.h"

#include <cstring>
#include <string>

#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/backend/a64/a64_guest_invocation_capture.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/guest_invocation_recorder.h"
#endif
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

DECLARE_bool(a64_enable_host_guest_stack_synchronization);

DECLARE_bool(log_safepoint_pc);
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
DECLARE_bool(guest_scheduler);
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace Xbyak_aarch64;

// Defined in a64_backend.cc.
extern uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

static uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {
  auto function = reinterpret_cast<Function*>(function_ptr);
  XELOGE("undefined extern call to {:08X} {}", function->address(),
         function->name());
  return 0;
}

static constexpr size_t kMaxCodeSize = 1_MiB;

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
void A64Emitter::EmitGuestInvocationCaptureEventGuard(
    uint32_t event_bit, const Xbyak_aarch64::Label& skip) {
  constexpr uint32_t kControlOffset = static_cast<uint32_t>(
      offsetof(ppc::PPCContext, guest_invocation_capture_control));
  static_assert(!(kControlOffset & 7) && kControlOffset <= 32760);
  ldr(x8, ptr(x20, kControlOffset));
  tbz_near(x8, kGuestInvocationCaptureControlMaskShift + event_bit, skip);
}

void A64Emitter::EmitGuestInvocationCaptureFunctionEntryGuard(
    uint32_t function_address, const Xbyak_aarch64::Label& skip) {
  Label capture;
  constexpr uint32_t kControlOffset = static_cast<uint32_t>(
      offsetof(ppc::PPCContext, guest_invocation_capture_control));
  static_assert(!(kControlOffset & 7) && kControlOffset <= 32760);
  ldr(x8, ptr(x20, kControlOffset));
  tbnz_near(x8,
            kGuestInvocationCaptureControlMaskShift +
                kGuestInvocationCaptureOwnerEventBit,
            capture);
  tbz_near(x8,
           kGuestInvocationCaptureControlMaskShift +
               kGuestInvocationCaptureRootEventBit,
           skip);
  cbz_near(w8, capture);
  mov(w9, function_address);
  cmp(w8, w9);
  b_near(NE, skip);
  L(capture);
}
#endif

// Register maps:
// GPR allocatable registers: x22, x23, x24, x25, x26, x27, x28
// (x19=backend context, x20=context, x21=membase are reserved)
const uint32_t A64Emitter::gpr_reg_map_[GPR_COUNT] = {
    22, 23, 24, 25, 26, 27, 28,
};

// VEC allocatable registers: v4-v15, v16-v31
// (v0-v3 are scratch)
const uint32_t A64Emitter::vec_reg_map_[VEC_COUNT] = {
    4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

A64Emitter::A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator)
    : CodeGenerator(kMaxCodeSize, Xbyak_aarch64::AutoGrow, allocator),
      processor_(backend->processor()),
      backend_(backend),
      code_cache_(backend->code_cache()),
      allocator_(allocator),
      feature_flags_(arm64::GetFeatureFlags()) {}

A64Emitter::~A64Emitter() = default;

bool A64Emitter::Emit(GuestFunction* function, hir::HIRBuilder* builder,
                      uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
                      void** out_code_address, size_t* out_code_size,
                      std::vector<SourceMapEntry>* out_source_map) {
  SCOPE_profile_cpu_f("cpu");

  guest_module_ = function->module();

  debug_info_ = debug_info;
  debug_info_flags_ = debug_info_flags;
  coverage_offset_ = function->coverage_offset();
  coverage_start_address_ = function->address();
  coverage_instruction_count_ =
      function->has_end_address()
          ? (function->end_address() - function->address()) / 4 + 1
          : 0;
  coverage_current_index_ = UINT32_MAX;
  coverage_out_of_range_ = false;
  sequence_samples_.clear();

  current_guest_function_ = function->address();
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  current_guest_function_end_ = function->end_address();
#endif

  // Reset state.
  stack_size_ = StackLayout::GUEST_STACK_SIZE;
  source_map_arena_.Reset();
  tail_code_.clear();
  label_bind_offsets_.clear();
  // Every path into a guest function runs in the scalar FPU mode: the
  // host-to-guest thunk restores fpcr_fpu on entry, every call site emits
  // EnsureFpuFpcrModeForTransition, every callee restores FPU before
  // returning, the guest-to-host thunk restores fpcr_fpu after host
  // callbacks, and the resolve thunk restores it after the host resolve.
  // Starting Known-Fpu lets pure-integer functions skip both the first
  // ChangeFpcrMode(Fpu) msr and the exit transition guard.
  fpcr_mode_ = FPCRMode::Fpu;

  // The prolog, epilog and helpers emit outside the per-opcode guard below, so
  // an unencodable operand needs catching here too.
  EmitFunctionInfo func_info = {};
  bool emitted = false;
  try {
    emitted = Emit(builder, func_info);
  } catch (const Xbyak_aarch64::Error& e) {
    XELOGE("A64: assembler error while emitting guest function {:08X}: {}",
           current_guest_function_, e.what());
    emitted = false;
  }
  if (!emitted) {
    // Emplace only runs on success, so a failed compile has to reset too.
    ResetPerFunctionState();
    return false;
  }

  // Emplace the code into the code cache.
  *out_code_address = Emplace(func_info, function);
  *out_code_size = func_info.code_size.total;

  // Copy source map.
  source_map_arena_.CloneContents(out_source_map);

  if (!sequence_samples_.empty()) {
    processor_->RecordSequenceSamples(function->address(),
                                      std::move(sequence_samples_));
    sequence_samples_.clear();
  }

  return *out_code_address != nullptr;
}

bool A64Emitter::Emit(hir::HIRBuilder* builder, EmitFunctionInfo& func_info) {
  // Decide up front whether single-instruction branches to tail labels are
  // safe; the prolog's stackpoint check needs the answer too. cbnz/b.cond
  // reach +/-1 MiB and the distance to a tail is bounded by the rest of the
  // body plus every tail block. 256 bytes per HIR instruction is far above
  // the fattest sequence this backend emits, so a function passing this test
  // cannot place any tail out of range. Functions failing it (which would
  // need ~3000+ HIR instructions) keep the expanded far form.
  function_has_vmx_ = false;
  expected_preds_.clear();
  incoming_fpcr_.clear();
  label_block_.clear();
  {
    // Labels resolve to blocks through each block's own label chain - the
    // same mapping the emitter binds branch targets from. A label's ->block
    // back-pointer can be stale after the passes reshape blocks, exactly like
    // the edges ControlFlowAnalysisPass recorded (which also never included
    // fall-through), so neither is trusted here.
    auto& label_block = label_block_;
    for (auto* b = builder->first_block(); b; b = b->next) {
      for (auto* label = b->label_head; label; label = label->next) {
        label_block[label] = b;
      }
    }
    size_t hir_instr_count = 0;
    for (auto* b = builder->first_block(); b; b = b->next) {
      // Expected predecessor count, recomputed from the final HIR. Branches
      // can sit MID-block (the not-taken path continues inside the same
      // block), so every instruction is scanned, not just the tail - the
      // edges ControlFlowAnalysisPass recorded are both stale and
      // tail-only.
      for (auto* i = b->instr_head; i; i = i->next) {
        const hir::Label* label = nullptr;
        if (i->opcode == &hir::OPCODE_BRANCH_info) {
          label = i->src1.label;
        } else if (i->opcode == &hir::OPCODE_BRANCH_TRUE_info ||
                   i->opcode == &hir::OPCODE_BRANCH_FALSE_info) {
          label = i->src2.label;
        }
        if (label) {
          auto it = label_block.find(label);
          if (it != label_block.end()) {
            ++expected_preds_[it->second];
          }
        }
      }
      // Fall-through exists unless the block's final instruction is an
      // unconditional branch.
      auto* last = b->instr_tail;
      if (b->next && !(last && last->opcode == &hir::OPCODE_BRANCH_info)) {
        ++expected_preds_[b->next];
      }
      for (auto* i = b->instr_head; i; i = i->next) {
        ++hir_instr_count;
        if (i->dest && i->dest->type == hir::VEC128_TYPE) {
          function_has_vmx_ = true;
        } else if (!function_has_vmx_) {
          uint32_t sig = i->opcode->signature;
          const hir::Instr::Op* ops[3] = {&i->src1, &i->src2, &i->src3};
          for (int k = 0; k < 3; ++k) {
            auto t = static_cast<hir::OpcodeSignatureType>(
                (sig >> (3 * (k + 1))) & 0x7);
            if (t == hir::OPCODE_SIG_TYPE_V &&
                ops[k]->value->type == hir::VEC128_TYPE) {
              function_has_vmx_ = true;
              break;
            }
          }
        }
      }
    }
    near_tail_branches_safe_ = hir_instr_count * 256 < (768 * 1024);
    // tbnz reaches only +/-32 KiB, so its tail form needs a tighter bound.
    near_tbz_branches_safe_ = hir_instr_count * 256 < (24 * 1024);
  }

  // Calculate local variable stack offsets.
  auto locals = builder->locals();
  size_t stack_offset = StackLayout::GUEST_STACK_SIZE;
  for (auto it = locals.begin(); it != locals.end(); ++it) {
    auto slot = *it;
    size_t type_size = hir::GetTypeSize(slot->type);
    // Align to natural size (at least 4 bytes for ARM64 alignment).
    size_t align_size = xe::round_up(type_size, static_cast<size_t>(4));
    stack_offset = xe::align(stack_offset, align_size);
    slot->set_constant(static_cast<uint32_t>(stack_offset));
    stack_offset += type_size;
  }
  // Align total stack offset to 16 bytes (ARM64 ABI requirement).
  stack_offset -= StackLayout::GUEST_STACK_SIZE;
  stack_offset = xe::align(stack_offset, static_cast<size_t>(16));

  const size_t stack_size = StackLayout::GUEST_STACK_SIZE + stack_offset;
  // ARM64 ABI: SP must always be 16-byte aligned.
  assert_true(stack_size % 16 == 0);
  func_info.stack_size = stack_size;
  func_info.lr_save_offset = StackLayout::HOST_RET_ADDR;
  stack_size_ = stack_size;

  struct {
    size_t prolog;
    size_t body;
    size_t epilog;
    size_t tail;
    size_t prolog_stack_alloc;
  } code_offsets = {};

  // ========================================================================
  // PROLOG
  // ========================================================================
  code_offsets.prolog = getSize();

  // sub sp, sp, #stack_size
  if (stack_size <= 4095) {
    sub(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    sub(sp, sp, x17, UXTX);
  }
  code_offsets.prolog_stack_alloc = getSize();

  // Store the guest PPC return address (passed in x0 by convention) and the
  // host return address (x30/LR) with one paired store; the layout keeps the
  // two slots adjacent for exactly this.
  static_assert(StackLayout::HOST_RET_ADDR == StackLayout::GUEST_RET_ADDR + 8);
  stp(x0, x30, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_RET_ADDR)));

  // Record stackpoint for longjmp recovery. Also zeroes the call-return slot
  // (no call made yet), paired with the entry-depth spill when the
  // synchronizer is on.
  PushStackpoint();

  // ========================================================================
  // BODY
  // ========================================================================
  code_offsets.body = getSize();

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  // The prolog has saved host bookkeeping, but no guest HIR has executed yet.
  // CallNativeSafe preserves every guest-allocatable GPR and vector register
  // while the callback snapshots the fully materialized PPC context.
  Label skip_capture_entry;
  EmitGuestInvocationCaptureFunctionEntryGuard(current_guest_function_,
                                               skip_capture_entry);
  mov(x1, static_cast<uint64_t>(current_guest_function_));
  mov(x2, static_cast<uint64_t>(current_guest_function_end_));
  mov(x3, x8);
  CallNativeSafe(reinterpret_cast<void*>(&CaptureGuestInvocationFunctionEntry));
  L(skip_capture_entry);
#endif

  // FTrace: log guest function entry when the backend was built with
  // function tracing available (gated at runtime by the trace_func flag).
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionEntry));
  }

  // Allocate the epilog label (owned by label_cache_ for cleanup).
  auto epilog_label_ptr = new Label();
  label_cache_.push_back(epilog_label_ptr);
  epilog_label_ = epilog_label_ptr;

  // Walk HIR blocks and emit ARM64 instructions.
  auto block = builder->first_block();
  synchronize_stack_on_next_instruction_ = false;
  while (block) {
    // FPCR tracking across blocks: msr fpcr is a serializing write on most
    // cores, and resetting the mode at every block entry makes the first
    // float op of every block re-emit it. Every branch emission records the
    // tracker's mode at that exact point into the target block's meet (a
    // branch can sit mid-block, so a per-block exit mode would be wrong), and
    // block ends record the fall-through mode. The entry mode carries over
    // only when every expected predecessor edge has contributed - which an
    // unemitted loop back edge has not - and all contributions agree; a call
    // on a predecessor path contributes Unknown, which blocks seeding. Host
    // transitions are guarded independently by
    // EnsureFpuFpcrModeForTransition, so no stale mode can escape into host
    // code.
    {
      // The function's first block starts in the scalar FPU mode by the
      // entry contract (see the initialization above); every other block
      // starts Unknown unless all its predecessor edges agreed.
      FPCRMode incoming =
          (block == builder->first_block()) ? FPCRMode::Fpu : FPCRMode::Unknown;
      auto exp_it = expected_preds_.find(block);
      auto in_it = incoming_fpcr_.find(block);
      if (exp_it != expected_preds_.end() && in_it != incoming_fpcr_.end() &&
          in_it->second.count == exp_it->second) {
        incoming = in_it->second.meet;
      }
      fpcr_mode_ = incoming;
    }
    // Flags from a previous block cannot be trusted either.
    ResetFlagsZeroTest();

    // Bind all labels targeting this block.
    auto label = block->label_head;
    while (label) {
      L(GetLabel(label->id));
      label = label->next;
    }

    // Process each instruction in the block.
    const hir::Instr* instr = block->instr_head;
    while (instr) {
      // After a guest call, check for longjmp on the next real instruction.
      // Skip SOURCE_OFFSET because the return address from the call would
      // point past the check, so it would never execute.
      if (synchronize_stack_on_next_instruction_) {
        // Skip annotations as well as SOURCE_OFFSET: under full debug info
        // the frontend emits COMMENT first, and a check emitted there sits
        // before the recorded source-map offset, so a longjmp repair would
        // land past it and it would never run.
        if (instr->GetOpcodeNum() != hir::OPCODE_SOURCE_OFFSET &&
            instr->GetOpcodeNum() != hir::OPCODE_COMMENT) {
          synchronize_stack_on_next_instruction_ = false;
          EnsureSynchronizedGuestAndHostStack();
          // The helper call clobbers NZCV.
          ResetFlagsZeroTest();
        }
      }
      const hir::Instr* new_tail = instr;
      bool selected = false;
      try {
        selected = SelectSequence(this, instr, &new_tail);
        // One-shot handoff: whatever this sequence declared about NZCV is
        // visible to exactly the next sequence and nothing later.
        ShiftFlagsZeroTest();
        // Record the FPCR mode this branch carries to its target (branches
        // can sit mid-block; the mode here is the mode the jump takes).
        {
          const hir::Label* label = nullptr;
          if (instr->opcode == &hir::OPCODE_BRANCH_info) {
            label = instr->src1.label;
          } else if (instr->opcode == &hir::OPCODE_BRANCH_TRUE_info ||
                     instr->opcode == &hir::OPCODE_BRANCH_FALSE_info) {
            label = instr->src2.label;
          }
          if (label) {
            auto it = label_block_.find(label);
            if (it != label_block_.end()) {
              RecordIncomingFpcr(it->second, fpcr_mode_);
            }
          }
        }
      } catch (const Xbyak_aarch64::Error& e) {
        // Uncaught this aborts the process with no context, so name the opcode
        // and the guest function and fail just this compile.
        XELOGE(
            "A64: assembler rejected HIR opcode {} in guest function {:08X}: "
            "{}",
            hir::GetOpcodeName(instr->GetOpcodeInfo()), current_guest_function_,
            e.what());
        return false;
      }
      if (!selected) {
        // No sequence matched — this is expected in Phase 1 before
        // sequences are implemented.
        XELOGE("A64: Unable to process HIR opcode {}",
               hir::GetOpcodeName(instr->GetOpcodeInfo()));
        return false;
      }
      instr = new_tail;
    }

    if (!MaybeFlushV128ConstPool()) {
      return false;
    }

    // Fall-through edge to the next block, unless this block cannot fall
    // through.
    {
      auto* last = block->instr_tail;
      if (block->next && !(last && last->opcode == &hir::OPCODE_BRANCH_info)) {
        RecordIncomingFpcr(block->next, fpcr_mode_);
      }
    }
    block = block->next;
  }

  // ========================================================================
  // EPILOG
  // ========================================================================
  L(*epilog_label_);
  epilog_label_ = nullptr;
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  // Use the return boundary saved by this frame's prolog, not the mutable PPC
  // LR. A function may legally change LR before taking its normal return path.
  Label skip_capture_exit;
  EmitGuestInvocationCaptureEventGuard(kGuestInvocationCaptureOwnerEventBit,
                                       skip_capture_exit);
  mov(x1, static_cast<uint64_t>(current_guest_function_));
  ldr(x2, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  mov(x3, x8);
  CallNativeSafe(reinterpret_cast<void*>(&CaptureGuestInvocationFunctionExit));
  L(skip_capture_exit);
#endif
  // FTrace: log the guest return value (r3) on normal return.
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionReturn));
  }
  code_offsets.epilog = getSize();

  // Pop stackpoint before leaving.
  PopStackpoint();

  // Restore host return address and deallocate stack.
  ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  if (stack_size <= 4095) {
    add(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    add(sp, sp, x17, UXTX);
  }
  ret();

  // ========================================================================
  // TAIL CODE
  // ========================================================================
  for (auto& tail_item : tail_code_) {
    // ARM64 instructions are always 4-byte aligned, so alignment is mostly
    // a no-op unless we want cache-line alignment for hot paths.
    L(tail_item.label);
    const size_t tail_size_before = getSize();
    try {
      tail_item.func(*this, tail_item.label);
    } catch (const Xbyak_aarch64::Error& e) {
      XELOGE("A64: assembler rejected tail code in guest function {:08X}: {}",
             current_guest_function_, e.what());
      return false;
    }
    // Before the pool flush, so the pool's bytes are not charged to whichever
    // sequence happened to register the last tail.
    if (tail_item.sample_index < sequence_samples_.size()) {
      sequence_samples_[tail_item.sample_index].tail_bytes +=
          static_cast<uint32_t>(getSize() - tail_size_before);
    }
    if (!MaybeFlushV128ConstPool()) {
      return false;
    }
  }
  code_offsets.tail = getSize();

  // ========================================================================
  // LITERAL POOL
  // ========================================================================
  if (!FlushV128ConstPool(false)) {
    return false;
  }

  // Fill in EmitFunctionInfo metrics.
  assert_zero(code_offsets.prolog);
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;

  return true;
}

void* A64Emitter::Emplace(const EmitFunctionInfo& func_info,
                          GuestFunction* function) {
  assert_true(func_info.code_size.total == getSize());

  void* new_execute_address;
  void* new_write_address;

  if (function) {
    code_cache_->PlaceGuestCode(
        function->address(),
        const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        function, new_execute_address, new_write_address, false);
  } else {
    code_cache_->PlaceHostCode(
        0, const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        new_execute_address, new_write_address);
  }

  // In xbyak_aarch64, labels are resolved at define time (backpatching),
  // so all relative offsets are already correct. We just need to reset
  // the codegen state for the next function.
  ResetPerFunctionState();

  return new_execute_address;
}

void A64Emitter::ResetPerFunctionState() {
  reset();
  tail_code_.clear();
  // reset() restarts xbyak label ids from 1, so recorded bind offsets from
  // this function must not leak into the next one.
  label_bind_offsets_.clear();

  // Clean up cached labels.
  epilog_label_ = nullptr;
  for (auto* cached_label : label_cache_) {
    delete cached_label;
  }
  label_cache_.clear();
  v128_consts_.clear();
  v128_consts_first_use_ = 0;

  // Clean up HIR->xbyak label map. HIR label ids restart at each function, so
  // stale entries would hand the next function this one's labels.
  for (auto& pair : label_map_) {
    delete pair.second;
  }
  label_map_.clear();
}

void A64Emitter::MarkSourceOffset(const hir::Instr* i) {
  auto entry = source_map_arena_.Alloc<SourceMapEntry>();
  entry->guest_address = static_cast<uint32_t>(i->src1.offset);
  entry->hir_offset = uint32_t(i->block->ordinal << 16) | i->ordinal;
  entry->code_offset = static_cast<uint32_t>(getSize());

  if ((debug_info_flags_ & DebugInfoFlags::kDebugInfoTraceFunctionCoverage) &&
      coverage_offset_ != GuestFunction::kInvalidCoverageOffset) {
    // A source offset is not guaranteed to land inside the range the scanner
    // reported, and counting outside the reserved slice writes through a wild
    // displacement into whatever the arena holds next.
    uint32_t instruction_index =
        (entry->guest_address - coverage_start_address_) / 4;
    if (entry->guest_address < coverage_start_address_ ||
        instruction_index >= coverage_instruction_count_) {
      coverage_current_index_ = UINT32_MAX;
      if (!coverage_out_of_range_) {
        coverage_out_of_range_ = true;
        XELOGW(
            "Coverage: {:08X} is outside {:08X} and the {} instructions after "
            "it, not counting it",
            entry->guest_address, coverage_start_address_,
            coverage_instruction_count_);
      }
      return;
    }
    // Everything emitted from here until the next source offset belongs to
    // this guest instruction.
    coverage_current_index_ = instruction_index;
    const size_t byte_offset =
        coverage_offset_ + static_cast<size_t>(instruction_index) * 8;
    // Counters are per thread, so this needs no atomic. x0 and x1 are scratch
    // and outside gpr_reg_map_ (x22-x28), and this sits on its own
    // OPCODE_SOURCE_OFFSET instruction, so no HIR value is live in either.
    //
    // add rather than adds because NZCV has to survive: a guest compare and
    // the branch consuming it are separate guest instructions, so a source
    // offset lands between them.
    ldr(x0, ptr(GetContextReg(),
                static_cast<int32_t>(offsetof(ppc::PPCContext, trace_counts))));
    mov(x1, static_cast<uint64_t>(byte_offset));
    add(x0, x0, x1);
    ldr(x1, ptr(x0));
    add(x1, x1, 1);
    str(x1, ptr(x0));
  }
}

// Count the MOVZ/MOVK chains in a just-emitted range that build a value wider
// than 32 bits. This is the emit-time counterpart of the replay's classifier in
// ppc_testing_main.cc, and reaches the same answer by a cheaper route: a
// move-wide with hw >= 2 places its immediate at bit 32 or 48, so the value it
// is building cannot fit in 32 bits, and no reconstruction is needed. Counted
// per sequence rather than per function so a chain sitting in one cold sequence
// cannot be charged at a hot sequence's execution rate.
static void CountWideMoveChains(const uint8_t* code, uint32_t bytes,
                                uint16_t* out_chains,
                                uint16_t* out_instructions) {
  uint32_t chains = 0, instructions = 0;
  uint32_t open_reg = 0xFF, open_count = 0;
  bool open_wide = false;
  auto close = [&]() {
    if (open_count && open_wide) {
      ++chains;
      instructions += open_count;
    }
    open_count = 0;
    open_wide = false;
    open_reg = 0xFF;
  };
  for (uint32_t off = 0; off + 4 <= bytes; off += 4) {
    uint32_t insn;
    std::memcpy(&insn, code + off, sizeof(insn));
    // Move-wide immediate group: MOVN/MOVZ/MOVK share bits 28:23 == 100101.
    if ((insn & 0x1F800000u) != 0x12800000u) {
      close();
      continue;
    }
    const uint32_t opc = (insn >> 29) & 0x3u;
    const uint32_t hw = (insn >> 21) & 0x3u;
    const uint32_t rd = insn & 0x1Fu;
    const bool wide = hw >= 2u;
    if (opc == 0x3u && open_count && rd == open_reg) {  // MOVK continuing
      ++open_count;
      open_wide = open_wide || wide;
      continue;
    }
    close();
    open_reg = rd;
    open_count = 1;
    open_wide = wide;
  }
  close();
  *out_chains = static_cast<uint16_t>(chains);
  *out_instructions = static_cast<uint16_t>(instructions);
}

void A64Emitter::RecordSequenceSample(const hir::Instr* i, uint32_t backend_key,
                                      uint32_t host_bytes) {
  if (coverage_current_index_ == UINT32_MAX || !host_bytes) {
    return;
  }
  SequenceSample sample = {};
  sample.key = hir::MakeSequenceSampleKey(i, backend_key);
  sample.guest_index = coverage_current_index_;
  sample.host_bytes = host_bytes;
  CountWideMoveChains(
      reinterpret_cast<const uint8_t*>(getCode()) + (getSize() - host_bytes),
      host_bytes, &sample.chains, &sample.chain_instructions);
  sequence_samples_.push_back(sample);
}

void A64Emitter::DebugBreak() { brk(0xF000); }

// The guest-to-host thunk passes the context in x0, so these take it directly.
static uint64_t TrapDebugPrint(void* raw_context) {
  auto thread_state =
      reinterpret_cast<ppc::PPCContext_s*>(raw_context)->thread_state;
  uint32_t str_ptr = uint32_t(thread_state->context()->r[3]);
  uint32_t str_length = uint32_t(thread_state->context()->r[4]);
  auto str = thread_state->memory()->TranslateVirtual<const char*>(str_ptr);
  std::string message(str, str_length);
  XELOGD("(DebugPrint) {}", message);
  return 0;
}

static uint64_t TrapDebugBreak(void* raw_context) {
  XELOGE("tw/td forced trap hit! This should be a crash!");
  if (cvars::break_on_debugbreak) {
    xe::debugging::Break();
  }
  return 0;
}

void A64Emitter::Trap(uint16_t trap_type) {
  // Mirrors X64Emitter::Trap. Emitting a bare `brk trap_type` instead meant a
  // debug-print trap killed the process where the x64 backend logs and
  // continues, and trap type 0 encoded to exactly the BRK the debugger patches
  // in for breakpoints, so the two were indistinguishable in the handler.
  switch (trap_type) {
    case 20:
    case 26:
      // 0x0FE00014 is a 'debug print' where r3 = buffer r4 = length
      CallNative(reinterpret_cast<void*>(&TrapDebugPrint));
      break;
    case 0:
    case 22:
      CallNative(reinterpret_cast<void*>(&TrapDebugBreak));
      break;
    case 25:
      break;
    default:
      XELOGW("Unknown trap type {}", trap_type);
      // Never brk #0: that is the breakpoint encoding the debugger patches in,
      // and the exception handler tells them apart by encoding.
      brk(0xF002);
      break;
  }
}

void A64Emitter::b(const Xbyak_aarch64::Cond cond,
                   const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::b(cond, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::b(static_cast<Xbyak_aarch64::Cond>(cond ^ 1), skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::WReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::XReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::WReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::XReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kCondBranchBackwardRange)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (IsBoundLabelInRange(label, kTestBranchBackwardRange)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  XELOGE("A64: Unimplemented HIR instruction: {}",
         hir::GetOpcodeName(i->GetOpcodeInfo()));
  DebugBreak();
}

// Executed-call-path counters for --count_call_paths. Plain racy
// read-modify-write, the same trade as xe_a64_physical_remap_hits:
// concurrent guest threads can lose increments, which understates a rate
// measurement but never adds a fence to the dispatch path.
extern "C" volatile uint64_t xe_a64_call_direct_hits = 0;
extern "C" volatile uint64_t xe_a64_call_walk_hits = 0;
extern "C" volatile uint64_t xe_a64_call_indirect_hits = 0;

namespace {
void EmitCallPathCount(A64Emitter& e, volatile uint64_t* counter,
                       const XReg& addr_reg, const XReg& val_reg) {
  e.mov(addr_reg, reinterpret_cast<uint64_t>(counter));
  e.ldr(val_reg, ptr(addr_reg));
  e.add(val_reg, val_reg, 1);
  e.str(val_reg, ptr(addr_reg));
}
}  // namespace

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
namespace {
void EmitInlineSaverestCaptureAccesses(
    A64Emitter& e, const WReg& guest_stack, uint32_t first_slot_offset,
    uint32_t span, ppc::GuestInvocationRecorderMemoryAccess access) {
  const bool writes = access != ppc::GuestInvocationRecorderMemoryAccess::kRead;
  Label skip;
  e.EmitGuestInvocationCaptureEventGuard(
      writes ? kGuestInvocationCaptureWriteEventBit
             : kGuestInvocationCaptureOwnerEventBit,
      skip);
  for (uint32_t offset = 0; offset < span;
       offset += kMaximumGuestInvocationCaptureMemoryAccessSize) {
    const uint32_t size =
        span - offset > kMaximumGuestInvocationCaptureMemoryAccessSize
            ? kMaximumGuestInvocationCaptureMemoryAccessSize
            : span - offset;
    // The native callback may clobber caller-saved x14, so reload the logical
    // guest stack before every chunk and leave it valid for the real access.
    e.ldr(guest_stack,
          ptr(e.x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
    e.sub(e.w1, guest_stack, first_slot_offset - offset);
    e.mov(e.w2, static_cast<uint64_t>(size));
    e.mov(e.w3, static_cast<uint64_t>(access));
    e.ldr(e.x4,
          ptr(e.x20, static_cast<uint32_t>(offsetof(
                         ppc::PPCContext, guest_invocation_capture_control))));
    e.CallNativeSafe(
        reinterpret_cast<void*>(&CaptureGuestInvocationMemoryAccess));
  }
  e.L(skip);
  e.ldr(guest_stack,
        ptr(e.x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
}
}  // namespace
#endif

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  // Release builds drop the assert, and every path below dereferences this
  // before anything else, so a null arrives as a fault at a field offset with
  // no diagnostic at all.
  assert_not_null(function);
  if (!function) {
    XELOGE("Call to an unresolved function symbol from {:08X}",
           current_guest_function_);
    return;
  }
  EnsureFpuFpcrModeForTransition();
  if (TryInlinePPCGprLrSaveRestore(instr, function)) {
    return;
  }
  if (function->is_declaration_only()) {
    // The loader declared this helper and never translated it, so there is no
    // body to call anywhere and resolving one would translate bytes the corpus
    // never carried. Reaching here means the inline above declined a form it
    // was expected to handle.
    XELOGE("Call to declaration-only function {:08X} from {:08X}",
           function->address(), current_guest_function_);
    return;
  }

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  if (instr->flags & hir::CALL_TAIL) {
    Label skip_capture_tail;
    EmitGuestInvocationCaptureEventGuard(kGuestInvocationCaptureOwnerEventBit,
                                         skip_capture_tail);
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    mov(x2, static_cast<uint64_t>(function->address()));
    mov(x3, x8);
    CallNativeSafe(reinterpret_cast<void*>(&CaptureGuestInvocationTailCall));
    L(skip_capture_tail);
  }
#endif

  auto fn = static_cast<A64Function*>(function);

  if (fn->machine_code()) {
    if (cvars::count_call_paths) {
      // x16/x17 are dead here: the direct path materializes its target in
      // x9, and the tail-call stack teardown re-sets x17 after this.
      EmitCallPathCount(*this, &xe_a64_call_direct_hits, x16, x17);
    }
    // Direct call — function is already compiled.
    mov(x9, reinterpret_cast<uint64_t>(fn->machine_code()));
    if (!(instr->flags & hir::CALL_TAIL)) {
      // Pass the next call's guest return address in x0.
      ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
      blr(x9);
      synchronize_stack_on_next_instruction_ = true;
    } else {
      // Tail call: pass our return address to the callee.
      PopStackpoint();
      ldp(x0, x30, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_RET_ADDR)));
      if (stack_size() <= 4095) {
        add(sp, sp, static_cast<uint32_t>(stack_size()));
      } else {
        mov(x17, static_cast<uint64_t>(stack_size()));
        add(sp, sp, x17, UXTX);
      }
      br(x9);
    }
    return;
  }

  if (code_cache_->has_indirection_table()) {
    if (cvars::count_call_paths) {
      // Before the guest address is armed into w16: both scratch registers
      // are legitimately overwritten below.
      EmitCallPathCount(*this, &xe_a64_call_walk_hits, x16, x17);
    }
    // Must leave the guest address in w16 for the resolve thunk to read.
    mov(w16, function->address());
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      // Encoded path: see A64CodeCache for the entry format. The three
      // constants live in the backend context, so each costs one ldr instead
      // of a 3-4 instruction immediate materialisation per call site.
      static_assert(
          offsetof(A64BackendContext, indirection_table_bias) < 4096 &&
          offsetof(A64BackendContext, external_indirection_table) < 4096);
      ldr(x14, ptr(x19, static_cast<uint32_t>(offsetof(
                            A64BackendContext, indirection_table_bias))));
      add(x14, x14, w16, UXTW);
      ldr(w9, ptr(x14, static_cast<uint32_t>(0)));

      if (near_tbz_branches_safe_) {
        // External targets (host thunks and trampolines) number in the
        // dozens per title, so that case is cold: emit it in the tail.
        auto& indirection_ready = NewCachedLabel();
        auto& external_target =
            AddToTail([&indirection_ready](A64Emitter& e, Label&) {
              e.and_(e.w15, e.w9, A64CodeCache::kIndirectionExternalIndexMask);
              e.ldr(e.x14, ptr(e.x19, static_cast<uint32_t>(offsetof(
                                          A64BackendContext,
                                          external_indirection_table))));
              e.add(e.x14, e.x14, e.x15, LSL, 3);
              e.ldr(e.x9, ptr(e.x14, static_cast<uint32_t>(0)));
              e.b(indirection_ready);
            });
        tbnz_near(w9, 31, external_target);

        // Internal: rel32 from code cache base.
        ldr(x14, ptr(x19, static_cast<uint32_t>(
                              offsetof(A64BackendContext, code_execute_base))));
        add(x9, x14, w9, UXTW);
        L(indirection_ready);
      } else {
        // Function too large to prove the +/-32 KiB tbnz reach: keep the
        // external case inline.
        Label external_target;
        Label indirection_ready;
        tbnz(w9, 31, external_target);

        // Internal: rel32 from code cache base.
        ldr(x14, ptr(x19, static_cast<uint32_t>(
                              offsetof(A64BackendContext, code_execute_base))));
        add(x9, x14, w9, UXTW);
        b(indirection_ready);

        // External: tagged index into the side table.
        L(external_target);
        and_(w15, w9, A64CodeCache::kIndirectionExternalIndexMask);
        ldr(x14, ptr(x19, static_cast<uint32_t>(offsetof(
                              A64BackendContext, external_indirection_table))));
        add(x14, x14, x15, LSL, 3);
        ldr(x9, ptr(x14, static_cast<uint32_t>(0)));

        L(indirection_ready);
      }
    }
  } else {
    // No indirection table: resolve at runtime.
    mov(x0, x20);  // context
    mov(x1, static_cast<uint64_t>(function->address()));
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address in x9
  }

  if (instr->flags & hir::CALL_TAIL) {
    PopStackpoint();
    ldp(x0, x30, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

bool A64Emitter::TryInlinePPCGprLrSaveRestore(const hir::Instr* instr,
                                              const GuestFunction* function) {
  if (!function->IsSaverest() ||
      function->SaverestType() != SaveRestoreType::GPR) {
    return false;
  }

  const unsigned first_gpr = function->SaverestIndex();
  if (first_gpr < 14 || first_gpr > 31) {
    return false;
  }

  const bool is_tail_call = (instr->flags & hir::CALL_TAIL) != 0;
  if ((function->IsSave() && is_tail_call) ||
      (function->IsRestore() && !is_tail_call)) {
    return false;
  }

  // Standard PPC helper layout:
  //   std/ld rN, -((33 - N) * 8)(r1), N = first_gpr..31
  //   stw/lwz r12, -8(r1)
  const uint32_t first_slot_offset = (33 - first_gpr) * 8;
  const uint32_t lr_slot_offset = (32 - first_gpr) * 8;

  ldr(w14, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  EmitInlineSaverestCaptureAccesses(
      *this, w14, first_slot_offset, lr_slot_offset + sizeof(uint32_t),
      function->IsSave() ? ppc::GuestInvocationRecorderMemoryAccess::kWrite
                         : ppc::GuestInvocationRecorderMemoryAccess::kRead);
#endif
  if (xe::memory::allocation_granularity() > 0x1000) {
    // Branch-free: w15 = w14 + 0x1000, keep it only when w14 >= 0xE0000000.
    mov(w15, 0xE0000000u);
    cmp(w14, w15);
    add(w15, w14, 1, 12);  // w15 = w14 + 0x1000 via LSL #12
    csel(w14, w14, w15, LO);
  }
  add(x14, x21, w14, UXTW);
  sub(x14, x14, first_slot_offset);

  if (function->IsSave()) {
    for (unsigned guest_reg = first_gpr; guest_reg <= 31; ++guest_reg) {
      ldr(x15, ptr(x20, static_cast<int32_t>(
                            offsetof(ppc::PPCContext, r[guest_reg]))));
      rev(x15, x15);
      str(x15, ptr(x14, static_cast<uint32_t>((guest_reg - first_gpr) * 8)));
    }

    ldr(w15, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[12]))));
    rev(w15, w15);
    str(w15, ptr(x14, lr_slot_offset));
    return true;
  }

  for (unsigned guest_reg = first_gpr; guest_reg <= 31; ++guest_reg) {
    ldr(x15, ptr(x14, static_cast<uint32_t>((guest_reg - first_gpr) * 8)));
    rev(x15, x15);
    str(x15, ptr(x20, static_cast<int32_t>(
                          offsetof(ppc::PPCContext, r[guest_reg]))));
  }

  ldr(w16, ptr(x14, lr_slot_offset));
  rev(w16, w16);
  str(x16, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[12]))));
  str(x16, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));

  // __restgprlr_N returns to the reloaded LR: take our epilogue when it is our
  // own return address, otherwise tail-call it. CallIndirect emits the
  // indirection lookup and, for a tail call, the stack teardown and jump.
  ldr(w15, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  cmp(w16, w15);
  if (near_tail_branches_safe_) {
    // The epilog is bound at function end, inside the same +/-1 MiB bound
    // that gates the other tail branches.
    b_near(EQ, epilog_label());
  } else {
    b(EQ, epilog_label());
  }
  CallIndirect(instr, 16);
  return true;
}

void A64Emitter::CallIndirect(const hir::Instr* instr, int reg_index) {
  EnsureFpuFpcrModeForTransition();
  auto target_w = WReg(reg_index);

  // A tail call through the indirection table reloads x0/x30 from the same
  // slot pair the ret-check reads, so one ldp up front serves both: the
  // b.eq-to-epilog path may clobber x0/x30 freely (the epilog reloads x30
  // itself and ignores x0), and the indirection loads touch neither. The
  // no-table fallback keeps the late reload because its resolve blr clobbers
  // x0/x30.
  const bool hoist_ret_slots =
      (instr->flags & hir::CALL_TAIL) && code_cache_->has_indirection_table();
  if (hoist_ret_slots) {
    ldp(x0, x30, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_RET_ADDR)));
  }

  // Check if this is a possible return (e.g., PPC blr).
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    // Compare target guest address with our function's return address.
    if (!hoist_ret_slots) {
      ldr(w0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    }
    cmp(target_w, w0);
    if (near_tail_branches_safe_) {
      // The epilog is bound at function end, inside the same +/-1 MiB bound
      // that gates the other tail branches.
      b_near(EQ, epilog_label());
    } else {
      b(EQ, epilog_label());
    }
  }

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  if (instr->flags & hir::CALL_TAIL) {
    // x0-x18 are caller-saved. Round-trip the target through the callback's
    // return value so w16 and any future scratch assignment are both safe.
    Label skip_capture_tail;
    EmitGuestInvocationCaptureEventGuard(kGuestInvocationCaptureOwnerEventBit,
                                         skip_capture_tail);
    mov(w2, target_w);
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    mov(x3, x8);
    CallNativeSafe(reinterpret_cast<void*>(&CaptureGuestInvocationTailCall));
    mov(target_w, w0);
    if (hoist_ret_slots) {
      // The native call consumes both ABI return registers. Restore the guest
      // and host return boundaries that the tail transition passes onward.
      ldp(x0, x30, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_RET_ADDR)));
    }
    L(skip_capture_tail);
  }
#endif

  // Load host code address from indirection table.
  if (code_cache_->has_indirection_table()) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    if (target_w.getIdx() != w16.getIdx()) {
      mov(w16, target_w);
    }
    if (cvars::count_call_paths) {
      // After the target is normalized into w16, x14/x17 cannot be the
      // incoming target register: x14 is overwritten by the walk below and
      // x17 only by the tail-call stack teardown. x0/x30 (possibly holding
      // the hoisted return slots) are untouched.
      EmitCallPathCount(*this, &xe_a64_call_indirect_hits, x14, x17);
    }
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      // Encoded path: see A64CodeCache for the entry format. Constants come
      // from the backend context (one ldr each) and the cold external case
      // sits in the tail when tbnz's +/-32 KiB reach is provable.
      ldr(x14, ptr(x19, static_cast<uint32_t>(offsetof(
                            A64BackendContext, indirection_table_bias))));
      add(x14, x14, w16, UXTW);
      ldr(w9, ptr(x14, static_cast<uint32_t>(0)));

      if (near_tbz_branches_safe_) {
        auto& indirection_ready = NewCachedLabel();
        auto& external_target =
            AddToTail([&indirection_ready](A64Emitter& e, Label&) {
              e.and_(e.w15, e.w9, A64CodeCache::kIndirectionExternalIndexMask);
              e.ldr(e.x14, ptr(e.x19, static_cast<uint32_t>(offsetof(
                                          A64BackendContext,
                                          external_indirection_table))));
              e.add(e.x14, e.x14, e.x15, LSL, 3);
              e.ldr(e.x9, ptr(e.x14, static_cast<uint32_t>(0)));
              e.b(indirection_ready);
            });
        tbnz_near(w9, 31, external_target);

        // Internal: rel32 from code cache base.
        ldr(x14, ptr(x19, static_cast<uint32_t>(
                              offsetof(A64BackendContext, code_execute_base))));
        add(x9, x14, w9, UXTW);
        L(indirection_ready);
      } else {
        Label external_target;
        Label indirection_ready;
        tbnz(w9, 31, external_target);

        // Internal: rel32 from code cache base.
        ldr(x14, ptr(x19, static_cast<uint32_t>(
                              offsetof(A64BackendContext, code_execute_base))));
        add(x9, x14, w9, UXTW);
        b(indirection_ready);

        // External: tagged index into the side table.
        L(external_target);
        and_(w15, w9, A64CodeCache::kIndirectionExternalIndexMask);
        ldr(x14, ptr(x19, static_cast<uint32_t>(offsetof(
                              A64BackendContext, external_indirection_table))));
        add(x14, x14, x15, LSL, 3);
        ldr(x9, ptr(x14, static_cast<uint32_t>(0)));

        L(indirection_ready);
      }
    }
  } else {
    // No indirection table: resolve at runtime.
    mov(w16, target_w);
    mov(x0, x20);  // context
    mov(x1, x16);  // guest address
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address
  }

  if (instr->flags & hir::CALL_TAIL) {
    // Tail call: pass our return address to the callee.
    PopStackpoint();
    if (!hoist_ret_slots) {
      ldp(x0, x30, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_RET_ADDR)));
    }
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    // Regular call: pass the next call's return address.
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  EnsureFpuFpcrModeForTransition();
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  uint32_t dependency_flags = ppc::kGuestInvocationDependencyExternOrBuiltin;
  if (function->behavior() == Function::Behavior::kExtern) {
    dependency_flags |= ppc::kGuestInvocationDependencyKernelExport;
  }
  Label skip_capture_dependency;
  EmitGuestInvocationCaptureEventGuard(kGuestInvocationCaptureOwnerEventBit,
                                       skip_capture_dependency);
  mov(x1, static_cast<uint64_t>(dependency_flags));
  mov(x2, x8);
  CallNativeSafe(
      reinterpret_cast<void*>(&CaptureGuestInvocationUnsupportedDependency));
  L(skip_capture_dependency);
#endif
  bool undefined = true;
  if (function->behavior() == Function::Behavior::kBuiltin) {
    auto builtin_function = static_cast<const BuiltinFunction*>(function);
    if (builtin_function->handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0, x2=arg1
      // Thunk rearranges to: x0=context, x1=arg0, x2=arg1, calls target
      mov(x0, reinterpret_cast<uint64_t>(builtin_function->handler()));
      mov(x1, reinterpret_cast<uint64_t>(builtin_function->arg0()));
      mov(x2, reinterpret_cast<uint64_t>(builtin_function->arg1()));
      ldr(x9, ptr(GetBackendCtxReg(),
                  static_cast<uint32_t>(offsetof(
                      A64BackendContext, guest_to_host_thunk_no_vec_address))));
      blr(x9);
    }
  } else if (function->behavior() == Function::Behavior::kExtern) {
    auto extern_function = static_cast<const GuestFunction*>(function);
    if (extern_function->extern_handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0
      mov(x0, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
      ldr(x1, ptr(GetContextReg(), static_cast<int32_t>(offsetof(
                                       ppc::PPCContext, kernel_state))));
      ldr(x9, ptr(GetBackendCtxReg(),
                  static_cast<uint32_t>(offsetof(
                      A64BackendContext, guest_to_host_thunk_no_vec_address))));
      blr(x9);
    }
  }
  if (undefined) {
    // Set arg0 = function pointer, then call UndefinedCallExtern via thunk.
    mov(x1, reinterpret_cast<uint64_t>(function));
    CallNativeSafe(reinterpret_cast<void*>(&UndefinedCallExtern));
  }
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNativeSafe(void* fn) {
  // The guest-to-host thunk unconditionally reinstalls fpcr_fpu on the way
  // back, so whatever mode this emitter thinks is live is stale afterwards.
  // Call, CallIndirect and CallExtern already do this; without it here a VMX
  // float op after the call skips its msr and runs with FZ clear.
  // The transition guard sits here, inside any inline-MMIO taken branch, so
  // the switch executes exactly on the path that reaches the thunk; the
  // fall-through path keeps its mode and the Unknown tracker afterwards makes
  // later ops re-establish it.
  EnsureFpuFpcrModeForTransition();
  // GuestToHostThunk: x0=target function, x1/x2=args (set by caller).
  // The thunk rearranges: saves x0 in x9, sets x0=context, calls x9.
  mov(x0, reinterpret_cast<uint64_t>(fn));
  ldr(x9, ptr(GetBackendCtxReg(),
              static_cast<uint32_t>(
                  offsetof(A64BackendContext, guest_to_host_thunk_address))));
  blr(x9);
}

void A64Emitter::SetReturnAddress(uint64_t value) {
  mov(x0, value);
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
  MarkX0HoldsConstant(value);
}

void A64Emitter::ReloadMembase() {
  // Reload x21 from context->virtual_membase.
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
}

bool A64Emitter::ChangeFpcrMode(FPCRMode new_mode, bool already_set) {
  if (fpcr_mode_ == new_mode) {
    return false;
  }
  const FPCRMode old_mode = fpcr_mode_;
  fpcr_mode_ = new_mode;
  if (!already_set) {
    // Load the pre-computed FPCR value from the backend context.
    // This avoids an expensive MRS + read-modify-write cycle.
    auto bctx = GetBackendCtxReg();
    uint32_t fpcr_offset;
    if (new_mode == FPCRMode::Vmx) {
      fpcr_offset =
          static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_vmx));
    } else if (new_mode == FPCRMode::VmxDaz) {
      fpcr_offset =
          static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_vmx_daz));
    } else {
      fpcr_offset =
          static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu));
    }

    // SET_NJM only toggles FZ in fpcr_vmx and nothing else writes it, so with
    // NJM on it already equals fpcr_vmx_daz and the two vmx modes are the same
    // bits. Skipping then saves the msr, which is the expensive part.
    const bool vmx_variant_switch =
        IsVmxFpcrMode(old_mode) && IsVmxFpcrMode(new_mode);
    Xbyak_aarch64::Label skip;
    if (vmx_variant_switch) {
      ldr(w0, Xbyak_aarch64::ptr(bctx, static_cast<uint32_t>(offsetof(
                                           A64BackendContext, flags))));
      tbnz(w0, kA64BackendNJMOn, skip);
    }
    ldr(w0, Xbyak_aarch64::ptr(bctx, fpcr_offset));
    msr(3, 3, 4, 4, 0, x0);  // msr FPCR, x0
    if (vmx_variant_switch) {
      L(skip);
    }
  }
  return true;
}

Label& A64Emitter::AddToTail(TailEmitCallback callback, uint32_t alignment) {
  TailEmitter tail;
  tail.alignment = alignment;
  tail.func = std::move(callback);
  // AddToTail runs while the sequence is still emitting, so the sample this
  // sequence is about to push is the one at the current end of the vector.
  tail.sample_index = coverage_current_index_ == UINT32_MAX
                          ? UINT32_MAX
                          : static_cast<uint32_t>(sequence_samples_.size());
  tail_code_.push_back(std::move(tail));
  return tail_code_.back().label;
}

Label& A64Emitter::NewCachedLabel() {
  auto* label = new Label();
  label_cache_.push_back(label);
  return *label;
}

uint32_t A64Emitter::MapReg(const hir::Value* v, const uint32_t* map, int count,
                            const char* set_name) {
  // reg.index is a signed int32 and is -1 while unassigned, so the unsigned
  // compare catches both "never allocated" and "past the end of the set".
  const uint32_t index = static_cast<uint32_t>(v->reg.index);
  if (index >= static_cast<uint32_t>(count)) {
    XELOGE(
        "A64: value v{} (type {}, def opcode {}) has no {} register assignment "
        "(index {} of {}); codegen would emit a bogus register",
        v->ordinal, static_cast<uint32_t>(v->type),
        v->def ? hir::GetOpcodeName(v->def->GetOpcodeInfo()) : "<none>",
        set_name, index, count);
    assert_always("register allocation missed a value");
    // Clamp so the assembler still produces a decodable instruction.
    return map[0];
  }
  return map[index];
}

void A64Emitter::EmitPreemptCheck(uint32_t guest_address) {
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  // The adjacent request bytes share one hot load when the scheduler is on.
  // The cold path still handles scheduler preemption first, then capture, so a
  // handler may re-set preempt_requested without consuming or delaying the
  // independent capture request. Scheduler-off code tests only capture.
  Label& after = NewCachedLabel();
  static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
  static_assert(offsetof(ppc::PPCContext, capture_rendezvous_requested) < 4096);
  const uint32_t preempt_offset =
      static_cast<uint32_t>(offsetof(ppc::PPCContext, preempt_requested));
  const uint32_t capture_offset = static_cast<uint32_t>(
      offsetof(ppc::PPCContext, capture_rendezvous_requested));
  const bool has_vmx = function_has_vmx_;
  const FPCRMode held_mode = fpcr_mode_;
  const bool scheduler_enabled = cvars::guest_scheduler;

  if (cvars::log_safepoint_pc && guest_address) {
    static_assert(offsetof(ppc::PPCContext, last_safepoint_pc) < 16384);
    mov(w9, guest_address);
    str(w9, ptr(x20, static_cast<uint32_t>(
                         offsetof(ppc::PPCContext, last_safepoint_pc))));
  }

  const uint32_t owning_function = current_guest_function_;
  Label& do_rendezvous = AddToTail([&after, scheduler_enabled, preempt_offset,
                                    capture_offset, has_vmx, held_mode,
                                    guest_address,
                                    owning_function](A64Emitter& e, Label&) {
    // The hot loads and branches leave NZCV untouched. Save it before any cold
    // callback so the block-head transition cannot perturb later sequences.
    e.mrs(e.x10, 3, 3, 4, 2, 0);  // mrs x10, NZCV
    e.str(e.x10, ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    if (has_vmx) {
      e.ldr(e.w0, ptr(e.x19, static_cast<uint32_t>(
                                 offsetof(A64BackendContext, fpcr_fpu))));
      e.msr(3, 3, 4, 4, 0, e.x0);  // msr FPCR, x0
    }

    if (scheduler_enabled) {
      Xbyak_aarch64::Label scheduler_done;
      e.ldrb(e.w8, ptr(e.x20, preempt_offset));
      e.cbz(e.w8, scheduler_done);
      e.strb(e.wzr, ptr(e.x20, preempt_offset));
      e.ldr(e.x0, ptr(e.GetBackendCtxReg(),
                      static_cast<uint32_t>(offsetof(
                          A64BackendContext, preempt_yield_handler_address))));
      e.ldr(e.x0, ptr(e.x0));
      e.cbz(e.x0, scheduler_done);
      e.mov(e.x1, static_cast<uint64_t>(guest_address));
      e.mov(e.x2, static_cast<uint64_t>(owning_function));
      e.ldr(e.x9, ptr(e.GetBackendCtxReg(),
                      static_cast<uint32_t>(offsetof(
                          A64BackendContext, guest_to_host_thunk_address))));
      e.blr(e.x9);
      e.L(scheduler_done);
    }

    Xbyak_aarch64::Label capture_done;
    e.ldrb(e.w8, ptr(e.x20, capture_offset));
    e.cbz(e.w8, capture_done);
    e.mov(e.x0, reinterpret_cast<uint64_t>(
                    &xe::cpu::HandleGuestExecutionCaptureJitSafepoint));
    e.mov(e.x1, static_cast<uint64_t>(guest_address));
    e.ldr(e.x9, ptr(e.GetBackendCtxReg(),
                    static_cast<uint32_t>(offsetof(
                        A64BackendContext, guest_to_host_thunk_address))));
    e.blr(e.x9);
    e.L(capture_done);
    if (held_mode != FPCRMode::Unknown && held_mode != FPCRMode::Fpu) {
      e.fpcr_mode_ = FPCRMode::Unknown;
      e.ChangeFpcrMode(held_mode);
      e.fpcr_mode_ = held_mode;
    }
    e.ldr(e.x10, ptr(e.sp, static_cast<uint32_t>(StackLayout::GUEST_SCRATCH)));
    e.msr(3, 3, 4, 2, 0, e.x10);  // msr NZCV, x10
    e.b(after);
  });
  if (scheduler_enabled) {
    static_assert(offsetof(ppc::PPCContext, capture_rendezvous_requested) ==
                  offsetof(ppc::PPCContext, preempt_requested) + 1);
    static_assert(!(offsetof(ppc::PPCContext, preempt_requested) & 1));
    ldrh(w8, ptr(x20, preempt_offset));
  } else {
    ldrb(w8, ptr(x20, capture_offset));
  }
  if (near_tail_branches_safe_) {
    cbnz_near(w8, do_rendezvous);
  } else {
    cbnz(w8, do_rendezvous);
  }
  L(after);
#else
  // Only safe at a block head, where the per-block register allocator leaves no
  // guest value live and ForgetFpcrMode has already run, so the unannounced
  // guest->host call cannot lose a register or desync the mode tracking.
  //
  // Tests the preempt flag other threads raise. The cold path clears it, a
  // deferred yield re-sets it.
  // The yield path clobbers FPCR at runtime, but it is cold: when the
  // tracker holds a known mode here (block-entry seeding often just
  // established one), the yield tail restores that mode after the host
  // call, so the hot path's static mode survives the check untouched. Only
  // an already-unknown mode stays unknown.
  Label& after = NewCachedLabel();
  // ldrb/strb unsigned-offset encoding caps at 4095.
  static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
  const uint32_t flag_offset =
      static_cast<uint32_t>(offsetof(ppc::PPCContext, preempt_requested));
  const bool has_vmx = function_has_vmx_;
  const FPCRMode held_mode = fpcr_mode_;
  Label& do_yield = AddToTail([&after, flag_offset, has_vmx, held_mode,
                               guest_address](A64Emitter& e, Label&) {
    // The yield calls host code, which must run in FPU mode. The runtime mode
    // here is whatever the interrupted block was in - unknowable at emission
    // - so functions that touch VEC128 switch unconditionally (cold path).
    if (has_vmx) {
      e.ldr(e.w0, ptr(e.x19, static_cast<uint32_t>(
                                 offsetof(A64BackendContext, fpcr_fpu))));
      e.msr(3, 3, 4, 4, 0, e.x0);  // msr FPCR, x0
    }
    e.strb(e.wzr, ptr(e.x20, flag_offset));
    // Null until the scheduler starts, and a stale flag can reach here after
    // it shuts down, so check before calling.
    e.ldr(e.x0, ptr(e.GetBackendCtxReg(),
                    static_cast<uint32_t>(offsetof(
                        A64BackendContext, preempt_yield_handler_address))));
    e.ldr(e.x0, ptr(e.x0));
    const bool restore_held =
        held_mode != FPCRMode::Unknown && held_mode != FPCRMode::Fpu;
    Xbyak_aarch64::Label rejoin;
    // A null handler (scheduler not yet started, or shut down with a stale
    // flag) must still pass through the held-mode restore below: the FPU
    // switch above already ran, and the hot path continues assuming
    // held_mode.
    e.cbz(e.x0, restore_held ? rejoin : after);
    e.mov(e.x1, static_cast<uint64_t>(guest_address));
    e.ldr(e.x9, ptr(e.GetBackendCtxReg(),
                    static_cast<uint32_t>(offsetof(
                        A64BackendContext, guest_to_host_thunk_address))));
    e.blr(e.x9);
    // Re-establish the mode the hot path still assumes (the host call left
    // FPCR in the scalar FPU state via the guest-to-host thunk). The
    // tracker still holds held_mode, which would make ChangeFpcrMode skip
    // the emission - clear it first to force the reload.
    if (restore_held) {
      e.L(rejoin);
      e.fpcr_mode_ = FPCRMode::Unknown;
      e.ChangeFpcrMode(held_mode);
      e.fpcr_mode_ = held_mode;
    }
    e.b(after);
  });
  if (cvars::log_safepoint_pc && guest_address) {
    // Diagnostic only: costs a materialize + store on every loop back-edge, so
    // it stays off unless a wedge is being chased.
    static_assert(offsetof(ppc::PPCContext, last_safepoint_pc) < 16384);
    mov(w9, guest_address);
    str(w9, ptr(x20, static_cast<uint32_t>(
                         offsetof(ppc::PPCContext, last_safepoint_pc))));
  }
  ldrb(w8, ptr(x20, flag_offset));
  if (near_tail_branches_safe_) {
    // Not-taken fall-through: two instructions on the hot path instead of an
    // inverted branch over an unconditional one.
    cbnz_near(w8, do_yield);
  } else {
    cbnz(w8, do_yield);
  }
  L(after);
#endif
}

void A64Emitter::EmitGuestExecutionSessionInstructionCoverage(
    uint32_t guest_instruction_count) {
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  if (!guest_instruction_count) {
    return;
  }
  Label done;
  ldr(x8,
      ptr(GetContextReg(),
          static_cast<uint32_t>(offsetof(
              ppc::PPCContext, guest_execution_session_instruction_counter))));
  cbz(x8, done);
  mov(x9, static_cast<uint64_t>(guest_instruction_count));
  if (IsFeatureEnabled(kA64EmitLSE)) {
    stadd(x9, ptr(x8));
  } else {
    Label retry;
    L(retry);
    ldxr(x10, ptr(x8));
    add(x10, x10, x9);
    stxr(w11, x10, ptr(x8));
    cbnz(w11, retry);
  }
  L(done);
#else
  (void)guest_instruction_count;
#endif
}

Label& A64Emitter::GetLabel(uint32_t label_id) {
  auto it = label_map_.find(label_id);
  if (it != label_map_.end()) {
    return *it->second;
  }
  auto* label = new Label();
  label_map_[label_id] = label;
  return *label;
}

// Half of LDR (literal)'s +-1MB reach, leaving room for the item in flight.
static constexpr size_t kV128ConstPoolReach = 512 * 1024;

Label& A64Emitter::GetV128ConstLabel(const vec128_t& value) {
  for (auto& entry : v128_consts_) {
    if (entry.first == value) {
      return *entry.second;
    }
  }
  if (v128_consts_.empty()) {
    v128_consts_first_use_ = getSize();
  }
  auto* label = new Label();
  label_cache_.push_back(label);
  v128_consts_.emplace_back(value, label);
  return *label;
}

bool A64Emitter::FlushV128ConstPool(bool branch_over) {
  if (v128_consts_.empty()) {
    return true;
  }
  Label* skip = nullptr;
  if (branch_over) {
    skip = new Label();
    label_cache_.push_back(skip);
    b(*skip);
  }
  // Functions start 16-byte aligned in the code cache.
  while (getSize() % 16) {
    dd(0);
  }
  try {
    for (auto& entry : v128_consts_) {
      L(*entry.second);
      for (int word = 0; word < 4; ++word) {
        dd(entry.first.u32[word]);
      }
    }
  } catch (const Xbyak_aarch64::Error& e) {
    XELOGE(
        "A64: v128 literal pool out of range in guest function {:08X} "
        "({} constants, {} bytes since the first use): {}",
        current_guest_function_, v128_consts_.size(),
        getSize() - v128_consts_first_use_, e.what());
    return false;
  }
  v128_consts_.clear();
  if (skip) {
    L(*skip);
  }
  return true;
}

bool A64Emitter::MaybeFlushV128ConstPool() {
  if (v128_consts_.empty() ||
      getSize() - v128_consts_first_use_ < kV128ConstPoolReach) {
    return true;
  }
  return FlushV128ConstPool(true);
}

void A64Emitter::HandleStackpointOverflowError(ppc::PPCContext* context) {
  if (debugging::IsDebuggerAttached()) {
    debugging::Break();
  }
  xe::FatalError(
      "Overflowed stackpoints! Please report this error for this title to "
      "Xenia developers.");
}

void A64Emitter::PushStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    // Still owns zeroing the call-return slot (see the prolog).
    str(xzr, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    return;
  }
  // Link this frame's node into the chain. All node fields are written
  // before the head store, so an async signal never observes a head
  // pointing at an uninitialized node, and the node sits above live SP so
  // signal frames cannot smash it. No array, no depth, no bounds check:
  // runaway recursion now dies on the host stack guard page (with the
  // fiber-overflow diagnostic from the exception handler) instead of the
  // old counted FatalError.
  static_assert(StackLayout::STACKPOINT_PREV ==
                StackLayout::GUEST_CALL_RET_ADDR + 8);
  static_assert(StackLayout::STACKPOINT_GUEST_SP ==
                StackLayout::STACKPOINT_PREV + 8);
  static_assert(StackLayout::STACKPOINT_GUEST_RET ==
                StackLayout::STACKPOINT_GUEST_SP + 4);
  ldr(x8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, stackpoint_head))));
  ldr(w9, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
  ldr(w10, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));
  // Zero the call-return slot and store prev_ with one pair.
  stp(xzr, x8, ptr(sp, static_cast<int32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
  stp(w9, w10, ptr(sp, static_cast<int32_t>(StackLayout::STACKPOINT_GUEST_SP)));
  add(x11, sp, static_cast<uint32_t>(StackLayout::STACKPOINT_PREV));
  str(x11, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, stackpoint_head))));
}
void A64Emitter::PopStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // head = this frame's prev_. Runs before the frame teardown at every pop
  // site, so there is no window where head points below live SP.
  ldr(x8, ptr(sp, static_cast<uint32_t>(StackLayout::STACKPOINT_PREV)));
  str(x8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, stackpoint_head))));
}

void A64Emitter::EnsureSynchronizedGuestAndHostStack() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // ResolveFunction marks this when it returns a return-site address inside an
  // existing frame. The marker lives in backend context because native SP can
  // still point at a skipped frame here.
  auto& return_from_sync = NewCachedLabel();

  ldr(x16, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_node))));
  // Bound forward target (adr + b below) — short form is safe.
  cbz_near(x16, return_from_sync);

  const uint32_t helper_offset = static_cast<uint32_t>(offsetof(
      A64BackendContext, synchronize_guest_and_host_stack_helper_address));
  auto& sync_label = AddToTail([helper_offset](A64Emitter& e, Label& lbl) {
    // x8 was set up in the body to point at return_from_sync; do that there
    // instead of here because adr's ±1 MiB range can't span body+tail in
    // large functions.
    //   x8 = return address (where to resume after fixup)
    e.ldr(e.x10, ptr(e.x19, helper_offset));
    e.br(e.x10);
  });
  adr(x8, return_from_sync);
  b(sync_label);

  L(return_from_sync);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
