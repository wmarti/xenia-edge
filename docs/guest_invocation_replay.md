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

## Campaign status and TODO (2026-08-27)

The replay consumer is implemented as isolated commits: strict artifact and
execution-corpus codecs, exact replay module, architectural reset, Apple/A64
warmed runner, canonical configuration fingerprint, bounded workload planning,
single-invocation CLI, current-thread CPU timing, paired fail-closed driver and
copyright-free synthetic fixtures. This is not yet a real-title replay result;
the linked end-to-end gate and recorder producer remain open.

- [ ] Finish adversarial hardening and the serialized Release validation matrix:
      `./xb format --all`, Python driver tests, full CPU tests, exactly 169,048
      PPC corpus cases, one valid linked replay, and actual omitted-page and
      `0x7F` child failures with no accepted marker.
- [x] Add a deterministic exact-corpus builder that retains the complete
      translation/declaration closure required by executed functions, including
      static callees and helper/save-restore metadata that translation looks up
      even when that function is not entered during the selected invocation.
- [ ] Add the bounded platform-neutral recorder state machine: explicit root and
      occurrence selection, convergence across attempts, pointer-free entry/exit
      state, page/granule closure, call-stack checks, dependency flags and strict
      count/size/deadline rejection.
- [ ] Add build-only A64 boundary, translation-dependency and pre-memory-access
      hooks. Reject extern/kernel, MMIO, clock, atomic/reservation, recursion,
      tail/longjmp imbalance, async reentry, self-modifying code and cross-thread
      writes instead of truncating or approximating the capture.
- [x] Add a reusable writer for one accepted invocation and exact corpus. It
      validates and round-trips the payloads, records the capture-build and
      canonical-config hashes, never replaces output or staging, and publishes
      through one same-parent directory rename.
- [ ] Wire the bounded recorder to hash its running executable and call the
      bundle writer. Captured title bytes remain local benchmark inputs and must
      never be committed.
- [ ] Pass the recorder's synthetic positive and negative matrix before enabling
      a bounded GTA capture. For each accepted title invocation, require exact
      baseline offline output parity and stable code shape before collecting CPU
      timing.
- [ ] Rank optimization candidates from paired per-invocation CPU deltas weighted
      only by matching live execution counts. Implement one candidate per commit
      and require focused tests, full PPC semantics, paired offline replay and a
      bounded title-level regression guard before adoption.

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

The paired v1 corpus is expected to be the compile closure needed by the
selected invocation, not an unfiltered whole-session function archive. Replay
still validates the complete recorded successful-definition order and resolves
every entry before timing; the recorder should therefore reject an invocation
whose required closure cannot be represented within the replay workload
budget, rather than truncating the order.

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

The canonical driver now fails closed unless the operator supplies
`--same-build-tree-toolchain-attested`. This is a procedural gate: it records an
explicit assertion that A and B were produced from the same build tree,
compiler, SDK, configuration and flags, but it is not machine evidence that the
assertion is true. The result JSON says `machine_verified: false`. A retained
configure/build log remains required provenance, and the attestation does not
make an externally supplied or separately configured binary acceptable.

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
8. Hash the actual warmed A64 code into an ASLR-normalized structural identity.
9. Run a bounded batch, checking placement again before accepting timing.
10. Reset once more, require a final verified invocation to match, and require
    the normalized warmed-code identity to remain unchanged.

The normalized identity is computed in captured function-definition order. It
includes guest function extents and every warmed A64 instruction word, except
that immediate values in a known greater-than-32-bit MOVZ/MOVN-plus-MOVK chain
and displacement bits in PC-relative instructions are masked. The wide-move
opcode, destination register, lane shifts, chain length and actual emitted
instruction count remain part of the identity. Thus an ASLR-induced change in
the materialization chain or emitted code size is rejected rather than hidden.
The encoding cannot distinguish a host pointer from another greater-than-32-bit
constant, so all such wide constants are intentionally masked structural inputs
rather than exact semantic values. The marker also reports the function count,
actual host instruction count, materialization-site count and PC-relative-site
count. The paired driver pins the complete tuple independently for role A and
role B across all subprocesses.

This is a code-*shape* identity, not an exact native-code digest. It deliberately
cannot distinguish a change only in a masked pointer value or branch
displacement. Artifact/configuration hashes and output verification remain
separate gates; shape consistency does not prove toolchain sameness or a title
performance result.

Before creating guest memory or compiling a function, replay also applies a
checked aggregate workload budget to the paired corpus: at most 32,768 eager
function definitions, 16 MiB summed across their inclusive guest extents and
128 MiB of nonzero captured host-code sizes. Overlapping guest extents are
charged once per function because the runner compiles each function. Every
addition is checked against the remaining budget, so a large encoded value
cannot wrap an accumulator. These are conservative safety ceilings with more
than two times the measured full-title function and guest-byte workload, not
capture targets; an invocation-specific corpus should normally be much
smaller.

The captured-host byte total is a bounded-work and provenance heuristic, not a
promise that a changed candidate backend emits the same number of bytes. The
128 MiB ceiling leaves substantial headroom in the current 256 MiB generated
code cache, but candidate expansion and other backend failures remain fatal
subprocess results and are never accepted as benchmark samples.

Execution replay rejects truncated or non-canonical corpora, duplicate records,
missing extent pages, data/code overlap, MMIO pages and physical aliases. During
untimed verification, every host protection granule outside the closure of the
recorded data set and code corpus must remain inaccessible. Every 4 KiB page in
an allowed granule is supplied and validated, so an adjacent-page access remains
deterministic. On a host with protection granules larger than 4 KiB, however,
replay cannot independently trap a new access to another supplied page in the
same granule. V1 records this granularity limitation instead of claiming a
guest-page access boundary that the host cannot enforce.

The runner itself is created only on Apple A64. An access outside the supplied
closure deliberately reaches a no-access mapping and may terminate the process;
some unsupported virtual ranges may fault earlier in memory-heap lookup. V1
does not recover in-process with a signal handler or non-local jump across JIT
and C++ frames. Canonical benchmarking runs every invocation executable as a
disposable subprocess through `tools/bench/bench_guest_invocation.py`. A signal,
nonzero exit, timeout, or missing canonical marker invalidates the campaign and
is never converted into a sample. Direct command-line replay is useful for
diagnosis, but it is not the fault-containment boundary and may terminate when
a capture omitted a dynamically accessed page. This subprocess boundary limits
failure propagation; it is not a security sandbox for untrusted artifacts.

Before title capture is enabled, a linked synthetic integration gate must run
an invocation that accesses an omitted page (including an unsupported `0x7F`
range) through the canonical driver. Passing means the real child fault or
rejection produces driver exit 2, an invalid report and no accepted sample. The
focused Python test mocks signaled and markerless child results; it proves the
driver's rejection logic, not the real JIT fault path or operating-system
containment by itself.

The Release `xenia-cpu-ppc-tests` binary creates the copyright-free inputs for
that linked gate with one explicit test-only option:

```text
xenia-cpu-ppc-tests \
  --cpu=a64 \
  --guest_invocation_synthetic_fixture_out=/new/explicit/output/directory \
  --test_benchmark_warmed=false \
  --guest_scheduler=false \
  --jit_corpus_allow_incomplete=false \
  --count_call_paths=false \
  --count_physical_remap_hits=false \
  --emit_mmio_aware_stores_for_recorded_exception_addresses=false \
  --enable_early_precompilation=false \
  --fold_readonly_guest_memory_loads=false \
  --inline_mmio_access=false \
  --serialize_guest_function_definitions=true \
  --trace_function_coverage=false \
  --cpu_trace_mask=0
```

The directory must not already exist. Generation initializes the real backend,
captures and validates its canonical replay configuration, warms and verifies a
four-instruction `lwz/addi/stw/blr` function, records the backend's actual host
code size and hashes the exact running executable. It then publishes one exact
corpus, a valid single-invocation artifact, omitted-page and `0x7F` fault
artifacts, and `manifest.json` together by renaming a sibling staging directory.
Every artifact is encoded, decoded, written, reread, rehashed and decoded again
before publication.

The manifest contains the complete effective configuration, configuration,
corpus, artifact and capture-executable hashes, the corpus shape, and the exact
reset-page and reset-byte expectations. The valid artifact must emit the one
canonical 17-field benchmark marker. Each fault artifact must be launched as a
disposable child; a signal, rejection or other nonzero exit with no accepted
marker is the expected containment result. These fixtures contain only bytes
constructed by the test generator and must never be replaced with or committed
alongside title-derived bytes.

Before each timed invocation, replay restores the full architectural input
state and copies only pages whose accepted final bytes differ from their input
bytes, plus any additional pages required by the host reset granularity. Reset
copy source and destination descriptors are prepared before timing, so the
repeated path performs no page lookup. The marker reports reset pages and bytes
and a separately measured reset-only batch after the primary measurement.

The timed metric on macOS is current-thread CPU time from
`THREAD_BASIC_INFO`; monotonic wall time is diagnostic. Boundaries are nested as
`wall start, CPU start, work, CPU end, wall end`: the primary CPU interval does
not include the wall-clock queries, while the diagnostic wall interval includes
both CPU-clock queries. Reset/copy work remains included in the primary
reset-plus-call metric. Its separate reset-only CPU and wall intervals have a
different cache and execution history, so they are raw diagnostics only and
must never be subtracted from the primary metric. Replay invokes and verifies
the guest again after that diagnostic before accepting any marker. Captures
with excessive reset cost or insufficient guest work are not benchmark
candidates.

The strict benchmark-output marker is `XENIA_GUEST_INVOCATION_BENCHMARK_V3`.
V3 adds the normalized warmed-code identity and four structural counts as one
atomic schema change. The driver rejects V1, V2 and all partially upgraded
markers. This replacement is safe because no title capture or accepted title
benchmark used the earlier markers.

## Performance acceptance

An optimization comparison is accepted only when all of these hold:

- baseline and candidate binaries pass their normal PPC correctness gates;
- artifact, corpus, build and configuration provenance is complete;
- baseline and candidate produce identical verified replay outputs;
- every timed run has the requested invocation count and unchanged code-cache
  placement generation;
- every subprocess for each role has the same normalized warmed-code identity;
- the procedural same-build-tree/compiler/SDK/flags attestation is present and
  backed by a retained configure/build log;
- A/A and B/B controls are stable enough to resolve the proposed effect;
- every A/B and B/A pair clears that measured floor in the same canonical sign;
  and
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
