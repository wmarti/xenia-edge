# Autonomous PPC JIT benchmarking, verification, and optimization

This is the working contract between the two machines that measure this
project. It is checked in so both sides read the same document, and so a run
months from now can be compared against the conditions this one was made under.

## 1. What the two machines are for

The JIT has two backends and neither machine can speak for the other.

| | Apple Silicon (this Mac, M4 Pro, 12 core) | ylab cluster (SLURM) |
|---|---|---|
| Backend | `a64` | `x64` |
| Owns | `[A64]` commits | `[x64]` commits |
| Timing method | wall clock, min of N, interleaved A/B | instruction counts (primary), exclusive-node wall clock (secondary) |
| Microarchitectures | M4 Pro | Zen 2 (AVX2) and Sapphire/Emerald Rapids (AVX-512) |
| Real game code | yes, the only place it can run | no |

`[CPU]` commits touch the shared PPC frontend and the HIR, so they change
*semantics for both backends at once*. They are the reason this is a two-machine
setup rather than two independent efforts: a NaN or FPSCR change that is right
on `a64` and wrong on `x64` is invisible to either machine alone.

### ylab node inventory

Timing never happens on `login-01` (8-core EPYC 7252, ~60 interactive users).
It is a submission host only. Everything real goes through `sbatch`/`srun`.

| Node | CPU | ISA ceiling | CPUs |
|---|---|---|---|
| `epyc-7502` | EPYC 7502 | AVX2 | 64 |
| `am-02`, `am-03` | EPYC 7402 | AVX2 | 48 |
| `a4500-02..04` | Threadripper 3960X | AVX2 | 48 |
| `ad-01` | Xeon Gold 5418Y | AVX-512 (vbmi/vnni/bf16) | 48 |
| `rtx6000-bw` | Xeon Silver 4514Y | AVX-512 | 64 |

Build and time on node-local `/tmp` (~320 GB free). `/home` is NFS at 96%
capacity and must not carry build trees or timing runs.

Covering both classes is not optional. The x64 backend gates code on
`kX64EmitAVX512Ortho` in seventeen places, plus `kX64EmitAVX512VBMI`,
`kX64EmitAVX512DQ`, `kX64EmitAVX512BW` and `kX64EmitGFNI` — none of which a
Zen 2 node ever executes. A gate that only ever runs on `epyc-7502` leaves
every one of those paths unmeasured, and a fault in one stays invisible until
the code reaches an Intel machine.

## 2. The measurement substrate

`xenia-cpu-ppc-tests` is both the correctness suite and the benchmark workload,
which is what makes an unattended loop affordable. Each case carries
`REGISTER_IN`/`REGISTER_OUT` annotations and memory assertions, so a run that
gets faster while breaking an assertion is a regression, not a win.

Measured on the M4 Pro: **169,055 cases across 486 suites in 40 seconds**, with
917 cases skipped by the checked-in `skip.txt`. A full-corpus correctness gate
therefore costs well under a minute per binary. There is no reason to gate on a
sampled subset.

Flags that matter:

- `--test_bin_path=<dir>` — the prebuilt corpus (`tools/bench/ppcbin.tar.zst`,
  493 suites). It is PowerPC machine code, so it is architecture-neutral and
  both backends answer the same questions from the same bytes.
- `--test_path=<dir>` — **drives discovery, and is a trap.** Suites are
  enumerated from the `.s` sources in this directory, not from the corpus. Point
  it at a ref whose tree lacks a test and that test is silently never run. Using
  each ref's own tree made `edge` look clean because it never saw the four cases
  written for the fixes. Always pass the *same* `--test_path` to both sides of a
  comparison.
- `--test_skip_file=<path>` — `skip.txt` is byte-identical between `edge` and
  `a64-fixes-on-edge`, so pass-set diffs across those two refs are honest. Check
  this again whenever a new ref enters the rotation.
- `--test_passed_file=<path>` — writes the name of every passing case. This is
  what makes the gate a set difference rather than a count comparison.
- Exit status is 0 only when everything passed, 1 otherwise.

### Per-suite isolation is mandatory

A whole-corpus run in one process is faster but useless as a gate the moment a
backend faults. `edge` dies of `SIGBUS` on `instr_seq_stacksync` and takes the
remaining 375 suites with it, so a naive comparison silently rests on 23% of the
corpus. `tools/bench/verify_corpus.py` gives each suite its own process and
records a crash as a verdict; `tools/bench/compare_verify.py` diffs two such
runs and exits non-zero on any regression.

Suite verdicts are ranked `pass < fail < timeout < crash`. A suite moving up
that ranking is a regression; moving down is a fix. Both are reported, because
a change that only ever produces "fixes" is usually a corpus or discovery bug
rather than good news.

## 3. The three loops

### L1 — Verification (the gate, runs on everything)

Full corpus, per-suite, on both backends, for every ref under consideration.
Two comparisons come out of it:

1. **A/B on one backend** — did this ref break something the previous ref got
   right?
2. **`a64` vs `x64` at the same ref** — do the backends still agree? This is the
   only check that can catch a `[CPU]` semantic change that is wrong on exactly
   one backend, and it is the reason ylab is in the loop at all.

Cost is roughly a minute per binary. It gates everything else.

### L2 — Benchmarking

Only runs on refs that cleared L1; timing a miscompile is meaningless.

- **Mac**: minimum of N interleaved runs, A/B order flipped every iteration so
  thermal drift is not charged to whichever ref happened to go first. The
  minimum is reported rather than the mean because it rejects scheduler
  migration and thermal noise.
- **ylab**: `perf_event_paranoid` is 4 on every node, so hardware counters are
  unavailable to us and `perf stat` is not an option. `valgrind` is installed,
  so **callgrind instruction counts are the primary x64 metric** — deterministic,
  immune to the fact that this is shared hardware, and comparable across weeks.
  JIT-generated code needs `--smc-check=all-non-file` or callgrind will read
  stale translations. Exclusive-node wall clock (`--exclusive`) is the secondary
  confirmation, and disagreement between the two is itself a finding.
- Every result records the conditions it was taken under: host, node, governor,
  load, thermal state, power source. A prior audit on a loaded shared box
  manufactured a 47% regression that repeat measurement erased completely, so a
  number without its conditions is not evidence.

#### Choosing a control

The noise floor has to come from a control, but picking the wrong one silently
destroys the headline number. The first attempt used a *suite* the branch was
believed not to touch (`instr__gen_vand`) on the theory that its delta is pure
measurement error. Measured, that control moved **-9.3%**, which on its face
would mean the branch's -8.8% total is entirely noise.

That reading was too confident, and later measurement undercut it. `vand` is a
0.15s suite, and sub-second suites on this machine do not resolve at all: at
N=25 the same suite came out **+0.84%** with 15.5% spread, and a second tool
put it at +0.65%. The -9.2% was not a measurement of anything. On x64,
callgrind — which is deterministic — puts `vand` at **+0.02%**.

So the honest position is that **the control moved by an unknown amount**,
which is worse for a control than moving by a known one. The argument for
distrusting a suite-based control does not depend on the number, though: this
branch changes guest-address zero-extend folding, stackpoint records, and the
`PPCContext` layout, all of which are on the path every suite executes, so
there is no suite it demonstrably leaves alone. A control has to be something
the change provably cannot reach.

**The control must be the same binary against itself.** Running ref A as both
sides of the comparison isolates measurement error, because the two sides are
byte-identical by construction. Any suite-based control needs an argument that
the branch cannot reach it, and for a change touching the common path no such
argument exists.

A useful side effect: when the A-vs-A floor is low and every suite including the
"control" improves by a similar amount, that is evidence the gains are
broad-based — in the shared prologue, addressing, and context layout — rather
than confined to the instruction each suite names. That is a stronger result
than a per-instruction win, not a weaker one.

### L3 — Optimization

Proposes changes, then has to pass L1 on both backends and show a win in L2
beyond the noise floor before the change is worth anything.

**The decided boundary for unattended operation is measure-and-report.** L1 and
L2 run autonomously on both machines and report what they find, including
bisecting a regression to the commit that introduced it. Code changes are not
proposed or landed without a human.

That boundary is not timidity about the tooling — the gate is strong and the
floor is real. It is that **the corpus is not a specification**. 169,059 cases
passing means no known case disagrees, not that the JIT is correct. A
miscompile can clear the entire corpus and still break a real title, and the
only place that can be checked is the Mac, by running actual game code. Until
that check is part of the loop, an automated "this passed, so it landed" is
writing cheques the corpus cannot cash.

What the loop is therefore allowed to do on its own:

- Run L1 on every ref pair and report regressions, with the exact set
  difference behind each verdict.
- Run L2 on anything that cleared L1 and report deltas against a measured
  floor.
- Bisect a regression to a commit and say which one.
- Re-run a result that failed to reproduce, and discard it rather than publish
  it.

What always stops for a human: writing or landing a change, and any judgement
about whether a semantic difference is acceptable.

## 4. Autonomy mechanism

Git is the coordination bus; both machines reach `github.com` (including ylab's
compute nodes, verified). No shared filesystem is needed and no new service has
to be stood up.

- Results land on a `bench-results` branch under `results/<machine>/<utc-date>/`,
  so each machine writes only its own subtree and the two never conflict.
- A result directory holds the two `verify_corpus.py` JSON documents, the
  `compare_verify.py` diff, the timing CSVs, and a `conditions.json` describing
  the machine state during the run.
- Each machine runs a periodic agent: fetch, find ref-pairs with no result,
  build, run L1, run L2 if L1 is clean, commit, report. On ylab that agent's job
  is to *submit* `sbatch` work and collect it afterwards, never to compute on
  the login node.

### Which python to run the bench tools with

`tools/bench/macwin.py` needs pyobjc for `--park`, window enumeration and
window-targeted capture. The system `/usr/bin/python3` does not have it;
`bench-work/venv/bin/python3` does. Run `frame_ab.py`, `live.py` and anything
else that touches windows with the venv interpreter, or `--park` degrades to
`--park ignored: pyobjc not importable` and the emulator window lands wherever
macOS puts it -- on top of whatever the user is doing.

Note also that `--extra-a` / `--extra-b` values must be passed with `=`
(`--extra-a=--guest_scheduler=false`). Given as a separate argument, argparse
takes the leading `--` as an option and the run dies before it starts.

### Running a pass

One command per machine, and both are idempotent — anything already done is
reused, anything missing is rebuilt:

    ./tools/bench/run_lane.sh                        # Mac, a64
    sbatch tools/bench/slurm/x64-pipeline.sbatch     # ylab, x64

Neither depends on state prepared by hand. That matters more on the cluster
than it sounds: podman's graph root is `/var/user/$UID/containers/storage` and
the build trees live in `/tmp`, both node-local, so an hour of setup on
`epyc-7502` buys nothing on `ad-01` and a cleanup erases it. The pipeline
archives the built image to NFS after building it, which turns a first run on
a new node from a ten-minute apt job into a one-second load.

### Sharing the repository with other sessions

Several Claude sessions edit this tree, and one pushed to the bench branch
mid-session while there were uncommitted changes in the same file — the merge
was real and had to be resolved by hand. The rules that keep that survivable:

- Fetch before editing anything shared, and rebase rather than merge.
- Never force-push the bench branch.
- Results stay local to the machine that produced them. Publishing them to a
  shared branch would add a second thing to collide over for no benefit, since
  whoever is coordinating can read both machines directly.
- Every result bundle records the tooling commit it came from and whether that
  tree was dirty, so a number can always be traced back to code.
- `run_lane.sh` warns when the branch has moved underneath it or when
  `tools/bench` has uncommitted changes.

## 5. Guardrails

These are what make unattended operation defensible rather than merely possible.

- Never push to `edge` or `a64-fixes-on-edge`. Work happens on dated branches.
- A correctness regression **stops** the loop and reports. It does not get
  averaged, retried until green, or carried forward.
- A performance result that cannot be reproduced on a second run is discarded
  rather than published.
- ylab holds at most one exclusive node at a time, with a bounded wall-time
  request, because it is shared departmental hardware.
- Every published number carries its conditions, and every gate decision carries
  the set difference it was based on.

## 6. Current state

### Measured on the Mac (M4 Pro, 2026-08-22)

`edge` = `1c7df55b`, `a64-fixes-on-edge` = `405af1e0` — a 44 commit stack
(30 `[A64]`, 8 `[CPU]`, 6 `[x64]`). `bench-macos-arm64` carries the tooling.

**Correctness**, full corpus, per-suite, both refs asked the same questions:

| ref | suites | cases | failed | crashed |
|---|---|---|---|---|
| `edge` | 493 | 169,058 | 4 (`instr_mcrf`) | 1 (`instr_seq_stacksync`, SIGBUS) |
| `a64-fixes-on-edge` | 493 | 169,059 | 0 | 0 |

Gate verdict: **no regressions, two fixes** — `instr_mcrf` fail→pass and
`instr_seq_stacksync` crash→pass. The stack fixes a real fault; the four cases
that exist only on the branch (`njm_flip_and_back`,
`njm_off_preserves_denormals`, `njm_on_flushes_denormals`,
`stacksync_longjmp_repair`) are what demonstrate it.

**Performance**, min of 5, interleaved with the A/B order flipped each run:

| | `edge` | `a64-fixes-on-edge` | delta |
|---|---|---|---|
| `instr__gen_fmadds` | 1.049 | 0.922 | **-12.1%** |
| `instr__gen_fmuls` | 0.092 | 0.083 | -9.9% |
| `instr__gen_vavgsb` | 0.155 | 0.140 | -9.5% |
| `instr__gen_fadd` | 0.084 | 0.077 | -9.1% |
| `instr__gen_vperm` | 3.639 | 3.319 | -8.8% |
| `instr__gen_vmaddfp` | 3.761 | 3.452 | -8.2% |
| `instr__gen_vsel` | 3.482 | 3.204 | -8.0% |
| `instr_mcrf` | 0.016 | 0.016 | +0.6% |
| **TOTAL** | **16.661** | **15.239** | **-8.5%** |

Measurement error, from running `edge` against itself: **0.07%** on a quiet
machine, **1.06%** while the machine was also driving SLURM jobs over SSH;
worst substantial suite ±0.7%. The improvement is between eight and eighty
times the floor depending on conditions, so it is not in question.

The total reproduced across two independent min-of-5 runs taken hours apart
under different load — **-8.5%** and **-8.4%**, with every per-suite figure
within about a point. A result that does not reproduce is discarded rather than
published, so this is the number of record. `instr_mcrf` getting marginally slower is
consistent with the fix: it now copies the CR field instead of comparing it
against zero.

**Only the suites above about a second are actually measured.** Everything
under roughly 0.4s — `vand`, `vaddfp`, `vavgsb`, `vavgsw`, `vpkuhus`, `fadd`,
`fmuls`, `mcrf` — runs a sample spread of 7-92%, and the delta between refs is
smaller than that spread. Raising the repetition count from 5 to 25 does not
converge them: at N=25 `vand` came out +0.84% with 15.5% spread and `fadd`
came out -31.6% with 72.5% spread. The variation is process startup, which no
amount of repetition removes, because it is not the thing being timed.

Two different tools disagree by more than ten points on the same sub-second
suite while agreeing to a fraction of a point on the large ones. Treat the
small-suite figures as **not measured**, which is a different statement from
"no change".

What survives that: the five suites over a second move -7.8% to -9.8% on a64,
with spreads of 1.7-4.9%, reproduced across two independent tools and four
runs. `ppc_testing_main.cc` is byte-identical between the refs, so the harness
is not contributing to it.

### ylab

- Source is cloned at `~/xenia-ci/src`; job scripts in `~/xenia-ci/jobs`, logs
  in `~/xenia-ci/logs`. Everything runs through `sbatch`; nothing computes on
  the login node.
- **Bare-metal gcc-13 does not work.** CMake configure fails at
  `Package 'gtk+-x11-3.0', required by 'virtual:world', not found`, because
  `xenia-cpu-ppc-tests` links `xenia-ui` and on Linux that pulls GTK3,
  fontconfig, xcb, X11 and wxWidgets — for a binary that never opens a window.
  There is no root and no module system to fix that with.
- **The container route works.** `localhost/xenia-build:noble` is built and
  holds the CI toolchain: clang-21, lld-21, g++-14, GTK3, fontconfig, meson,
  mako/pyyaml/packaging, and valgrind. Rootless podman needs
  `--cgroup-manager=cgroupfs --events-backend=file` and its own
  `XDG_RUNTIME_DIR` because compute nodes have no systemd user session; leave
  the storage root at its default, since overriding it collides with the
  existing user namespace. `--log-driver=none` silently discards container
  stdout under `sbatch`.
- **Two SLURM/podman rules that cost real time to find, and that any job on
  this cluster has to follow:**
  1. *Container stdout does not reliably reach the sbatch log.* Redirecting
     `podman run` output on the host produced empty or truncated logs, which
     read as "the build printed nothing" rather than as a capture failure. Have
     the container write to a bind-mounted file and read that file from the
     host afterwards. `--log-driver=none` makes it worse by discarding output
     outright.
  2. *`/tmp` is node-local.* Logs written there by a job on `epyc-7502` are not
     visible from the login node afterwards, and reading them means another
     `srun` onto the same node. Copy anything worth keeping back to NFS home
     before the job exits.
  3. *Run the tests inside the image they were built in.* The binary links
     `libSDL3` out of its own build tree, so on the bare node it dies at
     `error while loading shared libraries` before printing anything.
  4. *`--shm-size=2g` is required.* The JIT code cache is a 256 MB `shm_open`
     mapping, mapped twice — an execute view and a write view — and podman
     defaults `/dev/shm` to 64 MB. Without it the cache fails to allocate and
     every suite segfaults, which is easy to misread as a backend bug.

  The third and fourth of those produced the same symptom from opposite causes,
  and both initially read as a clean gate — see below.
- Building both refs in that image is in flight.
- Still open: `pip install` on the bare node is refused under PEP 668 and needs
  `--break-system-packages` or a venv — only relevant if we ever go back to a
  non-container build.
- The headless CMake target remains the durable fix and is worth doing even
  once the container works, because it removes the dependency on containers
  being available at all.

### Measured on ylab (AMD EPYC 7502, x64, in-container)

The same corpus, the same common test path, the same gate:

| ref | suites | cases | failed | crashed |
|---|---|---|---|---|
| `edge` | 493 | 169,058 | 4 (`instr_mcrf`) | 1 (`instr_seq_stacksync`) |
| `a64-fixes-on-edge` | 493 | 169,059 | 0 | 0 |

Repeated on `ad-01` (Xeon Gold 5418Y, Sapphire Rapids), which exercises the
seventeen `kX64EmitAVX512Ortho` sites and the VBMI/DQ/BW/GFNI paths that a Zen 2
node never reaches: **the same numbers again**. That run also started from a
bare node with nothing prepared and completed in eighteen minutes end to end,
which is what makes the pipeline usable unattended.

| backend | machine | `edge` | `a64-fixes-on-edge` | gate |
|---|---|---|---|---|
| `a64` | M4 Pro | 4 failed, 1 crashed | clean, 169,059 | pass, 2 fixes |
| `x64` AVX2 | `epyc-7502`, Zen 2 | 4 failed, 1 crashed | clean, 169,059 | pass, 2 fixes |
| `x64` AVX-512 | `ad-01`, Sapphire Rapids | 4 failed, 1 crashed | clean, 169,059 | pass, 2 fixes |

**Three backends and microarchitectures, identical verdicts on 169,059 cases**,
case count for case count: the same four `instr_mcrf` failures, the same
`instr_seq_stacksync` crash, the same two fixes.

That agreement is the whole reason for running two machines. The eight `[CPU]`
commits change PPC semantics — NaN propagation, FPSCR summary bits, denormal
flush, CR6 reduction — for `a64` and `x64` at once, and a change that is right
on one and wrong on the other is invisible to either machine alone. The two
backends reaching byte-identical verdicts on 169,059 cases is the strongest
statement available that those commits are semantically neutral.

It is not proof. Both backends share the PPC frontend and the HIR, so a mistake
made there is reproduced faithfully on both sides rather than exposed by the
comparison. What the differential rules out is a *backend-specific* divergence,
which is the failure mode the `[A64]` and `[x64]` commits actually risk.

### x64 performance, by instruction count

Deterministic counts from callgrind on `epyc-7502`, so there is no noise floor
to argue about — a repeat run reproduces these exactly.

| suite | `edge` | `a64-fixes-on-edge` | delta |
|---|---|---|---|
| `instr__gen_vand` (control) | 7,056,240,079 | 7,057,910,328 | **+0.02%** |
| `instr__gen_fadd` | 3,282,870,661 | 3,282,928,413 | +0.00% |
| `instr__gen_vpkuhus` | 7,126,542,664 | 7,128,889,457 | +0.03% |
| `instr__gen_vavgsb` | 7,096,569,406 | 7,070,813,334 | -0.36% |
| `instr__gen_vavgsw` | 7,097,459,022 | 7,068,726,035 | -0.40% |
| `instr_mcrf` | 216,830,888 | 215,710,348 | -0.52% |
| `instr__gen_fmuls` | 3,373,447,955 | 3,336,985,652 | -1.08% |
| `instr__gen_fmadds` | 40,983,983,671 | 40,281,803,563 | -1.71% |
| **TOTAL** | **76,233,944,346** | **75,443,767,130** | **-1.04%** |

Two things are worth reading carefully here.

**This table is not comparable to the wall-clock numbers, because it covers
the wrong suites.** Seven of the eight are sub-second, chosen because they are
what the six `[x64]` commits name — and those are exactly the suites where
nothing resolves. The one large suite in the set, `fmadds`, moves -1.71%. Read
this table as "the small suites do not move much on x64", not as the branch's
x64 improvement.

**On x64 the control behaves like a control**, moving +0.02%. On a64 the same
suite moved -9.2%, because the a64 commits change addressing, guest frames and
`PPCContext` layout — code every suite runs. The same suite is a valid control
for one backend and a useless one for the other, which is a good argument for
calibrating against an identical binary rather than against a suite believed to
be untouched.

A caveat on the metric: these counts cover the whole process, including JIT
compilation and the test harness, so a change confined to generated code is
diluted by everything around it. The counts are exact but they are not a
measurement of the emitted code alone.

### The large x64 suites: no change at all (job 103439)

Five hours of callgrind on `epyc-7502` over the two biggest suites in the
corpus, which the table above does not reach:

| suite | `edge` | `a64-fixes-on-edge` | delta |
|---|---|---|---|
| `instr__gen_vsel` | 171,712,643,046 | 171,717,222,596 | **+0.003%** |
| `instr__gen_vperm` | 172,050,578,523 | 172,055,008,443 | **+0.003%** |

Different binaries — the counts are close but not equal, so this is not the
same-binary trap. They execute the same x64 instruction stream. `vmaddfp`
completed its count (172,949,345,163) but exited non-zero and was dropped.

### The -7.8% x64 wall-clock figure is withdrawn

An exclusively allocated `ad-01` (Sapphire Rapids, AVX-512) reported -7.80%
overall, with spreads of 11-20%. An exclusively allocated `epyc-7502` (Zen 2,
AVX2), same tooling, same corpus, reports **+1.16%**, with `vmaddfp` at
**+16.74%** against ad-01's -7.53%. A sign flip of that size between two
measurements of the same two binaries is not a property of the binaries.

The cause was in this repo, not in the cluster: `bench_pair.py` calibrated
A-vs-A on `args.suites[0]`, which both jobs set to `instr__gen_vand` — 0.31s,
essentially all of it process startup. Startup is extremely reproducible, so
the calibration reported a 0.07% "measurement error" and the six-second suites
were read against it. Fixed in `dd3d69b37`: calibration now runs on the
longest non-startup-bound suite.

Callgrind is the tiebreak and it has no noise floor: on the two largest suites
the delta is +0.003%. **The branch does not change what the x64 backend
emits for these suites.** The a64 -8% stands — spreads of 1.7-4.9% against a
delta of 7.8-9.8%, and 30 of the 45 commits are `[A64]`.

### Eight of thirteen suites cannot measure anything

On ylab every one of `vand`, `vaddfp`, `vavgsb`, `vavgsw`, `vpkuhus`, `fadd`,
`fmuls`, `mcrf` finishes at 0.314s, which is bare process startup in the
container. Three of them showed a confident `+15.9%` with a 0.2% spread: that
is 0.314s → 0.364s, a fixed ~50 ms difference in how long the two binaries
take to load, and none of it is guest code.

This also clears the open `vpkuhus` lead. `1c102190d` replaces a guest→host
call per PACK 8_IN_16 unsigned execution with `vpminuw`/`vpackuswb`, and
`vpkuhus` is exactly that opcode. It showed nothing because the suite runs
almost no guest code, not because the commit does nothing. Validating it needs
a workload that executes the pack path — real game code, or a synthetic loop.

`bench_pair.py` now measures startup directly, marks these suites
startup-bound, and leaves them out of the total.

## What the corpus actually measures

Every test case in the generated corpus is one guest instruction and a `blr`:

```
test_vsel_1_GEN:
  #_ REGISTER_IN v1 [...]
  vsel v4, v1, v2, v3
  blr
```

Around that, `ppc_testing_main.cc` resets the guest memory heap
(`memory_->Reset()`, line 291), tears down and rebuilds `ThreadState`, JITs the
function, calls it once, and string-compares the registers. Measured cost per
case on an M4 Pro:

| suite | cases | µs/case |
|---|---|---|
| `instr__gen_vand` | 650 | 208.9 |
| `instr__gen_vsel` | 15,600 | 221.3 |
| `instr__gen_vperm` | 15,600 | 234.8 |
| `instr__gen_vmaddfp` | 15,600 | 241.0 |
| `instr__gen_fmadds` | 3,456 | 285.9 |

Roughly 800,000 cycles to execute one guest `vand`. Sampling `instr__gen_vsel`
(1,825 main-thread samples, `edge`) shows where they go:

| | samples | share |
|---|---|---|
| `BaseHeap::RebuildFreeBlocks` | 859 | 47% |
| `Memory::Reset` → `__bzero` | 233 | 13% |
| `ThreadState` ctor (mmap/munmap) | 183 | 10% |
| Capstone disassembly | 200 | 11% |
| all JIT frames | 108 | 6% |
| — of which code generation | 31 | 1.7% |
| **executing guest code** | **0** | **0%** |

`RebuildFreeBlocks` is a linear scan of the ~1M-entry page table, run once per
test case. There are no samples in `HostToGuest`, none in `Function::Call`,
none in JITted code.

## Attributing the a64 gain

`edge` rebuilt with one half of the branch applied at a time, five large suites,
min of 5:

| build | vs `edge` | detail |
|---|---|---|
| `base-only` — 26 `[Base/POSIX]` + `[VFS]` | **+0.11%** | all five unresolved |
| `cpu-only` — the 55 JIT commits | **-9.47%** | all five resolved, spreads 1.6-6.7% |
| full branch | **-8.70%** | matches `cpu-only` |
| `cpu-only`, harness debug dumping off | **-4.32%** | four of five resolved |

The gain is entirely in `src/xenia/cpu`; the POSIX layer contributes nothing.
That refuted the reading the profile suggested, which is why the split was
built rather than argued.

**About half the gain is the harness disassembling less code.**
`ppc_testing_main.cc:268` sets `DebugInfoFlags::kDebugInfoAll`, so every one of
the 15,600 JITted functions is dumped four ways — PPC source, raw HIR,
optimised HIR, and the emitted host code walked instruction-by-instruction
through Capstone by `A64Assembler::DumpMachineCode`. That last cost is
proportional to how much code the backend emits. Rebuilding both refs with the
flag set to `kDebugInfoNone` (a measurement-only patch, never landed) takes the
delta from -9.47% to **-4.32%**. The dumping portion itself falls from 3.60s to
2.58s, **-28%** — a direct proxy for the branch emitting less host code.

So the -8.7% decomposes as: roughly half a code-size effect visible only
because the harness disassembles everything, and roughly half JIT throughput
and per-function overhead. **None of it is the speed of the emitted code**,
which each suite executes exactly once per test case.

## What this means for the branch's performance commits

Of the 87 commits, ~35 make an explicit performance claim. **None has
individual runtime evidence** — the finest attribution that exists is the
four-row table above, and the smallest unit in it is 55 commits at once.

- **Cannot be tested by this corpus at all** — they change how fast an emitted
  sequence runs, not how large it is: `c4a4239d4` (SELECT_V128 BIT/BIF),
  `d73cfa239` (vperm REV32 tables), `e16e9995d` (merges as zip), `b07a4187d`,
  `38e7c5687`, `b1b9aab4f`, and all six `[x64]` commits — the last independently
  confirmed at +0.003% by callgrind.
- **Partially evidenced, as code size** — they shrink emitted code, which is
  what the -28% dumping drop measures: `6c66fa966` (denormal flush in four),
  `9d5f12670` (FMA fixup in four), `5b15a057a` (NaN paths to the tail),
  `818537738` (prologue safepoints), `caa641401` (stp/ldp pairing). Smaller is
  not shown to be faster.
- **Proven, but as correctness rather than performance**: `752ddf5f9` (mcrf)
  and `97343ebff` (stacksync), both confirmed on a64, AVX2 and AVX-512.

Closing this gap needs a workload that executes guest code in a loop — real
game code, or a synthetic harness — not more runs of this corpus.

## Measuring the emitted code

`tools/bench/gen_loop_bench.py` puts the opcode in a guest loop — 512M to 1B
executions per suite — so the emitted sequence is ~98% of the run instead of
~0%. `edge` vs the branch, M4 Pro, min of 5, measurement error 0.17%:

| suite | `edge` | branch | delta |
|---|---|---|---|
| `vsel_lat` | 1.118 | 0.325 | **-70.95%** |
| `vsel_tp` | 1.384 | 0.344 | **-75.15%** |
| `vperm_lat` | 1.472 | 0.498 | -66.18% |
| `vperm_tp` | 1.380 | 0.342 | -75.19% |
| `vmaddfp_lat` (nonfinite path) | 3.082 | 1.964 | -36.28% |
| `vmaddfp_tp` | 1.385 | 0.381 | -72.50% |
| `vnmsubfp_lat` (nonfinite path) | 3.424 | 2.285 | -33.27% |
| `vnmsubfp_tp` | 1.438 | 0.439 | -69.43% |
| **TOTAL** | **14.683** | **6.578** | **-55.20%** |

All eight resolved. Both refs compute the same final register in every suite,
checked by running a short version with a deliberately wrong `REGISTER_OUT` and
comparing what the harness reports — so this is not a faster wrong answer.

`vsel_lat` is the cleanest reading. 512M serial selects: `edge` spends 8.7
cycles each, the branch 2.5. That is three dependent instructions collapsing to
one, which is exactly what `c4a4239d4` claims, and it had never been measured
before because the corpus cannot see it.

**These are microbenchmarks of a single opcode, and the percentages are not
game speedups.** They say the emitted sequence is ~3.4x faster, not that
anything is 3.4x faster. What fraction of real guest code is `vsel` is a
separate question, and the next one worth answering — there are titles in
`~/Documents/X360-Games`.

The float `lat` suites diverge to Inf/NaN within a few iterations and stay
there, so they measure the nonfinite path rather than ordinary operands. That
is worth having — `ccaa671b0`, `dcf981c08`, `8c59d9030` and `1dd664b3f` all
target exactly that path, and -36% is the first evidence any of them work — but
it must be read as such. The `tp` suites keep their float inputs normal.

## x64 on the guest loop: the six [x64] commits work, and work hugely

`epyc-7502` (Zen 2, AVX2), exclusively allocated, min of 5, measurement error
1.63%, spreads mostly 0.1%:

| suite | `edge` | branch | delta |
|---|---|---|---|
| `vavgsb_lat` | 9.030 | 0.715 | **-92.08%** |
| `vavgsw_lat` | 6.475 | 0.715 | **-88.96%** |
| `vpkuhus_lat` | 2.718 | 0.665 | **-75.53%** |
| `vand_lat` | 1.466 | 0.415 | -71.72% |
| `vsel_tp` | 1.467 | 0.465 | -68.32% |
| `vperm_tp` | 1.567 | 0.515 | -67.15% |
| `vsel_lat` | 1.617 | 0.565 | -65.06% |
| `vmaddfp_tp` | 1.466 | 0.565 | -61.48% |
| `vperm_lat` | 1.817 | 0.765 | -57.88% |
| `vmaddfp_lat` | 2.218 | 1.166 | -47.43% |
| `vnmsubfp_tp` | 1.516 | 0.965 | -36.32% |
| `vnmsubfp_lat` | 3.169 | 2.267 | -28.45% |
| **TOTAL** | **34.527** | **9.783** | **-71.66%** |

All twelve resolved.

**This reverses the conclusion recorded above.** Callgrind put the two largest
corpus suites at +0.003% and the reading taken from it — "the branch does not
change what the x64 backend emits", "x64 got essentially nothing" — was wrong.
The count was not wrong; it measured a workload in which guest code is a
rounding error, so it could not have detected this and its near-zero result was
never evidence either way. Withdrawing a claim for being unresolved was right;
concluding "no change" from it was not.

`1c102190d` is the clearest case. It claims to replace a 16-iteration scalar
load/lea/sar/store loop through two stack spills with four instructions, and a
guest→host call per PACK with `vpminuw`/`vpackuswb`. Measured: `vavgsb` 12.6x,
`vavgsw` 9.1x, `vpkuhus` 4.1x. The corpus suites for exactly those three opcodes
are the ones sitting at the 0.314s startup floor, which is why this went
unmeasured for so long.

## Real game code: the first A/B that measures a title's own workload

A Halo 3 menu corpus captured with `--jit_corpus_out` (8,524 functions,
1,167,562 guest instructions), replayed through both builds. Same corpus file
both sides — capturing separately would measure scene drift, not codegen.

| | `edge` | `a64-fixes-on-edge` | delta |
|---|---|---|---|
| host instructions | 7,792,627 | 7,124,393 | -8.58% |
| **stable instructions** | 7,241,611 | 6,769,709 | **-6.52%** |
| host/gi | 6.6743 | 6.1019 | |
| **host-address chains** | **311,272** | **169,737** | **-45.5%** |
| as share of emitted | 9.43% | 5.52% | |

Compare the *stable* total, not the raw one: MOVZ/MOVK chains encode host
addresses, so raw counts move with load address between processes. The stable
metric charges each chain one instruction.

The branch's effect is broad rather than narrow — only five functions, 0.8% of
emitted bytes, were left essentially untouched.

### What is left: host-address materialization

169,737 chains, 5.52% of every host instruction emitted, averaging 2.32
instructions each. Every one is a 64-bit host address — a helper, the
guest-to-host thunk — built lane by lane, and every one is replaceable by a
single `ldr` from the backend context, because those addresses are stable once
the code cache initialises.

The case is unusually complete for an unstarted optimisation:

- The technique is proven in this tree. `616fb633e` moved three constants at
  indirect-call sites into `A64BackendContext`, citing `call_indirect` at 53.9
  bytes per occurrence as the largest single per-call cost in the backend.
- It measurably worked: 141,535 chains removed, 45.5% of the total.
- It was applied only at call sites. The remaining 169,737 are everywhere else.
- Generalising it removes ~223,728 instructions, **3.1% of all emitted code**,
  on real game code rather than a microbenchmark.

The `bench/mtl3-tracedump` worktree built the analysis that finds this — the
wide-move classifier and its "replaceable by one ldr from the backend context"
comment are theirs — and wrote two commits making the metric stable and
fail-closed (`d7d95f9c2`, `7a415f50d`). Neither acts on it, and a search of all
211 commits on that branch finds no fix.

### Secondary: five functions the branch did not move

`8271ACC8` at 34.80 host instructions per guest instruction, `827196C0` at
28.53, `822F9260` at 18.88, `8215B774` at 22.70, `825A18C0` at 19.20. Together
0.8% of emitted bytes, so not where the mass is — but they are the only
functions 87 commits left alone, and 35:1 on real code is worth understanding.

### What this does not say

Emitted size, not time. Fewer instructions is real icache and decode pressure,
but it is not a measured speedup. `gen_loop_bench.py` measures time on synthetic
code; the corpus measures size on real code. Nothing yet measures time on real
code — the replay compiles the corpus, it does not execute it, because doing so
needs the kernel and import thunks it deliberately does without.

### Two capture traps, both hit here

- **Do not capture with `--trace_function_coverage`.** It inlines per-guest-
  instruction counters into the emitted code, so the recorded `host_code_size`
  is one the replay can never reproduce. The first capture came back 46% larger
  than its own replay on the same binary.
- **The faithfulness gate cannot reach zero with `RawModule`.** It does not
  populate `instruction_flags_`, so `GetInstructionAddressFlags` returns
  nullptr, `IsPossibleMMIOInstruction` is false, and MMIO-aware stores are never
  emitted offline — while the capturing `XexModule` had `accessed_mmio` set.
  Expect a small negative delta on MMIO-touching functions; the clean capture
  here sat 3% under, with 3,365 of 8,524 functions identical.

## Where the x64 headroom is

The callgrind result says the x64 backend was left where it was, so the leads
below are all against `edge` as it stands.

**1. x64 forgets its MXCSR mode at every basic block.** `x64_emitter.cc:264`:

```cpp
while (block) {
  ForgetMxcsrMode();  // at start of block, mxcsr mode is undefined
```

This is the pattern `90aefe81e` replaced on a64 with a per-edge meet, where it
was measured at 1.3% of all emitted instructions on the float corpus suites.
`vldmxcsr` is considerably more expensive than the ARM `msr fpcr` this
replaced, and it is forced at the top of every block containing a float op.
The a64 commit already paid for the design: it documents three attempts
refuted by disassembled counterexamples — `ControlFlowAnalysisPass` edges are
stale by emission time and never included fall-through, label `->block`
back-pointers are equally stale, and branches sit *mid*-block in this HIR so a
per-block exit mode miscompiles a scalar-then-vector diamond. The x64 emitter
walks the same HIR through the same `block->label_head` chain. Three of the
five suites that carry signal are float suites.

**2. x64 `SELECT_V128` on AVX-512 emits three instructions where one would
do.** `x64_sequences.cc:936`:

```cpp
if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
  e.vmovdqa(e.xmm3, src1);
  e.vpternlogd(e.xmm3, src2, src3, ...);
  e.vmovdqa(i.dest, e.xmm3);
```

`vpternlogd` is destructive in its first operand, so when `dest` aliases any
source both copies go away — permuting the immediate covers the `src2`/`src3`
cases. The a64 audit behind `c4a4239d4` found 15,605 `SELECT_V128` sites in
this corpus, *all* of which take an aliased path. Worse, the AVX-512 branch is
tested first, so on AVX-512 hardware a select that `mayblend == Int8` would
render as a single `vpblendvb` becomes three instructions instead — AVX-512
hardware emitting more than AVX2 hardware for the same HIR. `instr__gen_vsel`
is the largest suite in the corpus at 171.7 billion instructions.

Caveat worth measuring rather than asserting: `vmovdqa` xmm→xmm is
move-eliminated at rename on recent Intel cores, so the cost here is
front-end and code-size rather than execution latency.

**3. The same staging pattern at the other `vpternlogd` sites.**
`x64_seq_vector.cc:740` and `:860` both do `vmovdqa32 xmm3, src1` before
`vpternlogd xmm3, ...`. Of the four sites in the backend, only `NOT_V128`
(`x64_sequences.cc:3123`) writes `i.dest` directly.

**4. The float-mode theme is seven commits on a64 and one on x64.** a64 got
the FPCR entry/return contract, conditional-region merges, the per-edge meet,
host-transition guards, `VSCR.NJ`-gated denormal flushes, a four-instruction
flush, and NaN handling gated on the result rather than the operands. x64 has
`77a164cb7` alone.

Nothing here is written yet — the autonomy boundary is measure-and-report, and
these are proposals for a human to take or refuse.

### Real game code (Mac only)

The corpus is the gate, but it is not the ceiling. The pieces for running real
titles are already on this machine and are what would eventually justify a
wider autonomy boundary:

- `~/Documents/X360-Games` holds real dumps — Gears of War (`4D5307D5`),
  `4D5307E6`, GTA IV, Halo Reach — and `~/Documents/xenia-bench` is a harness
  (XeniaBench) that already knows how to launch them: a host daemon, per-run
  copy-on-write checkouts, alias-based asset mapping in
  `configs/assets.local.json` so raw paths never leak, and content-addressed
  artifacts.

The honest caveat is that XeniaBench is built for a different question. It
evaluates whether *agent systems* can operationalize runtime failures, and it
carries the machinery that goes with that — blinded review, frozen experiment
manifests, reviewer quorums. It is also mid-edit: several files are modified in
the working tree, `.venv` is not installed, and `data/runs` is empty. Adopting
it wholesale to answer "does Gears of War still boot after this JIT change" is
using a research instrument as a smoke alarm.

What the JIT loop actually needs is much smaller: launch a title, drive it to a
known state, run for a fixed interval, and compare something stable against a
recorded baseline — that a frame was presented, that the guest reached a known
address, that no unimplemented-instruction path was taken. The `headless` cvar
suppresses UI prompts but does not remove the need for a window, so this stays
Mac-only and stays a coarse gate rather than a per-commit one.

Worth building deliberately rather than by adapting the larger harness, and
worth building before the autonomy boundary moves past measure-and-report.

### A gate that passed on nothing

Worth recording, because it is the failure mode an unattended loop is least
able to notice. When the x64 binary could not find `libSDL3`, every suite
exited before printing a count. Both refs failed identically, so the set
difference was empty, and the gate reported **"no regressions, gate: pass"** on
two runs that had executed zero test cases.

The verdict logic was right and the inputs were garbage. Two changes now make
that impossible: suite verdicts are counted directly rather than inferred from
the parsed `Failed:` totals, which are `0` when nothing ran at all, and a run
that executed no cases fails outright on whichever side it happened on. The
same shape of check belongs on anything else the loop learns to compare —
agreement between two broken measurements is not evidence.

### Immediate next steps

1. Finish the x64 build of both refs and run the same per-suite verification.
2. Run the cross-backend differential — `a64` vs `x64` pass-sets at the same
   ref. This is the check that no single machine can perform, and the `[CPU]`
   commits are what put it at risk.
3. Establish the x64 performance baseline with callgrind instruction counts,
   then confirm with exclusive-node wall clock on an AVX2 node and an AVX-512
   node.
4. Stand up the results branch and the periodic driver on both machines.

## Execution weighting, and the rankings it destroyed

Everything above this line ranks emitted code. None of it knows whether the code
runs. Closing that gap changed the answer by a factor of forty and retired two
optimizations that had looked like the obvious next moves.

### Getting the counters out

`--trace_function_coverage` had counted per-guest-instruction executions for a
while, but the only reader was `Profiler::Dump`, which needs
`-DXENIA_ENABLE_PROFILER=ON`. That build cannot run a title: `profiling.cc` mints
a MicroProfile token per guest function, `MICROPROFILE_MAX_TIMERS` is 1024, and
past that `MicroProfileGetToken` returns `MICROPROFILE_INVALID_TOKEN` whose
`0xFFFF` timer index is written past the end of the timer arrays. Halo 3 JITs
8,524 functions and the process dies two frames in, with no crash dump and no
exit marker. So the counters were unreachable on exactly the runs worth
measuring. `--trace_function_coverage_out` writes the tables from an ordinary
build; `--trace_function_coverage_period` rewrites them every N seconds so a
scripted run does not depend on someone quitting the emulator by hand.

### Three attribution errors, and what they cost

| what it did | what it produced |
| --- | --- |
| weighted a function's whole body by its guest-instruction executions | `82103AD8` and `82080608` -- ordinary loads and a return, one chain each -- ranked for billions of chain executions. That chain is the `preempt_yield_handler` materialization in the preemption **tail**, the cold side of a branch. |
| the same, on a function with a hot inner loop | `825A7EC8` spends its count in eight `db16cyc`. Its materializations sit at calls and in tails *outside* the loop. Smeared, it read as a 92.25B-execution hotspot. |
| priced a sequence at executions x its **average** emitted size | `load_offset i32` came top at 14.3%, from 110.87B executions x 30.2 average bytes. The key covers the normal, MMIO, constant and register variants, whose sizes differ, so the product is only valid if size and heat are uncorrelated. |

The fix is to charge each site its own bytes, keep tail bytes separate, and count
wide-move chains per sample at emit time. Then the saving from a proposed change
T is exactly

    score(T) = sum over affected sites s of  E_s * (C_old,s - C_new,s)

with `E_s` the site's own execution count, never a function or sequence average.

### What that says about the wide-move chains

| model | claimed saving from chain -> one ldr |
| --- | --- |
| gross chain instruction share, smeared per function | 4.02% |
| corrected for the replacement not being free | 2.36% |
| **exact, per site** | **0.10%** |

The chains are almost entirely in code that does not execute. This retires the
`a64-literal-pool` branch permanently, and Apple's optimization guide independently
agrees: 2.8.2 recommends short MOV sequences *over* PC-relative literal loads,
because a literal pool is an "island" of data in code space that the
instruction-side prefetcher will not fetch and the data-side prefetcher cannot
predict, costing "at least one or more data cache misses for each new island".

The already-landed thunk fix (`d9f96ccf5`) is not affected: it loads from the
backend context, a structure the emitted code touches constantly, not from an
island in code space.

### Two targets that look good and cannot pay

**Cross-block context promotion.** Context traffic is the largest single family in
the ranking -- `store_context` 12.16% and `load_context` 5.45% of executed host
instructions -- and `ContextPromotionPass` is deliberately block-local. Making it
CFG-aware cannot pay in this IR: `DataFlowAnalysisPass::AnalyzeFlow` forces any
value used outside its defining block through a local slot, emitting a
`StoreLocal` after the def and a `LoadLocal` at the top of each consumer, and
`register_allocation_pass.cc:74` states plainly that registers do not move across
blocks. Forwarding a context value across an edge therefore trades a context
store/load pair for a stack store/load pair, while the context store usually
still has to happen for correctness. The blocker is the block-local register
allocator, not the promotion pass. What *is* available without creating
cross-block values is dead-store elimination across a single-predecessor edge,
which only deletes stores.

**Mapping away the 0xE0000000 remap.** Every guest memory access whose address is
not constant pays `ApplyPhysicalRemapW0`, and it was four of the seven
instructions in `load i32` (measured at 6.99 executed host instructions per
execution). It cannot be removed by fixing the mapping: the 0xE0000000 and
0xC0000000 views alias the same physical memory 4 KiB apart, `MapViews` rounds a
view's file offset down to the host allocation granularity, and on a 16 KiB-page
host no pair of mmap offsets can place both aliases correctly. The 4 KiB
displacement is smaller than a page and is structurally unrepresentable, so the
CPU side has to make it up.

### Note on what the ranking still cannot do

It ranks emitted work by execution, not CPU time. A change that alters what
executes without altering what is emitted -- the remap branch below is exactly
that -- is invisible to both the corpus replay and the executed-bytes ranking,
and can only be scored by a paired runtime A/B. And the ranking still owes an
independent check against uninstrumented host-PC sampling; `--jit_perf_map`,
which arrived with the db16cyc series, is what that would need.

## Rebasing onto upstream, and what upstream already had

The upstream for this fork is **has207/edge**, not `origin/edge` -- `origin` is
this user's own fork. Checking currency against `origin` answered the wrong
question: the branch was 0 behind `origin/edge` and 76 behind `has207/edge` at
the same time.

Policy for the rebase, in the user's words: *upstream is bible, drop or alter
anything we have that's not as good / overlapping garbo.* Eight commits were
dropped as duplicated or superseded:

| ours | upstream's |
| --- | --- |
| SELECT_F64 borrow-mask blend | `bb41c0e3d`, which also handles a constant arm through a GPR cmov |
| lvrx zero-offset guard | `cc476d526`'s equivalent, same `movi`/`cbz`, different label name |
| constant vector operands out of aliased scratch | `e6a86116f` |
| four `[Base/POSIX]` threading commits | `addda160c`, `0cfa18775`, `1cf1169fa`, `cfcc929b6` |
| db16cyc ISB lowering | `d624e09eb`, which additionally **coalesces** consecutive barriers |

That last one matters beyond deduplication. Upstream's `DELAY_EXECUTION` returns
early when the previously emitted instruction was already an `isb`, so Halo 3's
sled of eight `db16cyc` collapses to one instruction. That is the
"compile-time delay batching" idea, already done and better than the ported
version, and it changes the meaning of the core-release counter: the count now
advances once per loop iteration rather than eight times, so the escalation
threshold moved from 16 to 2.

Two further pieces came back from the dropped commits on their own merits:
`--jit_perf_map`, which writes a JIT symbol map so a host profiler can attribute
samples inside the code cache to guest functions, and the core release itself,
reimplemented on top of upstream's coalescing so every path through the sequence
still ends in the `isb` the coalescing test looks for.

### A note on automated conflict resolution

141 commits were replayed and a script resolved the "both sides added something
here" shape by keeping both. It was wrong five times, and every failure was
silent at the diff level and loud at the compiler:

  - a struct member declared twice, once at its new home and once at the old
    site the commit existed to move it away from, breaking a size assertion
  - a class left without its closing brace, so every later definition in the
    file parsed as a member
  - upstream's range-keyed context tracking interleaved line-by-line with the
    offset-keyed version it replaced
  - call sites left passing an argument upstream's signature had dropped
  - a declaration deleted while its definition survived

None of this is an argument against automating the mechanical case; it is an
argument that anything auto-resolved is unverified until it compiles and the
169,048-case suite passes. Both gates were run before the result was trusted.

## The paired runtime A/B, on the rebased tree

Halo 3 is locked at 30 fps, so frame rate cannot show an improvement on it and
CPU percent is the figure of merit. Frame rate is still measured, as a guard.

### The branch against upstream

Five interleaved pairs, `has207/edge` (ed0e06112) against this branch:

| pair | upstream | ours | delta |
| --- | --- | --- | --- |
| 1 | 99.06% | 89.73% | -9.42% |
| 2 | 98.99% | 89.76% | -9.32% |
| 3 | 99.13% | 89.41% | -9.80% |
| 4 | 99.03% | 89.78% | -9.33% |
| 5 | 98.97% | 90.40% | -8.67% |

**-9.31% CPU, five pairs of five agreeing, differences spread over 1.14 points.**
Resolved on both statistics: best-against-best is -9.66% against a 1.10%
within-ref spread.

### What the db16cyc core release contributes

Isolated on ONE binary with ONE cvar, `db16cyc_yield_after` 0 against 2, so
nothing else can differ: **-4.99% CPU, four pairs of four agreeing** (-6.73%,
-4.11%, -4.38%, -4.75%). Roughly half the branch's total, with the rest coming
from the codegen work.

That is smaller than the -10.15% the same idea measured before the rebase, and
the reason is real rather than noise: upstream's `DELAY_EXECUTION` now coalesces
consecutive barriers, so Halo 3's sled of eight `db16cyc` is already one
instruction and much of what the core release used to recover has been recovered
more cheaply. The two overlap; this measures what is left.

Tuning it further did not pay. `after=1, sleep=150us` against the default
`after=2, sleep=60us` came out at a mean of -0.19% with two pairs each way --
not a result, so the defaults stand on evidence rather than on assumption.

### Reading an interleaved A/B correctly

The summary originally compared each ref's best run and called anything smaller
than that ref's own max-to-min spread unresolved. That is the wrong statistic
for interleaved runs. CPU percent drifts down over a session on this machine, so
both refs fall together and the within-ref spread measures the drift. The
db16cyc isolation came out as -4.54% against a 6.29% "spread" and was reported
as unresolved, when every one of its four pairs favoured the release path by
between 4.1% and 6.7%. Pairing by iteration is what the interleaving is for, and
the summary now does it.

Three harness faults were found the same way, each of which silently produced no
result rather than a wrong one:

  - the measurement window was counted in frames, and the frame number is only
    visible through the log prefix. A settled title stops logging while running
    at 87% of a core, so the run hung instead of measuring.
  - the teardown did not wait for the emulator to exit, so two of them competed
    for the GPU and one storage directory.
  - the warmup was 180 seconds because nobody had measured what reaching the
    menu costs. It is 49.2 seconds, identical with a cold or a warm shader
    cache. That alone turned a 14-minute experiment into 45.

## The spin loop, pinned

The post-rebase capture said one guest thread was 47.37% of all guest
instruction execution and that 79% of that thread sat in a single function.
`--dump_translated_hir_functions` settles what it is. The flag dumps **after**
`compiler_->Compile()`, so the text below is the optimized HIR the backend
actually emits from, not the frontend's first draft.

Function `0x821A8500`, 151 guest instructions, prologue executed exactly once,
**37.49% of all guest instruction execution**. Three blocks form the hot cycle:

| block | guest | body |
| --- | --- | --- |
| `label14` | `821A855C-8564` | `load_offset` a guest word, `cmpwi 1`, branch |
| `label2` | `821A8730-8734` | reload what it just stored, `cmpwi 2`, branch |
| `label13` | `821A873C-874C` | two chained guest loads, `cmpwi 1`, back edge |

No stores to guest memory. No calls on the hot path — both `call sub_821A0278`
sites are off it (`821A8568` runs 9,953 times, `821A8738` **zero**). Three
loads, three compares, three branches, 6,911,168,410 times. It is a poll, and
the inference from the counter shape was right.

### What it costs, and how much of that is dead

Per iteration, priced at the sequence table's measured instructions-per-
execution:

| | host I |
| --- | --- |
| `label14` | 25.16 |
| `label2` | 12.69 |
| `label13` | 37.91 |
| **per iteration** | **~75.8** |
| × 6,911,168,410 iterations | **523.9 billion = 19.5% of the capture** |

Of that, a large share is dead, and the reason is `PPCHIRBuilder::UpdateCR`.
Every PPC compare materializes all three bits of a CR field and stores each to
context:

```
v71 = compare_slt v65, 1     ; 2 host I
store_context +24, v71       ; 1  -- nothing reads it
v72 = compare_sgt v65, 1     ; 2
store_context +25, v72       ; 1  -- nothing reads it
v73 = compare_eq v65, 1      ; 2
store_context +26, v73       ; 1
branch_false v73, label2     ; consumes the SSA value, not the context
```

The field here is **cr6, not cr0**. `cr0` is the first member of `PPCContext`,
so the condition register is bytes `[0,32)` and `+24/+25/+26` are `cr6_0/1/2`.
The `// 0xA24` comment on `cr0` in `ppc_context.h` is stale, and an earlier
version of this section was written against it: a static table here counted
offsets `[24,56)` — cr6, cr7, and 24 bytes that are not the condition register
at all — and reported "70.6% of CR stores dead". **That table was wrong and has
been removed.** Halo 3's compiler uses cr6 for almost all of its integer
compares, which is why the numbers looked plausible.

Existing passes cannot remove these. `OPCODE_CONTEXT_BARRIER` is `IsFake()` and
carries no flags, so it is not the obstacle; the cross-block DSE is. Its kill
set is intersected over all successors, and `label14`'s other successor is the
`lwarx`/`stwcx.` retry block, which never rewrites cr6. The intersection is
empty, so nothing is killed on the hot path.

### What the pass actually recovers

`DeadCRStoreEliminationPass` — backward liveness over the 32 bytes cr0..cr7 —
removes the `lt` and `gt` write in `label14` and in `label2`, two compares and
two stores each:

| block | before | after | saved |
| --- | --- | --- | --- |
| `label14` | 25.16 | 19.16 | 6 host I |
| `label2` | 12.69 | 6.69 | 6 host I |
| `label13` | 37.91 | 37.91 | 0 |

`label13` keeps all three because `label14` opens with `check_preempt`, which
the pass treats as reading the whole context, so everything is live out of
`label13`'s back edge. That is deliberate — a preemption point can hand the
thread to the scheduler — and relaxing it is worth another 6 instructions an
iteration if it can be justified.

**12 of ~75.8 host instructions per iteration, 15.8% of the loop, 82.9 billion
instructions, 3.08% of the capture** — from this one loop. The earlier estimate
of 21 assumed all three compares could go; the third cannot, for the reason
above.

### The static picture, across the whole title

The same 8,508 functions dumped twice from one binary, `eliminate_dead_cr_stores`
false against true. Identical function sets, so this is a like-for-like diff:

| | before | after | |
| --- | --- | --- | --- |
| HIR instructions | 2,884,274 | 2,703,301 | **-6.27%** |
| `i8` compares | 202,752 | 148,831 | **-26.59%** |
| CR stores `[0,32)` | 236,979 | 138,412 | **-41.59%** |

By field, and the asymmetry is the whole point — `lt` and `gt` go, `eq` mostly
stays because it is the bit the branch consumes:

| off | field | before | after | removed |
| --- | --- | --- | --- | --- |
| +24 | `cr6_0` (lt) | 73,333 | 33,303 | 40,030 |
| +25 | `cr6_1` (gt) | 73,333 | 33,030 | 40,303 |
| +26 | `cr6_2` (eq) | 73,333 | 60,281 | 13,052 |
| +27 | `cr6_3` (so) | 4,450 | 1,751 | 2,699 |
| +0 | `cr0_0` | 4,098 | 3,023 | 1,075 |
| +1 | `cr0_1` | 4,098 | 2,991 | 1,107 |
| +2 | `cr0_2` | 4,098 | 3,797 | 301 |

Static, not execution-weighted. The 3.08% above is the sound per-site figure,
and the runtime A/B is what decides.

### What the first attempt cost, and what it proved

The first version was whole-function: drop any CR write nothing in the function
reads back. That is what the PowerPC ABI actually permits, since cr0, cr1 and
cr5-cr7 are caller-volatile. It failed **9,569 of 169,048** PPC tests — and
every single failure was a `cr` assert, with no GPR, FPR or VR wrong anywhere.
The transform never miscompiles arithmetic; it drops CR state the test harness
reads after the function returns. Useful evidence, because it bounds what the
aggressive version can break to exactly one thing.

The liveness version treats a return as reading everything and passes
169,048/169,048.

One detail was load-bearing and cost a whole build-and-dump cycle to find:
`BRANCH_TRUE` and `BRANCH_FALSE` carry `OPCODE_FLAG_VOLATILE`. Letting them fall
into the blanket "volatile reads everything" case relights every bit and
eliminates nothing at all — the first liveness build passed the tests and
removed exactly zero instructions, and the HIR dump was identical to the
baseline down to the line count. A branch reads no context; it takes its
target's live set.

## The extended-block change was a miscompile, and the gate did not see it

`--extended_block_scope` was landed with 169,048/169,048 corpus cases passing
and, correctly, no runtime claim attached to it. Running it was the next thing
that happened. The emulator does not reach gameplay with it on.

### The measurement

Two 240 s runs of Halo Reach's campaign under `--input_script`, one binary, one
cvar apart:

| `--extended_block_scope` | functions translated | outcome |
| --- | --- | --- |
| `false` | 18,614 | ran the full window |
| `true` | 8 | access violation during `KernelState: Launching module...` |

Re-run as an A/B on a fresh session it reproduces exactly, at the same guest PC
and the same fault address:

```
a (true):  crash=1  PC: 0x827EA6CC  Access Violation: read at 0x0000007000000051
b (false): crash=0
```

The fault address is the tell. The guest stack is at `0x7015FB40` — r1 and r31
both hold it — so the high half of `0x0000007000000051` is a stack pointer's and
the low half is not. That is a register read as though it held a different
value than it does.

### Bisecting it

The change has two halves, so they were split into two cvars —
`extended_block_promote` (context promotion forwards values along a chain) and
`extended_block_regalloc` (the allocator keeps registers along one) — and run
against each other. 75 s each, same binary:

| promote | regalloc | result |
| --- | --- | --- |
| false | true | clean |
| **true** | **false** | **crash, `0x0000007000000051`** |
| **true** | **true** | **crash, `0x0000007000000051`** |
| false | false | clean |

So it is **promotion** that breaks it, and extending the allocator neither
causes the fault nor repairs it. Allocator-only is a no-op exactly as expected:
nothing else in the tree produces a value that outlives its block, so there is
nothing for an extended allocator to keep alive.

That points straight at the shape of the defect. Promotion replaces a
`load_context` in a later block with the value defined in an earlier one, which
creates a value whose live range crosses a block boundary. The allocator's
`PrepareBlockState()` clears `upcoming_uses` and marks every register available;
it does not check whether anything is still live. So the register holding the
promoted value is handed to the next value allocated, and the later use reads
whatever landed there. `0x0000007000000051` has the high half of the guest stack
pointer (`0x7015FB40`, in r1 and r31) and a low half that belongs to something
else — a register read as though it held a different value, which is the
signature.

### A real defect found on the way, which was *not* this one

Chasing it turned up a separate latent bug, fixed on its own merits: instruction
ordinals. `hir_builder.cc:736` gives every instruction `ordinal = UINT32_MAX` at
birth, and `RegisterAllocationPass::Run` numbered them **block by block, inside
the same walk that allocates**. The allocator sorts usage lists by ordinal
(`CompareValueUse`) and decides a register has died by asking whether a use is
the last one in that order, so a value sorted while its later blocks are still
unnumbered gets a scrambled tail — every unnumbered use comparing equal at
`UINT32_MAX`, and `a->ordinal - b->ordinal` overflowing against the real ones.

Numbering the whole function first is obviously correct and is kept. **But it
did not fix the crash** — the fault address was unchanged, byte for byte, and
the bisect above was run with the fix already in. Worth recording plainly: the
mechanism was real, the reasoning about it was sound, and it was still the wrong
cause. It would have been very easy to build the fix, see the crash change
shape, and claim it.

### Root cause: a HIR block can branch out of its middle

`StartsExtendedBlock` treated "exactly one incoming edge, from exactly the
layout-previous block" as proof that every definition earlier in the chain has
run by the time a later block runs. That is false in this HIR, and the reason is
structural: **a block is not split at a conditional branch.** The frontend emits
`branch_true cond, target` and keeps appending the fall-through code to the same
block, so a block can branch out from its middle and carry on for another twenty
instructions.

Halo Reach's `827EA298` is the case that found it:

```
label33:
  ...
  branch_true v839, label34        <- instruction 10 of the block
  v842 = load_context +288         <- instruction 11
  ... 20 more instructions ...
  branch label35                   <- the actual tail
label34:
  ; in: label33, dom:1, uncond:0
  v877 = load_offset.1 v842, 50    <- promotion put v842 here
```

`label34` has one incoming edge, from its layout predecessor, and `v842` is
defined on the path that does **not** reach it. Promotion rewrote label34's
`load_context +288` into a use of `v842`, and the guest read a register never
written on that path.

`hir::Edge::DOMINATES` is no help and is actively misleading here: it is set
purely from "dest has exactly one incoming edge"
(`control_flow_analysis_pass.cc`), which for this shape is not dominance at all.
The dump prints `dom:1` on precisely the edge that does not dominate.

### The corrected predicate, and the measurement that killed the feature

The missing condition is that the incoming edge must leave the predecessor from
its **trailing run of branches**, or be the fall-through — either way the
predecessor has provably run to completion. With that in, the four-way cvar
matrix is clean where three of four legs faulted, and a 240 s scripted campaign
run completes on both arms at ~18,626 functions and frame ~8,080.

Then the opportunity was counted over the 18,625-function corpus
(134,753 non-head blocks):

| | blocks | share |
| --- | --- | --- |
| single edge from layout-previous — the old predicate | 15,800 | 11.73% |
| ...and the predecessor provably ran to completion | **2,729** | **2.03%** |
| **unsafe continuations the old predicate accepted** | **13,071** | **9.70%** |

**83% of what the old predicate accepted was wrong.** And what remains is worth
nothing: with promotion on, `load_context` over the whole title moves 603,043 ->
603,050 (+7, against two extra functions in the corpus — noise), every other op
count moves +0.00%, and the HIR of the crashing function is byte-identical with
the feature on and off.

So the entire apparent value of extended-block promotion came from the 9.70% of
cases that were miscompiles. **The feature has been reverted.** Kept from it:
the instruction-ordinal fix, which is a real latent bug on its own merits, and
this write-up.

### What it would actually take

Not a tighter predicate — the predicate is now correct and finds almost nothing.
The prerequisite is **real basic blocks**: splitting a HIR block at every
conditional branch so that a branch target is entered only by running a
predecessor to its end. That is a frontend/CFG change, not a pass change, and
`ControlFlowAnalysisPass` would have to build fall-through edges as well (today
it only creates edges from a trailing run of branch instructions, so a plain
fall-through has no edge at all — which is a second reason the 2.03% is so
small).

Until then, cross-block context traffic — 33.4% of all HIR ops — is not
addressable this way, and the honest statement is that the branch has no
mechanism for it.

### What this says about the gate

The gate is 169,048 PPC instruction-semantics cases. It passed all of them on a
build that cannot boot a title. That is not a gate failure, it is a statement of
what the gate covers: each case is a short, mostly single-block sequence, so a
defect that only appears once a value survives a block boundary has no case to
fail. **A corpus pass is evidence about instruction semantics and nothing else.**
It is not evidence of compiler-pass correctness, and it was cited here as though
the two were the same thing.

Two process consequences, both adopted:

- No compiler-pass change is described as verified until a title boots and runs
  a full window with it enabled. The gate comes first because it is cheap, not
  because it is sufficient.
- `d3e2396ea` is labelled `docs:` and carries this source change. A commit whose
  message describes only documentation shipped a crashing default, and the
  branch head carried it. Source and prose do not go in the same commit again.

## The context counts, re-derived on a frozen corpus

The earlier figures were withdrawn because two throwaway scripts run minutes
apart against a directory the emulator was still writing had been published as
one corpus. `tools/bench/hir_stats.py` replaces them: it refuses a directory
that is still changing, prints its own denominators, and asserts that the op-mix
store count and the dead-store denominator are the same number.

Over the settled 18,614-function dump above (`--extended_block_scope=false`,
so this is the shipping configuration), 4,944,539 HIR ops:

| | count | share of HIR |
| --- | --- | --- |
| `store_context` | 1,047,911 | 21.2% |
| `load_context` | 602,883 | 12.2% |
| **context total** | **1,650,794** | **33.4%** |

and of those 1,047,911 stores:

| | count | share of stores |
| --- | --- | --- |
| overwritten before any read, same block | 1,436 | 0.14% |
| volatile GPR pending at a return (ABI) | 45,625 | 4.35% |

The 0.14% stands, now on a corpus that can be recounted. It remains a static
share of one pattern and is not execution-weighted.

**The HIR-ops-per-guest-instruction ratio cannot be re-derived and stays
withdrawn.** The dump does not annotate guest instructions — a function's text
is HIR only — so there is no denominator in it. The script prints `0 guest
instructions` rather than inventing one. Recovering that ratio needs the dumper
to emit the guest instruction count per function, which it does not do today.

## TODO — the immediately optimizable parts

### 2026-08-25: the ranking is no longer a model

Everything below used to be ordered by *executed host instructions* — emitted
instruction count per sequence times guest execution count. That is a model of
cost, and item 8 has always said nothing had ever checked it against a profile
that did not come from the model. It has now been checked.

`tools/bench/live.py profile` samples a running emulator, drops every stack
whose innermost frame is a blocking primitive, and bisects the remaining JIT
addresses through `--jit_perf_map` into guest function names. Measured on
Halo: Reach in the campaign (NOBLE ACTUAL), driven there unattended by
`--input_script`, four 12-second windows:

| | share of all CPU | windows | range |
| --- | --- | --- | --- |
| guest JIT code | **53.5%** | 4/4 | 52.5–53.9 |
| `iokit_user_client_trap` (GPU submit) | 6.5% | 4/4 | 6.0–7.2 |
| `GuestToHostThunk` | **4.5%** | 4/4 | 3.7–5.0 |
| `_platform_memmove` | 2.2% | 4/4 | 2.1–2.4 |
| `objc_msgSend` | 1.9% | 4/4 | 1.7–2.4 |

By thread: GPU Commands 21.6%, RENDER 15.5%, NETWORK_RECEIVE 12.8%,
MAIN_THREAD 12.6%, each in 4/4 windows with a spread under 2.5 points.

Sanity check from an independent path: 6.5% of thread-time on a core across
~28 threads is ~180% CPU, against 160% measured by `ps` on the same run.

Two corrections it forced. The hottest single entry is **not** a guest
function — `code_cache_base.h` names a body `guest_{:08X}` when it has no
`function_info`, so the JIT's own transition thunks, emitted at the head of the
code cache with a guest address of zero, all arrive called `guest_00000000`.
And `sample` records every thread whether or not it is on a core, so an
incomplete blocked-primitive list made guest code read as 11.7% of CPU instead
of 53%.

### Tier 0 — found by the profile, not by the model

**0. The transition thunks write FPCR unconditionally — IMPLEMENTED, ungated.**
`GuestToHostThunk` is 4.5% of all CPU. Histogramming sample addresses across
its 152 bytes puts **79.4%** of that on the FPCR reload and `msr`, with 75.1%
on the single instruction *after* the `msr` — the signature of a
context-synchronizing write flushing the pipeline. All 28 vector save/restores
together are 20.4% of the thunk; the `blr` into the host function is 0.0%.

So roughly **3.6% of all CPU is one `msr fpcr`** — about twice what widening
the dead-context-store pass across the whole context could return.

`a64_emitter.cc` already tracks FPCR mode across blocks precisely because the
write is serializing; the thunks were never given the same treatment. The fix
reads FPCR back and skips the write when it already matches, at the two
per-transition sites (`EmitGuestToHostThunk`, `EmitHostToGuestThunk`).
`EmitResolveFunctionThunk` is left alone — one resolve per call site, ever.

**Gated and measured.** 169,048/169,048 cases, 493 suites, 0 failed, 0 crashed,
0 timed out.

The after-profile could not be taken in the same scene — the input script does
not land the guest in an identical place twice — so shares of total CPU are not
comparable across the two runs. The split *inside* the thunk is, because the 28
vector save/restores are byte-identical in both builds and act as a fixed
yardstick however often the thunk is entered:

| | FPCR share of thunk | vector share | FPCR/vector | hottest instruction |
| --- | --- | --- | --- | --- |
| before | 79.4% | 20.4% | **3.89** | `+0x54`, right after the `msr` — 75.1% |
| after (4 windows) | 37.0% | 61.2% | **0.61** | `+0x50`, the new `mrs` — 22–28% |

A **6.4x** relative reduction in FPCR cost per transition, and the flush
signature is gone: nothing in the thunk now exceeds 28% where one instruction
held 75%. Carried back at an unchanged transition rate that is ~3% of total
CPU, but that last step assumes a rate this measurement did not hold fixed, so
treat it as an estimate rather than a number.

*What is left in the thunk.* The `mrs` is now its hottest instruction, so the
read is not free either; skipping the check entirely for shims known never to
touch FP mode would remove it. And the vector save/restore is now 61% of the
thunk — it stores all 28 Q registers unconditionally, whether or not the guest
has anything live in them, which is the next thing here worth attacking.

**0b. NETWORK_RECEIVE burns 12.8% of all CPU polling.** (Per-thread aggregate,
11.7–14.0% across four windows — *not* the largest line in the profile, as was
claimed at one point: GPU Commands at 21.6% and RENDER at 15.5% are both bigger
threads.) Its stack is
`XThread::Delay` → `cthread_yield` → `swtch_pri`, with `KeDelayExecutionThread`
and `RtlEnter/LeaveCriticalSection` around it. Same shape as item 2 below, now
located rather than inferred — and it is why the thunk is hot, since every one
of those kernel exports is a guest→host transition.

**0c. `iokit_user_client_trap` at 6.5%** is GPU submission, and GPU Commands is
21.6% of CPU while the device sits at 25–45% utilisation. Not a JIT problem,
but it is the largest single line in the profile and the model was never going
to show it.

**0d. `_sigtramp` plus libunwind at ~2.3%** points at the signal-based memory
watch path firing far more than it should. Unexamined.

**0e. The transition thunk saved 28 vector registers it did not need —
IMPLEMENTED, gated.** With the FPCR write conditional, the vector save/restore
became 61% of the thunk: q4–q31, 896 bytes through a 464-byte frame, on every
host transition. A guest-to-guest `OPCODE_CALL` emits a bare `blr` into another
translated function whose prolog saves only x0/x30 and which then uses q4–q31
freely, so no HIR value can be live in a vector register across a call or the
existing code would already be broken; `ContextPromotionPass` stops its walk at
the first volatile instruction, "a call or a preempt check", so no promoted
value spans one either. `CallExtern` — the path every kernel export takes — is
therefore safe without the saves. `CallNativeSafe` is not: it is emitted inside
a sequence (inline MMIO fallback, `SpinWaitRelease`, memory tracers, debug
traps) where operands really are in flight, and it keeps the full thunk. Two
thunks, differing only in the save/restore; the light one's frame is 16 bytes.
169,048/169,048 cases. Runtime number still owed.

**0f. `NanoSleep` truncated every sub-microsecond sleep to zero — LANDED,
unmeasured.** `threading_posix.cc:240` is
`void NanoSleep(int64_t duration) { Sleep(std::chrono::nanoseconds(duration)); }`,
which resolves to the template at `threading.h:141` and `duration_cast`s to
**microseconds**. So `NanoSleep(100)` becomes `nanosleep({0, 0})` — a syscall
that returns immediately and releases nothing. That call is the *low-priority*
arm of the same `XThread::Delay` block as 0b (`xthread.cc:1271`), so
below-normal-priority guest threads spin on a no-op syscall exactly the way
normal-priority ones spin on `sched_yield`. Verified in this tree by reading
both functions. `NanoSleep(60000)` in the db16cyc release path is unaffected —
60,000 ns casts to 60 µs exactly. Upstream `0ac27adc6` fixes it on a parallel
line and reports ~93k `nanosleep(0)`/s on one Halo Reach thread. Unconditional,
no behaviour change intended, measurable on its own.

**0g. `page_size()` was an uncached libc call on every guest address
translation — LANDED, unmeasured.** `memory_posix.cc:94` is
`size_t page_size() { return getpagesize(); }` with
`allocation_granularity()` calling straight through. `PPCContext::TranslateVirtual`
(`ppc_context.h:461`) evaluates it as the **first** operand of
`allocation_granularity() > 0x1000 && guest_address >= 0xE0000000u`, so the
call happens on every translation regardless of the address. Confirmed against
the shipped binary rather than by reading: decoding `__text` and resolving the
stub at `0x10124cbe4` through the indirect symbol table finds **964 direct `bl`
to `_getpagesize` across 553 functions** — one each in the
`RtlEnter/LeaveCriticalSection` trampolines, two in `KeDelayExecutionThread`,
and six in each `ExecutePacketType3_INTERRUPT`. Two independent fixes, both
semantics-preserving: cache the value in a function-local `static const`, and
swap the `&&` operands so the address test short-circuits first. Both operands
are pure. Expected worth is a few tenths of a percent, not more — recorded
because it is real, cheap, and independently checkable by re-running the same
disassembly scan.


Ordered by expected payoff against confidence. Every entry names how it will be
measured, because the campaign has already had two rankings overturned by the
measurement rather than by the code.

### What landed on 2026-08-25, and what each is still owed

Six commits. **Not one of them has a runtime number yet**, which is the single
biggest gap in this list and the next thing being closed.

| commit | evidence it has | evidence it lacks |
| --- | --- | --- |
| `[Base]` NanoSleep sub-us truncation | upstream `0ac27adc6` verbatim; both functions read in this tree | any measurement here |
| `[Base/PPC]` getpagesize off the translation path | 964 `bl` sites decoded from the shipped binary | any measurement |
| `[A64]` near branch in the physical remap | gate 169,048/169,048 | deliberately unsized — needs the sequence coverage table |
| `[A64]` ccmp denormal screen | gate 169,048/169,048; 17→15 executed at n=3 | instruction counts only, no time |
| `[A64]` near branches in vector-store dispatch | reasoning only | **zero corpus coverage** — stvlx/stvrx cannot be assembled |
| `[CPU]` extended-block promotion | reverted; see above | — |

Also still owed from earlier: **the no-vec thunk runtime profile** (0e). Its
first attempt failed with the emulator exiting before `sample` fired, and it has
not been retried. The change has 169,048/169,048 and no runtime number.

The A/B that closes most of this is a two-build paired run of `57a51a146`
against the branch head — one binary per arm, since none of these is behind a
cvar. Expected combined effect is on the order of 1%, which is near this
harness's resolution, so the run has to be long and interleaved or the result
is not worth quoting.

### The 2026-08-25 changes, measured in-game: -0.25% CPU

Paired A/B, `57a51a146` against `6a8ca82ba`, Halo Reach campaign driven to
gameplay by `--input_script`, 190 s warmup, 120 s window, 3 interleaved pairs,
`--guest_scheduler=false` on both arms.

| | fps | CPU % |
| --- | --- | --- |
| base@57a51a146 | 29.82 (29.81, 29.82, 29.82) | 154.07 (154.08, 154.07, 154.10) |
| head@6a8ca82ba | 29.83 (29.82, 29.83, 29.82) | **153.61** (153.74, 153.75, 153.61) |
| paired mean | +0.01%, pairs disagree — **not a result** | **-0.25%, 3/3 pairs agree** |

Run-to-run spread is 0.09% against a 0.25% effect, so it clears its own noise
floor by roughly 3x, and every pair agrees in sign. fps sits on the 30 fps cap
on both arms, which is what makes the CPU comparison meaningful: the two builds
are doing the same work.

What is in that -0.25%: the NanoSleep truncation fix, `getpagesize` off the
translation path, the near branch in the physical remap, the `ccmp` denormal
screen, and the near branches in the vector-store dispatch. Five changes, and
this measures only their sum -- none is separable without five more builds, and
the per-change instruction-count estimates that predicted roughly -0.5% to -1%
were optimistic by about half.

**A methodology defect found here — and the first explanation of it was wrong.**
Every run through `frame_ab.py` had measured the title **idling at its menu**
rather than running. The tell was in the output the whole time: a settled menu
stops logging frames, so fps came back `n/a` while CPU percent kept reporting,
and that was read past for several runs.

The cause was originally recorded as `--hid=nop` suppressing the scripted input
driver. **That is false.** `EmulatorApp::CreateInputDrivers` (`xenia_main.cc`)
constructs the scripted driver first and unconditionally, ahead of any real
backend and regardless of `hid=`, with a comment saying it exists so a benchmark
can drive a title's menus with `--hid=nop` and nothing else attached; `Setup()`
fails only on an empty `--input_script`. The real cause was simpler and less
interesting: the runs did not pass `--input_script` at all.

The "fix" that followed from the wrong diagnosis -- dropping `--hid=nop`
whenever `--input_script` was present -- was worse than the bug, because it
attaches SDL as well and puts a real controller's state into a benchmark. It has
been reverted. `n/a fps` is a signal about the guest's state, not about the HID
backend.

Menu-idle is not a useless experiment -- it is just a different one, and it is
noisier: an interleaved menu run over the same two builds gave a run-to-run
spread of 3.57% against the same ~1.7% apparent effect, i.e. unresolved, where
the in-game run resolves cleanly at a third of the effect size. **In-game with
the input script is the methodology from here.**

*A second trap, recorded because it produced a plausible-looking number that was
garbage.* A `pgrep -f frame_ab` wait loop matches its own shell, so it never
terminates and reports the run as finished when it is not. Two harness processes
ended up interleaved into one log -- both ref labels appear in it -- with two
emulators competing for cores. That run reported -1.71% CPU with 3/3 pairs
agreeing and it was discarded, not published. Wait on the output JSON existing,
never on a `pgrep` of the harness's own name.

### Tier 1 — implementable now, target already measured

**1. CR-bit liveness — DONE.** `DeadCRStoreEliminationPass`, backward liveness
over the 32 bytes `cr0..cr7`. Statically **-41.59% of CR stores, -26.59% of
`i8` compares, -6.27% of all HIR** across the title's 8,508 functions;
169,048/169,048 PPC tests. Gated on `eliminate_dead_cr_stores`.
*What is left in it:* `check_preempt` is treated as reading the whole context,
which is what keeps the third compare in the spin loop alive. Justifying a
narrower rule there is worth another 6 host instructions per iteration.

**2. Generalized spin-loop release.** `delay_execution` fires 937,637 times from
12 sites — **0.001%**. The db16cyc detector sees none of these polls, because
they carry no `db16cyc`; they are plain load/compare/branch. Detect a
self-looping HIR block whose body has no stores, no calls, no `reserved_load`/
`reserved_store`, and no `VOLATILE` op, and route it into the same
`DELAY_EXECUTION` core-release path. The db16cyc release measured **-4.99% CPU**
on its own; this reaches roughly a hundred times more execution.
*The prototype already exists and is not in this tree.* Commit `0a895a85c`
"[Kernel] Escalate hot guest Delay(0) loops to a real core release" (XeniOS,
2026-08-08) does exactly this and lives only on the `jitq/*` branches. That, not
`PreciseSleep`, is the directly relevant starting point — the observed
zero-timeout path goes through `MaybeYield()` / `sched_yield()`
(`xthread.cc:~1270`, `threading_posix.cc:~219`) and never reaches `PreciseSleep`
at all.

*Risk:* the highest of anything on this list — releasing a core inside a loop
that is not actually waiting will cost frames. Validation has to cover total
CPU, frame pacing, and input/network responsiveness, not CPU alone. And the
guard title has to be *verified* uncapped: GTA IV being present on disk
establishes nothing about whether it is capped or whether it exercises the same
path. Both titles on this machine,
Halo 3 and Halo Reach, are locked at 30 fps, so neither can show the damage as
a frame-rate drop; the guard has to be something else — frame-time variance
inside the window, or a title that is not capped.

*Two sibling commits on the same parallel line, both absent here, and one of
them is free.* `0ac27adc6` "[Base] Fix NanoSleep sub-microsecond truncation" —
**taken, see 0f above**. And `wait_timeout_backoff`
(`jitq/bench/kernel-wait-stack:src/xenia/kernel/xobject.cc:37`) applies the same
escalation to `XObject::Wait`/`SignalAndWait`/`WaitMultiple`: 64 cheap yields,
then a park growing 25 us per miss to a 250 us cap, reset after a 1 ms gap,
default off. This tree has bare `MaybeYield()` at all four timeout sites
(`xobject.cc:540, 637, 810, 842`). Its cvar comment independently corroborates
the measurement here — "the top non-JIT CPU cost on device for poll-heavy
titles (~11-15%)" — but the NETWORK_RECEIVE stack shows no wait shim, so it is
not the fix for *that* thread.

*Two corrections `0a895a85c` needs before it is portable.* It calls
`Clock::QueryHostUptimeMillis()` per delay, which on macOS is an uncached
`mach_timebase_info()` plus two 64-bit divides (`clock_posix.cc:20-79`) added to
the very path being made cheaper — the db16cyc emitter's raw
`mach_absolute_time()` tick comparison is the right precedent. And its 2 ms gap
cannot, at 1 ms clock granularity, distinguish a tight loop from a thread doing
real work between yields, so genuine workers drift toward a forced sleep; the
db16cyc analogue uses 1 us in raw ticks.

*`--guest_scheduler` narrows this much further than it first appears.* With the
tree default `guest_scheduler=true` (`kernel_flags.cc:18`) `XThread::Delay`
never reaches `MaybeYield`: `fiber_` is non-null (`xthread.cc:437`), so
`Delay(0)` becomes `YieldCurrentThread(false)` — an fcontext switch, no syscall
— and `Delay(>0)` parks the fiber. Any change to the `timeout_ms == 0` non-fiber
branch is therefore **dead code under the default configuration**. It cannot
regress the default, which lowers landing risk, but it also will never be
exercised by anyone running defaults, which lowers confidence. It only matters
for `guest_scheduler=false`, which is the configuration the Reach campaign is
forced into by the livelock bisected to upstream `34357e257` — so **fixing that
livelock attacks the same 12.8% from the supported end**, and is probably the
better use of the same effort.

*`timeout_ms == 0` is not the same as "the guest passed 0".* `XThread::Delay`
converts with integer division (`timeout_ms = -ticks/10000`), so any relative
interval under 1 ms floors to 0 and lands in the same branch. Nothing measured
so far distinguishes a genuine `Sleep(0)` from a truncated 100 us sleep, and the
right fix differs between the two. **Measure the interval distribution before
changing anything**: a per-`XThread` histogram of the raw `*interval_ptr` at
`KeDelayExecutionThread` (`xboxkrnl_threading.cc:468`) plus a call count, dumped
at shutdown. Zero risk, and the branch already has precedent for this style of
counter.

*The strongest evidence against this whole class of fix is in the tree's own
history, and it is not in this repository either.* `docs/xmp_poll_throttle.md`
(`88ad8a535`, same parallel line, 230 lines) records a guest render thread at
90% of a core that looked exactly like a spin-wait-policy problem and was not:
the cause was an emulator-inserted 10 ms `Sleep` in `XmpApp` landing on the
frame *producer*, and the spinning consumer was a symptom. Removing it took that
thread to 15.6% and whole-process CPU from 1.39 to 0.77 cores at identical fps.
**Three spin-wait levers measured inert before the real cause was found**,
including the shipped db16cyc release and `wait_timeout_backoff` itself. Its
stated lesson: *"a guest thread at 100% duty is not necessarily a spin-wait
policy problem. Ask what the thread is waiting for, and profile that producer
instead."* So before escalating anything, name the producer: `--jit_perf_map` to
identify the loop, then a hardware write watchpoint on the polled slot (needs
`settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS`, or lldb
stops on Xenia's own write-watch faults and never reaches it).

*One caveat on the 12.8% itself.* Blocked stacks were filtered out of that
profile, but `sched_yield` leaves a thread runnable, not blocked, so the
`swtch_pri` samples should have counted as on-core. If they were excluded the
thread's true share is nearer ~32% and 12.8% is the non-yield remainder. That is
a 2.5x difference in the size of the prize and has to be resolved from the raw
`sample` output before either number is quoted again.

*The guard, in full.* CPU alone cannot approve this: a change that makes a
waiting thread sleep always reduces that thread's CPU, including when it is
doing damage. All three of these, not one — (1) total process CPU, paired and
interleaved, whole-process cores rather than the thread's own line, because the
XMP case shows the cost can move to another thread; (2) **frame pacing, not
frame rate** — a 30 fps cap hides a mean regression but not a variance one, and
**this instrument does not exist yet**: `frame_ab.py` derives fps from the frame
number in the log prefix and its own docstring notes the emulator does not stamp
times, so a host timestamp per presented frame reported as p50/p95/p99 plus a
count of frames over ~1.5x the cap period has to be built first; (3) input and
network responsiveness, via `--input_script` replaying a fixed sequence with
guest-visible response latency recorded — neither CPU nor pacing can see a
controller or packet handled a few hundred microseconds late.

**3. `storev_left` / `storev_right` arm selection — RESOLVED, and the headline
number was wrong by 2.05x.** 213 sites, 45.00 instructions of *emitted
footprint*. The sequence branches over size arms and one invocation runs exactly
one arm (`a64_seq_vector.cc:~2200`), so 45 was never a per-execution cost.

Every instruction of the 45 is now accounted for: 13 of prologue (`rev32`, the
scratch store, `ComputeMemoryAddress`, `ApplyPhysicalRemapW0`, the offset
arithmetic) plus 32 for `EmitPartialVectorStore`. Executed, by guest address
alignment `off = addr & 15`:

| arm taken | STVL `off` | STVR `off` | executed |
| --- | --- | --- | --- |
| `from8` | 0–8 | 8–15 | 21 |
| `from4` | 9–12 | 4–7 | 23 |
| `from2` | 13–14 | 2–3 | 25 |
| `from1` | 15 | 1 | 22 |
| nothing stored | — | 0 | 20 |

Uniform-alignment mean **22.06 (STVL) / 22.00 (STVR)**, so the honest share is
4.46% x 22/45 = **2.18%** of executed host instructions, not 4.46%.

*Two ideas killed by this measurement, both worth recording.* Moving rare arms
to `AddToTail` removes nothing from any executed path — it would only make
`host_bytes` report a fabricated ~17-instruction saving, which is exactly the
circularity item 6 warns about. And constant-alignment specialization has **zero
sites to specialize**: the 45.00 average across all 213 sites is exact, which is
only possible if every site is the register-address/register-source form, and
`stvlx/stvrx` build `ea = CalculateEA_0(rA, rB)` — a runtime GPR sum
(`ppc_emit_altivec.cc:254-289`).

What was taken instead: the four dispatch branches were going through
`A64Emitter::b(Cond, Label)`, which shadows an unbound forward label into
`<inverse> over; b target; over:`. All four labels are bound within the helper,
so the near forms are provably in range — 28 emitted where there were 32, and
one fewer executed on 15 of 16 alignments.

*What is still on the table and deliberately not taken.* Every arm's second
access offset (`sub w6, count, N`) can be folded into a fixed immediate off the
block-aligned address, and STVL's `count == 0` test is provably dead. Together
~22 -> ~19 executed, alignment-independent. Not implemented: **the corpus cannot
test any of it.** `stvlx/stvrx/lvlx/lvrx` are Xenon-only opcodes the assembler
cannot build — `bench-work/ppcbin` holds `instr_stvl.skipped`,
`instr_stvr.skipped`, `instr_stvlx128.skipped`, `instr_stvrx128.skipped` and no
matching `.bin` — so the 169,048-case gate exercises this code **zero times**. A
silent off-by-one there corrupts guest memory with nothing to catch it. It needs
a hand-written AArch64 harness over all 16 alignments first.

**4. `denormal_quirk` — PARTLY DONE.** 5.29% at 19.36 instructions per
execution over 15,649 sites. Unlike storev the inline number is nearly honest:
the cold finiteness check is emitted inline and branched over only at sites
where `near_tail_branches_safe_` is false (functions of 3072+ HIR ops,
`a64_emitter.cc:219`), which reconciles 19.36 against a clean-path 17.2 at a
`!tail_ok` weight of ~16%. True executed share ~**4.7%**, so this one was not
badly over-counted.

The running-minimum screen is now a `ccmp` chain: 17 -> 15 executed for three
operands, 12 -> 11 for two, unchanged at 7 for one, and one fewer `csel` in the
dependency chain per extra operand. Operand counts come from
`ppc_emit_fpu.cc`: `fmadds/fmsubs/fnmadds/fnmsubs` n=3, `fadds/fsubs/fmuls/fdivs`
n=2, `fsqrts` n=1. Blended ~-10% of a 4.7% share ~= -0.47% of executed JIT host
instructions. **Instruction counts, not measured time** — no runtime A/B has
been run on it.

### Tier 2 — measurement repairs, needed before Tier 1 results can be trusted

**5. Tail taken-counters.** Tails are separated from inline code but carry no
execution counter, so executed tail cost (415.1 billion, 15.43%) is an upper
bound charged at the enclosing site's rate. `check_preempt` alone contributes
195.1 billion of it and is almost never taken.

**6. Inline branch arms in the byte attribution.** `host_bytes` counts the whole
emitted body, so any sequence that branches internally without `AddToTail` is
over-counted — `storev_left` reads 45.00 when one arm of ~5 runs. Either record
a minimum-path length per sample, or move the arms to tails so the existing
counter means something.

**7. Per-sample call/MMIO variant attributes.** Still outstanding from the
original repair list.

**8. Validate against uninstrumented host-PC sampling — DONE, and it moved
things.** See the table above. The model was not wrong that guest code is where
the time is (53.5%), but it could not see the 4.5% in a transition thunk, the
12.8% in a polling guest thread, or the 6.5% in GPU submission, because none of
those are emitted guest instructions. Reweighting the model in cycles rather
than instruction counts is the remaining half of this item.

### Structural — large, and blocked on one thing

**9. Context traffic.** `load_context i64` 4.60%, `store_context i8` 4.14%,
`store_context i64` 3.21%, `store_context ci64` 1.39% — **13.34% of executed
host instructions at ~1 instruction each**, which is to say the cost is the
count, and the count exists because `DataFlowAnalysisPass::AnalyzeFlow` forces
cross-block values through local slots and the register allocator is
block-local. Nothing peephole-shaped will move it. Item 1 attacks a slice of it
from the frontend instead, which is why it is first.

*Widening the dead-store pass — PROVISIONALLY REJECTED, AND THE NUMBERS ARE
WITHDRAWN PENDING RE-DERIVATION.* The plan was to take
`DeadCRStoreEliminationPass` from the 32 bytes of `cr0..cr7` to the whole
`PPCContext`. The counting that argued against it does not survive audit and is
withdrawn:

- The op mix (2,990,109 HIR ops, `store_context` 22.0% ≈ 658k) and the
  dead-store denominator (1,022,844 stores) were produced by two throwaway
  scripts run minutes apart **against a directory the emulator was still
  writing into** — 14,057 functions in one, 18,362 in the other. They were then
  published side by side as one corpus. 22.0% of 2,990,109 is ~658k, not
  1,022,844; and 1,022,844 is 34.2%, uncomfortably close to the claimed 34.5%
  *combined* load+store share. Whether the gap is corpus growth or a counting
  error cannot be established, because the dumps were deleted.
- The 0.14% measures exactly one static pattern — a store overwritten before
  any read **within one block** — and is not execution-weighted. It is not
  "anything removable by any peephole", which is how it was used.

`tools/bench/hir_stats.py` now does this counting: it refuses to run against a
directory still being written, prints its denominators, and asserts that the
op-mix store count and the dead-store denominator are the same number — the
cross-check whose absence let the above through. Nothing here is closed until
it is re-derived on a frozen corpus.

*Why CR was different.* The CR pass removed 41.59% of CR stores because a PPC
compare materialises lt/gt/eq and stores all three while the branch consumes
only the SSA value. That is a frontend pattern peculiar to the condition
register. GPR stores have no equivalent: they are not redundant.

*What the context share is, stated without the overreach it was stated with.*
Explicit guest register state does live in `PPCContext`, and the frontend
stores every guest register write there. Three claims made around that were
wrong and are retracted:

- **"3.21 HIR ops per guest instruction says the frontend is lean."** It says
  no such thing. It is a static IR-density ratio with no baseline to compare
  against, and it is silent on how expensively any of those ops expand into
  a64.
- **"Every cross-block value must go through the context."** False. Generic
  cross-block SSA reaches memory as a local stack slot, not a context field.
  Worth being exact about the route, because the obvious candidate is dead
  code: `DataFlowAnalysisPass` — which is what would lower cross-block values
  through `StoreLocal`/`LoadLocal` — is declared but **never instantiated
  anywhere in the tree**. The only thing that actually allocates a local is
  `SpillOneRegister` in the register allocator. So cross-block values exist in
  registers until the allocator has to spill one.
- **"Only a global allocator can move the 53.5%."** False, and the most
  misleading of the three. The 53.5% is *all* guest JIT execution. Emission
  quality, code layout, memory behaviour, helper routines and frontend
  transformations can each move it. The allocator is one lever among several,
  not the only one.

What the allocator work does need, stated precisely: cross-block context
promotion **and** allocation continuity, together. Promotion alone replaces a
`load_context` with a value the block-local allocator then tends to materialise
through a local slot — trading a context access for a stack access. And even
with both, calls, exits, preemption points and joins still force state back to
the context.

### Closed — recorded so they are not reopened

| | verdict |
| --- | --- |
| wide-move chains → literal pool | 0.11% by exact accounting. Retired twice. Apple's guide §2.8.2 agrees. |
| cross-block context promotion | Blocked by the block-local register allocator: trades a context access for a local one. |
| eliminating the `0xE0000000` physical remap by mapping | Structurally impossible — the `0xE0000000` and `0xC0000000` views alias the same physical memory 4 KiB apart, unrepresentable on a 16 KiB-page host. |
| db16cyc tuning (`after=1/150us` vs `after=2/60us`) | -0.19%, two pairs each way. Not a result; defaults stand on evidence. |
| `call - symbol` as a major target | Was 17.02 instructions per execution pre-rebase, is 8.95 now. Upstream got there first. |
