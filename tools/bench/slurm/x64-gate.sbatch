#!/bin/bash
# Full-corpus correctness gate for the x64 backend, both refs, one node.
#
#   sbatch tools/bench/slurm/x64-gate.sbatch
#
# Expects the worktrees and the unpacked corpus under $W (see x64-build.sbatch)
# and the bench python next to it. Read tools/bench/slurm/README.md first — the
# podman flags below are not decoration.
#SBATCH --job-name=xenia-x64-gate
#SBATCH --partition=epyc-7502
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=03:00:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -uo pipefail

# No systemd user session on compute nodes.
export XDG_RUNTIME_DIR=/tmp/podman-$(id -u); mkdir -p "$XDG_RUNTIME_DIR"
export TMPDIR=/tmp
PM="podman --cgroup-manager=cgroupfs --events-backend=file"

W="${W:-/tmp/x64-$(id -u)}"
TOOLS="${TOOLS:-$HOME/xenia-ci}"
LOGS="${LOGS:-$HOME/xenia-ci/logs}"
IMAGE="${IMAGE:-localhost/xenia-build:noble}"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
O=/tmp/x64gate-$STAMP
mkdir -p "$O" "$LOGS"; chmod 777 "$O"

echo "host=$(hostname) start=$(date -Is)"

cat > "$O/run.sh" <<'INNER'
set -uo pipefail
W=/w
TP=$W/wt-a64-fixes-on-edge/src/xenia/cpu/ppc/testing
EA=$(find $W/wt-edge/build -name xenia-cpu-ppc-tests -type f | head -1)
EB=$(find $W/wt-a64-fixes-on-edge/build -name xenia-cpu-ppc-tests -type f | head -1)
# The binaries link libSDL3 out of their own build trees.
export LD_LIBRARY_PATH="$(dirname "$EA"):$(dirname "$EB"):${LD_LIBRARY_PATH:-}"
grep " /dev/shm " /proc/mounts
# Both refs get the SAME test path, or the baseline is never asked about the
# cases the candidate adds and silently looks clean.
python3 /tools/gate.py \
  --exe-a "$EA" --exe-b "$EB" \
  --ref-a edge --ref-b a64-fixes-on-edge \
  --backend x64 --corpus "$W/ppcbin" --test-path "$TP" \
  --skip-file "$TP/skip.txt" --out-dir /out
echo "GATE_RC=$?"
INNER
chmod 777 "$O/run.sh"

# --shm-size only changes the size; /dev/shm stays noexec and the JIT maps its
# code cache PROT_EXEC out of it. Mounting the tmpfs exec is the actual fix.
$PM run --rm --tmpfs /dev/shm:rw,exec,nosuid,nodev,size=2g \
  -v "$W:/w" -v "$O:/out" -v "$TOOLS:/tools:ro" \
  "$IMAGE" bash /out/run.sh > "$LOGS/x64-gate-$STAMP.log" 2>&1
echo "podman rc=$?"

sed -n '/^== x64/,$p' "$LOGS/x64-gate-$STAMP.log" | head -40
# /tmp is node-local; keep the bundle somewhere reachable afterwards.
mkdir -p "$HOME/xenia-ci/results" && cp -r "$O" "$HOME/xenia-ci/results/"
echo "results=$HOME/xenia-ci/results/$(basename "$O")"
echo "done=$(date -Is)"
