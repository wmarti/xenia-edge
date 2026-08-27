# Guest-capture external-event and mutation accounting inventory

This inventory maps every source path that produces an external event or a
guest-memory mutation during a captured interval, and states for each class
whether an offline replay can validate it deterministically, inject a captured
input, or must reject the session. It backs required-architecture item 4 of
`guest_execution_replay.md` (the external-event log) and is the source map for
the narrow central adapters this lane lands.

Only one adapter is implemented in the accompanying commit: a bounded,
fail-closed state machine for the synchronous kernel-export / extern-dispatch
class (`src/xenia/cpu/guest_execution_external_event.h/.cc`). It is a pure,
thread-safe observer with no production call site yet; the runtime that owns
capture start/stop and publication must call it at the boundary named below.
Every other class is left explicitly UNCOVERED here so the coordinator can rank
the remaining work; nothing in this lane claims runtime coverage of any class.

Line numbers are against the parent commit
`467031339a624062c6728815a110145a8f8f6f17`.

## Sibling adapters (do not duplicate)

- `GuestExecutionCaptureHostCallRoster`
  (`src/xenia/cpu/guest_execution_capture.h/.cc`): the permanent outer
  host-to-guest dispatch observer. It records the begin/end of every host call
  into guest code (`GuestFunction::Call`, `src/xenia/cpu/function.cc:210`).
  APC/DPC/interrupt/callback delivery all re-enter guest code through it, so the
  *boundary* of those deliveries is already observed there.
- Worker A — scheduler observer (`src/xenia/kernel/guest_scheduler.*`): owns
  thread-dispatch and synchronization ordering. Not implemented here.
- Worker E — `PM4_XE_SWAP` marker
  (`src/xenia/gpu/pm4_command_processor_implement.h:508`): the guest-work swap
  boundary. Not implemented here.
- Execution replay tape (`fc828f978`, `src/xenia/cpu/guest_execution_replay_tape.*`
  on branch `codex/guest-execution-event-tape`, **not** on base `467031339`):
  the deterministic order consumer that a recorded external-event stream feeds
  at replay.

## Implemented adapter: kernel-export / extern-dispatch external events

`GuestExecutionCaptureExternalEventLog` accounts for the one class where the
external work runs synchronously on the active guest thread's own stack: an
extern/builtin dispatched from the JIT and a kernel export trampoline. It models
a deliberately narrow **subset** of that class, and only that subset is
canonical: a single integer result in `r[3]` and a single contiguous declared
guest-memory effect range. Anything outside the subset — a floating-point
result in `f[1]`, an export that mutates `r[1]`/`lr`/`ctr`, or more than one
discontiguous effect range — is unmodeled and must be recorded as
`kRejectSession` (or rejected at the call site), never approximated. Within
the subset the record carries:

- typed kind (`kKernelExport` / `kExternOrBuiltin`, the session codec's own
  enumerators, reused so no parallel numbering can drift);
- stable participant id (`capture_instance_id` + `guest_thread_id`, reused from
  `GuestExecutionCaptureParticipantIdentity`);
- canonical little-endian `r[3]` returned scalar;
- one contiguous declared guest-memory effect range with exact preimage bytes
  snapshotted at begin and postimage bytes at end;
- an explicit disposition (`kReplayCaptured` / `kValidateDeterministic` /
  `kRejectSession`) and mutation source, restricted to
  `kActiveGuestThread`/`kNone` so a synchronous export store can never be
  mislabeled GPU/DMA/host; a `kRejectSession` event is still recorded, and the
  snapshot's sticky `reject_session_count` / `first_reject_session_sequence`
  make `replayable()` false from then on;
- last-in-first-out pairing per participant, so a nested export that re-enters
  guest code and dispatches another export closes in order;
- bounded active calls, recorded events, per-event effect bytes and total
  payload bytes, each rejecting (latched) rather than truncating; a latched log
  reports `CanDetach()` true so a failed log can always be released.

### Intended production call site (NOT wired in this commit)

The correct central boundary is the shared kernel-export trampoline
`ExportRegistrerHelper<...>::RegisterExport`'s `X::Trampoline`
(`src/xenia/kernel/util/shim_utils.h`, around the `result.Store(ppc_context)`
site). Every shim-based export return flows through it, and JIT extern calls
reach it because `GuestFunction::extern_handler_` is that same trampoline (bound
in `src/xenia/kernel/kernel_module.cc:112` and
`src/xenia/cpu/xex_module.cc:1343`). A future capture-build-only edit there
would, under the compile-out macro: read the participant from
`ppc_context->thread_state`, snapshot the declared out-parameter preimage before
the call, and on return record `ppc_context->r[3]` and the postimage. Routing,
as with the host-call observer, would go through new `Processor` lease methods
(mirroring `Processor::BeginGuestExecutionCaptureHostCall`,
`src/xenia/cpu/processor.cc:404`).

That wiring is deferred because it edits the hot central kernel path and
`processor.cc/.h`, which cannot be compile-checked in this source-edit-only
worktree and would collide with the concurrent scheduler/checkpoint lanes. The
adapter is landed first, exactly as `GuestExecutionCaptureHostCallRoster` was
landed before its `function.cc` call site.

**One boundary, not two.** The JIT `CallExtern` extern branch
(`a64_emitter.cc:1283`) calls `GuestFunction::extern_handler_`, which *is*
`X::Trampoline`. Hooking both the JIT extern call and the trampoline would
record every export twice. Wire the trampoline only; the JIT site contributes
provenance (return address) at most. Builtins (`CallExtern` builtin branch,
`a64_emitter.cc:1272`) bypass the trampoline and are a separate, still-uncovered
boundary.

**Wiring prerequisites the call site must satisfy** (none of this is in the
adapter):

- A per-export disposition table. Pure, deterministic exports (e.g. `Rtl*`
  string/memory helpers, `XeCrypt*`, `RtlImageXexHeaderField`) may be recorded
  as `kValidateDeterministic`; nondeterministic-but-modeled exports as
  `kReplayCaptured` with their declared effect; any export not in the table is
  `kRejectSession`. Unknown work rejects.
- The declared effect range must not overlap guest memory that nested guest
  code writes between begin and end (APC/DPC/callback delivery during
  alertable waits re-enters guest code through the host-call roster). Such
  overlap must be rejected or the range declared narrower.
- Blocking exports (`kBlocking` tag, waits, `KeDelayExecutionThread`, alertable
  `NtWaitFor*`) let **other participants run between begin and end**, so the
  postimage may contain other-guest-thread writes. Until Worker A's scheduler
  tape orders those, a blocking export's effect range must be
  `kRejectSession`, not attributed to the active thread.
- Reject when armed and the observer is absent; the pure adapter cannot detect
  that.

### Not covered even for this class

- Results that are not a single `r[3]` integer scalar (floating-point returns
  in `f[1]`; exports that mutate `r[1]`/`lr`/`ctr` such as
  `xboxkrnl_threading.cc:353,1475,1521` and `xthread.cc:1487`).
- More than one discontiguous guest-memory effect range per call.
- Writes to guest-resident kernel objects and per-CPU state outside the
  declared range: dispatcher headers (`KeSetEvent`, `KeReleaseSemaphore`),
  `KPCR` fields (`KfRaiseIrql`/`KfLowerIrql` `current_irql`), `KTHREAD`
  bookkeeping and TLS slots. These are real guest-memory mutations by the
  export body; the call site must either declare them in the effect range or
  reject.
- Builtins that mutate host/context state instead of guest memory
  (`CheckGlobalLock` writes `ppc_context->scratch`,
  `src/xenia/cpu/ppc/ppc_frontend.cc:60`).

## Class inventory summary

Disposition legend: validate = deterministic re-execution check; inject = feed
captured result/effect as a modeled input; reject = fail the session closed;
UNCOVERED = no adapter in this lane.

| Class | Representative call site (file:line) | Capture identity available? | Replay disposition |
|---|---|---|---|
| Kernel export return (single boundary; JIT extern branch `a64_emitter.cc:1283` reaches the same trampoline — wire once) | `kernel/util/shim_utils.h` `X::Trampoline` `result.Store` | Yes (`ppc_context->thread_state`) | inject (subset: `r[3]` + one range) / per-export table / else reject; adapter READY, call site NOT wired |
| Builtin dispatch (JIT builtin branch, bypasses trampoline) | `cpu/backend/a64/a64_emitter.cc:1272` `CallExtern`; HIR `cpu/backend/a64/a64_seq_control.cc:546` | Yes (`ppc_context`) | validate/inject; adapter kind reserved (`kExternOrBuiltin`), UNCOVERED |
| Host→guest dispatch boundary | `cpu/function.cc:210` `GuestFunction::Call` → `cpu/processor.cc:404/434` | Yes (`ThreadState`) | boundary observed by sibling host-call roster; payload UNCOVERED |
| Builtins mutating host/context | `cpu/ppc/ppc_frontend.cc:60` `CheckGlobalLock`/`EnterGlobalLock` | Yes (`ppc_context`) | validate/inject; UNCOVERED |
| Clocks / timebase | `base/clock.cc:179,185,194,198`; A64 `cpu/backend/a64/a64_seq_memory.cc:915` `LOAD_CLOCK` | Yes (`ppc_context`) | inject; UNCOVERED (A64 hook currently rejects) |
| MMIO read/write | `cpu/mmio_handler.cc:101,112`; A64 `a64_seq_memory.cc:1553,1574`; window `cpu/backend/a64/a64_guest_invocation_capture.cc` | Yes (`ppc_context`) | inject read / reject side-effecting write; UNCOVERED (A64 hook currently rejects) |
| Interrupt / APC / DPC delivery | `kernel/xthread.cc:876,882`; `kernel/xboxkrnl/xboxkrnl_threading.cc:1501,1514,1591,1774`; `gpu/graphics_system.cc:380`→`kernel/kernel_state.cc:1358` | Yes (delivered on a guest `ThreadState` via `Processor::Execute`) | inject delivery as control tape; boundary observed by host-call roster; payload/ordering UNCOVERED (Worker A owns scheduling) |
| Atomics / reservations | `a64_seq_memory.cc:1448,1499` (CAS), `:1658` (`RESERVED_LOAD`), `:1698` (`EmitReservedStore`); global `reserve_generation` counter | Yes (`ppc_context`) | reject unless modeled; UNCOVERED (A64 hook currently rejects) |
| GPU shared-memory writes | `gpu/command_processor.cc:555,634,747`; `gpu/pm4_command_processor_implement.h:986,1007,1056,1125,1176,1214`; `gpu/shared_memory.cc:638` | No (async CP/GPU thread) | inject captured postimage as `kGpu`, else reject; UNCOVERED (Worker E swap marker is sibling) |
| DMA / copy engines | `memory.cc:605,609,613` `Zero`/`Fill`/`Copy`; `memory.cc:787` alloc-zero | Depends on caller thread | inject postimage as `kDma`/`kHost`, else reject; UNCOVERED |
| Host memory writes | `kernel/kernel_module.cc:66` trampoline store; `kernel/xboxkrnl/xboxkrnl_threading.cc:1475-1521` APC scratch; `apu/audio_system.cc:275` | Host thread (no active guest) | inject postimage as `kHost`, else reject; UNCOVERED |
| Other-guest-thread writes | recorder cross-thread check (`cpu/guest_invocation_recorder.*`, rejection `kCrossThreadMutation`) | Partial (originating identity known) | reject today / model via scheduler; detection present, accounting UNCOVERED (Worker A) |
| Scheduler decisions | `kernel/guest_scheduler.*` | Yes | control tape; UNCOVERED (Worker A owns) |
| GPU swap marker | `gpu/pm4_command_processor_implement.h:508` `PM4_XE_SWAP` | n/a (guest-work boundary) | marker only; UNCOVERED (Worker E owns) |

## Ordering and preimage rules that constrain any wiring

- The A64 memory-access hook already snapshots discovered pages at the final
  root entry before the body mutates them
  (`cpu/guest_invocation_recorder.h` contract; A64
  `a64_guest_invocation_capture.cc`), so an export's out-parameter preimage must
  be read before the export body runs — the adapter takes the preimage at begin
  for exactly this reason.
- MMIO, clock, atomic and extern boundaries currently emit
  `OnUnsupportedDependency` (A64 `a64_seq_memory.cc` and
  `a64_guest_invocation_capture.cc`), which rejects the single-invocation
  recorder. A continuous runtime must replace reject-with-record only where a
  canonical, bounded, typed payload exists; unknown work keeps rejecting.
- No adapter may attribute an asynchronous mutation (GPU/DMA/host/other-thread)
  to the active guest thread; the implemented adapter enforces this by allowing
  only `kActiveGuestThread`/`kNone` sources.
