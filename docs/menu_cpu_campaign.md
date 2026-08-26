# Menu CPU campaign — Halo 3 and Halo Reach

Objective, in the user's words: *"pin down the largest CPU side optimization
targets based on all the profiling data we can collect and hit the biggest
ones ... bring CPU time down measurably in Halo 3 and Reach menus."*

Scope decisions taken by the user on 2026-08-25:
- **Target state:** whatever the profile says is biggest. Not assumed to be the menu,
  not assumed to be the JIT.
- **Lane:** go where the CPU is. If GPU submission, present, or kernel wait paths
  dominate, work on those too rather than optimising the wrong thing.
- Halo 3 and Halo Reach only. **ODST is not a target.**
- Both titles are locked at 30 fps, so CPU percent is the figure of merit and fps is
  only a liveness guard.

## Standing rules for this campaign

1. **Nothing is a result until it is measured on a title.** The 169,048-case PPC
   corpus is instruction-semantics evidence and nothing else; it passed on a build
   that could not boot.
2. **One change per commit**, each with the evidence it has and the evidence it lacks
   stated in the message.
3. **Never publish a number from a contaminated run.** Two harness processes competing
   for cores produced a plausible -1.71% that had to be withdrawn. Wait on the output
   JSON, never on `pgrep` of the harness's own name -- it matches its own shell.
4. `--hid=nop` disables the scripted input driver. `n/a fps` in frame_ab output means
   the guest is idling at a menu, not playing.
5. Emitted footprint is not executed work. Moving code to a tail reduces `host_bytes`
   by construction.
6. Rebase on `has207/edge` before each measurement session; upstream wins on overlap.

## Measurement prerequisites

- [ ] **Menu noise floor, against an identical binary.** Unknown. The only menu A/B
      taken was contaminated. Nothing measured at a menu can be believed until this
      exists. In-game floor is 0.09% over 3 pairs.
- [ ] **Per-guest-function timing.** `885a5697c` + `485e53bdb` on
      `origin/a64-fixes-on-edge` add a CNTVCT_EL0 per-function profiler. If it works,
      it is the ranking instrument this campaign needs.

## Measured menu profiles (2026-08-25, 20s samples, --guest_scheduler=false)

Raw samples kept at `/tmp/xe-sample-87981.txt` (Reach) and `/tmp/xe-sample-92650.txt`
(Halo 3). Re-derive with `tools/bench/live.py`; the on-disk `menu-*/profile.txt`
predate two instrument fixes and their denominators are wrong.

**The two menus have different bottlenecks. Nothing ranks first for both.**

| Reach menu (156% CPU) | share | spin | Halo 3 menu (86% CPU) | share | spin |
| --- | --- | --- | --- | --- | --- |
| NETWORK_RECEIVE | **45.8%** | 58% | GPU Commands | **24.8%** | 2% |
| libdispatch-manager | 8.1% | 3% | IOGPU submit thread | 14.7% | 0% |
| RENDER | 7.8% | 2% | Metal completion dispatch | 12.9% | 1% |
| GPU Commands | 7.3% | 3% | RENDER | 10.5% | 0% |
| MAIN_THREAD | 5.8% | 1% | `xe::threading::TimerQueue` | **10.3%** | 58% |
| `xe::threading::TimerQueue` | 4.3% | 70% | MAIN_THREAD | 6.7% | 0% |
| **in a yield trap** | **33.0%** | | **in a yield trap** | **10.6%** | |

## Noise floor

- **Menu, Reach, identical binary, 3 pairs:** CPU run-to-run spread **0.49%**,
  paired mean +0.16%, pairs disagree — correctly UNRESOLVED. Anything below
  ~0.5% cannot be resolved at a menu.
- **fps is NOT a usable guard at a menu.** The same identical-binary run gave
  29.83 / 34.60 / 29.80 on one arm — a 15.9% outlier — because the frame rate
  depends on which menu state is up. In-game fps sits on the 30 fps cap and is
  usable; at a menu it is not. A work-equality guard for menus does not exist
  yet (per-thread CPU-seconds via `thread_info` is the cheapest candidate).
- **In-game floor** for comparison: 0.09% over 3 pairs.

## Status

| # | Target | State | Measured |
| --- | --- | --- | --- |
| T1 | Reach NETWORK_RECEIVE zero-delay yield trap (~27% of Reach menu CPU) | **adopted, default `zero_delay_spin_limit=16`** (2026-08-26, by the T5 precedent: huge where it acts, provably null elsewhere) | **-54.58% Reach menu 3/3 pairs; re-confirmed -51.56% on the current build; -39.36% Reach campaign, no livelock. Null on Halo 3 and GTA IV** |
| T2 | `TimerQueue` spin_wait -> blocking_wait on POSIX (10.3% Halo 3 / 4.3% Reach) | landed | **-2.74%** |
| T3 | Halo 3 host GPU submit/encode (53% aggregate, no single hot spot) | **closed for cheap fixes** -- four hypotheses killed with measurement; what is left is texture-cache policy, not submission mechanics | ~100 MiB/frame of real invalidation, 46.5% GPU-origin / 53.5% guest-origin |
| T4 | Redundant guest address computation (`guest_828BF420`, `guest_82A46D70`) | **rejected** -- under the noise floor once repriced | 13.90% of executed memory operands, but **~0.38% CPU after discount** vs a 0.28% floor; 94% of it in two functions |
| T5 | Guest spin-wait at `828BF420`/`82A46D70` (docks) | **closed, adopted** -- `spin_wait_yield_after=100000` default-on after four-state ledger | **-14% docks CPU x3 runs, no throughput cost; inert (zero escalations) on Halo 3 menu, Reach menu, Reach campaign** |
| B1 | `PACK_D3DCOLOR` returned 0xFFFFFFFF always | fixed, guard proven to fail on the bug | correctness |
| T6 | rlwinm hot path: `lsl`+`ands #0xFFFFFFFF` -> W-form `lsl` (a W write zeroes the upper half; the `ands` flags are dead at these sites) | **reopened** by the 2026-08-26 adversarial review -- an earlier same-day rejection double-applied the 2x discount | priced **0.46-0.55% CPU**, 1.6-2.0x the floor; **implemented + statically verified** (169,048/169,048 semantics; replay -13,921 laid-down stable instrs; disasm-verified 2->1 rewrite); runtime A/B null at Halo 3 menu (no regression); **closed, adopted** |
| T7 | Denormal-quirk recomputation over provably-single doubles (lfs/stfs round trips) | **closed, adopted** -- fold + `VALUE_NEVER_F64_DENORMAL`; one adversarial-review defect (`--no_round_to_single`) found and fixed | **-3.34% laid-down, 2.47% of executed host instrs saved; 169,048/169,048; runtime guard clean (-0.25% Halo 3 menu pair)** |
| T8 | Guest call machinery (14.4% in the static emitted-byte model -- NOT executed host instrs, see review) | **reviewed, deprioritized behind T9** -- multiword live rewrite ruled unsafe (Arm DDI 0487 B2.2.5); single-BL gate redesign viable, >=7 instrs saved per patched dispatch, but higher-risk than T9 | **94.66% of executed symbol calls walk the table (nominal 147.28M/s, 144.51M/s timestamp-derived; racy lower bound); direct 5.16%; register-indirect 3.31%; corroborated by compile-order estimate 94.07%** |
| T9 | SetReturnAddress + StoreLR same-constant rematerialization | **next up** (from the T8 review's ranking) | modeled ceiling **1.0395%** (static model); runtime unproven |

### Next up, in order (re-ranked 2026-08-26 per the T8 adversarial review)

1. **T9 -- fuse SetReturnAddress with the adjacent same-constant StoreLR.**
   The PPC frontend emits them as separate adjacent HIR ops
   (`ppc_emit_control.cc:36`) and the backend materializes the same constant
   twice (`a64_emitter.cc:1227`, `a64_sequences.cc:343`). Modeled ceiling
   1.0395% (static emitted-byte model -- see the T8 correction below for what
   that does and does not mean). The one-shot fusion machinery
   (`a64_emitter.h:156`, the T6 pattern) already fits. Lower risk than any
   code-patching work; do this first. Runtime benefit unproven until A/B'd.
2. **Measurement repair before any CPU claim on call work**: per-thread or
   thread-local counters (the racy globals are a lower bound), return-taken
   counts for the possible-return check, disassembly of the actual counter
   bump (the mov may expand to MOVZ/MOVK pairs), a real retired-instruction
   denominator, and paired uninstrumented A/A then A/B. A normal present rate
   alone does not prove the path mix was undistorted.
3. **T8 option (c), redesigned around the single-BL gate** (see the review
   section): fixed in-range `Call` sites only, patchpoint metadata in
   `EmitFunctionInfo`, an aligned atomic `PatchInstruction32` (the existing
   `PatchCode` memcpy is not guaranteed atomic), and a pending-site registry
   with a mutex + acquire re-read of `machine_code()` drained after
   `A64Function::Setup`. Option (b)'s placement-time fixup folds in here as
   the pre-publication half; it is not worth standalone work (5.16% of
   dispatches). CallIndirect is excluded -- a PIC is a separate project.
4. **Secondary: prove ordinary LR returns** to drop the dynamic
   possible-return check (modeled ceiling ~0.971%), but the longjmp warning
   at `ppc_emit_control.cc:116` makes it less mechanically safe than T9.
5. **Nothing on T4.** Repriced below the noise floor; do not re-open.
   **T3 remnant** stays parked behind a frame-comparison gate.

### Standing configuration decisions

- **`--guest_scheduler=false` is permanent on macOS, by user decision
  (2026-08-26): "Keep guest scheduler OFF always, no need to test it for now.
  It's borked on macOS."** Do not A/B it, do not propose flipping it, and do not
  attribute a result to it. It is the baseline, not a variable. Any future
  scheduling work has to live inside `gs=false`.
- A leg whose present counter does not advance is discarded by `state_ab.py`
  rather than averaged (`b817a829f`). A stalled guest parks its threads, so its
  CPU reads LOWER than a healthy leg -- a stall looks like a win.

## T3's present-rate question, resolved: there is no unpaced present loop

The ranking workflow flagged a risk that Halo 3's 53% aggregate host GPU cost
might be an artifact of an unpaced present loop rather than addressable CPU,
on the grounds that three defaults compose badly on macOS. Two of those three
are confirmed:

- `metal_allow_tearing` defaults **true** (`metal_presenter.mm:58`), so
  `metal_layer.displaySyncEnabled = NO` and the compositor does not gate
  presents.
- `framerate_limit` defaults **0** (`gpu_flags.cc:55`), so
  `CommandProcessor::ThrottlePresentation()` returns immediately. The Metal
  presenter's own comment calls `framerate_limit` "authoritative" while it is
  off by default.

**But the conclusion does not follow, because presentation is paced by the
guest.** `logging::IncrementFrameNumber()` has exactly one call site
(`pm4_command_processor_implement.h:802`), immediately after `IssueSwap`, on
the PM4_XE_SWAP path. The `i> f:` counter in the log is therefore a count of
host presents, and `frame_ab.py` measures **29.85 presents/s at the Halo 3
menu and 29.84 at Reach's** over 120s windows. Both titles present at 30/s
because the guest issues swap packets at 30/s and the host presents once per
packet.

The 88.5 fps that raised the alarm came from a `live.py status` window that
`live.py` itself labelled "not steady state, do not read this as a
measurement".

**Consequence: T3 is a genuine target.** The GPU submit and encode cost is
real per-frame CPU at 30 presents/s, not three times the frames the title
needs, and capping presentation would return nothing because it is already
capped.

**A second finding, recorded before anyone reaches for it:**
`ThrottlePresentation` enforces `framerate_limit` with a **busy spin** on
`Clock::QueryGuestTickCount()` (`command_processor.cc:462-470`). Setting that
cvar to pace anything would trade GPU work for spin CPU on the command
processor thread, which at the Halo 3 menu is already the largest single
consumer at 24.8%.

## GTA IV at the docks: the state where the JIT actually matters

Added 2026-08-25 on request. `tools/bench/states/gtaiv-docks.script`, verified
by screenshot at Roman's taxi in the docks with the radar and tutorial text up.
Settles by ~75 s and holds **400-416% CPU** (4.1 cores) against 86% for the
Halo 3 menu and 153% for Reach's. It is the only state here that is **not
frame-capped**: 31-35 fps, GPU 14-16 ms, so fps is a genuine work-equality
guard rather than a liveness check.

Profile at the docks, 25 s sample, 388% process CPU:

| | share | spin |
| --- | --- | --- |
| XThread16D1B (guest) | 21.4% | 0% |
| GPU Commands | 19.8% | 0% |
| XThread1E013 (guest) | 16.8% | 0% |
| IOGPU submit thread | 9.8% | 0% |
| user-interactive dispatch queue | 7.9% | 1% |
| XThread28F0B | 5.8% | **61%** |
| Main XThread | 5.4% | 1% |

Hottest by self time: `guest_828BF420` 8.4%, `guest_82A46D70` 7.3%,
`swtch_pri` 6.8%, `iokit_user_client_trap` 6.7%, **`_platform_memmove` 5.8%**,
`guest_829321A0` 3.3%, `guest_82199924` 3.0%, `mach_absolute_time` 1.3%,
`objc_msgSend` 1.3%, **`__mprotect` 1.2%**, `_platform_memset` 1.2%,
`MetalCommandProcessor::WriteRegister` 0.8%, `_sigtramp` 0.7%.

**`guest JIT code: 50.3% mapped to a function.`** That single line is why this
state was worth adding. Both menus are dominated by threads spinning in yield
traps -- work the JIT cannot touch -- so every JIT change measured against them
was being asked to move a small remainder. Here half of all on-core CPU is
executing translated guest code, spin is only 6.8%, and the two hottest guest
functions are 15.7% between them.

Consequences for the campaign:
- **JIT and backend work should be measured here, not at a menu.** The a64
  emission changes that returned -0.25% in a Reach campaign run have roughly
  twice the surface to act on in this state.
- **New targets visible only here:** `_platform_memmove` at 5.8% and
  `__mprotect` + `_sigtramp` at 1.9% (the signal-based memory watch path).
  Neither appears meaningfully in either menu profile.
- The 61%-spin and 98%-spin threads suggest `zero_delay_spin_limit` may return
  something here too, but far less than at the Reach menu: total spin is 6.8%
  of on-core against Reach's 33%.

## The zero-delay lever is Reach-specific, and the docks noise floor

`--zero_delay_spin_limit=16` measured on all three states, same binary, one
cvar apart:

| state | CPU | verdict |
| --- | --- | --- |
| **Reach menu** | 152.79 -> **69.47** | **-54.58%, 3/3 pairs. Confirmed by Metal HUD at 29.97 fps and by NETWORK_RECEIVE falling 5,986 -> 1,863 samples.** |
| Halo 3 menu | 83.0/83.8/83.6 vs 83.9/83.7 | null, as predicted -- no NETWORK_RECEIVE thread, a third of Reach's spin |
| GTA IV docks | 391.05 vs 390.30 | **null**, pairs disagree, mean -0.19% |

So the lever is not a general win: it acts on one guest thread in one title.
GTA IV has a 61%-spin and a 98%-spin thread and `swtch_pri` at 6.8%, and still
returns nothing -- those threads are a small share of a 390% total, and the 2x
discount on sampled shares would have predicted about 2 points at best.
It stays default-off.

**GTA IV docks noise floor**, taken from that run because the cvar is inert
there, so both arms are effectively the same binary:

- CPU: 390.51 / 391.59 / 391.05 -- spread **0.28%**
- swaps/s: 38.11 / 38.10 / 35.72 -- spread **6.7%**

CPU is tight here; the swap rate is not, because the scene is uncapped and
run-to-run frame pacing varies. Any throughput claim in this state needs to
clear ~7%, which the constant-upload fix does by 4x.

## Rejected: bounding the logger's idle spin

The logger's writer thread (`logging.cc:293`) waits on the same disruptorplus
`spin_wait_strategy` the timer queue used, and at the GTA IV docks it was the
second largest spinner -- 853 of 27,842 on-core samples, behind only the
guest's own poll loop. `WriteThread` also carries an idle backoff
(`idle_loops >= 1000` -> sleep 50 ms) that is **dead code**: the untimed
`wait_until_published` does not return until something is published, so the
counter never increments.

Bounding the wait to 1 ms made that backoff reachable, and the profile looked
like a win: `swtch_pri` fell 7.4% -> 4.1% of on-core (2,078 -> 1,132 samples)
and the logger thread dropped out of the top threads.

**It is not a win, and the profile was misleading.** `spin_wait_strategy`'s
timed overload spins in `spin_once()` pause-loops until its deadline, whereas
the untimed escalation reaches `sleep_for(1 ms)` every twentieth yield. So the
change traded cheap yield syscalls for expensive busy-waiting: `swtch_pri` fell
because the spin moved to a form the sampler attributes elsewhere, not because
it stopped.

Measured, GTA IV docks, 3 interleaved pairs: CPU pairs disagree (mean -1.10%,
not a result) and swaps/s is down 6.4-10.4% consistently. Whole-run `cputime`
over identical runs went 611.3 -> 621.4 s on one attempt and 630.4 -> 625.9 s
on the next, i.e. inside +/-1.7% noise. Reverted.

**Two lessons kept:**
- A fall in one sampled symbol is not a saving. `swtch_pri` halving looked
  decisive and meant only that the same waiting had changed shape.
- The swaps/s guard in `state_ab.py` reads the tail of the log file, so it is
  **not independent of anything that changes logging timing**. Making the
  logger sleep up to 50 ms when idle biases it: the log is current at window
  start, when shader compilation is still logging, and stale at window end.
  Any future change to logging must be judged by the Metal HUD or by whole-run
  CPU time, never by that counter.

The underlying observation still stands and is worth someone's time: the
logger's declared idle backoff has never once executed. Fixing it needs a wait
that actually sleeps rather than spins, and the strategy object is shared with
the producer claim path every logging thread goes through, so switching it to
`blocking_wait_strategy` -- the timer queue's fix -- would put a mutex and a
`notify_all` on every log line. A consumer-only condition variable would be the
shape to try.

## Correction: the constant-sizing win is real but smaller than first reported

`077903aa7` was committed as "+28% frame throughput, -18.3% CPU per frame". A
second independent 3-pair run, this time reading throughput from
`--present_count_file` rather than scraping the log, does not reproduce that
magnitude:

| run | presents/s before -> after | CPU before -> after |
| --- | --- | --- |
| first (log-derived counter) | 35.90 -> 45.91 (**+28.0%**) | 392.50 -> 409.92 (+4.4%) |
| second (present counter) | 37.01 -> 42.11 (**+13.8%**) | 396.13 -> 410.75 (+3.7%) |

Per-pair throughput in the second run: +4.25%, +17.30%, +20.32%. The docks
presents noise floor is ~7%, so individual pairs are barely separable even
though every one favours the change.

**What is solid:** the direction. Six of six pairs across two independent runs
favour the fix on both metrics, and the mechanism is confirmed twice --
`_platform_memmove` falls from 5.79% of on-core to 1.6%, and `Bind`'s share of
the GPU Commands thread falls with it. The CPU figure is stable across runs
(+4.4%, +3.7%), which on an uncapped title means more frames for slightly more
total CPU.

**What is not:** the magnitude. CPU per present improves 18.3% by the first
run's numbers and 8.9% by the second. Quote it as **roughly -9% to -18% CPU per
frame, throughput up somewhere between 4% and 28%**, and do not repeat the
single 28% figure as though it were the result. It came from one run whose
before-arm happened to sit at the low end of the scene's variance.

**Method note.** This is the reason the docks state needs more than three pairs
for a throughput claim: the scene is uncapped and its frame rate wanders, so
three pairs can straddle a 5x range in the measured effect while the sign stays
constant. CPU is far tighter (0.28% floor) and should carry the claim wherever
the title is frame-capped -- which is both Halo states, but not this one.

## The biggest remaining GPU cost, sized: 527 command buffers per present

`iokit_user_client_trap` is 8.6% of on-core CPU at the GTA IV docks and
**2,204 of its 2,214 samples are `IOGPUCommandQueueSubmitCommandBuffers`** --
the cost scales with the number of command buffers submitted, nothing else.
Sizing it needed a count per frame, which only existed as `COUNT_profile_set`
counters visible in the profiler UI. `--gpu_counters_file` now writes them out.

Measured at the docks, 45 s window, 1,792 presents:

| | per present |
| --- | --- |
| **command buffers** | **527.4** |
| of which `texture_upload_batch` | **484.4** |
| `submission_other` | 31.1 |
| `submission_copy_draw_sync` | 8.0 |
| render passes | 60.1 |

Roughly **19,400 command buffer submissions a second**. `RequestTextures`
brackets an upload batch, and it is called per draw, so a draw that uploads
anything gets its own command buffer.

### The obvious fix is wrong, and the same data says why

Widening the bracket to the whole frame works exactly as intended on the
numbers: command buffers fall to 55.8 per present, `texture_upload_batch` to
11.9, and presents rise 39.8 -> 47.8/s (+20%).

**It also corrupts the image** -- blown-out geometry, missing textures, tail
lights as red blobs. Verified by screenshot, reverted.

The reason is in the table above: `submission_other` is **31.1 per present**,
so draw work is committed across ~32 command buffers per frame, not one.
Deferring uploads to the end of the frame puts them after most of the draws
that read them. The per-`RequestTextures` bracket is not naive -- it is what
guarantees an upload precedes its consumer.

### Submission-boundary scope was tried too. It corrupts identically.

The obvious correction to the frame-wide attempt is to flush at every
**submission boundary** instead: `MetalCommandProcessor::EndCommandBuffer` is a
single chokepoint, called ~32 times a frame (that is what `submission_other`
31.1 per present counts -- recreations of `current_command_buffer_`), and it
runs immediately before that submission's draw buffer commits. One upload
buffer per submission is ~32 per present instead of 484, and Metal executes a
queue's buffers in commit order, so the ordering argument looks airtight.

**It produces the same corruption, pixel for pixel** -- blown-out geometry,
missing textures, tail lights as red blobs. Reverted.

That the two scopes fail *identically* is the useful part: the problem is not
the width of the batch.

**Ruled out by inspection, so nobody repeats it:**
- *Ordering against the draw buffer.* The flush happens before
  `current_command_buffer_->commit()` in the same function.
- *Staging buffer lifetime.* `release_buffer_immediate` already defers to
  `buffer_pool->ReleaseAfter(cmd, buffer)` or the command buffer's completion
  handler, so widening the batch does not free anything early.

**Still open, and where to look next.** `LoadTextureDataFromResidentMemoryImpl`
picks its command buffer in preference order (`metal_texture_cache.cc:1100-1114`):
the *current draw command buffer* first -- "it needs no extra buffer at all" --
then the batch, then a private one. Holding a batch open changes which uploads
take the first branch, so the change is not only "fewer command buffers", it
also moves uploads out of the draw buffer they used to be encoded into. That is
a reordering relative to draws already encoded in the current command buffer,
and it is the remaining candidate.

Anyone attempting this should instrument that preference (count uploads per
branch, with and without the batch) before changing scope again. Two attempts
have now looked correct on every counter and been wrong on screen; the gate is
a frame comparison, not a CPU or throughput number.

## The upload batching question is the wrong question: 469 uploads per present

`--gpu_counters_file` now also reports which command buffer each texture upload
lands in. Measured at the docks over 1,984 presents at 44.5/s:

| | per present |
| --- | --- |
| `upload_via_current_cb` | **0.00** |
| `upload_via_batch` | **469.3** |
| `upload_via_private` | 1.03 |
| `texture_upload_batch` command buffers | 399.2 |

Two things follow.

**The third hypothesis for the corruption is dead.** Uploads never land in the
current draw command buffer -- `use_current_command_buffer` additionally
requires `!HasActiveRenderEncoder()`, and that is evidently never true when an
upload happens. So widening the batch does not move uploads out of the draw
buffer they were previously encoded into, because they were never in it. Also
ruled out earlier: ordering against the draw buffer, and staging-buffer
lifetime. Encoder nesting is ruled out too -- the compute encoder ends at
`metal_texture_cache.cc:1284` and the blit at `:1415`, both before the next
pair. The cause of the corruption remains unidentified.

**And the target was misjudged.** 469 texture uploads per present is roughly
20,900 uploads a second. Batching them into fewer command buffers attacks the
submission cost of a workload that should not exist at this size in a settled
scene, where the camera is stationary and the visible set is not changing. The
529-command-buffers-per-present figure is a symptom.

The question worth answering next is why the texture cache uploads ~469 times a
frame at all: whether the same textures are re-uploaded every frame (an
invalidation or key problem), whether one texture is uploaded per level or per
slice and is being counted per sub-upload, or whether GTA IV genuinely streams
that hard at the docks. `upload_via_batch` counts calls into
`LoadTextureDataFromResidentMemoryImpl`, so a per-texture-identity counter would
separate "many textures" from "the same texture repeatedly". That is the next
measurement, and it is cheap.

If it is re-upload churn, fixing it removes the uploads, their command buffers,
their encoder pairs (3.13% of on-core in encoder creation alone) and their
memmove traffic together -- which is a far larger prize than batching, and does
not require touching upload ordering at all.

## The texture cache re-uploads the same 1,315 textures ~709 times each

Measured at the GTA IV docks over a 45.5 s window, 1,984 presents at 43.6/s,
camera stationary:

| | |
| --- | --- |
| upload calls | **932,729** (470.1 per present, ~20,500/s) |
| distinct texture keys | **1,315 at the start, 1,315 at the end -- zero new** |
| most-uploaded single key | **37,595 times** |

Not streaming. The working set does not change at all during the window, and
the same textures are re-uploaded roughly 709 times each. One is re-uploaded
37,595 times.

This subsumes almost every GPU-side target in this document. Each upload drags
with it a command buffer (527 per present, 484 of them uploads), a compute
encoder and a blit encoder (encoder creation alone is 3.13% of on-core against
0.88% for the actual copy), its share of `_platform_memmove`, and the
write-watch `mprotect` traffic that re-arms the pages afterwards -- 388
`__mprotect` samples on the GPU Commands thread plus `TriggerCallbacks` and
`_sigtramp`. Removing the churn removes all of it together.

It also reframes two ticks of work: batching those command buffers optimises
the submission cost of uploads that should not be happening. The corruption
those attempts caused is still unexplained, but it no longer matters much
whether it is fixed.

### Two of the three candidates are now settled

**It is not the stale-flag path.** `Texture::MakeUpToDateAndWatch`
(`texture_cache.cc:639`) returns early WITHOUT clearing `base_outdated_` /
`mips_outdated_` when the shared-memory range is not valid, which would make the
caller re-upload the same texture on every subsequent draw forever. That was the
obvious suspect and it is wrong. Instrumented and measured at the docks:

| | per present | share of calls |
| --- | --- | --- |
| `upload_calls` | 431.4 | |
| `uptodate_calls` | 431.4 | |
| `uptodate_base_invalid` | 1.5 | **0.34%** |
| `uptodate_mips_invalid` | 0.2 | 0.05% |

It succeeds 99.6% of the time. The flags are cleared and the watch re-armed on
essentially every upload, so the textures are being invalidated again by the
watch actually firing -- not by a flag that never clears.

**So the guest, or something that looks like the guest, is writing those pages.**
Which leaves granularity, and there is a concrete reason to suspect it on this
machine specifically.

### The Apple Silicon page-size mismatch

`Memory::system_page_size_` is `xe::memory::page_size()`, which is **16,384
bytes** here (confirmed: `sysconf(_SC_PAGESIZE)` returns 16384 on M4 Pro). The
Xbox 360 guest page is 4,096 bytes. Write watches are armed and re-armed at
system-page granularity, so **one system page spans four guest pages**, and a
guest write anywhere in that 16 KiB invalidates every texture whose data lies in
it -- including three guest pages the write never touched.

On a 4 KiB-page host (Windows and Linux on x86) that fan-out does not exist.
This is a plausible 4x over-invalidation that is specific to Apple Silicon, and
it would explain re-uploading a third of a static working set every frame.

**Measured, and the page-size mismatch is NOT the dominant cause.** Counting
invalidation events against the pages and textures each one touches, at the
docks over 1,984 presents:

| | per present |
| --- | --- |
| `FireWatches` events | 85.6 |
| pages covered | 6,647.7 -- **77.6 pages per event** |
| texture watch callbacks | 755.4 -- **8.8 textures per event** |
| uploads | 482.4 |

Each event covers about **1.24 MiB** of guest memory, and ~85 of them fire per
frame -- on the order of 100 MiB invalidated per frame. Rounding a range that
size to 16 KiB boundaries instead of 4 KiB adds at most ~2.5% at its two edges.
So the host/guest page mismatch is real, and on this evidence it is worth
roughly a couple of percent of the invalidation traffic, not a factor of four.
It is not the explanation for the churn.

**What the churn actually is: a few very large range invalidations, not many
small ones.** 85 events a frame, each averaging 1.24 MiB, each hitting 8.8
resident textures. That is the shape of GPU render-target resolves writing back
to guest memory and invalidating every texture that overlaps the written range,
rather than of a guest scribbling on texture data.

**Split by origin, and it is close to even -- so the churn is largely real.**
Over 2,112 presents at the docks:

| origin | events/present | pages/present | pages per event |
| --- | --- | --- | --- |
| GPU (`invalidated_by_gpu`) | 43.7 (46.5%) | 3,929 (55%) | 89.8 |
| CPU (guest writes) | 50.3 (53.5%) | 3,218 (45%) | 64.0 |

At 16 KiB a page that is roughly **85 MiB a frame written by the GPU and 51 MiB
a frame written by the guest**, both into memory that overlaps resident
textures. Neither side is firing spuriously: these are ranges something actually
wrote.

**So this line of investigation ends without a fix, and that is the finding.**
Four hypotheses were tested and killed with measurement: stale outdated-flags
(0.34% of calls), the upload branch preference (uploads never use the draw
command buffer), the host/guest page-size mismatch (~2.5% of a 1.24 MiB range,
not 4x), and spurious invalidation (both origins are writing real bytes).

What remains is not a bug with a small fix. GTA IV at the docks genuinely
rewrites on the order of 100 MiB a frame of memory that has resident textures
mapped over it. The plausible remaining explanations need real GPU-emulation
judgement rather than another counter:

- **Textures that should have been evicted.** 1,311 textures stay resident
  while the guest reuses their backing memory for other purposes. A texture
  whose memory has been repurposed should be dropped, not re-uploaded on every
  draw that still binds its fetch constant.
- **Resolve extents wider than the data.** A resolve invalidates every texture
  overlapping its target extent; overlapping textures may intersect only part of
  it, and a sub-range test at texture granularity would cut the GPU half.

Both are substantial changes to cache policy, and both are exactly the kind of
change this campaign has twice got wrong in ways only a screenshot caught. They
should be attempted with a frame comparison as the gate and more room than a
single tick.

**What the four ticks did establish**, and it is worth keeping: the GPU-side
cost of this state is dominated by texture upload traffic that is a *consequence*
of invalidation volume, not by the submission mechanics that were the obvious
target. Batching command buffers, caching pipeline formats, and shrinking the
per-draw memcpy all attack symptoms downstream of ~100 MiB/frame of
invalidation.

### Where to look

`TextureCache::LoadTextureDataFromResidentMemory` (`texture_cache.cc:962`,
`:1057`) uploads when `base_outdated` or `mips_outdated` is set. Those come from
the write-watch machinery, which marks a texture outdated when the guest writes
anywhere in the pages backing it. Three candidates, in order of how cheaply
they can be told apart:

1. **The guest genuinely writes those pages every frame** -- a scratch or
   streaming buffer that happens to share pages with resident textures. Then the
   invalidation is correct and the fix is granularity, not logic.
2. **The watch is re-armed too broadly**, so an unrelated write in the same page
   invalidates a texture whose bytes did not change.
3. **Something clears the outdated flags unconditionally**, so every draw
   re-uploads regardless of whether a write occurred.

The cheapest discriminator is to count invalidations against writes: log how
many times a texture is marked outdated and how many distinct guest pages
triggered it. If a handful of pages are responsible for most invalidations,
that is (1) or (2) and the page can be identified.

**Do not "fix" this by weakening invalidation without proving the guest did not
write.** A texture that is genuinely stale renders wrong, and this campaign has
already shipped two rendering regressions that every counter called a win.

## Open defects found, not yet resolved

- ~~**`vector_nan_propagation_test.cc` fails 2 assertions in this tree.**~~
  **Closed by upstream, in our favour.** It expected `0xFFC00000` (x86's negative
  indefinite) for `+inf + (-inf)` while our a64 returns `0x7FC00000` (ARM's, and
  PPC's, positive default QNaN). Upstream `d3094f5e6` "[X64] Return PPC's
  positive default QNaN from the VMX float binops" changed the **x64 backend** to
  match, and rewrote the test to expect `0x7FC00000`. So the a64 result was right
  and the test was encoding SSE behaviour, as suspected. Arrived in the
  2026-08-26 rebase. **Not yet re-run here** -- confirming it now passes on a64
  needs a build, which has not been done since the rebase.

## Corrections taken from the verification workflow

- `bc2f386ff` (signal-wake WaitMultiple) is **already in-tree** under `cfcc929b6`
  plus three refinements. Cherry-picking it would revert them. Struck.
- Of the 23 `origin/a64-fixes-on-edge` commits absent by patch-id, **15 are
  already present** under other SHAs, 2 partially, and only 6 genuinely absent —
  **none of which reduces CPU.**
- The CNTVCT_EL0 per-function profiler is **struck as the ranking instrument**.
  Its counter advances at 24 MHz (41.67 ns), it measures wall-clock rather than
  CPU, attributes inclusively with no call counts, and accumulates non-atomically
  across guest threads. 93.9% of short guest calls record exactly zero ticks.
- The command-processor 500-`sched_yield` stall loop is **not** a menu cost
  (2% of GPU Commands on Halo 3, 0.23% of on-core at Reach). Struck.

## Log

- 2026-08-25: campaign opened. Live menu profiles of Reach and Halo 3 collecting.
  Static inventory workflow running over the 23 unmerged `origin/a64-fixes-on-edge`
  commits, the CNTVCT_EL0 profiler, `bc2f386ff` (signal-wake WaitMultiple), the
  instrument inventory, and menu architecture.
- 2026-08-26: T3 closed for cheap fixes and T4 rejected; T5 promoted to top
  target. `guest_scheduler` fixed OFF by decision, removing it as a variable.
  `state_ab.py` now discards a stalled leg instead of reporting it as a row.

## Reach menu, paired A/B against upstream (2026-08-26)

Two interleaved pairs, `bench-work/reach-ab.log`:

| leg                    | fps   | CPU    |
|------------------------|-------|--------|
| upstream `052365bc0`   | 29.81 | 212.4% |
| ours `1860207a4`       | 29.84 | 162.2% |
| ours `1860207a4`       | 29.84 | 161.2% |
| upstream `052365bc0`   | 29.82 | 213.9% |

**-24.2% CPU at an unchanged framerate.** Both pairs agree to within 1.5%, far
outside the 0.28% noise floor, and the legs are interleaved so drift cannot
produce the ordering.

fps is pinned at the 30 fps cap in *both* legs (29.81-29.84), which is the point:
Reach cannot convert CPU savings into frames, so the entire delta is headroom
rather than throughput traded away. On a capped title this is the only shape a
win can take, and it is why CPU% -- not fps -- is the metric here.

What the delta actually contains, checked rather than assumed:

- 146 commits separate the two legs. Exactly **one** of them is upstream's
  (`052365bc0..6290cb274` is a single commit), so this is our branch, not
  upstream progress being credited to us.
- **Neither build had the zero-delay lever.** `zero_delay` appears 0 times in
  both full CONFIG DUMPs, because `10ea11bd8` postdates `1860207a4`. The -54.58%
  zero-delay result is therefore *not* inside this number, and the two must never
  be added together -- they overlap on the same idle-spin time.
- `052365bc0` is a genuine `has207/edge` commit, not a local approximation of
  upstream.

Caveat worth keeping: `1860207a4` is not HEAD. HEAD (`cf426a292`) carries four
further commits plus the zero-delay cvar, so this number is a floor for the
branch as it stands, not a measurement of it.

## A screenshot can name the wrong commit (2026-08-26)

`build/version.h` is generated by `xenia-build.py`, **not** by CMake. An
incremental `cmake --build` never regenerates it, so the binary's window title
and log banner keep naming whatever commit was checked out the last time
`xenia-build.py` ran, while the compiled code moves on.

This tree was in that state: the binary reported `1860207a4` while actually
containing code from ~50 commits later. It was caught by testing the binary for
strings that postdate the claimed commit --

    strings <binary> | grep -qx zero_delay_spin_limit   # PRESENT

-- all of which were present, proving the stamp and not the code was stale.

This matters because the stamp is the *only* provenance a screenshot carries.
Every visual gate in this campaign is labelled with that string, so a stale
version.h makes a capture claim to be a commit it is not, and makes two
genuinely different builds produce screenshots that appear to be the same one.

`tools/bench/stamp_version.sh` regenerates it from HEAD; run it before any
incremental benchmark build. Note this cuts both ways -- it also means earlier
captures in this campaign may carry a stamp older than the code they show, so
a screenshot's label is evidence only when the build flow refreshed it.

## Three-state sweep on HEAD `cbdbde7b4` (2026-08-26)

First run of all three states on one build, after correcting a stale version.h
that had the binary reporting a commit ~50 behind its own code.

| state             | presents/s | CPU    | menu renders |
|-------------------|-----------:|-------:|--------------|
| GTA IV docks      |      41.39 | 430.8% | see below    |
| Halo 3 menu       |      29.95 |  84.0% | clean        |
| Reach menu (spin_limit=16) | 29.93 |  74.9% | clean        |

Both menus sit exactly at the 30 fps cap (29.93/29.92 on the HUD), so nothing
that landed has cost throughput. Halo 3 shows its full menu tree and Reach its
title and campaign entry, both against the correct `cbdbde7b4` stamp.

GTA IV is the odd one out and the reason it was added: uncapped and CPU-bound at
430% across ~10 cores, so CPU savings there convert into frames instead of
headroom. It is the only one of the three that can price a JIT change directly.

## Open defect: dark wedges at the GTA IV docks

Both docks captures (150 s and 240 s into the script, different time-of-day
lighting) show large hard-edged black triangles converging on the centre of the
frame. They survive a lighting change, so they are not a transient.

What is established:

- Seen at HEAD `cbdbde7b4`, at both capture times.
- Seen at `1860207a4` in the previous sweep.
- **Not** seen at `1860207a4` in `gta-prof4/hud.jpg`, a clean capture of the same
  state at a different camera position.

The same binary therefore produces both a clean and a wedged frame, which rules
out any commit in this campaign as the cause and points at scene or camera state.

The constant-sizing change was the obvious suspect -- truncated float constants
would give exactly this "vertices stretched off-screen" shape -- and it was
checked and cleared:

- `rebuild_packed_float_constants` tight-packs the buffer, writing only
  referenced vec4s contiguously from offset 0, so `float_count * 16` is the
  correct length for *that* buffer rather than an assumption about it.
- Packing and sizing key off the same object: `dxil_vertex_shader` is
  `static_cast<DxilShader*>(vertex_shader)`, and `UpdateGuestConstantCaches` is
  called with it immediately above the sizing.
- `float_count` is 256 whenever `float_dynamic_addressing` is set, so the
  dynamic-indexing case uploads the full CBV and cannot be truncated.

Unresolved: whether this is a backend defect or correct rendering of dark
geometry. Settling it needs an upstream control build at the same scene point,
which is blocked on disk (a build needs ~9 GB; 2 GB free).

**Consequence for the harness:** while this artifact is present, the GTA IV
screenshot gate cannot detect a *new* rendering regression in the same region of
the frame. The menu gates are unaffected and remain the trustworthy ones.

## T4: where 98 host instructions go for 9 guest instructions

`guest_82A46D70` is 9.0% of running samples at the GTA IV docks and is nine
guest instructions long -- a leaf accessor filling a three-word struct:

    li   r11, 0            ; [r4+0] = 0
    stw  r11, 0(r4)
    lwz  r11, 0x40A0(r3)   ; [r4+4] = [r3+0x40A0]
    stw  r11, 4(r4)
    lwz  r11, 0x40A8(r3)
    lwz  r10, 0x40A0(r3)
    subf r11, r11, r10     ; [r4+8] = [r3+0x40A0] - [r3+0x40A8]
    stw  r11, 8(r4)
    blr

**Corrected 2026-08-26:** the 98-instruction figure below was measured under the
replay's own `--guest_scheduler=true` default, which injects a preempt check the
benchmarked build does not have. Re-measured under the capture's actual config
the function is **87 instructions, 348 bytes, 9.67 host/gi** -- and the replay
now reproduces the capture exactly (348 replayed, 348 captured, 0 differ), so
these numbers are the emitted code and not an offline approximation of it. The
shares below are restated against 87.

`--jit_corpus_disasm` says where they go, and it is not address materialization:
the function has one address chain of three instructions.

Six of the nine guest instructions touch memory, and each becomes the same
eight-instruction sequence:

    mov  w0, w22             ; guest base register
    mov  w17, #0x40a0        ; displacement, too wide for an add immediate
    add  w0, w0, w17
    lsr  w17, w0, #29        ; physical-remap test: is it >= 0xE0000000?
    cmp  w17, #7
    b.ne +8
    add  w0, w0, #1, lsl #12
    ldr  w24, [x21, x0]      ; the access itself

That is **50 of 87 instructions (57%) in memory-access sequence, of which 24
(28%) are the remap test alone**. Prologue, return dispatch and the two
out-of-line stubs are 33 (38%); the return dispatch already has a fast path when
the guest LR matches the host return address, so it is not the thing to attack.
Only 4 instructions (5%) are the arithmetic and context stores the guest asked
for.

**The opportunity, unmeasured:** guest `82A46D78` and `82A46D84` both address
`r3+0x40A0` and each pays the full seven-instruction computation. `r3` is not
redefined between them, and the address arithmetic is pure -- it reads no
memory -- so it is CSE-able even though the *load* is not (an intervening store
through `r4` may alias). Eliminating one recomputation here is ~7 instructions
of 98.

**Why this is recorded as a target and not attempted:** the remap test is
address translation. A wrong guest address is silent and catastrophic, unlike
the rendering regressions this campaign has already shipped and caught on
screen. Hoisting the test to a base register is *not* obviously safe -- base+0
and base+8 can straddle the 0xE0000000 boundary, and nothing in the JIT can
prove they do not. Any attempt needs a correctness gate that fails on the bug
before it needs a benchmark.

## The corpus replay did not reproduce its own capture: --guest_scheduler

Replaying the 13,564-function GTA IV corpus on the build that captured it:

    vs capture   241 functions identical, 13,323 differ
                 captured 37,140,228 bytes, replayed 38,440,072  (+3.50%)

The tool's own verdict is that this invalidates the run. Both obvious causes
were tested and **both are refuted**:

1. **Address-materialization chains differing between processes.** Refuted:
   `|delta|/4` exceeds the function's chain count in 10,930 of 13,564 functions
   (mean 4.27 instructions per chain), and the *identical* group averages more
   chains (9.4) than the modal +44 group (4.5).
2. **Memory-access codegen differing because the offline `Memory` lacks the
   guest heap.** Refuted: the 1,472 functions with **zero** memory operations
   are 0% identical and still have a median delta of exactly +44 bytes.

Every delta is a whole number of instructions (0 of 13,564 are not a multiple
of 4). The distribution is strongly modal at +44 bytes (11 instructions, 4,506
functions), with 88 and 132 also common and 2,551 functions *smaller* than
capture. That shape -- a fixed block, in both directions, independent of memory
ops and calls -- is not explained. Coverage instrumentation was checked and is
off in both.

**Consequence:** the `vs capture` line cannot currently certify a replay, so a
codegen change scored on emitted bytes from this corpus is scored on an
instrument with a known, unexplained offset. The tool already computes the
metric that survives this -- `stable`, which it labels "compare THIS across
processes" -- and that is the only number to compare until the +44 is explained.


### Resolved: the replay was compiling with a different pass pipeline

The cause is `--guest_scheduler`. It gates `PreemptCheckInjectionPass`
(`preempt_check_injection_pass.cc:45`), it defaults to **true**, and every
benchmark state in this campaign runs `--guest_scheduler=false` for the Reach
livelock. The replay tool took its own process default, so every capture here
was being replayed under a different compiler than the one that produced it.

Found from the minimal case rather than by inspection. The smallest function in
the corpus with the modal +44 delta is one guest instruction -- a bare `blr` --
and `--jit_corpus_disasm` shows it emitting 44 host instructions offline against
33 at capture. Counting the blocks: prologue 9, **preempt check 2**, return
dispatch 14, fast return 5, **preempt slow path 9**, indirect-target slow path
5. The preempt check plus its handler is exactly 11 instructions, exactly 44
bytes, exactly the mode.

Matching the setting on the 13,564-function GTA IV corpus:

| | identical | total vs capture |
| --- | ---: | ---: |
| replay under its own default | 241 | +3.50% |
| replay matching the capture | 5,548 | -0.95% |

`kVersion` is now 2 and the header carries the setting, so a replay applies what
was captured and says so when it differs. v1 corpora are rejected by the
existing version check.

### The residual is real, smaller, and still unattributed

Matching `guest_scheduler` does not make the replay exact: -0.95% on GTA IV and
-3.32% on Halo 3, now with the replay **smaller** than capture. The differing
functions are the large ones (mean 168.7 guest instructions against 22.8 for the
identical ones, 28.0 address chains against 3.7).

`emit_mmio_aware_stores_for_recorded_exception_addresses` was the obvious next
suspect -- it emits extra store code only for addresses that actually faulted at
runtime, which offline has no equivalent, and it would push in the observed
direction. **Refuted by a controlled capture:** disabling it on *both* sides
moved 112 bytes of 27,289,116 and changed the identical count from 3,404 to
3,403.

Branch distance is the untested hypothesis -- a live code cache is larger and
more spread out than an offline one, and this backend chooses near or far
branch forms by distance. It is not tested because the obvious probe does not
work: compiling one large function from a single-function corpus **SIGSEGVs**,
which is the "recompiling untrusted guest code can fault" case the tool already
documents. The single-function technique is sound for small leaves and not for
large functions.

**Standing rule until this is closed:** score a codegen change on `stable`,
which the tool labels "compare THIS across processes", not on emitted-byte
totals against capture.

## T4 sized: redundant guest address computation is 2.12% of emitted code

Measured over the 13,564-function GTA IV corpus by decoding every D-form memory
operand and counting repeats of the same `(base register, displacement)` where
the base is provably not redefined in between and no branch intervenes:

| | |
| --- | ---: |
| D-form memory operands | 363,483 |
| redundant address computations | 29,148 (**8.02%**) |
| functions containing at least one | 4,180 (30.8%) |
| upper bound at ~7 host instructions each | **2.12% of emitted instructions** |

The count is deliberately conservative -- a branch clears the state, and an
opcode-31 instruction is assumed to write both its register fields -- so the
true redundancy is at least this.

**Read this as a ceiling, not a forecast.** It is static code size, not executed
instructions: an eliminated computation in cold code saves nothing, and the
campaign's standing correction is that shares over-predict reclaimable CPU by
about 2x. Against a 0.28% CPU noise floor at the docks a real win would be
detectable, but only if it lands near the top of this range.

### Why this needs a HIR pass and not an emitter-side cache

The obvious implementation -- have the a64 emitter remember "this guest address
is already in host register X" -- is the wrong place. The HIR is SSA, so keying
on `(value id, displacement)` is sound in principle; `v6.i64` really is the same
value at both `load_offset v6, 40A0` sites. But the cached quantity is a *host
register*, and the register allocator may spill, reload or reassign it between
the two uses. An emitter cache would have to duplicate the allocator's model of
liveness to stay correct, and the failure mode is a wrong guest address: silent,
data-dependent, and not visible on a screenshot the way the two rendering
regressions were.

The correct shape is an explicit address-computation opcode in the HIR that the
existing value-numbering machinery can dedupe, leaving lifetimes to the
allocator. That is a design, not a patch, and it is not being started on the
strength of a 2.12% ceiling without first checking how much of the redundancy
falls in hot code.

**Next step before any implementation:** weight the 29,148 redundancies by
execution frequency rather than by count. `--trace_function_coverage` already
exists and gives per-function execution counts; the redundancy above is
per-function, so the two can be joined without new instrumentation.

## T4 repriced, and T5: GTA IV's hottest code is a guest spin-wait

Joined the static redundancy count against `--trace_function_coverage` executed
counts from a GTA IV docks run (90.9 billion executed guest instructions).

**The opportunity is real but concentrated to the point of being a special case:**

| | |
| --- | ---: |
| weighted redundant / executed memory operands | 13.90% (static was 8.02%) |
| `82A46D70` share of the weighted opportunity | **84.64%** |
| `828BF420` share | 9.58% |
| top 3 functions | **95.47%** |

So a general HIR value-numbering pass would be built to serve two functions.

### The instruction-count framing overstates the win by 5.7x

`82A46D70` is **51.3% of executed host instructions** (87 emitted instructions x
4.455 billion invocations of 755.4 billion total) and **9.0% of sampled on-core
time**. Those are both correct measurements of different things, and the ratio
between them is 5.7x: this function retires instructions far more cheaply than
the average, which is what a short dependency-free sequence of L1 hits does.

Priced in executed host instructions the CSE looks like **3.48-4.18%**. Priced
on sampled time, which is the unit a CPU campaign is denominated in:

- `82A46D70`: 6 of 87 instructions x 9.0% = 0.62%
- `828BF420`: 6 of 405 instructions x 9.2% = 0.14%
- **total 0.76%, ~0.38% after the standing 2x discount, against a 0.28% floor**

The instruction-count headline is the same trap as the sampled-share one this
campaign already corrected for, in a different currency. It is recorded here so
the 3.48% figure is never quoted on its own.

### T5: the two functions are a spin loop and its body

`828BF420` contains a six-instruction loop at `828BF474`:

    828BF474 addi  r4, r1, 0x50      ; &out
    828BF478 lwz   r3, 0x22A4(r30)   ; object
    828BF47C bl    +0x1878F4         ; -> 0x82A46D70   (offset resolves exactly)
    828BF480 lwz   r11, 0x58(r1)     ; out[2]
    828BF484 cmplwi cr6, r11, 2
    828BF488 bge   cr6, -0x14        ; -> 0x828BF474, loop while >= 2

`82A46D70` writes `out[2] = [r3+0x40A0] - [r3+0x40A8]`, and `r4 = r1+0x50`, so
`[r1+0x58]` is exactly that difference. **The guest spins re-reading two counters
until their difference falls below 2**, and that is what 4.455 billion
invocations in a ~240 s window (about 18.6 million calls per second) are.

Together the pair is 18.2% of sampled on-core time at the docks.

This reframes T4 entirely: eliminating a redundant address computation makes the
*waiting* about 7% cheaper. The prize is not 0.38% for a cheaper spin, it is the
18.2% the spin costs -- if the wait can be shortened at all.

**Established** (all checkable from the disassembly and counters above): the call
target, the loop back-edge, the data flow from the two counters to the loop
condition, the invocation count, and the sampled share.

**Not established**: what `[r3+0x40A0]` and `[r3+0x40A8]` are, and crucially
*who writes them*. If another guest thread advances them, the host cannot help
and this is the guest's own design. If the host advances them -- a queue the GPU
or APU drains -- then the emulator controls how long the guest waits, and this
belongs in the same family as the Reach zero-delay result. That is the next
question, and it is answerable: watch those two guest addresses and record what
writes them.

Note the zero-delay lever is *not* this. That lever acts on zero-length delay
calls into the kernel; this loop never leaves guest code, which is exactly why
the lever measured null at the docks.

## T5: the counters are guest-written, so the ring is the guest's own

Scanned all 13,564 captured functions for D-form accesses to `+0x40A0` and
`+0x40A8`. Both counters are written by guest code:

| | |
| --- | --- |
| `+0x40A0` incremented | `82A46804`, in `82A467D8` |
| `+0x40A8` incremented | `82A4610C` in `82A46098`, `82A462E8` in `82A46198` |

The producer's sequence is unambiguous:

    82A467F8 lwz  r11, 0x40A0(r31)
    82A46800 addi r11, r11, 1
    82A46804 stw  r11, 0x40A0(r31)     ; submitted++
    82A46808 bl   82A00DC0             ; then does the work

and the consumer does the same increment on `+0x40A8`. So **`+0x40A0` is a
submitted count and `+0x40A8` a completed count**, and `828BF420` spins until
fewer than two operations are outstanding.

**This narrows T5 but does not close it.** No host write is needed to explain
the counters, which removes the most attractive hypothesis -- that the emulator
controls the wait the way it controls Reach's zero-delay spin. What advances
`+0x40A8` is a guest function; the open question is now *which thread calls it
and what that thread is itself waiting on*. If the consumer thread is blocked on
something the host owns, the emulator still sets the pace, one level removed.

`82A00DC0` was checked in case it named the subsystem. It does not: it opens
with `std`, `rlwinm`, `dcbt` and a decrementing pointer pair, which is a byte
copy helper, and the producer calls it with r5 = 0x18 -- a 24-byte move, not a
submission to an identifiable device.

**Caveat on the scan:** the corpus was captured by a run the disk guard cut
short, so a function that only compiles later would be missing. Every writer
found sits in the same `82A46xxx`-`82A4Dxxx` region as the spin loop, which is
consistent with one subsystem, but absence of a *host* writer is weaker evidence
than the presence of the guest ones.

## T5: the spin loop has no hint, so nothing in the emulator can see it

Two mechanisms in this tree can make a spinning guest thread yield, and
**neither one reaches this loop**:

1. `PreemptCheckInjectionPass` injects `OPCODE_CHECK_PREEMPT` at function entry
   and at every back-edge target -- exactly the right points -- but returns
   immediately when `!cvars::guest_scheduler`
   (`preempt_check_injection_pass.cc:45`). `guest_scheduler=false` is permanent
   here, so the pass is off.
2. `OPCODE_DELAY_EXECUTION` implements a per-guest-thread, `CNTVCT_EL0`-timed
   consecutive-spin counter that escalates to a host sleep or yield, emitted
   inline on a64 (`a64_seq_memory.cc:107-150`, tuned by `db16cyc_yield_after=2`,
   `db16cyc_consecutive_gap_ns=1000`, `db16cyc_sleep_ns=60000`). It only fires
   on a guest loop that contains the `db16cyc` hint, which PPC spells
   `or r28,r28,r28` = `0x7FFFFB78` (`ppc_emit_alu.cc:781`).

Scanned all 13,564 captured functions for `0x7FFFFB78`:

| | |
| --- | --- |
| functions containing `db16cyc` | **11 of 13,564** |
| occurrences in `828BF420` | **0** |
| occurrences in `82A46D70` | **0** |

So this loop spins with no mitigation of any kind.

### The loop, decoded

    828BF474  38810050  addi   r4, r1, 0x50      ; out = &frame[0x50]
    828BF478  807E22A4  lwz    r3, 0x22A4(r30)   ; the object
    828BF47C  481878F5  bl     82A46D70
    828BF480  81610058  lwz    r11, 0x58(r1)     ; out[2]
    828BF484  2B0B0002  cmplwi cr6, r11, 2
    828BF488  4098FFEC  bge    cr6, 828BF474     ; spin while outstanding >= 2

and the callee is nine instructions with no branch:

    82A46D70  39600000  li     r11, 0
    82A46D74  91640000  stw    r11, 0(r4)        ; out[0] = 0
    82A46D78  816340A0  lwz    r11, 0x40A0(r3)   ; submitted
    82A46D7C  91640004  stw    r11, 4(r4)        ; out[1] = submitted
    82A46D80  816340A8  lwz    r11, 0x40A8(r3)   ; completed
    82A46D84  814340A0  lwz    r10, 0x40A0(r3)   ; submitted, re-read
    82A46D88  7D6B5050  subf   r11, r11, r10     ; submitted - completed
    82A46D8C  91640008  stw    r11, 8(r4)        ; out[2] = outstanding
    82A46D90  4E800020  blr

This confirms the earlier reading of the counters, and adds the part that
matters for a fix: **every store in the loop is to the caller's own stack
frame** (`r4 = r1+0x50`). The loop reads two fields of a guest object, computes
a difference into its own frame, and branches. It makes no observable progress.
That is a spin-wait by shape, not by inference.

### Why the cheap version of the fix does not work

The obvious pass -- "inject the existing `DELAY_EXECUTION` escalation at
back-edge targets of spin-shaped loops" -- needs a shape filter, because the
`db16cyc` escalation is only safe on loops the *guest* marked. Its trigger is
two consecutive iterations less than 1 us apart, which any hot compute loop
also satisfies; firing it there would insert a 60 us sleep into real work.

A conservative filter is "short body, no stores outside the current frame, no
calls". This loop passes the first two and **fails the third**: its body is a
`bl`. Xenia compiles functions independently, so a HIR pass cannot see that
`82A46D70` is a nine-instruction leaf whose only stores are through the pointer
it was handed. Restricting the filter to call-free loops is safe and would not
catch this one.

So T5 does not have a cheap codegen fix. What is left is the consumer side:
`+0x40A8` is incremented by `82A4610C` (in `82A46098`) and `82A462E8` (in
`82A46198`), and the open question is still which thread runs those and whether
it is runnable while the spinner burns a core. At ~18.5 M calls/s the spinner is
holding roughly one full host core. Apple's guidance on spin-waiting on an
asymmetric core design is summarised in the local, uncommitted notes at
`edge-benchmarks/apple_silicon_cpu_rules.md` (ch. 7); it supports treating this
as worse than idling, at a magnitude Apple itself rates only medium.

**Caveat on the scan.** The corpus is format version 1, so its header carries no
config word and the capture settings cannot be read back from it; and it was
written by a run the disk guard cut short. A function that only compiles later
would be missing. The negative result -- no `db16cyc` in these two functions --
is about functions that *are* present, and both are present in full.

## T5 priced: the guest spins 1.5 million times per event it waits for

The open question was which thread advances the counters and whether it is
runnable. Coverage answers a more useful version of it -- how *often* they run
at all. Over the same ~240 s window that recorded 4.455e9 polls:

| address | role | executed instrs | share | ~executions |
| --- | --- | ---: | ---: | ---: |
| `828BF420` | the spin loop | 26,730,431,699 | 29.40% | 504,347,768 |
| `82A46D70` | its callee, reads both counters | 40,095,556,257 | 44.11% | **4,455,061,806** |
| `82A467D8` | producer, `submitted++` (`+0x40A0`) | 304,483 | 0.00% | **853** |
| `82A46098` | consumer A, `completed++` (`+0x40A8`) | 103,600 | 0.00% | **1,644** |
| `82A46198` | consumer B, `completed++` (`+0x40A8`) | 134,168 | 0.00% | **1,328** |

**The spinner polls about 1.5 million times per consumer execution**
(4,455,061,806 / 2,972). In rates over the window: it polls ~18.6 M times a
second while the events it is waiting for occur ~12 times a second, and the
producer submits ~3.6 times a second.

So the mean wait is on the order of **80 ms**, and the guest spends it in a
six-instruction loop. That is four to five orders of magnitude longer than a
thread switch, which is the threshold Apple gives for when spinning is
defensible at all (see the local notes at
`edge-benchmarks/apple_silicon_cpu_rules.md`, ch. 7). This is not a short wait
being handled reasonably; it is a long wait handled by burning a core.

**Why this changes the economics.** Earlier the target was framed as 18.2% of
on-core time that a yield might partially reclaim. These counts say the thread
does essentially nothing else: 73.5% of all executed guest instructions are
these two functions, and the useful work in between is 2,972 function calls.
Sleeping between polls -- which is what the existing `DELAY_EXECUTION`
escalation already does, at `db16cyc_sleep_ns` = 60 us -- would cut the poll
count by roughly three orders of magnitude while adding at most 60 us of
detection latency to an 80 ms wait. That is a ~0.07% latency cost for
essentially all of the spin.

**It also makes a conservative detector viable.** The objection to a shape-based
spin detector was that its runtime trigger (two iterations under 1 us apart)
also fires on hot compute loops. With the wait this long, the trigger can be far
stricter -- thousands of consecutive iterations, over milliseconds, in a loop
whose body performs no stores -- and still fire here within a fraction of one
wait. The earlier objection assumed the escalation had to be sensitive. It does
not.

**What this still does not establish.** Nothing here shows the consumer is
*starved*; it shows it is *infrequent*. A subsystem that genuinely completes ~12
operations a second would produce these counts whether or not the host is
holding it back, so the starvation hypothesis is neither confirmed nor
eliminated. It also does not measure what a fix returns: the 18.2% is a sampled
share, and this campaign's rule is that a sampled share over-predicts reclaimable
CPU by ~2x. And the counts are attributed per function by coverage, which is
exact, but the ~240 s window is taken from the earlier profiling run rather than
re-measured here.

## T5: sizing the false-positive surface of a shape-based spin detector

If a pass injects the `DELAY_EXECUTION` escalation at back-edge targets of
loops that "look like" spin-waits, the question is how much real code it also
tags. Scanned the corpus for backward, non-call branches whose loop body is
<=16 guest instructions with **no stores**:

| | |
| --- | --- |
| such loops | 4,663 across 2,193 functions |
| executed share of functions containing one | **47.53%** |
| restricted to bodies containing a call (T5's shape) | 1,504 loops, 964 functions, 36.28% |

So a static shape filter cannot carry the safety argument: functions holding a
matching loop are nearly half of executed code (the T5 pair inflates this --
but `82A23370` at 2.57% and `82A01A20` at 2.23% are real compute with
storeless scan loops of their own). Safety has to come from the **runtime
trigger**, and the corpus numbers say what it must survive:

- The spinner does ~1.5 M polls per 80 ms wait. A trip threshold of ~1 M
  consecutive sub-microsecond iterations costs the first ~5-10 ms of each wait
  and then sleeps away the remaining ~90%.
- A terminating scan loop that runs >1 M consecutive iterations without a
  1 us gap is the pathological case: after tripping it would eat a sleep per
  re-trip. The corpus cannot prove absence of such a loop, so the lever must
  be default-off and the A/B at the docks -- which contains both the spinner
  and those compute functions -- is the only honest gate.
- The existing `db16cyc` re-release behaviour resets the budget after every
  sleep, which for a 1 M threshold would re-spin ~5 ms per 60 us sleep and
  reclaim ~1%. A sticky variant (refresh the tick after the sleep, keep the
  count saturated) is required for the reclaim to survive the arithmetic.

Design forced by this: separate counter state and threshold from the hinted
`db16cyc` path (11 hinted functions must keep their tuned behaviour), an HIR
flag on the injected `DELAY_EXECUTION` so the emitter can tell them apart, and
a default-off cvar, same posture as the T1 zero-delay lever.

## Corrections from the 2026-08-26 adversarial review (Opus verifier)

An independent pass over the pricing arithmetic, instructed to refute it,
overturned one rejection, resolved a methodology defect, and found a record
inconsistency. Each item below was verified against the tree before being
recorded.

**1. The rlwinm 2->1 rejection is reversed; it is a live candidate again.**
Priced correctly it is **0.46-0.55% CPU, 1.6-2.0x the 0.28% floor**, not
"below/near the floor" as this document briefly recorded. Three errors in the
original arithmetic cancelled to a nearly right number attached to the wrong
verdict: the guest-instruction denominator was too generous, the whole-program
conversion was never applied, and the 2x discount was applied to a figure that
was already time-denominated (see item 2). The verifier also confirmed the
mechanics end-to-end: the `ands` wide-mask is dead for flags at rlwinm sites
(the is64 recorded by `AND_I64` never matches the 32-bit compare that follows,
so the compare-vs-zero fusion cannot fire there), meaning the saving applies at
essentially all hot-path sites including `rlwinm.`; and
`simplification_pass.cc:469-476` manufactures the same shl+and pattern
independently of `InstrEmit_rlwinmx`, so the 8.81% opcode share *undercounts*
the pattern.
**Measurement path when attempted** (per item 4): price exactly with
`rank_sequences.py` over the existing docks coverage capture, prove the
instruction delta with the 169k corpus replay (zero measurement noise), and
treat a docks CPU A/B as confirmatory only.

**2. The 2x discount is scoped: instruction-derived estimates only.** The rule
"a sampled on-core share over-predicts reclaimable CPU by ~2x" traces to a
single calibration (`jit_bench_autonomy.md`): per-change *instruction-count*
estimates landed at about half. That is an instruction->time correction. It
must NOT be applied to a figure that is already denominated in sampled time --
doing so subtracts the same physical effect twice. T4's headline 0.38% is such
a double-discount (0.76% before it); T4 stays rejected anyway, on the better
grounds this document already records -- 94% of the opportunity sits inside the
spin pair, where making the *waiting* cheaper buys nothing.

**3. The spin pair's sampled share exists in three versions: 15.7%, 15.8%, and
18.2%.** The first two come from profiles that are on disk; the 18.2% comes
from a profile that is not, and it is the one this document carried into every
T5 estimate. Until re-measured, T5 pricing should quote **15.7-18.2%** and note
that only the low end is reproducible from disk.

**4. Sub-1% codegen changes cannot be adjudicated at the docks at all -- by
either metric.** This document says both "JIT and backend work should be
measured here" (most JIT surface) and "CPU cannot carry the claim here"
(uncapped; throughput is the metric, floor ~7%). Both are true, which is the
problem: a sub-1% codegen change clears neither bar at this state. Resolution,
now standing policy: codegen changes are proven by corpus-replay **exact
instruction deltas** (no measurement noise) plus a **CPU A/B at a frame-capped
state**; the docks number is confirmatory, and only throughput can veto there.

**5. The guest-call `bl` candidate was already retired by exact accounting, at
0.10%.** This document's 0.9%-gross/0.45%-discounted estimate for replacing the
64-bit constant materialisation was 4-9x too high: `jit_bench_autonomy.md`
prices the wide-move-chain family at **0.10-0.11% exact** ("retired twice").
The rejection stands, on those grounds. One record fix: the retirement's cited
Apple objection (§2.8.2) reaches literal *pools* -- an island of data in code
space -- and does not apply to `bl`, which is a branch immediate. The target is
dead because it is worth 0.10%, not because of §2.8.2.

The reopened candidate is tracked as **T6** in the status table: rlwinm hot
path, `lsl`+`ands #0xFFFFFFFF` -> W-form `lsl`, priced 0.46-0.55% CPU by the
corrected arithmetic, proof path = rank_sequences pricing + corpus-replay exact
delta + frame-capped CPU A/B, docks throughput as veto only.

## Exact host-side accounting at the docks: even the sequence ranking is the spinner

`rank_sequences.py` over the docks coverage (675.8e9 executed host
instructions, exact per-site):

| share | sequence | I/exec | executions |
| ---: | --- | ---: | ---: |
| 25.27% | `load_offset i32` (byte-swapped guest load) | 8.64 | 19.77e9 |
| 10.97% | `call` (direct guest call) | 15.94 | 4.65e9 |
| 8.99% | `call_indirect` | 13.06 | 4.65e9 |
| 5.79% | `store_offset i32` | 7.97 | 4.91e9 |

Read with the T5 counts in hand, the ranking mostly re-measures the spinner:
the call families' 4.65e9 executions are the spinner's 4.455e9 calls plus
change, and ~70% of the top load sequence's executions are the loop's own
polls (~13.9e9 of 19.77e9). Two durable facts survive after subtracting it:

- **A guest 32-bit load costs 8.64 host instructions**, a store ~7-8. Guest
  memory access is ~46% of all executed host instructions; non-spin, still
  ~7.5% for loads alone. The per-access physical-remap check (4 of those ~8.6
  instructions) is the obvious component -- but this is a post-T5 target: on
  today's capture, any per-access saving is measured mostly in cheaper
  *waiting*.
- **T6's pair does not appear in the top 15** (cutoff 1.14% = 7.7e9), which is
  consistent with the 0.46-0.55% pricing and inconsistent with anything
  larger. The reopened estimate survives its first exact-accounting contact.

The instrument's own caveat: sequence tables count host instructions, and
instruction share is not time share -- loads carry the cache misses, so their
time share is likely above 46%, not below. Conversion to CPU% goes through the
sampled guest-JIT share (~35-42%) as standing policy.

## T5 lever, first measurement: the CPU cut is real, the throughput answer is not in

GTA IV docks, `spin-off` vs `spin-100k` (`--spin_wait_yield_after=100000`),
same binary `737456bd1`, `--guest_scheduler=false` both legs, 3 interleaved
pairs attempted. The harness was killed externally during leg 6, so **two
complete pairs** survive plus one unpaired off-leg; the JSON and paired
summary were never written and the numbers below are computed from the leg
lines.

| | off CPU | on CPU | dCPU | off swaps | on swaps | dswaps |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| pair 1 | 414.84 | 363.88 | **-12.28%** | 44.53 | 37.47 | -15.85% |
| pair 2 | 429.68 | 362.56 | **-15.62%** | 39.62 | 41.58 | +4.95% |
| unpaired off leg 5 | 426.69 | | | 37.92 | | |

- **CPU: -13.95% mean, both pairs agree in sign**, and the on-legs are
  remarkably stable (363.88 / 362.56) while the off-legs swing 15 points.
- **Throughput: pairs disagree (-15.9% / +5.0%) -- not a result**, exactly as
  the standing rule predicts for three pairs. Swaps drifted downward all
  session regardless of config (44.5 -> 37.9 across the five legs), which is
  the shape of thermal or accumulated-state drift, not of the lever.
- **Visual gate: PASS.** Window-id screenshots of an off-leg and an on-leg
  (`bench-work/spin-shots/`) render the same scene with no corruption. The
  on-leg's frame-interval trace is visibly steadier, which is what reduced
  core contention would look like, and is not evidence of anything by itself.

**Verdict: the lever stays default-off, and is NOT cleared for adoption.**
The CPU cut alone cannot carry the claim at this state, throughput is
unresolved, and the external review below found real defects that have to be
fixed before the next measurement.

## The GPT review of the lever: three code defects and a false claim

The independent review (gpt-5.6-sol, max reasoning; full text at
`bench-work/gpt_review_out.md`) verified the campaign's numbers against the
tree and then attacked the lever's implementation. Confirmed against the code:

1. **Cross-loop sleep contamination, worse than the known scan-loop risk.**
   `spin_wait_spins` accumulates across every tagged site on the thread, and
   the armed re-trip budget of 64 can be satisfied by a *different* loop than
   the one that armed it -- the victim of a sleep need not be the loop that
   earned it. The pre-trip validation has the same hole in aggregate: with a
   1M threshold the elapsed budget is a full second, so up to a second of
   real work still "validates".
2. **The first trip always rejects.** `spin_wait_reset_tick` initializes to
   zero, so the first threshold crossing computes an elapsed time since boot
   and resets; the first sleep costs two full thresholds, possibly spread
   across sites.
3. **Thresholds 1-63 underflow.** `threshold - 64` is unsigned; small values
   wrap and re-trip on effectively every iteration.
4. **"At most 60 us of added latency" is false.** The sleep is NanoSleep, and
   this tree's own threading_posix.cc documents Darwin nanosleep oversleeping
   by 100-500 us under load. Per-wakeup latency on a latency-critical tagged
   site is up to ~500 us, which at tens of events per frame is milliseconds --
   a candidate explanation for pair 1's throughput leg.

Also from the review, on instruments: `rank_sequences` prices *laid-down*
bytes, not retired instructions -- the physical-remap check lays down 4
instructions but executes 3 on the common path, so the "4 of 8.64" framing in
the earlier section overstates the remap's dynamic cost; and
`weight_opcodes`'s uniform-within-function attribution could be replaced by a
per-PC join since the coverage tables carry per-site counts.

**Required for the next measurement, per the review:** per-site trip state
(the armed sleep must be keyed to the site that earned it), an underflow
guard, reset-tick initialization, per-site trip logging with guest PC, and a
hard gate: **any site other than `828BF474` sleeping during the docks run
fails the change**. Its target ranking: T6 first (best CPU-per-risk), the
texture-invalidation policy second (largest throughput mechanism), and
dynamic repricing of the remap check third.

## T5 lever, second A/B: -14.91% CPU, no throughput cost, one impostor caught

Same protocol, binary `87c437165` with the site-keyed escalation. Three
complete pairs this time:

| pair | off CPU | on CPU | dCPU | off swaps | on swaps | dswaps |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 416.63 | 360.19 | -13.55% | 45.12 | 42.75 | -5.26% |
| 2 | 430.32 | 363.25 | -15.59% | 32.63 | 37.84 | +15.97% |
| 3 | 432.91 | 365.35 | -15.61% | 36.56 | 36.44 | -0.32% |

- **CPU -14.91% mean, all pairs agree**; the lever-on legs sit within 2.6
  points of each other while the off legs swing 16.
- **Throughput: mixed signs, means 38.10 -> 39.01.** No consistent cost; the
  first run's -15.9% pair does not reproduce under the site-keyed fix. Not a
  throughput win claim either -- within-config variance this session was far
  above the documented 7%.
- **Site gate: one impostor.** The per-episode log names exactly two loops
  across all three lever-on legs: `828BF474` (1,577 / 6,430 / 6,373
  episodes -- the target) and `82A05838` (9 / 9 / 8 -- deterministic). The
  latter decodes to a byte-wise parse loop advancing a cursor; it was doing
  real work and got slept for it. `2e5513a9f` rejects loops containing an
  induction variable (a guest register self-update through its context
  slot), which the replay shows removes the scanner's injections while
  keeping the target's, and shrinks the corpus-wide injection footprint by
  roughly a third.

**Standing: the lever remains default-off.** Adoption needs (1) a
confirmation A/B with the induction filter showing a clean site log, and
(2) a Halo-state CPU check, since both Halo titles idle differently and the
lever is generic. The CPU reclaim at the docks is the largest this campaign
has measured at this state; the throughput question that killed the first
run is answered as "no consistent effect".

## T5 lever, third and fourth A/Bs: gate clean, and provably inert on Halo 3

**Docks confirmation with the induction filter** (binary `c4190ee4e`):

| pair | off CPU | on CPU | dCPU | dswaps |
| --- | ---: | ---: | ---: | ---: |
| 1 | 419.03 | 363.13 | -13.34% | -11.91% |
| 2 | 425.69 | 361.79 | -15.01% | +1.29% |
| 3 | 426.41 | 364.29 | -14.57% | -2.54% |

CPU **-14.31% mean, all pairs agree** -- the third consecutive A/B at this
magnitude (-13.95, -14.91, -14.31 across three runs; every lever-on leg ever
measured sits in 360-365% against off-legs at 415-433%). The site log is
**clean**: `828BF474` only, in every lever-on leg (1,081 / 6,899 episodes and
similar); the byte-scanner no longer appears. Throughput across all six
filtered+unfiltered pairs: mean -0.46%, signs split 4:2 -- consistent with no
effect at this noise level.

**Halo 3 menu** (30 fps locked, CPU is the metric): -0.94% / +0.43% / +0.24%,
mean **-0.09%, pairs disagree -- a null**, and mechanically explained:
**zero escalation episodes** in any lever-on leg. No Halo 3 menu loop ever
reaches 100k consecutive sub-microsecond iterations, so the lever does not
act at all. Presents pinned at 29.95 in all six legs.

**Standing after four A/Bs:** the lever does exactly one thing -- it puts the
GTA IV docks spinner to sleep -- and does it for -14% CPU with no measurable
throughput cost and no effect anywhere else tested. Still default-off. The
remaining step before proposing default-on is a Reach check (its menu has the
kernel-side zero-delay spin; the interaction should be measured, not
presumed).

## T6 implemented: shl+and-0xFFFFFFFF fuses to one W-form lsl

The backend now recognizes `SHL_I64(v, const n, 0<n<32)` whose single use is
an immediately following `AND_I64(..., 0xFFFFFFFF)` and emits one W-form
`lsl wD, wS, #n` instead of the two X-form instructions: an AArch64 W-register
write zeroes the upper 32 bits, and a left shift cannot move bits downward, so
the mask is implied. The AND is skipped through a one-shot fusion handoff on
the emitter (`MarkFusedSkip`/`ConsumeFusedSkip`); the plain `lsl` sets no
flags where the old path's `ands` did. The safety argument originally written
here ("the backend never carries NZCV between HIR instructions") was WRONG --
the codex cross-check found the deliberate one-shot NZCV fusion
(`DeclareFlagsZeroTest` -> `ShiftFlagsZeroTest` -> `FlagsHoldZeroTest`): ANDS
declares a zero-test that exactly the next sequence may consume, and
`COMPARE_EQ/NE` against 0 then emits a bare `cset` with no `cmp`. The fusion
is still safe, for the narrower reason the code comments state: skipping the
AND only removes a *declaration*; the skipped sequence still passes through
the one-shot shift so no armed state can go stale, the fused W-form `lsl`
writes no flags, and every tracker consumer falls back to an explicit
`cmp`/`tst` when nothing is armed. At `shl+and+cmp0` sites the counts are
even (`lsl w`+`cmp`+`cset` vs `lsl`+`ands`+`cset`); everywhere else the
fusion is one instruction ahead. The commit message of `3425b31fa` carries
the wrong generalization; this paragraph is the correction of record.

Evidence, in gate order:

- **Semantics: 169,048/169,048 pass, 0 fail** (36 s, prebuilt corpus via
  `--test_bin_path`). Note the local baseline is now cleaner than the
  documented edge profile (4 mcrf fails + 1 SIGBUS): the CR-observability
  commits fixed those, so the expected profile for future gates is 0/0.
- **Corpus replay exact delta** (docks corpus, 13,564 functions): stable
  emitted instructions **9,018,217 -> 9,004,296 = -13,921 laid-down
  (-0.154%)**, host bytes -55,732. Laid-down, not executed -- the executed
  share is what the 0.46-0.55% pricing estimated and what the A/B must show.
- **Replay disasm spot-check** (`82160700`, the largest shrinker, -960 B):
  address-normalized diff against the unfused build decomposes into **242
  exact `lsl xA,xB,#n; ands xA,xA,#0xffffffff` -> `lsl wA,wB,#n` rewrites**
  (shift amount, source, and destination all preserved), 281 host-address
  mov/movk chains that differ only by link address, and the summary table.
  Zero unexplained codegen hunks.

Evidence it lacks: a runtime CPU measurement. Next: frame-capped CPU A/B
(Halo 3 menu, CPU is the metric) of this commit's app against `c4190ee4e`,
with docks throughput as veto only per the standing rule for sub-1% codegen.

## T6 closed: runtime null at the capped state, adopted on static evidence

The Halo 3 menu CPU A/B (base `c4190ee4e` vs fusion `3425b31fa`,
guest_scheduler=false both sides) was killed at the user's request after 5 of
6 legs -- future capped-state regression guards run `--runs 2`. What
completed: base legs 83.58 / 83.86 / 82.97 (mean 83.47), fusion legs
83.67 / 83.38 (mean 83.53); the two complete pairs read +0.11% and -0.57%,
signs split. Presents pinned at 29.92-29.93 in all five legs. Verdict: **null
at the 0.28% floor -- no regression**, which is all the A/B had to prove; the
0.46-0.55% pricing was built from docks execution weights, and the Halo 3
menu barely runs those rlwinm sites. T6 ships on its static evidence:
strictly fewer emitted instructions (-13,921 laid down), 169,048/169,048
semantics, and the disasm-verified rewrite. The docks CPU number cannot
adjudicate a sub-1% codegen change (standing rule), so no docks run was
spent on it.

Adversarial cross-check (codex/gpt-5.6-sol, read-only): its brief was to
break the fusion's three claims. It refuted the NZCV generalization (the
correction above), and found **no defect** in the fusion guard itself (claim
2) or in the W-form-lsl equivalence (claim 3).

## T5 Reach-menu check: inert, and a measurement lesson

Methodology note first: per the user's standing order (2026-08-26), menu
A/Bs now run 10 s windows, single pair first, escalating only on a signal.
The 0.28% noise floor was established at 60 s windows and does not transfer;
at 10 s the Reach menu's OFF-legs alone span 161.0-171.9% CPU (~+/-6%
drift -- the scene is not static). Sub-1% claims at menus are no longer
adjudicable, and that trade is accepted.

The check (binary `3425b31fa`, lever off vs `--spin_wait_yield_after=100000`,
guest_scheduler=false both sides, 30 fps pinned throughout):

- First pair read **+11.92%** (162.51 -> 181.89) -- alarming, and exactly the
  shape the kernel-side zero-delay-spin interaction predicted. It did NOT
  reproduce: confirmation pairs **+0.77% / -0.46%, mean +0.16%, pairs
  disagree -- a null**. The 181.89 leg was window drift, matched by a later
  OFF-leg at 171.88.
- **Zero escalation episodes in every lever-on leg** (site log grepped per
  leg). On the Reach menu the lever never acts; the only possible cost is
  the injected counter fast path, which Halo 3's 60 s-window null already
  bounded at ~0.

**Verdict: the Reach-menu adoption gate passes.** The lever is docks -14%,
Halo 3 inert, Reach menu inert. Remaining before flipping the default:
one Reach *campaign* spot check -- the known intermittent livelock
([guest_scheduler] class) is the one plausible bad interaction with a
lever that sleeps spinning guest threads, and it should be seen surviving
the lever once. Then the default flip is its own commit.

## T5 CLOSED: spin-wait lever adopted, default 100000

The Reach campaign gate passed (single pair, 10 s window, per the standing
short-run rule): CPU -0.40% (205.05 -> 204.23), the swap-counter
work-equality guard within tolerance (-1.68%), no livelock -- both legs
reached the state and ran to completion -- and **zero escalation episodes**,
the fourth state in a row where the lever provably never acts outside its
target. Note for the record: the swaps column here is the guest swap
counter, NOT fps -- the Metal HUD holds 30 throughout, and README's
36.2-vs-29.9 example is the same mismatch; a 27.x reading at the campaign
is normal and was misread once (by me) as dropped frames.

Final ledger for `--spin_wait_yield_after=100000`:
- GTA IV docks: **-13.95 / -14.91 / -14.31% CPU** across three 3-pair runs,
  throughput no consistent effect, screenshot gate passed, site log =
  `828BF474` only.
- Halo 3 menu: inert (0 escalations), CPU null at 60 s windows.
- Reach menu: inert (0 escalations), CPU null (10 s windows, 3 pairs).
- Reach campaign: inert (0 escalations), CPU null, livelock survived.

Default flipped in this commit. Evidence it LACKS: nothing further was
measured after the flip decision per the user's instruction ("you should
have way more than enough data"); untested titles rely on the runtime
trigger's narrowness (100k consecutive sub-microsecond iterations,
site-keyed validation) rather than per-title measurement.

## T3 slice 1: precise resolve invalidation, implemented and proven offline

`--precise_resolve_invalidation` (default **off**): instead of invalidating
every texture overlapping a resolve's whole bounding interval, the Metal
resolve path invalidates the per-band spans the resolve can actually write.
`GetResolveInfo` decomposes the 2D destination extent into portion-height
bands (32 blocks at 4+ bytes per block, 64 at 2, 128 at 1 -- tiled
addressing is only independent within those square portions, per
`GetTiledAddressUpperBound2D`), columns rounded out to portion boundaries,
each span clamped to the whole-rect interval. 3D/array destinations,
resolution-scaled resolves, and >48-band rects keep the old single-interval
path, as do D3D12/Vulkan entirely.

Evidence it HAS: a 50-case invariant test against the real tiled-address
functions (`bench-work/t3-span-invariant-test.cc`, linked against
libxenia-gpu) proving, for bytes-per-block 1..16 and ten rect shapes
including the degenerate ones: (1) every byte of every texel in the rect
lies inside a span -- nothing a resolve writes can escape invalidation; and
(2) the span union is contained in the old interval -- the lever can only
shrink the invalidated set, never grow it. Partial-pitch rects shrink to
2.6-52% of the interval; full-pitch rects produce exactly one span equal to
the old interval. The test also caught two real design errors on the way:
a 32-row band decomposition is unsound for 1-2 byte blocks (portion
interleaving), and unclamped portion-rounded spans overshoot the interval.

Evidence it LACKS: everything runtime. Before this cvar is ever enabled:
(1) `fire_watches_gpu_*` counters at the docks, on vs off -- the doc's
46.5%-GPU-origin churn share predicts a large drop; (2) the compressed
window-id screenshot gate against the unmodified build -- this is exactly
the twice-burned change class; (3) docks throughput. Next tick.

## T3 slice 1 measured: correct, visually clean, and INEFFECTIVE at the docks

The runtime gate (docks, 110 s legs, counters + window-id screenshot,
`9a1030c9c` on vs off):

| counter | off | on | delta |
| --- | ---: | ---: | ---: |
| upload_calls | 1,403,721 | 1,391,786 | -0.85% |
| fire_watch_gpu_pages | 12,680,488 | 12,452,371 | -1.8% |
| fire_watch_gpu_events | 163,190 | 163,846 | **+0.4%** |

The events row is the verdict: with spans on, one resolve fires one
FireWatches call per span, so events would multiply if resolves decomposed.
They rose 0.4% -- **docks resolves average ~1.004 spans: they are full-pitch,
the bounding interval was already tight, and there is nothing for the span
decomposition to cut here.** Screenshot gate passes (scene-identical, no
corruption), so the lever is correct -- just unproductive at this state.

**This refutes "resolve extents wider than the data" as the docks churn
mechanism.** The GPU half of the invalidation traffic is dense full-width
resolves rewriting bytes that resident textures genuinely cover --
render-target feedback, real data flow. Fixing THAT means keeping resolved
data host-side rather than round-tripping guest memory, which is the
architectural change the doc already put outside cheap-fix scope. The cvar
stays default-off: proven harmless, potentially useful for titles with
partial-screen resolves, not a campaign lever. Consistent with this,
upload_distinct_keys held at ~1,300 across both legs while upload_max_repeats
hit ~19k -- the churn re-uploads the SAME keys, which also predicts the
repurpose-eviction remnant (new keys, dead old ones) would not move this
number. T3 stays closed for cheap fixes; next target is the GPT review's
rank 3, dynamic repricing of the physical-remap check.

## Physical-remap check repriced on post-T5 weights: 0.7-1.4% CPU, real

Fresh capture at the docks on the current default build (lever on,
`3425b31fa`): corpus2.bin + coverage2.csv, 347.6e9 executed guest
instructions over the window, spinner pair still 52.8% of executed
instructions (it spins at full speed between sleep episodes; the lever
reclaims the *wait*, not the share). Non-spin (47.2%, 164.0e9):

- Named loads/stores (lwz/stw/lfs/stfs) are **15.74% of executed non-spin
  guest instructions** -- a lower bound; op-31 indexed forms (lwzx etc.,
  17.95% bucket) add an unquantified amount.
- Every non-constant access on a 16 KiB-granularity host retires the
  3-instruction remap check (`lsr/cmp/b.ne`, the `add` only above
  0xE0000000) AND loses the direct `[membase, W, UXTW]` addressing form to
  an extra `mov` (`GuestMemDirectIndex` refuses remap hosts), so the real
  per-access tax is ~4 instructions of the 8.64. Constant addresses already
  fold the check at compile time (`ComputeMemoryAddressOffset`).
- Price: >=4 x 15.74% / 6.1 host-per-guest = **>=10% of non-spin executed
  host instructions**, x0.47 non-spin fraction x0.35-0.42 JIT share /2
  discount (the check is well-predicted ALU, time share below instruction
  share) = **~0.7-1.4% CPU at the docks**. 2.5-5x the floor.

Elimination designs, assessed:
(a) **Bake +0x1000 into the host mapping: impossible on this hardware.**
    `MapViews` masks the 0xE view's target offset with the allocation
    granularity (`memory.cc:433`, map_info target `0x...100001000`); 16 KiB
    pages cannot alias at a 4 KiB offset. This is why the check exists.
(b) **PROT_NONE the 0xE host range and drop every check.** An unchecked
    0xE access then faults loudly instead of silently reading the wrong
    page -- the catastrophic-silent failure mode becomes a crash, and a
    fault handler could fix up stragglers. Viability turns entirely on how
    often guest code actually touches >=0xE0000000 at runtime (360 titles
    use physical allocs there for GPU-visible buffers; frequency unknown).
    **Next step: instrument ApplyPhysicalRemapW0's taken path with a
    per-context counter and measure the docks rate before any design work.**
(c) **Static value-range elision**: constants already handled; variable
    bases dominate and the doc's earlier warning stands (base+disp can
    straddle the boundary). Low ceiling, not pursued.

## Remap-check elimination: closed. The guest really uses 0xE0000000+

`--count_physical_remap_hits` (new, default off; racy by design) counted
taken remap fixups at the docks, steady-state window t=100..140 s:
**2,059,172 taken over 40 s = ~51,500/s, ~1,150 per present** (44.8
presents/s, in state). GTA IV is continuously reading/writing physical
allocations above 0xE0000000 -- GPU-visible buffers, as suspected.

Design (b), PROT_NONE + fault fixup, is therefore dead: 51.5k faults/s at
2-10 us each is 10-50% of a core. With (a) impossible on 16 KiB pages and
(c) low-ceiling, **the 0.7-1.4% CPU remap tax has no viable cheap
elimination and the target is closed.** Two mitigating notes for the
record: taken fraction is ~0.01% of all checks (the branch is essentially
perfectly predicted, so the true time cost sits at or below the /2
discount), and the counter cvar remains as a diagnostic.

## T1 adopted: zero_delay_spin_limit=16 by the T5 precedent

The original verdict ("title-specific, so it stays off") predates the T5
adoption logic: a lever that is large where it acts and provably null
where it does not act is a default, not an option. T1's evidence now:

- Reach menu, current build (both new levers in baseline): **-51.56% CPU**
  (160.62 -> 77.81), presents pinned -- reproducing the historic -54.58%.
- Reach campaign: **-39.36% CPU** (198.70 -> 120.50), swap guard -1.84%,
  no livelock. The lever helps real gameplay, not just menu idle.
- Halo 3 menu and GTA IV docks: null (doc's three-state matrix; Halo 3 has
  no NETWORK_RECEIVE thread and a third of Reach's spin, docks pairs
  disagree at -0.19%).

Default flipped to 16 (this commit). Evidence it LACKS: any multiplayer /
networking session -- the affected thread is Reach's netcode poll loop and
the park is guest-observable timing; nothing here exercises live netplay.
If networked play ever regresses, this default is the first suspect.

## T7: the single-precision denormal quirk folds away over provably-single data

Fresh-capture ranking put `denormal_quirk` at **4.89% of executed host
instructions (18.32 per execution, 7.59e9 executions)** -- the screen that
gives single-precision ops the 360's default-QNaN answer when a double
denormal operand slips in. But a double denormal cannot come out of
single-precision data: the smallest single denormal (2^-149) is far above
double's normal floor (2^-1022). So a quirk whose operands all trace to
lfs unpacks, TO_SINGLE results, selects between such values, or
non-denormal constants is statically 0 and folds away, Select consumer
and all.

Implemented as `VALUE_NEVER_F64_DENORMAL` (set where lfs's NaN-repairing
unpack is constructed -- both select arms carry a single widened to double)
plus a `SimplificationPass` rule recursing through TO_SINGLE / CONVERT-
from-f32 / SELECT defs, after context promotion has connected FPR
store/load chains. Unprovable defs (lfd, double arithmetic, load_context
from unknown context) keep the full screen.

Evidence it HAS: **169,048/169,048 semantics pass**; docks corpus replay
laid-down stable **10,208,128 -> 9,867,116 (-341,012, -3.34%)** with 1,790
functions shrinking; coverage-weighted executed saving **2.47% of all
executed host instructions, 5.30% of non-spin** (uniform intra-function
weighting, the instrument's documented caveat). Priced ~0.4-0.5% CPU by
the standing conversion.

Evidence it LACKS: a runtime pair (Halo 3 menu guard queued); any case in
the 169k suite that exercises the fold itself -- suite operands come from
context and stay unprovable, so the suite proves no-collateral-damage
while the fold's own soundness rests on the 2^-149 >> 2^-1022 argument
and the three construction sites.

T7 runtime guard: Halo 3 menu pair, pre-fold `2179519a2` vs `475253748`:
**-0.25% CPU, swaps identical** -- no regression; the predicted ~0.5% is
below the 10 s-window floor by design. T7 ships on its static evidence,
same standing as T6.

## T8 scoped: guest call machinery is ~14.4% of executed host instructions

Fresh-capture ranking: `call - symbol` 7.94% at 15.79 I/exec,
`call_indirect` 6.50% at 13.38, `set_return_address` 1.56% at 3.00. The
mechanism, from `A64Emitter::Call` (a64_emitter.cc:811): a call site whose
callee is already compiled at EMIT time gets the direct path (3-instr
absolute-address materialization + blr + return-addr ldr); every other
site -- the common case under lazy JIT, since callers compile before their
callees -- walks the encoded indirection table on EVERY call forever:
bias ldr, add, slot ldr, out-of-lined external-target check, blr. The
table slot gets patched when the callee compiles, but the WALK never
collapses to a direct branch.

Design options, in rising ambition:
(a) **Instrument first** (the remap-counter pattern): count executed
    direct-path vs indirection-path calls at the docks to size the
    recoverable share before touching the convention.
(b) **Relative bl for compiled callees**: kGeneratedCodeSize is 256 MB
    (code_cache_base.h:623) so +-128 MB `bl` does not universally reach,
    and code is placed after emission -- needs a fixup pass at placement
    plus an out-of-range fallback. Saves ~2-3 I/exec on the direct subset
    only.
(c) **Backpatch call sites** when a callee compiles (registry of pending
    sites + icache flush): collapses the 5-6-instruction walk to bl/blr
    for the dominant subset. The real prize, and the real risk: patching
    live code pages on a multithreaded JIT under macOS W^X.

Priced ceiling if (c) recovered ~4 of 13-16 I/exec across both call
forms: ~3-4% of executed host instructions, ~0.6-0.8% CPU by the standing
conversion. Next step is (a) -- measurement before mechanism, per the
campaign's standing lesson that sampled shares over-predict 2x.

**T7 adversarial review (codex/gpt-5.6-sol, 483 KB transcript): one real
defect, fixed.** Claim 1 (the math): DEFECT -- `--no_round_to_single`
(an x64 debug cvar, "not for users, breaks games") turns TO_SINGLE into an
identity, so a finite double denormal passes through unrounded and the fold
would wrongly report 0. Fixed by gating the TO_SINGLE acceptance on
`!cvars::no_round_to_single` (the cvar moved to shared cpu_flags so the
backend-agnostic pass can see it; a64 always rounds and never hits it).
Claims 2 (dataflow/pass-ordering) and 3 (other FPR producers): no defect
found. The lfs flag, the SELECT recursion, and the constant rule survived
attack unchanged.

**T7 recognizer widened with NEG/ABS pass-through (null on the docks
corpus).** fneg/fabs/fnabs are sign-bit-only by PPC semantics and
denormality lives entirely in exponent+mantissa, so the proof passes
through OPCODE_NEG/OPCODE_ABS in both directions. 169,048/169,048; corpus
replay 10,470,877 laid-down vs 10,470,871 after the original fold -- a +6
wobble at the 0.00006% level, i.e. zero fneg-fed quirk sites at the docks.
Kept: compile-time-only recursion, no runtime cost where it does not fire,
and the docks corpus is not evidence about other titles' FPU idiom.

## T8 measured: the indirection walk is 94.66% of executed symbol-call dispatches

Instrumented per the plan's option (a): `--count_call_paths` (default off)
emits a racy 4-instruction counter bump (the `xe_a64_physical_remap_hits`
pattern) at the three dispatch paths in `A64Emitter::Call` / `CallIndirect`,
dumped through `--gpu_counters_file`. Register safety: the Call bumps use
x16/x17 before the walk arms w16 with the guest address; the CallIndirect
bump runs after the target is normalized into w16 and uses x14/x17, which
the walk overwrites anyway. One ~145s docks leg (`bench-work/callpath-rate/`,
clone of the remap-rate harness), counters differenced over t=100..140s so
JIT warmup is excluded:

| path | delta over 40s | rate | share |
| --- | --- | --- | --- |
| direct (callee compiled at emit) | 332,046,056 | 8.30 M/s | 5.16% |
| indirection walk (`Call`) | 5,891,215,256 | 147.28 M/s | 91.53% |
| register-indirect walk (`CallIndirect`) | 213,336,036 | 5.33 M/s | 3.31% |

Presents over the same window: 43.2/s, inside the docks' normal range, so
the counter overhead (~0.65G instrs/s with the cvar on) did not distort the
scene. `physical_remap_hits 0` confirms that cvar was off.

What the split decides:

- **Option (b) -- relative bl for emit-time-compiled callees -- is dead.**
  The direct path is 5.16% of executed dispatches; optimizing it is
  optimizing the exception. Under lazy JIT, callers compile before callees
  and a call site emitted as a walk stays a walk forever -- exactly the
  mechanism the scoping note predicted, now quantified.
- **The `call_indirect` corpus share was a mirage of the return check.**
  The corpus ranked call_indirect at 6.50% of executed host instructions,
  nearly equal to call-symbol -- but true register-indirect dispatches are
  29x rarer than symbol calls at runtime. The counter sits after the
  possible-return epilog check, so the corpus share is dominated by `blr`
  returns that never reach the table walk. No optimization target there.
- **The whole T8 prize is option (c), and it reprices to ~0.4-0.5% CPU.**
  Backpatching the 147.28M/s walk from ~6 executed instructions to ~2 saves
  ~0.59G instrs/s against an implied ~30.9G instrs/s total (computed from
  the corpus 7.94%-at-15.79-I/exec calibration and the measured symbol-call
  rate): 1.90% of executed host instructions, ~0.4-0.5% CPU by the standing
  conversion. Below the scoping note's 0.6-0.8% ceiling because the
  register-indirect pool evaporated.

Evidence this HAS: measured executed-dispatch rates at the docks over a
warmup-excluded window, on the adopted-default build. Evidence it LACKS: a
second scene (the split could differ at menus, though nothing rides on
that), and any safety story for patching live code pages under MAP_JIT --
which is now the sole load-bearing question. Next step: adversarial review
(codex) of single-instruction B/BL concurrent-modification rules per the
Arm ARM before any design work; if only a single-instruction patch is
blessed, the site layout must be designed around that constraint from the
start.

## T8 adversarial review (codex): the record corrected, the walk confirmed, the rewrite redesigned

Full report: `bench-work/t8-codex-scope.md`. Verdicts and what they change:

**DEFECT -- "14.4% of executed host instructions" was the wrong noun.** The
arithmetic reproduces (call-symbol 7.9359% + call_indirect 6.4981% =
14.4340% of `coverage2.csv`'s totals), but the model charges every emitted
inline byte whenever its guest PC executes (`a64_sequences.cc:5551` sampled
delta x `processor.cc:387` executions) -- including instructions an internal
branch skips. It is a static emitted-byte model, not retired instructions.
Every corpus-ranking percentage in this document is that model; the standing
~2x over-prediction discount exists precisely because of this. Correction of
record for the T8 scoping section's headline and f0d2c35d5's commit message.

**DEFECT confirmed -- the call_indirect 6.50% pool was the return check.**
Most `blr` executions take the epilog branch (`a64_emitter.cc:1048`) before
the walk; a three-instruction return-check floor would price ~1.46%, and the
shared epilog itself sits outside sequence accounting entirely.

**NO DEFECT (directional) -- the 94.66% walk share.** Independently
corroborated by a compile-order estimate over the corpus (94.07% of ordinary
symbol calls record caller before callee). Precision caveats of record: the
counters are racy lower bounds; the harness window was actually 40.768s
(timestamp-derived walk rate 144.51M/s, not 147.28M/s); the counter bump's
mov can expand to MOVZ/MOVK pairs so overhead was a lower bound too; and a
normal present rate does not prove the mix undistorted without a paired
uninstrumented leg.

**DEFECT -- rewriting the multiword walk in place is architecturally
unsafe.** Arm DDI 0487 M.c B2.2.5 permits concurrent modification+execution
only for B/BL/NOP-class single instructions; MOV/LDR/ADD/BR/BLR are not in
the set, and a multiword rewrite would need every executor stopped or a full
DC CVAU/DSB/IC IVAU/DSB + per-PE ISB rendezvous (a writer-side ISB does not
broadcast). `pthread_jit_write_protect_np` is per-thread W^X, not a
rendezvous (Apple JIT guide, via sosumi).

**The safe (c) design, if T8 is ever picked back up:** emit every
not-yet-direct call site as `ldr x0, [sp, RET_ADDR]; gate: bl slow_block`,
where the per-site slow block does the existing walk and ends `br x9` (LR
already correct from the gate's bl). Publishing a compiled callee patches
exactly one aligned 32-bit word, `bl slow_block -> bl callee_or_veneer`,
both encodings in the blessed set, target written and cache-maintained
before the patch. Out-of-range targets stay on the slow path; the slow block
stays valid forever. That saves >=7 executed instructions per patched
dispatch (not the 4 the scoping note assumed -- the safe layout replaces at
least nine source instructions with ldr+bl). Cost side: patchpoint metadata,
an atomic aligned `PatchInstruction32` (the existing `PatchCode`
memcpy at `code_cache_base.h:136` is not guaranteed atomic), and a
mutex+acquire-re-read pending-site registry drained after
`A64Function::Setup` to close the missed-wakeup window.

**Re-ranking:** T9 (SetReturnAddress/StoreLR fusion, modeled 1.0395%,
existing fusion machinery) first; measurement repair second; the (c)
prototype third and only for fixed in-range Call sites, with (b) folded in
as its placement half. The possible-return proof (~0.971% modeled) stays
secondary because of the longjmp path at `ppc_emit_control.cc:116`.
