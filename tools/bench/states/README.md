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

**The `i> f:` counter in the log is a swap-packet count, not a present count.**
`logging::IncrementFrameNumber()` fires per PM4_XE_SWAP; Apple's Metal HUD
(`MTL_HUD_ENABLED=1`) reads ~20% lower and is the oracle. Reach's menu reads
36.2 by our counter and 29.9 on the HUD. Use our counter only to compare two
arms that share it; never quote it as fps.

**Warmup is set by the title, the window by the content.** CPU settles by ~75 s
in every state here. The window has to be long enough to average what the scene
does: 90 s at a Reach menu, less where the scene is static.
