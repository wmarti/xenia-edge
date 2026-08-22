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
#   SETUP_ONLY=1 ./tools/bench/bench_macos.sh      # prepare both refs, then stop
#   BUILD_ONLY=1 ./tools/bench/bench_macos.sh      # build both refs, then stop
#   LOCAL_ONLY=1 ./tools/bench/bench_macos.sh      # forbid network submodule clones
#   XENIA_SEED=~/Documents/xenia-edge ./tools/bench/bench_macos.sh
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
RUNS="${RUNS:-5}"
# A suite whose emitted code this branch does not change. Its measured delta
# is pure measurement error, so it calibrates everything else in the table:
# any result smaller than the control is indistinguishable from noise.
CONTROL="${CONTROL:-instr__gen_vand}"
REUSE="${REUSE:-0}"
SETUP_ONLY="${SETUP_ONLY:-0}"
BUILD_ONLY="${BUILD_ONLY:-0}"
LOCAL_ONLY="${LOCAL_ONLY:-0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${WORK:-/tmp/xenia-bench}"
CORPUS="$WORK/ppcbin"
XENIA_SEED="${XENIA_SEED-${HOME}/Documents/xenia-edge}"
ROOT_COMMON_GIT="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
BENCH_CACHE="${BENCH_CACHE:-$ROOT_COMMON_GIT/bench-cache}"
TOOL_ROOT="$BENCH_CACHE/tool-root"
SLANG_CACHE="$TOOL_ROOT/.slang"
MSC_CACHE="$TOOL_ROOT/.metal-shader-converter"
DATA_CACHE="$BENCH_CACHE/data_repos"
SEED_COMMON_GIT=""

if [ -n "$XENIA_SEED" ] && git -C "$XENIA_SEED" rev-parse --git-dir >/dev/null 2>&1; then
  SEED_COMMON_GIT="$(git -C "$XENIA_SEED" rev-parse \
    --path-format=absolute --git-common-dir)"
fi

# The corpus ships prebuilt because assembling the 573 .s sources needs a
# PowerPC assembler, which macOS does not have.
ARCHIVE="$ROOT/tools/bench/ppcbin.tar.zst"

: "${SUITES:=$CONTROL instr__gen_vsel instr__gen_vaddfp instr__gen_vnmsubfp \
instr__gen_vmaddfp instr__gen_vperm instr__gen_fmadds instr__gen_vavgsb \
instr__gen_vavgsw instr__gen_vpkuhus instr__gen_fadd instr__gen_fmuls \
instr_mcrf}"

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "this script is for macOS"
[ -f "$ARCHIVE" ] || die "missing $ARCHIVE (are you on the bench-macos-arm64 branch?)"
command -v zstd >/dev/null || tar --help 2>&1 | grep -q zstd || die "need zstd or a tar with --zstd"

# CMake configures the whole project even when only one target is requested,
# so the graphics-side prerequisites are needed even for this CPU-only test
# binary. Checked up front because otherwise the failure surfaces deep inside
# a wall of CMake output ("spirv_to_dxil needs meson>=1.4 and ninja on PATH").
missing=""
command -v meson >/dev/null || missing="$missing meson"
command -v ninja >/dev/null || missing="$missing ninja"
command -v cmake >/dev/null || missing="$missing cmake"
python3 -c 'import mako, yaml, packaging' 2>/dev/null \
  || missing="$missing python-mako/pyyaml/packaging"
command -v xcrun >/dev/null && xcrun --find metal >/dev/null 2>&1 \
  || missing="$missing MetalToolchain"
if [ -n "$missing" ]; then
  cat >&2 <<MSG
error: missing build prerequisites:$missing

  brew install meson ninja cmake
  python3 -m pip install --user mako pyyaml packaging

Also make sure the Metal toolchain is present (once per machine):
  sudo xcodebuild -downloadComponent MetalToolchain
MSG
  exit 1
fi

mkdir -p "$WORK"
if [ ! -d "$CORPUS" ]; then
  echo "==> unpacking corpus"
  tar -x --zstd -f "$ARCHIVE" -C "$WORK"
  echo "    $(ls "$CORPUS"/*.map | wc -l | tr -d ' ') suites"
fi

resolve_ref() {
  local ref="$1"
  local resolved=""
  if resolved="$(git -C "$ROOT" rev-parse --verify "$ref^{commit}" 2>/dev/null)"; then
    printf '%s\n' "$resolved"
    return
  fi
  if resolved="$(git -C "$ROOT" rev-parse --verify "origin/$ref^{commit}" 2>/dev/null)"; then
    echo "    resolved $ref as origin/$ref" >&2
    printf '%s\n' "$resolved"
    return
  fi
  die "cannot resolve ref $ref (tried $ref and origin/$ref)"
}

safe_ref_name() {
  local ref="$1"
  local safe=""
  safe="$(printf '%s' "$ref" | LC_ALL=C tr -c 'A-Za-z0-9._-' '-')"
  [ -n "$safe" ] || safe="ref"
  printf '%s\n' "$safe"
}

# Finds a local submodule Git directory that contains the exact gitlink commit.
# The durable ~/Documents/xenia-edge stores are preferred; the benchmark
# clone's stores are also searched so an interrupted shallow clone can be
# resumed without downloading the same objects again.
find_seed_git() {
  local oid="$1"
  local suffix="$2"
  local common=""
  local candidate=""
  for common in "$SEED_COMMON_GIT" "$ROOT_COMMON_GIT"; do
    [ -n "$common" ] || continue
    for candidate in \
      "$common/modules/$suffix" \
      "$common"/worktrees/*/modules/"$suffix"; do
      [ -d "$candidate" ] || continue
      if git --git-dir="$candidate" cat-file -e "$oid^{commit}" 2>/dev/null; then
        printf '%s\n' "$candidate"
        return
      fi
    done
  done
  return 1
}

add_alternate() {       # add_alternate <module-worktree> <seed-git-dir>
  local module="$1"
  local seed="$2"
  local module_objects=""
  local seed_objects=""
  local alternates=""
  module_objects="$(git -C "$module" rev-parse \
    --path-format=absolute --git-path objects)"
  seed_objects="$(git --git-dir="$seed" rev-parse \
    --path-format=absolute --git-path objects)"
  alternates="$module_objects/info/alternates"
  mkdir -p "$module_objects/info"
  if [ ! -f "$alternates" ] \
      || ! grep -Fqx "$seed_objects" "$alternates" 2>/dev/null; then
    printf '%s\n' "$seed_objects" >> "$alternates"
  fi
}

clone_shared_submodule() { # clone_shared_submodule <repo> <name> <path> <oid> <seed>
  local repo="$1"
  local name="$2"
  local path="$3"
  local oid="$4"
  local seed="$5"
  local module="$repo/$path"
  local upstream=""

  if [ -d "$module" ] && [ -n "$(ls -A "$module" 2>/dev/null)" ]; then
    die "$module is non-empty but is not an initialized submodule"
  fi
  git -C "$repo" submodule init -- "$path"
  upstream="$(git -C "$repo" config -f .gitmodules \
    --get "submodule.$name.url")"
  git init --quiet "$module"
  add_alternate "$module" "$seed"
  git -C "$module" remote add origin "$upstream"
  git -C "$module" checkout --quiet --force --detach "$oid"
  # Keep submodule metadata out of the source directory, matching a normal
  # `git submodule update` layout while retaining the shared-object alternate.
  git -C "$repo" submodule absorbgitdirs -- "$path"
}

sync_submodule() {      # sync_submodule <repo> <name> <path> <oid> <seed-suffix>
  local repo="$1"
  local name="$2"
  local path="$3"
  local oid="$4"
  local suffix="$5"
  local module="$repo/$path"
  local seed=""
  seed="$(find_seed_git "$oid" "$suffix" || true)"

  if [ -e "$module/.git" ]; then
    if ! git -C "$module" cat-file -e "$oid^{commit}" 2>/dev/null; then
      if [ -n "$seed" ]; then
        echo "    local object: $path"
        # Borrow the durable store rather than fetching from it: a fetch by
        # object ID can transfer the seed's entire reachable history.
        add_alternate "$module" "$seed"
      elif [ "$LOCAL_ONLY" = "1" ]; then
        die "no local object store contains $path at $oid"
      else
        echo "    shallow fetch: $path"
        git -C "$repo" submodule update --init --force --checkout \
          --depth=1 -- "$path"
      fi
    fi
    git -C "$module" checkout --quiet --force --detach "$oid"
  elif [ -n "$seed" ]; then
    echo "    local clone: $path"
    # Initialize an objectless repository whose alternate points at the
    # durable seed. It doesn't enumerate or duplicate the seed's history.
    clone_shared_submodule "$repo" "$name" "$path" "$oid" "$seed"
  elif [ "$LOCAL_ONLY" = "1" ]; then
    die "no local object store contains $path at $oid"
  else
    echo "    shallow clone: $path"
    git -C "$repo" submodule update --init --force --checkout \
      --depth=1 -- "$path"
  fi

  [ "$(git -C "$module" rev-parse HEAD)" = "$oid" ] \
    || die "$path did not checkout the required commit $oid"
}

setup_submodules() {    # setup_submodules <repo> <seed-prefix> [excluded-path]
  local repo="$1"
  local prefix="$2"
  local excluded="${3:-}"
  local key=""
  local name=""
  local path=""
  local oid=""

  while IFS= read -r key; do
    name="${key#submodule.}"
    name="${name%.path}"
    path="$(git -C "$repo" config -f .gitmodules --get "$key")"
    [ "$path" = "$excluded" ] && continue
    oid="$(git -C "$repo" rev-parse "HEAD:$path")"
    sync_submodule "$repo" "$name" "$path" "$oid" "$prefix$path"
  done < <(git -C "$repo" config -f .gitmodules --name-only \
    --get-regexp '^submodule\..*\.path$')
}

ensure_shared_link() { # ensure_shared_link <link> <directory>
  local link="$1"
  local target="$2"
  local actual=""
  mkdir -p "$target"
  if [ -L "$link" ]; then
    actual="$(readlink "$link")"
    [ "$actual" = "$target" ] \
      || die "$link points to $actual instead of $target"
  elif [ -e "$link" ]; then
    die "$link already exists and is not the shared benchmark cache"
  else
    ln -s "$target" "$link"
  fi
}

ensure_tool_excludes() {
  local wt="$1"
  local exclude=""
  local pattern=""
  exclude="$(git -C "$wt" rev-parse \
    --path-format=absolute --git-path info/exclude)"
  mkdir -p "$(dirname "$exclude")"
  for pattern in '/.slang' '/.metal-shader-converter'; do
    if [ ! -f "$exclude" ] \
        || ! grep -Fqx "$pattern" "$exclude" 2>/dev/null; then
      printf '%s\n' "$pattern" >> "$exclude"
    fi
  done
}

prepare_tool_cache() {
  local wt="$1"
  mkdir -p "$TOOL_ROOT"
  # Run from the cache root while using the ref's pinned downloader. The
  # downloader may replace its dot-directory on a version change, which is
  # unsafe to do through a symlink.
  (
    cd "$TOOL_ROOT"
    "$wt/xenia-build.py" slang
    "$wt/xenia-build.py" msc
  )
  ensure_shared_link "$wt/.slang" "$SLANG_CACHE"
  ensure_shared_link "$wt/.metal-shader-converter" "$MSC_CACHE"
  ensure_tool_excludes "$wt"
}

prepare_data_cache() {
  local wt="$1"
  local link="$wt/build/data_repos"
  local marker="$DATA_CACHE/.bench-complete"
  local actual=""
  mkdir -p "$wt/build" "$BENCH_CACHE"

  if [ ! -f "$marker" ]; then
    [ ! -e "$DATA_CACHE" ] \
      || die "$DATA_CACHE is incomplete; inspect it before retrying"
    echo "    fetching shared data repositories once"
    (
      cd "$wt"
      ./xenia-build.py fetchdata
    )
    mv "$link" "$DATA_CACHE"
    : > "$marker"
  fi

  if [ -L "$link" ]; then
    actual="$(readlink "$link")"
    [ "$actual" = "$DATA_CACHE" ] \
      || die "$link points to $actual instead of $DATA_CACHE"
  elif [ -e "$link" ]; then
    die "$link already exists and is not the shared benchmark cache"
  else
    ln -s "$DATA_CACHE" "$link"
  fi
}

PREPARED_WT=""
PREPARED_LABEL=""
prepare_ref() {
  local ref="$1"
  local resolved=""
  local label=""
  local wt=""
  local actual=""
  resolved="$(resolve_ref "$ref")"
  label="$(safe_ref_name "$ref")"
  wt="$WORK/wt-$label"

  if [ -d "$wt" ]; then
    actual="$(git -C "$wt" rev-parse HEAD 2>/dev/null || true)"
    [ "$actual" = "$resolved" ] \
      || die "$wt is at ${actual:-an unknown commit}, expected $resolved"
  else
    git -C "$ROOT" worktree add -f --detach "$wt" "$resolved"
  fi

  echo "    worktree: $wt"
  echo "    initializing top-level submodules"
  setup_submodules "$wt" "" "third_party/DirectXShaderCompiler"
  echo "    initializing wxWidgets submodules"
  setup_submodules "$wt/third_party/wxWidgets" \
    "third_party/wxWidgets/modules/"
  echo "    preparing shared build-time tools"
  prepare_tool_cache "$wt"
  echo "    preparing shared data repositories"
  prepare_data_cache "$wt"

  PREPARED_WT="$wt"
  PREPARED_LABEL="$label"
}

BUILT_EXE=""
build_ref() {
  local ref="$1"
  local wt=""
  local label=""
  local exe=""
  prepare_ref "$ref"
  wt="$PREPARED_WT"
  label="$PREPARED_LABEL"

  if [ "$REUSE" = "1" ]; then
    exe="$(find "$wt/build" -name xenia-cpu-ppc-tests -type f \
      -perm +111 2>/dev/null | head -1 || true)"
    if [ -n "$exe" ]; then
      BUILT_EXE="$exe"
      return
    fi
  fi

  (
    cd "$wt"
    # xenia-cpu-ppc-tests is gated behind XENIA_BUILD_TESTS and is not part of
    # the normal app build; its CMake target already links the a64 backend on
    # AArch64.
    if ! ./xenia-build.py build --config=Release --build-tests \
      --target=xenia-cpu-ppc-tests >"$WORK/build-$label.log" 2>&1; then
      tail -30 "$WORK/build-$label.log"
      die "build failed for $ref (see $WORK/build-$label.log)"
    fi
  )
  exe="$(find "$wt/build" -name xenia-cpu-ppc-tests -type f \
    -perm +111 2>/dev/null | head -1 || true)"
  [ -n "$exe" ] || die "built $ref but found no binary"
  BUILT_EXE="$exe"
}

time_suite() {          # time_suite <exe> <suite> -> echoes elapsed seconds
  local exe="$1"
  local suite="$2"
  # Timed in python rather than /usr/bin/time: the BSD time on macOS writes to
  # stderr with no -o option, so silencing the program's own stderr would
  # swallow the measurement with it.
  python3 - "$exe" "$CORPUS/" "$suite" <<'PY'
import subprocess, sys, time
exe, corpus, suite = sys.argv[1:4]
t0 = time.perf_counter()
subprocess.run([exe, "--test_bin_path=" + corpus, suite],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
               check=True)
print("%.4f" % (time.perf_counter() - t0))
PY
}

bench_pair() {          # alternates A/B order and writes a.csv plus b.csv
  local s=""
  local run=""
  local t_a=""
  local t_b=""
  local best_a=""
  local best_b=""
  local worst_a=""
  local worst_b=""
  : > "$WORK/a.csv"
  : > "$WORK/b.csv"
  echo "a_ref,b_ref,suite,run,first,a_seconds,b_seconds" \
    > "$WORK/samples.csv"
  for s in $SUITES; do
    [ -f "$CORPUS/$s.map" ] || { echo "    skip $s (not in corpus)"; continue; }
    best_a=""
    best_b=""
    worst_a=""
    worst_b=""
    for run in $(seq 1 "$RUNS"); do
      # Flip order each run so thermal/background drift isn't assigned to one
      # ref merely because all of its samples happened first.
      if [ $((run % 2)) -eq 1 ]; then
        t_a="$(time_suite "$EXE_A" "$s")"
        t_b="$(time_suite "$EXE_B" "$s")"
        echo "$REF_A,$REF_B,$s,$run,a,$t_a,$t_b" >> "$WORK/samples.csv"
      else
        t_b="$(time_suite "$EXE_B" "$s")"
        t_a="$(time_suite "$EXE_A" "$s")"
        echo "$REF_A,$REF_B,$s,$run,b,$t_a,$t_b" >> "$WORK/samples.csv"
      fi
      best_a="$(python3 -c "print(min($t_a, ${best_a:-9999}))")"
      best_b="$(python3 -c "print(min($t_b, ${best_b:-9999}))")"
      # The spread between a ref's fastest and slowest sample is the per-suite
      # noise this machine actually produced during this run. A delta smaller
      # than the spread has not been measured, only guessed at.
      worst_a="$(python3 -c "print(max($t_a, ${worst_a:-0}))")"
      worst_b="$(python3 -c "print(max($t_b, ${worst_b:-0}))")"
    done
    echo "$s,$best_a,$worst_a" >> "$WORK/a.csv"
    echo "$s,$best_b,$worst_b" >> "$WORK/b.csv"
    printf '    %-28s %8.3fs %8.3fs  (spread %.0f%%/%.0f%%)\n' "$s" \
      "$best_a" "$best_b" \
      "$(python3 -c "print(100*($worst_a-$best_a)/$best_a if $best_a else 0)")" \
      "$(python3 -c "print(100*($worst_b-$best_b)/$best_b if $best_b else 0)")"
  done
}

if [ "$SETUP_ONLY" = "1" ]; then
  echo "==> preparing $REF_A"
  prepare_ref "$REF_A"
  echo "==> preparing $REF_B"
  prepare_ref "$REF_B"
  echo "==> setup complete; SETUP_ONLY=1 skipped both builds"
  exit 0
fi

echo "==> building $REF_A"
build_ref "$REF_A"
EXE_A="$BUILT_EXE"
echo "    $EXE_A"
echo "==> building $REF_B"
build_ref "$REF_B"
EXE_B="$BUILT_EXE"
echo "    $EXE_B"

if [ "$BUILD_ONLY" = "1" ]; then
  echo "==> builds complete; BUILD_ONLY=1 skipped benchmarking"
  exit 0
fi

echo "==> benchmarking $REF_A vs $REF_B (interleaved, min of $RUNS)"
bench_pair

echo
python3 - "$WORK/a.csv" "$WORK/b.csv" "$REF_A" "$REF_B" <<'PY'
import sys, csv
a_path, b_path, ref_a, ref_b = sys.argv[1:5]
rd = lambda p: {r[0]: float(r[1]) for r in csv.reader(open(p)) if r}
control = "instr__gen_vand"
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
if control in A and control in B:
    noise = abs(100*(B[control]-A[control])/A[control])
    print()
    print(f"noise floor from the control suite ({control}, which this branch")
    print(f"does not change): {noise:.1f}%. Treat any per-suite result smaller")
    print("than that as indistinguishable from measurement error.")
PY
