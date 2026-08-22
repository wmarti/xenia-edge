# Running the x64 side on the ylab cluster

`login-01` is a submission host, not a compute host: eight cores shared with
roughly sixty interactive users. Everything here goes through `sbatch`.

## Start here

    sbatch tools/bench/slurm/x64-pipeline.sbatch

That one job creates everything it needs — checkout, container image, both
worktrees, submodules, builds, corpus — and then runs the gate. It is
idempotent: work already present on the node is reused, anything missing is
rebuilt.

**Nothing on this cluster persists where you would expect.** Podman's graph
root is `/var/user/$UID/containers/storage` and the build trees live in `/tmp`,
both of which are node-local. An hour of setup on `epyc-7502` buys nothing on
`ad-01`, and a cleanup erases it. The pipeline therefore archives the built
image to `~/xenia-ci/image/` on NFS, so a first run on a new node loads it in
seconds instead of spending ten minutes in apt. Only the checkout, the image
archive, logs and results live on NFS; everything else is expected to vanish.

`x64-gate.sbatch` and `x64-callgrind.sbatch` are fast paths for when the builds
are already on the node. All three take their bench tooling from the checkout
rather than from a copied-in directory, so it cannot drift from what is
committed.

The job scripts below encode a handful of things that are not obvious and each
cost a debugging cycle to find. They are in the repository so the next run
starts from the working configuration rather than rediscovering them.

## Why a container

`xenia-cpu-ppc-tests` links `xenia-ui`, and on Linux that pulls GTK3,
fontconfig, xcb, X11 and wxWidgets — for a binary that never opens a window.
The cluster has none of those and no root to install them, so a bare `gcc-13`
build dies at `Package 'gtk+-x11-3.0' ... not found`. Rootless podman with an
`ubuntu:24.04` image reproduces the CI toolchain exactly.

Building the image is a one-time cost; `container/Containerfile` records what
it needs. Beyond the CI apt list it also wants `make` (cmake otherwise falls
back to Unix Makefiles, finds no build program, and reports the misleading
`CMAKE_C_COMPILER not set`), `liblz4-dev`, and unversioned `clang`/`clang++`
alternatives, because `xenia-build.py --cc clang` writes
`CMAKE_C_COMPILER=clang` while apt.llvm.org installs only the `-21` names.

## The four rules

1. **`--tmpfs /dev/shm:rw,exec`.** The JIT allocates its 256 MB code cache with
   `shm_open` and maps it `PROT_EXEC`, twice — an execute view and a write
   view. Podman mounts `/dev/shm` `noexec`, so the mapping fails, and every
   suite segfaults with `Unable to allocate code cache generated code storage`.
   **`--shm-size` does not fix this** — it changes the size, not the mount
   options.
2. **Run the tests inside the image they were built in.** The binary links
   `libSDL3` out of its own build tree; on the bare node it dies at
   `error while loading shared libraries` before printing anything.
3. **Redirect podman's output on the host.** Container stdout does not
   reliably reach the sbatch log, and `--log-driver=none` discards it outright.
   A run that printed nothing looks exactly like a run that found nothing.
4. **`/tmp` is node-local.** Build and time there — `/home` is NFS at 96%
   capacity — but copy anything worth keeping back to `/home` before the job
   exits, or reading it means another `srun` onto the same node.

Rootless podman also needs `--cgroup-manager=cgroupfs --events-backend=file`
and a self-set `XDG_RUNTIME_DIR`, because compute nodes have no systemd user
session. Leave the storage root at its default; overriding `--root` collides
with the existing user namespace.

## Measurement

`perf_event_paranoid` is 4 on every node, so hardware counters are unavailable
and `perf stat` is not an option. `callgrind_bench.py` counts instructions
instead, which is deterministic and therefore indifferent to the fact that this
is shared hardware. Wall-clock timing, if wanted, needs `--exclusive` and
should be treated as confirmation rather than as the primary number.

## Node choice

`epyc-7502`, `am-*` and `a4500-*` are AVX2-only; `ad-01` and `rtx6000-bw` have
full AVX-512. Run the gate on both classes when a change touches vector
codegen — they take different paths through the x64 backend.
