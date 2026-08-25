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
