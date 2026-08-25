#!/usr/bin/env python3
"""Paired A/B over a scripted guest state, on a storage root that actually
has the title's content.

Why this exists alongside frame_ab.py: frame_ab creates a fresh per-ref
storage directory and seeds it from a hardcoded Halo 3 content path. That is
fine for the Halo titles and wrong for anything else -- GTA IV booted into a
different state entirely and measured 107% CPU where the docks state settles
at 400-416%, with no frames logged at all. Rather than special-case that, this
takes the storage root as an argument and leaves it alone.

Differences from frame_ab.py that matter:
  - storage root is given, not invented, and is shared by both refs. Both arms
    are the same title, so a warm shader cache helps both equally; a cold one
    would just add minutes per leg.
  - the measurement window is wall-clock and CPU is the process's own CPU time
    over it, the same figure frame_ab reports, so numbers are comparable.
  - frames are read from the log's `f:` prefix. That is a SWAP PACKET count,
    not a present count -- Apple's Metal HUD reads ~20% lower. It is reported
    only as a work-equality guard between two arms that share the counter, and
    is never to be quoted as fps.
  - legs are interleaved A,B,B,A,A,B so a machine that warms or throttles
    across the run does not favour one ref.
"""
import argparse, os, subprocess, sys, time, json, re

FRAME_RE = re.compile(rb"^.> f:(\d+)", re.M)


def frames(path):
    """Present count and its timestamp, from --present_count_file.

    NOT from the log. The log's `f:` prefix carries the same counter, but the
    log is written by a batching writer thread, so its freshness depends on
    logging timing -- which made it useless for judging a change TO logging: it
    read ~9% low purely because the writer had been made to sleep when idle.
    This file is rewritten directly by the counter's own increment.

    Returns (count, monotonic_ns) or (0, 0).
    """
    try:
        with open(path, "r") as f:
            parts = f.read().split()
        if len(parts) >= 2:
            return int(parts[0]), int(parts[1])
    except (OSError, ValueError):
        pass
    return 0, 0


def cpu_seconds(pid):
    out = subprocess.run(["ps", "-o", "cputime=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        return None
    parts = out.replace("-", ":").split(":")
    try:
        parts = [float(p) for p in parts]
    except ValueError:
        return None
    total = 0.0
    for p in parts:
        total = total * 60 + p
    return total


MIN_FREE_GB = 2.0


def free_gb(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / (1024 ** 3)


def run_once(app, game, script, storage, work, warmup, window, extra):
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, "run.log")
    counter = os.path.join(work, "presents.txt")
    for p in (log, counter, os.path.join(work, "stdout.log")):
        try:
            os.unlink(p)
        except OSError:
            pass
    out = open(os.path.join(work, "stdout.log"), "wb")
    env = dict(os.environ)
    env["MTL_HUD_ENABLED"] = "1"
    cmd = [app, f"--storage_root={storage}", f"--log_file={log}",
           "--log_level=2", "--hid=nop", "--discord=false",
           f"--present_count_file={counter}",
           f"--input_script={script}", *extra, game]
    p = subprocess.Popen(cmd, cwd=work, stdin=subprocess.DEVNULL, stdout=out,
                         stderr=subprocess.STDOUT, start_new_session=True)
    try:
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < warmup:
            if p.poll() is not None:
                return None, None, "exited during warmup"
            time.sleep(1.0)
        (f0, n0), c0 = frames(counter), cpu_seconds(p.pid)
        t1 = time.perf_counter()
        while time.perf_counter() - t1 < window:
            if p.poll() is not None:
                return None, None, "exited during window"
            time.sleep(1.0)
        (f1, n1), c1 = frames(counter), cpu_seconds(p.pid)
        elapsed = time.perf_counter() - t1
        if c0 is None or c1 is None:
            return None, None, "no cpu time"
        cpu = 100.0 * (c1 - c0) / elapsed
        # Rate over the counter's OWN timestamps, so a late sample does not
        # look like a slow one.
        if f1 > f0 and n1 > n0:
            swaps = (f1 - f0) / ((n1 - n0) / 1e9)
        else:
            swaps = 0.0
        return cpu, swaps, None
    finally:
        try:
            os.killpg(os.getpgid(p.pid), 9)
        except OSError:
            p.kill()
        out.close()
        time.sleep(4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app-a", required=True)
    ap.add_argument("--app-b", required=True)
    ap.add_argument("--ref-a", default="A")
    ap.add_argument("--ref-b", default="B")
    ap.add_argument("--game", required=True)
    ap.add_argument("--script", required=True)
    ap.add_argument("--storage", required=True)
    ap.add_argument("--work", default="/private/tmp/xenia-bench/stateab")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup", type=float, default=100)
    ap.add_argument("--window", type=float, default=60)
    ap.add_argument("--extra-a", action="append", default=[])
    ap.add_argument("--extra-b", action="append", default=[])
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    # Absolute paths, checked. run_once sets cwd to the work directory, so a
    # relative --script or --game silently resolves against that instead and
    # the title boots into a different state -- GTA IV lands on "Please
    # reconnect controller" and measures 111% CPU where the docks state is
    # 409%. That produced a clean-looking, entirely wrong A/B once already.
    a.script = os.path.abspath(a.script)
    a.game = os.path.abspath(a.game)
    a.storage = os.path.abspath(a.storage)
    a.app_a = os.path.abspath(a.app_a)
    a.app_b = os.path.abspath(a.app_b)
    for label, path in (("script", a.script), ("game", a.game),
                        ("storage", a.storage), ("app-a", a.app_a),
                        ("app-b", a.app_b)):
        if not os.path.exists(path):
            sys.exit(f"error: {label} does not exist: {path}")

    # Refuse to measure on a nearly full disk. A run that cannot write its log
    # produces a profile that looks like a hung guest -- two threads pinned on
    # one PC, everything else parked -- and nothing says why. That cost two
    # discarded profiles and a wrong "the emulator is wedged" conclusion.
    if free_gb(a.storage) < MIN_FREE_GB:
        sys.exit(f"error: only {free_gb(a.storage):.1f} GB free on the storage "
                 f"volume; free space before measuring (shader caches and logs "
                 f"grow during a run)")

    refs = [(a.ref_a, a.app_a, a.extra_a), (a.ref_b, a.app_b, a.extra_b)]
    # A,B,B,A,A,B -- interleaved so drift across the run does not favour a ref.
    order = []
    for i in range(a.runs):
        order += [0, 1] if i % 2 == 0 else [1, 0]

    res = {refs[0][0]: [], refs[1][0]: []}
    aborted = None
    for n, idx in enumerate(order):
        name, app, extra = refs[idx]
        # Checked before every leg, not just at startup. GTA IV rewrites about
        # 2 GB of shader cache, so a run that begins with room can exhaust the
        # volume midway; the legs after that point are degenerate in exactly
        # the way a hung guest is, and averaging them into the earlier ones
        # produces a confident number built on contaminated data. This happened
        # on a guest_scheduler A/B: legs 1-2 ran clean, the volume hit 0, and
        # legs 3-6 were unusable.
        have = free_gb(a.storage)
        if have < MIN_FREE_GB:
            aborted = (n + 1, have)
            print(f"  ABORTED before leg {n+1}: only {have:.1f} GB free "
                  f"(need {MIN_FREE_GB:.1f}); the legs already collected are "
                  f"reported below but this is NOT a complete A/B",
                  flush=True)
            break
        cpu, swaps, err = run_once(app, a.game, a.script, a.storage,
                                   f"{a.work}-{idx}", a.warmup, a.window, extra)
        if err:
            print(f"  leg {n+1} {name}: {err}", flush=True)
            continue
        res[name].append((cpu, swaps))
        print(f"  leg {n+1} {name:22} {cpu:7.2f}% CPU   {swaps:6.2f} swaps/s",
              flush=True)

    print()
    for name in (refs[0][0], refs[1][0]):
        v = res[name]
        if v:
            print(f"{name:22} CPU {sum(c for c, _ in v)/len(v):7.2f}   "
                  f"swaps {sum(s for _, s in v)/len(v):6.2f}   (n={len(v)})")

    va, vb = res[refs[0][0]], res[refs[1][0]]
    n = min(len(va), len(vb))
    if n:
        print("\npaired by iteration (CPU, lower is better)")
        deltas = []
        for i in range(n):
            d = 100.0 * (vb[i][0] - va[i][0]) / va[i][0]
            deltas.append(d)
            print(f"  pair {i+1}: {va[i][0]:7.2f} vs {vb[i][0]:7.2f}   {d:+.2f}%")
        agree = all(d < 0 for d in deltas) or all(d > 0 for d in deltas)
        mean = sum(deltas) / len(deltas)
        print(f"  mean {mean:+.2f}%, {'all pairs agree in sign' if agree else 'PAIRS DISAGREE -- not a result'}")
        print("\npaired swaps/s (work-equality guard, NOT fps)")
        for i in range(n):
            d = 100.0 * (vb[i][1] - va[i][1]) / va[i][1] if va[i][1] else 0.0
            print(f"  pair {i+1}: {va[i][1]:6.2f} vs {vb[i][1]:6.2f}   {d:+.2f}%")
    if aborted:
        leg, have = aborted
        print(f"\n*** INCOMPLETE: aborted before leg {leg} with {have:.1f} GB "
              f"free. Whatever is printed above rests on fewer pairs than were "
              f"asked for, and the volume was filling while they ran. Free "
              f"space and repeat the whole A/B; do not quote this. ***")
    if a.out:
        json.dump({"results": res, "aborted": aborted}, open(a.out, "w"),
                  indent=1)


if __name__ == "__main__":
    main()
