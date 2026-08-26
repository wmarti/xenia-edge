# Guest invocation replay

This document defines the narrow first step from compile-only JIT corpora to
repeatable offline execution of code captured from a real title. The target is
CPU optimization measurement, not a general save-state format.

## What v1 proves

An accepted v1 replay proves that one captured guest function invocation:

- starts from real title code, architectural PPC state and guest memory;
- executes through the normal warmed JIT backend without a kernel, GPU or APU;
- returns to the captured guest address;
- produces the captured architectural state and dirty-memory contents; and
- performs no lazy code placement inside the timed region.

This is evidence about the captured invocation. It is not evidence of an FPS
change, whole-title determinism, GPU performance or save-state correctness.

The existing JIT corpus cannot provide this proof by itself. It contains guest
code pages and function extents for compilation, but no PPC registers, writable
memory or chronological execution state.

## Artifact split

Code and execution inputs remain separate:

1. A `.jcorpus` supplies immutable guest code pages, captured function extents,
   replay-relevant guest function metadata and successful-definition order.
2. A versioned invocation artifact supplies provenance, entry and expected exit
   PPC state, initial data pages (including the complete host-protection-granule
   closure needed by replay) and expected dirty pages whose final bytes differ
   from their initial bytes.

The invocation artifact records the capture build hash as provenance, plus
compatibility hashes for the exact code corpus and canonical replay
configuration. Replay hashes the corpus and configuration again and fails on
either mismatch. It reports the candidate executable's own identity but does
not require it to equal the capture build: an optimized A/B candidate is a
different executable by definition. Captured title bytes and title-derived
artifacts are benchmark inputs and must not be committed to the source tree.

The file format is pointer-free and endian-defined. It never serializes
`PPCContext` as a raw host structure: host pointers, padding, preemption state
and backend-private storage are not portable execution state.

## Canonical replay configuration

Capture and replay use one shared, versioned configuration serializer. It
fingerprints the selected host backend, host platform/ABI, runtime-selected
code-cache indirection mode, the actual initialized writable-executable or
split-view code mapping, backend codegen predicates derived from the actual
guest-memory mapping, host protection-page size, detected host ISA feature
mask, Release and LTO build modes, JIT tracing/profiling features and an
explicit allowlist of effective CVar values read by PPC translation,
optimization, backend emission or generated-code runtime helpers. Effective
values are read after command-line, per-title and global-config precedence;
hashing only global or command-line values would silently miss a live title
override.

The allowlist is deliberately not a scan of every CPU-category option. Log and
dump paths, disassembly output and compile-only reporting do not change a
warmed invocation and must not invalidate a replay. Conversely, a missing
allowlisted CVar, unknown build-feature bit, unsupported backend or schema/order
change fails closed. Changing the allowlist requires a serializer version bump.

A timed v1 replay is limited to the A64 backend on an Apple host. Configuration
capture and serialization remain generic for cross-platform diagnostics, but
other backend/host pairs are not benchmark inputs. Timed replay additionally
requires `guest_scheduler=false`, an explicit CMake `Release` build with
assertions disabled, no JIT tracing or profiling, no CPU trace or coverage
counters, no call-path/remap counters, no debug optimization mode, no execution
breakpoint and no recorded-MMIO-aware store specialization. It also disables
early precompilation, compile-time reads from read-only guest memory and MMIO
inlining, and requires serialized guest-function definition so code placement
cannot race. These are acceptance gates, not values copied from an artifact.

The current fingerprint distinguishes LTO from non-LTO binaries and the two
runtime code-mapping layouts. It does not yet canonically identify the compiler,
SDK or complete compile and link flags. Therefore externally supplied binaries
and binaries produced by a different build tree are not accepted for v1 timing,
even if their current replay-configuration hashes match. That restriction can
be lifted only after those build-mode inputs have a canonical fingerprint.

V1 intentionally canonicalizes these compile-time inputs: capture and replay
must both use `enable_early_precompilation=false`,
`fold_readonly_guest_memory_loads=false`, `inline_mmio_access=false` and
`serialize_guest_function_definitions=true`. This keeps compilation dependent
only on captured inputs and makes definition order deterministic. It does not
claim the resulting JIT shape matches a stock live run unless that run was
captured with the same fixed controls.

## Accepted dependency envelope

V1 is deliberately limited to self-contained guest computation. A capture is
not replayable if the selected invocation observes any of the following:

- an MMIO access;
- a kernel export, builtin or other guest-to-host call;
- the guest clock or time base;
- an atomic operation or live reservation;
- a physical-memory alias, until replay can guard and reset every alias of the
  same backing page;
- memory mutated by another guest thread during capture;
- recursion, an unbalanced/tail return, or an incomplete trace; or
- a memory page that was not snapshotted before the invocation could write it.

Dependency flags are part of the artifact contract. Unknown flags and any flag
outside the accepted envelope fail closed. An exclusion is a capture result,
not a reason to weaken replay validation; another hot function should be
selected instead.

This envelope will not catch an uninstrumented host writer by assertion alone.
Baseline replay must therefore match the live-captured output exactly, and more
than one representative live invocation must be collected before a benchmark
is treated as representative.

## Bounded capture

Capture is opt-in and compiled only in a dedicated instrumented `Release`
build. It must not enable the general ITRACE, DTRACE, FTRACE or profiler build
options: those features are part of the compatibility fingerprint and are
rejected by timed replay. The dedicated recorder hooks are intentionally not
part of the semantic configuration hash because a quiet runner omits them; the
separately recorded capture-executable hash provides their provenance. Normal
emulator builds do not contain or run recorder callbacks.

The recorder targets explicit guest entry addresses and uses dedicated hooks
at the function and guest-memory access instrumentation points. A target is
collected with a bounded convergence loop:

1. A discovery invocation records every guest data page touched by the target
   and its nested guest calls.
2. At the next target entry, all known pages are copied before guest execution.
3. If execution touches a new page, that sample is incomplete and the page set
   is expanded for a later invocation.
4. A sample is written only when no new page appears, the target returns
   normally, no excluded dependency occurs, and every configured count and
   size bound is respected.
5. Initial contents for every touched page and its required host-protection
   closure, final contents for pages whose bytes differ, and complete
   pointer-free entry and exit PPC state are recorded for that exact invocation.

Capture attempts, pages, bytes and accepted samples all have explicit limits.
Hitting a limit rejects the sample instead of truncating it.

The legacy `Memory::Save` stream is not used as an intermediate capture format.
It is implicit, unversioned at the memory layer and tied to native heap-page
state; importing it here would inherit the save-state problems this format is
intended to avoid.

## Offline replay

Replay performs these operations outside the timed region:

1. Parse both artifacts with strict size, version, ordering and hash checks.
2. Create bare guest memory, a backend, a `Processor` with no export resolver,
   and one simulated `ThreadState`.
3. Map the corpus code pages and invocation data pages, then create a
   replay-only module containing the corpus's exact function-entry and extent
   map.
4. Restore the architectural entry state and initial pages while preserving
   runner-owned context pointers. Clear preemption, reservation and stale
   backend stack state, then derive the backend scalar rounding and VMX NJM
   caches from the restored FPSCR and VSCR.
5. Resolve and invoke the selected function once as a warm-up.
6. Require the warm-up exit state and every captured data page to match: changed
   pages compare with their final images and all others with their initial
   images.
7. Reset state, resolve the root again and snapshot code-cache placement.
8. Run a bounded batch, checking placement again before accepting timing.
9. Reset once more and require a final verified invocation to match.

Execution replay rejects truncated or non-canonical corpora, duplicate records,
missing extent pages, data/code overlap, MMIO pages and physical aliases. During
untimed verification, every host protection granule outside the closure of the
recorded data set and code corpus must remain inaccessible. Every 4 KiB page in
an allowed granule is supplied and validated, so an adjacent-page access remains
deterministic. On a host with protection granules larger than 4 KiB, however,
replay cannot independently trap a new access to another supplied page in the
same granule. V1 records this granularity limitation instead of claiming a
guest-page access boundary that the host cannot enforce.

Before each timed invocation, replay restores the full architectural input
state and copies only pages whose accepted final bytes differ from their input
bytes, plus any additional pages required by the host reset granularity. The
marker reports those reset pages and bytes so reset cost is visible rather than
silently attributed to guest execution.

The timed metric on macOS is current-thread CPU time from
`THREAD_BASIC_INFO`; monotonic wall time is diagnostic. Reset/copy work inside
a repeated batch must be reported explicitly because it dilutes the guest-code
signal. Captures with excessive reset cost or insufficient guest work are not
benchmark candidates.

## Performance acceptance

An optimization comparison is accepted only when all of these hold:

- baseline and candidate binaries pass their normal PPC correctness gates;
- artifact, corpus, build and configuration provenance is complete;
- baseline and candidate produce identical verified replay outputs;
- every timed run has the requested invocation count and unchanged code-cache
  placement generation;
- A/A and B/B controls are stable enough to resolve the proposed effect;
- A/B and B/A ordering agree in sign beyond that measured floor; and
- no crash, timeout, zero-work run, missing marker or rejected sample is
  included in the statistics.

Results from a handful of functions are weighted by measured live execution
counts only when that provenance matches the captured title and scenario.
Unweighted results remain per-invocation results.

## Path to continuous gameplay segments

A fixed-work multi-thread gameplay replay remains a later layer. The current
legacy save-state path is not its foundation yet: its kernel stream layout is
misaligned, failure cleanup is unsafe, thread and clock state are incomplete,
and GPU state is not preserved.

That layer starts only after save/restore is versioned, fail-closed and covered
by synthetic round-trip tests. Its first fidelity reference uses the same Metal
backend and measures guest CPU threads only. Null GPU and no-op APU modes are
admitted only if their untimed work and end-state digests match the Metal
reference.
