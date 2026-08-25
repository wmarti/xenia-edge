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
| T1 | Reach NETWORK_RECEIVE zero-delay yield trap (~27% of Reach menu CPU) | lever landed, default off (`--zero_delay_spin_limit`) | not yet |
| T2 | `TimerQueue` spin_wait -> blocking_wait on POSIX (10.3% Halo 3 / 4.3% Reach) | landed | A/B running |
| T3 | Halo 3 host GPU submit/encode (53% aggregate, no single hot spot) | blocked on present-rate question | not yet |
| B1 | `PACK_D3DCOLOR` returned 0xFFFFFFFF always | fixed, guard proven to fail on the bug | correctness |

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

## Open defects found, not yet resolved

- **`vector_nan_propagation_test.cc` fails 2 assertions in this tree.** It expects
  `0xFFC00000` for `+inf + (-inf)`; our a64 returns `0x7FC00000`. These are the
  x86 and ARM hardware default QNaNs respectively, so the test encodes SSE's
  behaviour. Codex commit `2837d5436` (not in our tree) concluded ARM's is right
  for PPC. Pre-existing, unrelated to this campaign, and genuinely ambiguous —
  needs the Xenon VMX semantics settled before either side is changed.

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
