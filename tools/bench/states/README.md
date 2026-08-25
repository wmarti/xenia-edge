# Benchmark states

One file per reproducible guest state, replayed by `--input_script`
(`src/xenia/hid/script`). Every performance claim in
`docs/menu_cpu_campaign.md` names the state it was measured in.

| state | title | script | settles at | figure of merit |
| --- | --- | --- | --- | --- |
| Halo 3 menu | `4D5307E6` | `halo3-menu.script` | ~75 s, 86% CPU | CPU (30 fps cap) |
| Reach menu | Halo Reach GOD | `reach-menu.script` | ~75 s, 153% CPU | CPU (30 fps cap) |
| Reach campaign | Halo Reach GOD | `reach-campaign.script` | ~190 s, 154% CPU | CPU (30 fps cap) |
| GTA IV docks | `GTAIV` GOD | `gtaiv-docks.script` | ~75 s, 400-416% CPU | **fps and CPU** |

## Things that cost time to find out

**GTA IV will not boot to a playable state without a controller.** With
`--hid=nop` and no `--input_script` it stops at "Please reconnect controller"
indefinitely. The scripted driver registers only when a script is supplied
(`Setup()` fails on an empty one), so for this title the script is a hard
prerequisite. `--hid=nop` does NOT suppress the scripted driver -- it is
constructed first and unconditionally in `EmulatorApp::CreateInputDrivers`.

**GTA IV is the only state here that is not frame-capped.** It runs 31-35 fps
GPU-bound at the docks, so fps is a real work-equality guard. Halo 3 and Reach
are locked at 30 and their fps can only ever be a liveness check -- and at a
menu it is not even that, because the attract sequence makes it swing (an
identical-binary run gave 29.83 / 34.60 / 29.80).

**The present counter is sound; an earlier claim here that it read ~20% high
was wrong.** `logging::IncrementFrameNumber()` fires once per PM4_XE_SWAP,
which is once per present. Measured against Apple's Metal HUD at the GTA IV
docks: 40.17 presents/s from the counter, 40.6 on the HUD -- agreement within
1%.

The Reach figures that prompted the original claim (36.2 ours, 29.9 HUD) were
a 90-second average compared against an instantaneous HUD reading, on a menu
whose attract sequence varies its frame rate. That is scene variance, not
counter bias.

Read it through `--present_count_file`, not the log. The log carries the same
counter, but it is written by a batching writer thread, so its freshness
depends on logging timing -- which made it useless for judging a change TO
logging: it read ~9% low purely because the writer had been made to sleep when
idle.

**Warmup is set by the title, the window by the content.** CPU settles by ~75 s
in every state here. The window has to be long enough to average what the scene
does: 90 s at a Reach menu, less where the scene is static.
