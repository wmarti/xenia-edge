#!/bin/bash
# Instruction-count comparison for the x64 backend.
#
#   sbatch tools/bench/slurm/x64-callgrind.sbatch
#
# Counts rather than times, because perf_event_paranoid is 4 on every node and
# the hardware is shared. Read tools/bench/slurm/README.md for the podman
# flags. Expect this to be slow: callgrind costs roughly 50x, so a suite that
# runs in three seconds natively takes a couple of minutes here.
#SBATCH --job-name=xenia-x64-callgrind
#SBATCH --partition=epyc-7502
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=08:00:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -uo pipefail

export XDG_RUNTIME_DIR=/tmp/podman-$(id -u); mkdir -p "$XDG_RUNTIME_DIR"
export TMPDIR=/tmp
PM="podman --cgroup-manager=cgroupfs --events-backend=file"

W="${W:-/tmp/x64-$(id -u)}"
TOOLS="${TOOLS:-$HOME/xenia-ci}"
LOGS="${LOGS:-$HOME/xenia-ci/logs}"
IMAGE="${IMAGE:-localhost/xenia-build:noble}"
# instr__gen_vand is the control; the rest are the suites the [x64] commits
# actually touch, plus a few scalar float ones for contrast.
SUITES="${SUITES:-instr__gen_vand instr__gen_vavgsb instr__gen_vavgsw \
instr__gen_vpkuhus instr__gen_fadd instr__gen_fmuls instr__gen_fmadds \
instr_mcrf}"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
O=/tmp/x64cg-$STAMP
mkdir -p "$O" "$LOGS"; chmod 777 "$O"

echo "host=$(hostname) start=$(date -Is)"

cat > "$O/run.sh" <<INNER
set -uo pipefail
W=/w
EA=\$(find \$W/wt-edge/build -name xenia-cpu-ppc-tests -type f | head -1)
EB=\$(find \$W/wt-a64-fixes-on-edge/build -name xenia-cpu-ppc-tests -type f | head -1)
export LD_LIBRARY_PATH="\$(dirname "\$EA"):\$(dirname "\$EB"):\${LD_LIBRARY_PATH:-}"
# -u so per-suite lines reach the log as they happen. Without it python block
# buffers into a redirect and an eight-hour job shows nothing until it ends.
python3 -u /tools/callgrind_bench.py \\
  --exe-a "\$EA" --exe-b "\$EB" --ref-a edge --ref-b a64-fixes \\
  --corpus "\$W/ppcbin" --out /out/callgrind.json \\
  --suites $SUITES
echo "CG_RC=\$?"
INNER
chmod 777 "$O/run.sh"

$PM run --rm --tmpfs /dev/shm:rw,exec,nosuid,nodev,size=2g \
  -v "$W:/w" -v "$O:/out" -v "$TOOLS:/tools:ro" \
  "$IMAGE" bash /out/run.sh > "$LOGS/x64-cg-$STAMP.log" 2>&1
echo "podman rc=$?"
grep -vE '^time=' "$LOGS/x64-cg-$STAMP.log" | tail -25
mkdir -p "$HOME/xenia-ci/results" && cp -r "$O" "$HOME/xenia-ci/results/"
echo "results=$HOME/xenia-ci/results/$(basename "$O")"
echo "done=$(date -Is)"
