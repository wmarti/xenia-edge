#!/usr/bin/env bash
# Benchmark the PPC JIT natively on Apple Silicon.
#
# Runs xenia's own PPC test corpus through the a64 backend on this machine and
# compares two refs. Everything measured during the audit was indirect (static
# code size, instruction counts under unicorn on x86, x86 cycle timings), so
# this is the first number that reflects real hardware.
#
#   ./tools/bench/bench_macos.sh                  # edge vs a64-fixes-on-edge
#   ./tools/bench/bench_macos.sh REF_A REF_B      # any two refs
#   REUSE=1 ./tools/bench/bench_macos.sh          # skip rebuilds if binaries exist
#   RUNS=5 SUITES="instr__gen_vsel" ./tools/bench/bench_macos.sh
#
# Notes that matter for trustworthy numbers:
#   - The minimum of N runs is reported, not the mean: it rejects scheduler
#     migration and thermal noise, which on a laptop can otherwise invent
#     double-digit differences.
#   - Do not run this in Low Power Mode, and let the machine idle otherwise.
#     Measuring this project on a shared, loaded box once manufactured a 47%
#     regression that repeat measurement erased completely.
set -euo pipefail

REF_A="${1:-edge}"
REF_B="${2:-a64-fixes-on-edge}"
RUNS="${RUNS:-3}"
REUSE="${REUSE:-0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${WORK:-/tmp/xenia-bench}"
CORPUS="$WORK/ppcbin"

# The corpus ships prebuilt because assembling the 573 .s sources needs a
# PowerPC assembler, which macOS does not have.
ARCHIVE="$ROOT/tools/bench/ppcbin.tar.zst"

: "${SUITES:=instr__gen_vsel instr__gen_vaddfp instr__gen_vnmsubfp \
instr__gen_vmaddfp instr__gen_vperm instr__gen_fmadds instr__gen_vavgsb \
instr__gen_vavgsw instr__gen_vpkuhus instr__gen_fadd instr__gen_fmuls \
instr_mcrf}"

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "this script is for macOS"
[ -f "$ARCHIVE" ] || die "missing $ARCHIVE (are you on the bench-macos-arm64 branch?)"
command -v zstd >/dev/null || tar --help 2>&1 | grep -q zstd || die "need zstd or a tar with --zstd"

mkdir -p "$WORK"
if [ ! -d "$CORPUS" ]; then
  echo "==> unpacking corpus"
  tar -x --zstd -f "$ARCHIVE" -C "$WORK"
  echo "    $(ls "$CORPUS"/*.map | wc -l | tr -d ' ') suites"
fi

build_ref() {           # build_ref <ref> -> echoes path to the binary
  # Each name gets its own statement: `local a=1 b=$a` declares both names
  # before assigning either, so $a is unset while b is evaluated and `set -u`
  # aborts.
  local ref="$1"
  local wt="$WORK/wt-$ref"
  local exe=""
  if [ "$REUSE" = "1" ] && [ -d "$wt" ]; then
    exe=$(find "$wt/build" -name xenia-cpu-ppc-tests -type f -perm +111 2>/dev/null | head -1)
    [ -n "$exe" ] && { echo "$exe"; return; }
  fi
  if [ ! -d "$wt" ]; then
    git -C "$ROOT" worktree add -f --detach "$wt" "$ref" >/dev/null 2>&1 \
      || die "cannot create worktree for $ref"
  fi
  (
    cd "$wt"
    # Submodules are per-worktree; only what the CPU tests link is needed.
    git submodule update --init --depth=1 -j"$(sysctl -n hw.ncpu)" \
      $(grep -oE 'path = .+' .gitmodules | sed 's/path = //' \
        | grep -v 'DirectXShaderCompiler') >/dev/null 2>&1 || true
    # wxWidgets vendors pcre and libpng as nested submodules; without this
    # its CMake stops at "wxregex file does not exist".
    git submodule update --init --recursive --depth=1 \
      -j"$(sysctl -n hw.ncpu)" third_party/wxWidgets >/dev/null 2>&1 || true
    # xenia-cpu-ppc-tests is gated behind XENIA_BUILD_TESTS and is not part of
    # the normal app build; its CMake target already links the a64 backend on
    # AArch64.
    ./xenia-build.py build --config=Release --build-tests \
      --target=xenia-cpu-ppc-tests >"$WORK/build-$ref.log" 2>&1 \
      || { tail -30 "$WORK/build-$ref.log"; die "build failed for $ref (see $WORK/build-$ref.log)"; }
  )
  exe=$(find "$wt/build" -name xenia-cpu-ppc-tests -type f -perm +111 2>/dev/null | head -1)
  [ -n "$exe" ] || die "built $ref but found no binary"
  echo "$exe"
}

bench_exe() {           # bench_exe <exe> <label> -> writes $WORK/<label>.csv
  local exe="$1"
  local label="$2"
  local s=""
  local run=""
  local t=""
  local best=""
  : > "$WORK/$label.csv"
  for s in $SUITES; do
    [ -f "$CORPUS/$s.map" ] || { echo "    skip $s (not in corpus)"; continue; }
    best=""
    for run in $(seq 1 "$RUNS"); do
      # Timed in python rather than /usr/bin/time: the BSD time on macOS
      # writes to stderr with no -o option, so silencing the program's own
      # stderr would swallow the measurement with it.
      t=$(python3 - "$exe" "$CORPUS/" "$s" <<'PY'
import subprocess, sys, time
exe, corpus, suite = sys.argv[1:4]
t0 = time.perf_counter()
subprocess.run([exe, "--test_bin_path=" + corpus, suite],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("%.4f" % (time.perf_counter() - t0))
PY
)
      [ -n "$t" ] || t=9999
      best=$(python3 -c "print(min($t, ${best:-9999}))")
    done
    echo "$s,$best" >> "$WORK/$label.csv"
    printf '    %-28s %8.3fs\n' "$s" "$best"
  done
}

echo "==> building $REF_A"
EXE_A=$(build_ref "$REF_A")
echo "    $EXE_A"
echo "==> building $REF_B"
EXE_B=$(build_ref "$REF_B")
echo "    $EXE_B"

echo "==> benchmarking $REF_A (min of $RUNS)"
bench_exe "$EXE_A" a
echo "==> benchmarking $REF_B (min of $RUNS)"
bench_exe "$EXE_B" b

echo
python3 - "$WORK/a.csv" "$WORK/b.csv" "$REF_A" "$REF_B" <<'PY'
import sys, csv
a_path, b_path, ref_a, ref_b = sys.argv[1:5]
rd = lambda p: {r[0]: float(r[1]) for r in csv.reader(open(p)) if r}
A, B = rd(a_path), rd(b_path)
common = [s for s in A if s in B]
w = max([len(s) for s in common] + [16])
print(f"{'suite':{w}} {ref_a:>12} {ref_b:>12} {'delta':>9}")
ta = tb = 0.0
for s in sorted(common):
    ta += A[s]; tb += B[s]
    print(f"{s:{w}} {A[s]:12.3f} {B[s]:12.3f} {100*(B[s]-A[s])/A[s]:+8.1f}%")
if ta:
    print(f"{'TOTAL':{w}} {ta:12.3f} {tb:12.3f} {100*(tb-ta)/ta:+8.1f}%")
PY
