#!/usr/bin/env bash
# One unattended pass of the Mac lane: fetch, build both refs, gate, and only
# then measure. The x64 counterpart is tools/bench/slurm/x64-pipeline.sbatch.
#
#   ./tools/bench/run_lane.sh                       # edge vs a64-fixes-on-edge
#   ./tools/bench/run_lane.sh REF_A REF_B
#   RUNS=5 ./tools/bench/run_lane.sh
#
# Timing a miscompile is meaningless, so a failed gate stops the pass rather
# than producing numbers nobody should trust. The result bundle records the
# machine conditions alongside the verdict, because a number without them is
# not evidence.
set -uo pipefail

REF_A="${1:-edge}"
REF_B="${2:-a64-fixes-on-edge}"
RUNS="${RUNS:-5}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${WORK:-/tmp/xenia-bench}"
RESULTS="${RESULTS:-$WORK/results}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$RESULTS/lane-$STAMP"
mkdir -p "$OUT"

say() { echo "[$(date -u +%H:%M:%SZ)] $*"; }
say "refs $REF_A..$REF_B on $(hostname)"

# A laptop that is thermally throttled or on battery produces numbers that look
# like regressions. Record it rather than refusing to run; the gate does not
# care and the benchmark can be discarded later.
if [ "$(uname -s)" = "Darwin" ]; then
  pmset -g ps 2>/dev/null | head -1 | grep -q "AC Power" \
    || say "WARNING: on battery — timings will not be comparable"
  pmset -g therm 2>/dev/null | grep -qi "No thermal warning" \
    || say "WARNING: thermal pressure recorded"
fi

git -C "$ROOT" fetch --quiet origin 2>/dev/null || say "fetch failed, using local refs"

say "building (reusing anything already built)"
if ! BUILD_ONLY=1 REUSE=1 "$ROOT/tools/bench/bench_macos.sh" "$REF_A" "$REF_B" \
     >"$OUT/build.log" 2>&1; then
  say "BUILD FAILED — see $OUT/build.log"
  tail -20 "$OUT/build.log"
  exit 1
fi

exe_of() { find "$WORK/wt-$1/build" -name xenia-cpu-ppc-tests -type f \
             -perm +111 2>/dev/null | head -1; }
EXE_A="$(exe_of "$REF_A")"
EXE_B="$(exe_of "$REF_B")"
[ -n "$EXE_A" ] && [ -n "$EXE_B" ] || { say "missing binaries after build"; exit 1; }

# The superset tree, so both refs are asked the same questions. Pointing each
# ref at its own tree hides every case the candidate adds.
TP="$WORK/wt-$REF_B/src/xenia/cpu/ppc/testing"

say "gate"
python3 "$ROOT/tools/bench/gate.py" \
  --exe-a "$EXE_A" --exe-b "$EXE_B" \
  --ref-a "$REF_A@$(git -C "$WORK/wt-$REF_A" rev-parse --short HEAD)" \
  --ref-b "$REF_B@$(git -C "$WORK/wt-$REF_B" rev-parse --short HEAD)" \
  --backend a64 --corpus "$WORK/ppcbin" --test-path "$TP" \
  --skip-file "$TP/skip.txt" --out-dir "$OUT" 2>&1 | tee "$OUT/gate.log" | tail -20
GATE_RC=${PIPESTATUS[0]}

if [ "$GATE_RC" -ne 0 ]; then
  say "GATE FAILED — not benchmarking. Bundle: $OUT"
  exit 1
fi

say "benchmarking (min of $RUNS, interleaved)"
RUNS="$RUNS" REUSE=1 "$ROOT/tools/bench/bench_macos.sh" "$REF_A" "$REF_B" \
  >"$OUT/bench.log" 2>&1
sed -n '/^==> calibrating/,$p' "$OUT/bench.log" | tail -25
cp "$WORK/samples.csv" "$OUT/" 2>/dev/null

say "PASS — bundle: $OUT"
