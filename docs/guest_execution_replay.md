# Bounded guest execution replay

This document defines the campaign endpoint above the single-invocation
primitive in `guest_invocation_replay.md`: capture and deterministically replay
a user-selected interval of real title CPU execution through Xenia's normal JIT.
The interval may be one invocation, one or more explicit frame markers, a guest
instruction budget, a capture-time duration, or a manual start/stop window.
Duration is a capture policy, not an artifact-format assumption.

Title-derived code and data remain local benchmark inputs and must never be
committed. Capture instrumentation is permitted only in a dedicated opt-in
Release build. Normal builds must compile out every capture field, branch,
helper and emitted hook instruction.

## Evidence boundary

An accepted interval replay proves that the recorded guest CPU work, thread
ordering and modeled external inputs reproduce the captured checkpoints and
final state. It is not by itself an FPS, GPU, APU, energy or whole-title result.
Those claims still require bounded live-title validation.

Replay executes the captured PPC instruction stream through Xenia's normal JIT
with a persistent `ThreadState` and host worker for every participating guest
thread. Events are a deterministic control tape for that execution; they must
not substitute recorded outputs for deterministic game computation. Only
explicitly modeled nondeterministic inputs may be injected.

The single-invocation recorder is the smallest segment producer and correctness
fixture. It is not the final workload unit. Production Processor, frontend and
A64 hooks target a serialized capture event sink that is independent of root
address, segment count, frame count and duration.

## Required architecture

1. **Capture session.** Assign a session epoch and one global event sequence.
   Attach the event sink only after per-title configuration is final and before
   translation. On shutdown, stop guest execution, detach the sink, then destroy
   its owner.
2. **Initial checkpoint.** Record every participating guest thread's pointer-free
   PPC state plus the sparse guest-memory preimages and emulator state required
   to begin replay. Use copy-on-first-write or an equivalently exact sparse
   scheme rather than copying unchanged pages for every event.
3. **Ordered CPU segments.** Retain exact successful function-definition order,
   translation/declaration dependencies, entries, exits and pre-access guest
   memory events. Multiple invocation recorders may contribute independently
   accepted segments to one session.
4. **External-event log.** Record the order, returned values and guest-memory
   effects of kernel/extern calls, MMIO, clocks, interrupts, atomics and
   reservations. Account explicitly for GPU, DMA, host and other-thread writes;
   uninstrumented mutation rejects the session instead of being attributed to
   the active guest thread.
5. **Deterministic scheduling.** After the start rendezvous has parked every
   participant at a re-enterable boundary, run the capture interval under a
   controlled guest schedule and record its thread-dispatch and synchronization
   order. This avoids pretending that coarse boundary events can reconstruct an
   arbitrary unobserved host-memory race. Replay must release the matching
   persistent worker at each event cursor; missing, extra or out-of-order events
   fail closed.
6. **Chunked storage.** Publish fixed-size chunks and periodic checkpoints with
   shared, content-addressed code and page data. Each chunk has strict count,
   byte and duration limits, hashes and an atomic same-parent publication step.
   Hitting a limit stops or rejects cleanly and never silently truncates.
7. **Offline runner.** Restore a checkpoint, recompile the captured PPC with the
   selected baseline or candidate JIT, replay recorded scheduling and external
   inputs, and verify every checkpoint, thread context, dirty page and consumed
   event before accepting timing.
8. **Timing.** Keep parsing, decompression, JIT warm-up, checkpoint restore and
   verification outside the primary CPU interval. Report reset/restore cost
   separately and reject workloads with insufficient guest work.

## Continuation architecture decision

An interval checkpoint resumes from synchronized guest state, not a captured
host instruction pointer or native stack. Each participant checkpoint records
the PPC context, the next guest PC, the owning guest-function extent and its
scheduler state. Replay restores the PPC context and enters through
`Processor::ExecuteRaw(pc)`. The normal frontend therefore recompiles from the
captured PPC boundary, and ordinary guest returns lazily resolve the recorded
caller continuation. This is the same architecture-neutral continuation model
used by Xenia's older host-thread save-state path; it remains valid across JIT
code-cache placement and baseline/candidate code generation.

The older `XThread::Save`/`Restore` implementation is not the capture path for
the canonical scheduler-on lane. It suspends and stack-walks a host `Thread`,
but scheduler-backed guest threads own fibers instead, and restore always
creates a host thread. The session path must rendezvous running fibers through
the existing emitted JIT safepoints, snapshot synchronized PPC state while the
scheduler owns dispatch, encode ready/running/blocked state explicitly, and
restore participants as fibers. Native fiber stacks are neither serialized nor
treated as portable replay state. A blocked participant whose wait/export state
cannot yet be reconstructed rejects the checkpoint.

One session assembler owns the global event sequence, stop policy and chunk
limits. Scheduler, PM4 and external-event components are source adapters only;
their local counters may prove gap-free delivery but never become competing
global sequence or stop authorities.

## Current status and ranked TODO (2026-08-27)

- [x] Implement strict invocation/corpus codecs, exact replay module, warmed A64
      runner, architectural reset, canonical configuration, provenance, bounded
      workload planning, paired subprocess driver and synthetic fault fixtures.
- [x] Implement the platform-neutral bounded single-invocation recorder, exact
      execution-corpus encoder and atomic one-segment bundle writer.
- [x] Land the capture-build-only serialized event coordinator, scheduler
      safepoint delivery, publication reentrancy/lock/race hardening and
      Processor/PPC frontend definition/dependency plumbing as isolated commits.
- [x] Add capture-build-only A64 function-entry, normal/abnormal-exit and every
      pre-memory-access hook, including fail-closed unsupported-dependency
      reporting. Normal builds compile out the hook path and its state.
- [x] Add the capture build option and bounded one-segment application lifecycle:
      strict configuration, executable/config hashing, safe detach and atomic
      publication. This is the implemented primitive, not the continuous-session
      owner.
- [x] Preserve the earlier complete capture Release `aa6bb8695` matrix: literal
      format/lint, 32 focused capture cases / 791 assertions, 26 focused
      execution-capture cases / 278 assertions, all 464 CPU cases / 109,720
      assertions, all 570 PPC suites / 169,515 cases and 30 Python driver tests.
- [x] Close every captured code/data page to the real 16 KiB macOS protection
      granule without weakening strict replay validation (`082480653`,
      `b478e7816`).
- [x] Make runtime-root selection and capture epochs coherent for already-
      translated code, reject capture-sink callback reentry and qualify the A64
      test-only entry hook (`359cbec91`, `d06a980e5`, `5c0787615`).
- [x] Publish the first atomic scheduler-on Halo title bundle and replay its
      title-derived PPC through the normal capture-disabled A64 JIT with exact
      architectural/page parity and stable warmed code shape. Matching the
      artifact required its recorded `spin_wait_yield_after=0` value.
- [x] Validate the equivalent capture-enabled exact head `403add9ae`: all 472
      CPU cases / 110,084 assertions, 26 focused execution-capture cases / 298
      assertions, 37 focused capture cases / 843 assertions and 14 recorder
      cases / 982 assertions. Capture-enabled timed replay also fails closed as
      required by `af1807327`.
- [x] Complete the capture-disabled final-head build and run the exact-head
      linked matrix, including the normal-build zero-hook, synthetic child-fault
      and distinct-binary attestation gates. The normal validator passed 407 CPU
      cases / 108,874 assertions, all 570 PPC suites / 169,515 cases, 30 Python
      driver tests and the capture-disabled Debug object compile. Symbol
      inspection found zero capture hooks; both negative child fixtures died by
      signal 10 without an accepted marker.
- [x] Integrate the capture-build-only scheduler transition observer
      (`760e24510`) after literal full-tree format/lint, capture-enabled
      `-Werror` object probes and 15 focused cases / 2,246 assertions. Its
      sequence remains source-local; the session assembler will assign the
      single global sequence.
- [ ] Finish the versioned continuation checkpoint: next PPC PC, exact owning
      function extent, explicit participant control state, and separate event
      actor/subject identities. Old artifacts must continue to decode with an
      unambiguous non-resumable state; malformed or ambiguous slice ownership
      must fail closed.
- [ ] Add the scheduler capture barrier. Request all running fibers at existing
      JIT safepoints, park dispatch without calling host-thread Suspend, snapshot
      synchronized PC/PPC/scheduler state, and release without changing ready or
      blocked queues. Cover running, ready, blocked, timeout, detach and
      rejection paths with focused tests.
- [ ] Make exact-corpus replay accept only checkpoint-declared PPC slice entries,
      restore scheduler participants as fibers and resume with
      `Processor::ExecuteRaw(pc)`. Do not map directly to a captured A64 PC or
      serialize native stacks. Reject blocked states until their wait/export
      reconstruction is modeled.
- [ ] Capture enough warmed, representative Halo 3 main-menu guest work to
      establish repeated A/A CPU-time noise outside restore, JIT and
      verification. The accepted frame-0 12-byte, one-function startup smoke is
      not eligible performance evidence and does not satisfy retained fixture 1
      below. Its two strict paired calibrations failed closed at 4.138% and
      2.267% A/A noise rather than establishing a floor.
- [x] Define and test the versioned session manifest, global event and sparse-
      checkpoint codecs, bounded reel collector, content-addressed page/code
      bundle, chunk closure and crash-safe publication/recovery rules.
- [ ] Wire the automatic continuous-session path: delayed PM4 arm/markers,
      permanent definition catalog, quiescent thread checkpoints, sparse memory
      preimages, content providers, scheduler dispatch/synchronization and typed
      external events. Replay must use persistent real `ThreadState` workers,
      not recorded deterministic outputs.
- [ ] Continue integrating Fable 5 only by verified component. The scheduler
      observer is now integrated. Split the PM4 source plumbing from its
      duplicate controller before integration: the assembler alone owns stop
      and global sequence, while the adapter must deliver every post-arm marker.
      The assembler, external adapters and persistent runner remain staging
      inputs; the runner still rejects scheduler/safepoint events emitted by a
      real session, and no Fable candidate carries Halo runtime evidence.
- [ ] Prove progressive synthetic intervals: nested calls, multiple roots,
      multiple threads, cross-thread writes, external events, chunk boundaries,
      stop/detach races, missing events, corruption and storage-limit rejection.
- [ ] Capture Halo 3 main-menu intervals progressively: one accepted segment,
      starting only after the retained warm/marker gate, one explicit guest-
      marker window, one second, multiple seconds and manual stop. Require exact
      baseline fidelity and report replayed CPU coverage.
- [ ] Establish A/A noise on retained Halo 3 main-menu sessions, re-evaluate T8
      and T9, rank new candidates by measured replay CPU weight, and implement
      one verified optimization per clean commit with full correctness and
      bounded live-title regression gates.

The accepted pre-protocol smoke bundle is
`/Users/admin/Documents/edge-benchmarks/title-captures/halo3-menu-8258d720-gs1-952aa5517-live1`.
It was captured at frame 0 and contains only root `8258D720-8258D728`, one
12-byte function and no dirty data pages. It proves that real Halo PPC can be
captured and executed offline; it does not prove main-menu interval fidelity or
performance.

## Halo 3 retained-baseline protocol

The retained workload is the unattended Halo 3 main menu driven by
`tools/bench/states/halo3-menu.script`. Use absolute title, script, storage and
output paths, the dedicated assertions-disabled capture Release build and the
canonical replay controls, including `--guest_scheduler=true`. The scheduler
mode is captured, fingerprinted and must match at replay; it is the retained
lane, not an A/B variable. Keep the input
script even with `--hid=nop`: the scripted driver is created first and supplies
one neutral controller for the full run.

The frame-0 startup smoke above predates this retained warm/marker boundary and
must not be promoted to fixture 1 or used to rank an optimization.

Warm for exactly 100 seconds and require the PM4 swap/present counter to advance.
Then arm at the next matching `kPm4Swap` marker. Its identity is the
`PM4_XE_SWAP` opcode, and it means that Xenia accepted an executed guest-
generated swap packet. It is a guest-work boundary, **not** proof that a host
drawable was presented. The progressive retained fixtures are, in order:

1. one accepted top-level invocation segment after the start marker;
2. one complete interval between consecutive matching PM4 markers;
3. a one-second duration window;
4. a ten-second duration window; and
5. a manual window stopped at an operator-selected PM4 boundary.

Do not advance to the next fixture until baseline replay consumes every event
and matches every participant context, checkpoint and page digest. For each
accepted fixture report both:

- semantic coverage: replayed architectural PPC instructions divided by
  accepted captured PPC instructions, which must be exactly 100%; and
- live CPU coverage: cumulative Mach thread-CPU deltas of participating guest
  threads divided by the process CPU delta over the same capture window.

The second ratio is the optimization ceiling; stack samples and the 30 fps swap
rate are not CPU-time accounting. For timing, reuse the identical retained
session for every subprocess, keep restore/JIT/verification outside the primary
interval, and run A/A and B/B controls plus both A/B and B/A orders. Every pair
and both cross-order means must clear the measured control/drift floor in the
same sign. Halo 3's capped swap rate remains a liveness check only.

For the one-segment, one-marker and one-second stages, cap the complete session
bundle at 1 GiB and refuse to start unless the Data volume has at least the cap
plus 8 GiB free. Raise the cap only after measuring a smaller fixture and
recording the new bound. Store title-derived bundles in a stable local capture
root outside Git source worktrees and outside `$TMPDIR` or `/private/tmp`, where
the trace-temp cleaner may reclaim stale artifacts. The target directory and
its `.part` sibling must not exist, and neither title bytes nor bundles may be
committed.

### Scheduler-mode provenance

`--guest_scheduler` is codegen provenance, not a tuning knob: it gates
`PreemptCheckInjectionPass`
(`src/xenia/cpu/compiler/passes/preempt_check_injection_pass.cc:45`), so a
capture and its replay must run the same value, and the effective value
is already an allowlisted entry of the canonical replay configuration
(`src/xenia/cpu/guest_invocation_replay_config.cc:71`), so the configuration
hash pins it. The tree default is `true` (`src/xenia/kernel/kernel_flags.cc:18`).
Two lanes exist, and every launch names one explicitly:

- **Scheduler-on (`--guest_scheduler=true`) is the primary retained Halo 3
  lane.** It is the normal title configuration, and its event stream includes
  the cooperative scheduler's thread-dispatch order.
- **Scheduler-off (`--guest_scheduler=false`) is a secondary compatibility and
  diagnostic lane**, not the canonical baseline. It remains useful for
  bisecting a scheduler-dependent divergence and for the synthetic `callret`
  lane.

Baseline and candidate are compared only on the identical retained session:
the same captured scheduler mode, the same event stream and the same bundle
hashes. A scheduler-on capture is never compared against a scheduler-off
capture, and a replay never changes the mode it was captured under.

**Gate.** Scheduler-on capture is not removed from rejection by deleting the
check. It is enabled only once the production scheduler observer records the
dispatch/synchronization order into the session event stream. Until then the
runtime keeps refusing `guest_scheduler=true` twice, at
`src/xenia/cpu/guest_invocation_capture_runtime.cc:330` and through the shared
validator it calls at `:335-337`, and the launch
harness refuses earlier with an exact diagnostic by scanning the executable:
scheduler-on requires the literal `XENIA_GUEST_EXECUTION_SCHEDULER_CAPTURE_V1`
and the absence of the legacy rejection text
`capture requires explicit --guest_scheduler=false`; every fixture beyond the
one-segment primitive requires `XENIA_GUEST_EXECUTION_SESSION_CAPTURE_V1`. The
runtime that delivers each capability must embed its literal in the arm banner
it logs, must not reuse the legacy literal for any other diagnostic, and must
record the effective `guest_scheduler` value in the published manifest, which
the harness checks against the requested lane after the run.

Locations that currently reject, require or assume `guest_scheduler=false`,
with their disposition:

| Location | Assumes | Disposition |
| --- | --- | --- |
| `src/xenia/cpu/guest_invocation_capture_runtime.cc:330-331` | rejects `true` with `capture requires explicit --guest_scheduler=false` | keep until the observer is integrated; then refuse only when the observer is absent, with a new diagnostic and the capability literals in the arm banner |
| `src/xenia/cpu/guest_invocation_capture_runtime.h:42-44` | "Capture fails closed unless it is false" | reword with the gate above when the runtime changes |
| `src/xenia/emulator.cc:235` | passes the effective `cvars::guest_scheduler` | unchanged; this is the provenance source |
| `src/xenia/cpu/guest_invocation_replay_config.cc:71` | `guest_scheduler` is a hashed codegen entry | unchanged; the configuration hash carries the mode |
| `src/xenia/cpu/guest_invocation_replay_config.cc:558` | capture and timed replay require the constant `false`: `ValidateGuestInvocationReplayBenchmarkConfig` also runs at capture time (`src/xenia/cpu/guest_invocation_capture_runtime.cc:335-337`), so removing the `:330` check alone does not enable scheduler-on capture | must become "replay value equals the captured value" and, at capture, "explicit either way"; `src/xenia/cpu/testing/guest_invocation_replay_config_test.cc:418-420` pins the constant and changes with it |
| `src/xenia/cpu/jit_corpus.h:51-61`, `src/xenia/cpu/jit_corpus.cc:61-63` | records `kConfigGuestScheduler` at capture (`kVersion` 3) | unchanged; already mode-neutral |
| `src/xenia/cpu/ppc/testing/ppc_testing_main.cc:1365-1384` | corpus replay adopts the capture's mode | unchanged; correct |
| `tools/bench/bench_guest_invocation.py:119` | `FIXED_RUNNER_FLAGS` hardcodes `false` | take the mode from the artifact manifest, identical on both arms; until then it drives only scheduler-off artifacts |
| `tools/bench/bench_callret.py:66` | synthetic `callret` lane hardcodes `false` | secondary lane only; unchanged |
| `tools/bench/test_verify_corpus.py:30,38`, `tools/bench/test_guest_invocation_bench.py:379-396`, `tools/bench/test_callret_bench.py:170-181`, `src/xenia/cpu/testing/guest_invocation_capture_bundle_test.cc:105` | test fixtures pass `false` | unchanged; they test flag forwarding |
| `docs/guest_invocation_replay.md:44,74,147,241,350` | describes the implemented one-segment primitive and timed-replay validator | unchanged until the runtime changes |
| `docs/menu_cpu_campaign.md:228-236` | `false` was permanent and the baseline | superseded for this campaign in that bullet; historical live rows at `:70,104,735,1165` stay as recorded |
| `docs/guest_execution_replay.md` (this file, formerly protocol and gates) | `false` in the protocol and PPC gate | replaced by the explicit lane |

### Evidence classes

Keep these four claims separate and name which one a result is:

1. **Scheduler-on primary evidence**: a retained scheduler-on session whose
   baseline replay consumed every event and matched every checkpoint, then a
   paired candidate replay of that same session.
2. **Scheduler-off secondary evidence**: the same procedure on a scheduler-off
   session. It supports a diagnosis and never substitutes for the primary
   lane.
3. **Offline CPU proof**: the replayed CPU-time comparison inside an accepted
   session, whichever lane. It bounds the JIT's share and says nothing about
   presentation, GPU, APU or whole-title behavior.
4. **Bounded live-title proof**: a paired live run of the same title state
   under the same lane that clears the measured live floor. It is required
   before any title-speed claim.

### Progressive commands (documented, not executed)

`tools/bench/halo_capture_launch.py` is the only launch path. It refuses to run
when any precondition fails, prints every failure, and records the executable
SHA-256, the embedded build commit, the tool commit, the complete command,
the configuration flags, both volumes' free bytes, the present-counter samples
and the published manifest digests in `<target>.launch.json`. Add `--dry-run`
to check every precondition and print the command without launching. The
lanes below name the fixture, in order; `--scheduler on` is the primary lane,
and each run needs a fresh `--run-name`. Nothing below has been executed.

```text
python3 /abs/tools/bench/halo_capture_launch.py \
  --exe /abs/Xenia-edge.app/Contents/MacOS/Xenia-edge \
  --build-commit <40-hex> --expected-exe-sha256 <64-hex> \
  --title /abs/halo3 --input-script /abs/tools/bench/states/halo3-menu.script \
  --storage-root /abs/storage --capture-root /abs/captures \
  --scheduler on --fixture segment --run-name halo3-menu-segment-001 \
  --root-address <8-hex> --root-end-address <8-hex> --occurrence <n>

... --fixture marker      --run-name halo3-menu-marker-001
... --fixture second      --run-name halo3-menu-second-001
... --fixture ten-seconds --run-name halo3-menu-10s-001 \
    --bundle-cap-bytes <measured> \
    --prior-fixture-report /abs/captures/halo3-menu-second-001.launch.json
... --fixture manual      --run-name halo3-menu-manual-001 \
    --bundle-cap-bytes <measured> \
    --prior-fixture-report /abs/captures/halo3-menu-10s-001.launch.json
```

The segment fixture uses the implemented occurrence-selected primitive
(`--guest_invocation_capture_*`); a segment published before the 100 s warmup
is not accepted. The marker, duration and manual fixtures pass the proposed
session flags `--guest_execution_capture_output`, `_marker_source=pm4_swap`,
`_warmup_ms=100000`, `_boundary=guest_marker_count:1 |
capture_duration_ns:<n> | manual`, `_bundle_cap_bytes` and, for manual,
`_stop_file=<target>.stop`; the session runtime that implements them must
adopt these names or this section changes with it. The ten-second and manual
fixtures require an explicit cap justified by an accepted smaller fixture's
recorded bundle size.

## Acceptance gates

No title interval is accepted unless all applicable gates are green:

- `./xb format --all` leaves the tree clean;
- a fresh linked Release capture build and a normal build succeed with warnings
  treated as errors for changed translation units;
- focused coordinator, lifecycle, session-codec and replay tests pass;
- the complete source-backed PPC corpus has nonzero work and no failed, crashed,
  timed-out or unrun suite under the artifact-matching scheduler mode, with
  `--guest_scheduler=true` required for the retained Halo lane;
- the normal build contains no capture hook instructions or capture-path state;
- every expected fault/rejection produces no canonical success marker;
- replay consumes the complete event stream and matches all recorded state;
- baseline and candidate use the identical retained session and produce identical
  verified outputs; and
- paired A/A and A/B results clear the measured noise floor without a bounded
  live-title regression.
