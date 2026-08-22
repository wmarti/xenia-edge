#!/bin/bash
# Self-contained x64 gate: source, image, builds and corpus are all created if
# absent, so this runs on any node with nothing prepared by hand.
#
#   sbatch tools/bench/slurm/x64-pipeline.sbatch
#   REF_A=edge REF_B=some-branch sbatch tools/bench/slurm/x64-pipeline.sbatch
#   PARTITION=ad-01 sbatch -p ad-01 -w ad-01 tools/bench/slurm/x64-pipeline.sbatch
#
# Why it has to rebuild rather than assume: podman's graph root on this cluster
# is /var/user/$UID/containers/storage and the build tree lives in /tmp, both
# of which are NODE-LOCAL. An hour of setup on epyc-7502 buys nothing on ad-01,
# and nothing survives a cleanup. Everything expensive is therefore cached on
# node-local disk for speed but reconstructed automatically when missing, and
# the container image is archived to NFS so a new node loads it instead of
# spending ten minutes in apt.
#
# Read tools/bench/slurm/README.md for the podman flags. They are not optional.
#SBATCH --job-name=xenia-x64-pipeline
#SBATCH --nodes=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=64G
#SBATCH --time=08:00:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -uo pipefail

REF_A="${REF_A:-edge}"
REF_B="${REF_B:-a64-fixes-on-edge}"
REPO="${REPO:-https://github.com/wmarti/xenia-edge.git}"
BRANCH="${BRANCH:-bench-macos-arm64}"          # branch carrying the bench tools
HOME_ROOT="${HOME_ROOT:-$HOME/xenia-ci}"        # NFS: durable
SRC="$HOME_ROOT/src"
ARCHIVE="$HOME_ROOT/image/xenia-build-noble.tar"
LOGS="$HOME_ROOT/logs"
RESULTS="$HOME_ROOT/results"
W="${W:-/tmp/x64-$(id -u)}"                     # node-local: fast, ephemeral
IMAGE=xenia-build:noble
STAMP=$(date -u +%Y%m%dT%H%M%SZ)

export XDG_RUNTIME_DIR=/tmp/podman-$(id -u); mkdir -p "$XDG_RUNTIME_DIR"
export TMPDIR=/tmp
PM="podman --cgroup-manager=cgroupfs --events-backend=file"

mkdir -p "$LOGS" "$RESULTS" "$(dirname "$ARCHIVE")" "$W"
say() { echo "[$(date -u +%H:%M:%SZ)] $*"; }
say "host=$(hostname) cpus=$(nproc) refs=$REF_A..$REF_B"

# ---------------------------------------------------------------- source
if [ -d "$SRC/.git" ]; then
  git -C "$SRC" fetch --quiet origin "$BRANCH" "$REF_A" "$REF_B" 2>/dev/null
else
  say "cloning"
  git clone --quiet --branch "$BRANCH" "$REPO" "$SRC" || { say "clone FAILED"; exit 1; }
fi
git -C "$SRC" checkout --quiet --force "origin/$BRANCH" 2>/dev/null \
  || git -C "$SRC" checkout --quiet --force "$BRANCH"
say "source at $(git -C "$SRC" rev-parse --short HEAD)"

# ---------------------------------------------------------------- image
if ! $PM image exists "$IMAGE" 2>/dev/null; then
  if [ -f "$ARCHIVE" ]; then
    say "loading image from NFS archive"
    $PM load -i "$ARCHIVE" >"$LOGS/imgload-$STAMP.log" 2>&1 \
      || say "load failed, will build"
  fi
fi
if ! $PM image exists "$IMAGE" 2>/dev/null; then
  say "building image (ten minutes or so)"
  $PM build -t "$IMAGE" -f "$SRC/tools/bench/slurm/container/Containerfile" \
    "$SRC/tools/bench/slurm/container" >"$LOGS/imgbuild-$STAMP.log" 2>&1 \
    || { say "image build FAILED"; tail -20 "$LOGS/imgbuild-$STAMP.log"; exit 1; }
  say "archiving image to NFS so other nodes skip the apt work"
  $PM save -o "$ARCHIVE.tmp" "$IMAGE" >/dev/null 2>&1 \
    && mv "$ARCHIVE.tmp" "$ARCHIVE"
fi
say "image ready"

# ---------------------------------------------------------------- corpus
if [ ! -d "$W/ppcbin" ]; then
  tar -x --zstd -f "$SRC/tools/bench/ppcbin.tar.zst" -C "$W" \
    || { say "corpus unpack FAILED"; exit 1; }
fi
say "corpus: $(ls "$W"/ppcbin/*.map 2>/dev/null | wc -l) suites"

# ---------------------------------------------------------------- builds
build_ref() {
  # One name per statement. `local a=1 b=$a` declares both names before
  # assigning either, so $a is still unset while b is evaluated and `set -u`
  # aborts the job — which is exactly how this failed the first time.
  local ref="$1"
  local wt="$W/wt-$ref"
  local want=""
  want=$(git -C "$SRC" rev-parse "origin/$ref" 2>/dev/null \
         || git -C "$SRC" rev-parse "$ref")
  if [ -d "$wt" ] && [ "$(git -C "$wt" rev-parse HEAD 2>/dev/null)" = "$want" ] \
     && find "$wt/build" -name xenia-cpu-ppc-tests -type f 2>/dev/null | grep -q .; then
    say "  $ref already built at ${want:0:9}"
    return 0
  fi
  say "  preparing $ref at ${want:0:9}"
  rm -rf "$wt"
  git -C "$SRC" worktree prune
  git -C "$SRC" worktree add -f --detach "$wt" "$want" >/dev/null 2>&1 \
    || { say "  worktree add FAILED for $ref"; return 1; }
  ( cd "$wt" && git submodule update --init --recursive --depth=1 -j 16 ) \
    >"$LOGS/submod-$ref-$STAMP.log" 2>&1 || say "  submodule warnings for $ref"
  cat > "$W/build-$ref.sh" <<INNER
set -x
cd /w/wt-$ref || exit 90
./xenia-build.py slang
./xenia-build.py fetchdata
./xenia-build.py build --cc clang --config=release --build-tests \
  --target=xenia-cpu-ppc-tests
echo "BUILD_RC=\$?"
INNER
  chmod 777 "$W/build-$ref.sh"
  $PM run --rm -v "$W:/w" -w "/w/wt-$ref" "$IMAGE" bash "/w/build-$ref.sh" \
    >"$LOGS/build-$ref-$STAMP.log" 2>&1
  if find "$wt/build" -name xenia-cpu-ppc-tests -type f 2>/dev/null | grep -q .; then
    say "  $ref built"
  else
    say "  $ref BUILD FAILED"
    grep -inE "ld.lld: error|CMake Error|error:" "$LOGS/build-$ref-$STAMP.log" | head -5
    return 1
  fi
}
say "builds:"
build_ref "$REF_A" || exit 1
build_ref "$REF_B" || exit 1

# ---------------------------------------------------------------- gate
O="$W/gate-$STAMP"; mkdir -p "$O"; chmod 777 "$O"
cat > "$O/run.sh" <<INNER
set -uo pipefail
W=/w
TP=\$W/wt-$REF_B/src/xenia/cpu/ppc/testing
EA=\$(find \$W/wt-$REF_A/build -name xenia-cpu-ppc-tests -type f | head -1)
EB=\$(find \$W/wt-$REF_B/build -name xenia-cpu-ppc-tests -type f | head -1)
# The binaries link libSDL3 out of their own build trees.
export LD_LIBRARY_PATH="\$(dirname "\$EA"):\$(dirname "\$EB"):\${LD_LIBRARY_PATH:-}"
grep " /dev/shm " /proc/mounts
# Tooling comes from the checkout, never from a hand-copied file, so it cannot
# drift from what is committed. Both refs get the SAME test path, or the
# baseline is never asked about the cases the candidate adds.
python3 -u /src/tools/bench/gate.py \\
  --exe-a "\$EA" --exe-b "\$EB" --ref-a "$REF_A" --ref-b "$REF_B" \\
  --backend x64 --corpus "\$W/ppcbin" --test-path "\$TP" \\
  --skip-file "\$TP/skip.txt" --out-dir /out
echo "GATE_RC=\$?"
INNER
chmod 777 "$O/run.sh"

say "running gate"
# --shm-size only changes the size; /dev/shm stays noexec and the JIT maps its
# 256 MB code cache PROT_EXEC out of it. Mounting the tmpfs exec is the fix.
$PM run --rm --tmpfs /dev/shm:rw,exec,nosuid,nodev,size=2g \
  -v "$W:/w" -v "$O:/out" -v "$SRC:/src:ro" \
  "$IMAGE" bash /out/run.sh >"$LOGS/gate-$STAMP.log" 2>&1
say "podman rc=$?"
sed -n '/^== x64/,$p' "$LOGS/gate-$STAMP.log" | head -40

# /tmp is node-local; the bundle has to come back to NFS to be readable later.
DEST="$RESULTS/x64-$(hostname)-$STAMP"
cp -r "$O" "$DEST" 2>/dev/null && say "results=$DEST"
GATE=$(python3 -c "import json;print(json.load(open('$DEST/result.json'))['gate'])" 2>/dev/null || echo unknown)
say "GATE=$GATE"
say "done"
