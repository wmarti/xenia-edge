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

It is not. The premise is simply wrong for this branch. Several commits change
code that every suite executes regardless of which instruction it is testing:
guest-address zero-extend folding touches every load and store, stackpoints as
linked records touch every guest frame, and moving `preempt_requested` into the
`PPCContext` padding hole changes the layout every test shares. There is no
suite whose emitted code this branch leaves alone, so a suite-based control
measures the branch's general improvement and reports it as noise.

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

The gains are close to uniform across suites that test unrelated instructions,
which is what the diff predicts — `hir_builder`, `ppc_hir_builder`, and
`ppc_context.h` are on the path every suite executes. `ppc_testing_main.cc` is
byte-identical between the refs, so the harness is not contributing.

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

**The x64 gain is -1.04%, against -8.8% wall clock on a64.** That is not a
contradiction and not a disappointment: the stack is thirty `[A64]` commits and
six `[x64]` ones. Most of this work does not touch the x64 backend at all, and
the measurement says so. Anyone quoting "-8.8%" as the branch's improvement
should be quoting it as the *a64* improvement.

**On x64 the control behaves like a control**, moving +0.02%. On a64 the same
suite moved -9.2%, because the a64 commits change addressing, guest frames and
`PPCContext` layout — code every suite runs. The same suite is a valid control
for one backend and a useless one for the other, which is a good argument for
calibrating against an identical binary rather than against a suite believed to
be untouched.

The improvement concentrates where the `[x64]` commits actually are:
`vavgsb`/`vavgsw` (vector average without scalar loops), and the scalar float
suites that the MXCSR-tracker and F64-binop commits touch. `vpkuhus` shows
nothing, despite the byte-pack commit naming it — worth a look.

A caveat on the metric: these counts cover the whole process, including JIT
compilation and the test harness, so a change confined to generated code is
diluted by everything around it. The counts are exact but they are not a
measurement of the emitted code alone.

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
