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

## Status

| # | Target | State | Measured |
| --- | --- | --- | --- |
| — | (populated as the campaign runs) | | |

## Log

- 2026-08-25: campaign opened. Live menu profiles of Reach and Halo 3 collecting.
  Static inventory workflow running over the 23 unmerged `origin/a64-fixes-on-edge`
  commits, the CNTVCT_EL0 profiler, `bc2f386ff` (signal-wake WaitMultiple), the
  instrument inventory, and menu architecture.
