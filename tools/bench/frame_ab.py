#!/usr/bin/env python3
"""Frame-rate A/B for two emulator builds on the same title.

Everything else measured so far is a proxy: emitted bytes, chain counts,
instruction counts on a corpus that never executes. This is the one number that
is not a proxy, so it is also the one worth the most scepticism - the run has
to be long enough and sampled in a window where the title is doing steady work.

Which metric matters depends on the title. Halo 3 is locked at 30 fps, so its
frame rate cannot show an improvement and every run lands at 29.5-29.8 whatever
the backend does; CPU percent is the figure of merit there, and frame rate only a
guard that nothing got worse. On a title that is not hitting a cap, frame rate is
the primary signal. Check which case you are in before reading the output: the
db16cyc core-release series measured -10.15% CPU at 0.47% fps, and reading the
fps line would have called a resolved 10% win marginal.

Method, following the same rules bench_pair.py settled on:
  - The measurement window starts after the title reaches `--warmup` frames, so
    shader compilation and first-touch JIT are excluded. Those dominate the
    first thousand frames and have nothing to do with steady-state codegen.
  - Frames per second over the window, minimum of N runs is NOT used here:
    a longer window is worth more than more runs, because the noise is
    per-frame and averages out inside one window. Instead every run is
    reported and the spread between runs is printed next to the delta, exactly
    so a delta smaller than the spread can be read as unresolved.
  - Refs alternate, so drift over the session is not charged to one build.

The frame number comes from the log line prefix; the emulator does not stamp
times, so this samples the log at a fixed rate and pairs frame counts with the
sampling clock.
"""
import argparse, json, os, shutil, subprocess, sys, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import macwin
except Exception:  # pyobjc absent: parking and focus checks go quiet
    macwin = None

GAME = ("/Users/admin/Documents/X360-Games/4D5307E6/00007000/"
        "5B98A58CE2D8B103DB49A0B813996BD84D")
CONTENT = "/Users/admin/.local/share/Xenia/content/E03000006470AD12"
FRAME_RE = re.compile(rb"^.> f:(\d+)", re.M)


def current_frame(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            back = min(f.tell(), 65536)
            f.seek(-back, os.SEEK_END)
            tail = f.read()
    except OSError:
        return 0
    hits = FRAME_RE.findall(tail)
    return int(hits[-1]) if hits else 0


def cpu_seconds(pid):
    """Total CPU seconds the process has consumed, or None."""
    try:
        out = subprocess.run(["ps", "-o", "cputime=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    t = out.strip()
    if not t:
        return None
    # [dd-]hh:mm:ss[.ff]
    days = 0
    if "-" in t:
        d, t = t.split("-", 1)
        days = int(d)
    parts = t.split(":")
    try:
        parts = [float(x) for x in parts]
    except ValueError:
        return None
    sec = 0.0
    for x in parts:
        sec = sec * 60 + x
    return sec + days * 86400


def run_once(app, work, warmup_s, window_s, timeout, extra=(), game=None):
    """(fps, cpu_percent) over `window_frames` after warmup, or (None, None).

    CPU percent is the metric the db16cyc core-release work moved: it releases
    the core from a guest spin loop, so the frame rate is meant to stay put
    while the machine stops burning cores on a wait. Reporting only fps would
    score that change as no change.
    """
    # The storage directory is REUSED across runs of the same ref, not wiped.
    # Wiping it throws away the shader cache, so every run recompiles every
    # shader from cold and boot-to-menu costs minutes instead of seconds. That
    # cost is not what is being measured, and paying it eight times turned a
    # ten-minute experiment into three quarters of an hour. Each ref keeps its
    # own directory, so neither side sees the other's cache.
    first = not os.path.isdir(os.path.join(work, "storage"))
    os.makedirs(os.path.join(work, "storage", "content"), exist_ok=True)
    if first:
        subprocess.run(["cp", "-R", CONTENT,
                        os.path.join(work, "storage", "content")],
                       stderr=subprocess.DEVNULL)
    log = os.path.join(work, "run.log")
    out = open(os.path.join(work, "stdout.log"), "wb")
    # No --apu=nop: it fails CreateDriver and the guest dies at boot.
    before = macwin.frontmost() if macwin else ""
    # --hid=nop always. The scripted input driver is constructed ahead of any
    # real backend and regardless of `hid=` (xenia_main.cc CreateInputDrivers),
    # precisely so a benchmark can drive a title with nothing else attached, so
    # --input_script works fine alongside it. An earlier version of this file
    # dropped --hid=nop whenever --input_script was passed, on the theory that
    # nop suppressed the scripted driver. It does not, and attaching SDL as
    # well would put a real controller's state into a benchmark.
    #
    # What `n/a fps` actually means: the run reached a state that stopped
    # logging frames -- a settled menu does that -- OR the input script ran off
    # its end and the pad went neutral. It is a signal about the guest's state,
    # not about the HID backend.
    p = subprocess.Popen(
        [app, f"--storage_root={work}/storage", f"--log_file={log}",
         "--log_level=2", "--hid=nop", "--discord=false", *extra,
         game or GAME],
        cwd=work, stdin=subprocess.DEVNULL, stdout=out,
        stderr=subprocess.STDOUT, start_new_session=True)
    # Wall-clock windows, not frame-count windows. The frame number is only
    # visible through the log line prefix, and a title that has settled at its
    # menu can stop logging entirely for minutes at a time while still running
    # at full speed -- verified by watching its CPU time climb with the log
    # untouched. Waiting for a frame count that only advances when something
    # happens to be logged hangs the run instead of measuring it.
    result = (None, None)
    try:
        start = time.perf_counter()
        while time.perf_counter() - start < warmup_s:
            if p.poll() is not None:
                return result
            time.sleep(1.0)
        if macwin:
            after = macwin.frontmost()
            ws = macwin.windows(pid=p.pid)
            if ws:
                w = ws[0]
                print(f"    window {w['id']} at {w['x']},{w['y']} "
                      f"{w['w']}x{w['h']}"
                      + ("" if after == before
                         else f"  [took focus from {before!r}]"), flush=True)
        f_start = current_frame(log)
        c_start = cpu_seconds(p.pid)
        t_start = time.perf_counter()
        while time.perf_counter() - t_start < window_s:
            if p.poll() is not None:
                return result
            time.sleep(1.0)
        elapsed = time.perf_counter() - t_start
        f_end = current_frame(log)
        c_end = cpu_seconds(p.pid)
        cpu = (100.0 * (c_end - c_start) / elapsed
               if c_start is not None and c_end is not None else None)
        # Frames only if the log actually moved; otherwise say so rather than
        # reporting a rate computed from a counter that stood still.
        fps = (f_end - f_start) / elapsed if f_end > f_start else None
        result = (fps, cpu)
    finally:
        p.kill()
        try:
            p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            os.kill(p.pid, 9)
            p.wait()
        # The window server can hold a dead app's surface briefly; give the
        # next run a clean machine rather than one shared with a survivor.
        time.sleep(3.0)
        out.close()
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app-a", required=True)
    ap.add_argument("--app-b", required=True)
    ap.add_argument("--ref-a", default="A")
    ap.add_argument("--ref-b", default="B")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup", type=float, default=60.0,
                    help="seconds to run before measuring. Halo 3 reaches its "
                         "menu in a measured 49s, cold cache or warm, so this "
                         "is that plus a short settle -- not a guess")
    ap.add_argument("--window", type=float, default=40.0,
                    help="seconds to measure over")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--work", default="/private/tmp/xenia-bench/frameab")
    ap.add_argument("--out", default="")
    # Flags applied to one ref only. With the same binary on both sides this
    # turns the harness into a cvar A/B, which isolates one behaviour without a
    # rebuild and without any chance of an unrelated code difference leaking in.
    # Repeatable rather than nargs="*": the values are themselves flags, and
    # argparse hands a leading "--" to the option parser instead of the list.
    ap.add_argument("--game", default=GAME,
                    help="path to the title to launch; defaults to Halo 3. "
                         "Halo 3 and Halo Reach are both locked at 30 fps, so "
                         "CPU percent is the figure of merit on either")
    # Geometry is seeded into the wxConfig both binaries read, so parking the
    # window cannot become a difference between the refs -- and it is applied
    # before the frame is constructed, which is the only point at which macOS
    # lets a position be chosen without Accessibility.
    ap.add_argument("--park", default="",
                    help="X,Y to park the emulator window at, so a run does "
                         "not sit on top of whatever else is on screen. "
                         "'auto' tucks it into the bottom-right corner")
    ap.add_argument("--extra-a", action="append", default=[])
    ap.add_argument("--extra-b", action="append", default=[])
    args = ap.parse_args()

    if args.park and macwin:
        if args.park == "auto":
            dx, dy, dw, dh = macwin.displays()[0]
            # Leave a corner on screen: a window with no pixels on any display
            # risks being throttled, and then CPU percent measures a stalled
            # emulator rather than a running one.
            px, py = dx + dw - 220, dy + dh - 180
        else:
            px, py = (int(v) for v in args.park.split(","))
        macwin.park(px, py)
        print(f"window parked at {px},{py} (both refs)", flush=True)
    elif args.park:
        print("--park ignored: pyobjc not importable", flush=True)

    a_fps, b_fps = [], []
    for i in range(args.runs):
        order = [("a", args.app_a, a_fps, args.extra_a),
                 ("b", args.app_b, b_fps, args.extra_b)]
        if i % 2:
            order.reverse()
        for tag, app, acc, extra in order:
            fps, cpu = run_once(app, f"{args.work}-{tag}", args.warmup,
                                args.window, args.timeout, extra,
                                args.game)
            name = args.ref_a if tag == "a" else args.ref_b
            if cpu is None:
                print(f"run {i+1} {name:28} did not reach the window",
                      flush=True)
            else:
                shown = f"{fps:6.2f} fps" if fps is not None else "  n/a fps"
                print(f"run {i+1} {name:28} {shown}  {cpu:6.1f}% CPU",
                      flush=True)
                acc.append((fps, cpu))
    if not a_fps or not b_fps:
        print("\nnot measured: at least one ref never reached the window.")
        return 1
    def paired(label, idx, lower_is_better):
        """Per-iteration differences, which is what interleaving is for.

        The refs alternate so that each iteration measures both of them minutes
        apart, and the difference within an iteration is insensitive to drift
        across the session. Comparing best-vs-best against a ref's own spread
        throws that away: on this machine CPU percent falls steadily over a
        session, so both refs drift down together, the within-ref spread picks
        up the drift rather than the noise, and a change that wins every single
        pair gets reported as unresolved.
        """
        pairs = [(a[idx], b[idx]) for a, b in zip(a_fps, b_fps)
                 if a[idx] is not None and b[idx] is not None]
        if not pairs:
            return None
        deltas = [100.0 * (bb - aa) / aa for aa, bb in pairs]
        mean = sum(deltas) / len(deltas)
        wins = sum(1 for d in deltas if (d < 0) == lower_is_better)
        print(f"\n{label} -- paired by iteration")
        for n, ((aa, bb), d) in enumerate(zip(pairs, deltas), 1):
            print(f"  pair {n}: {aa:8.2f} vs {bb:8.2f}   {d:+7.2f}%")
        print(f"  mean {mean:+.2f}%, spread of the differences "
              f"{max(deltas) - min(deltas):.2f} points, "
              f"{wins}/{len(deltas)} pairs favour {args.ref_b}")
        agree = wins == len(deltas) or wins == 0
        consistent = wins == len(deltas) and len(deltas) >= 3
        if consistent:
            print(f"  RESOLVED: every pair agrees in sign.")
        elif not agree:
            print(f"  NOT RESOLVED: the pairs disagree; this is not a result.")
        else:
            # Saying "they disagree" when they unanimously agree but are merely
            # few is the same fault as a gate that passes on nothing: a true
            # statement about the verdict, a false one about the data.
            print(f"  NOT RESOLVED: only {len(deltas)} pair(s); every one of "
                  f"them agrees in sign, but three are required.")
        return {"pairs": pairs, "deltas": [round(d, 3) for d in deltas],
                "mean_delta_pct": round(mean, 3), "agreeing_pairs": wins,
                "resolved": consistent}

    def report(label, idx, best):
        va = [r[idx] for r in a_fps if r[idx] is not None]
        vb = [r[idx] for r in b_fps if r[idx] is not None]
        if not va or not vb:
            print(f"{label}: not measured")
            return None
        ma, mb = best(va), best(vb)
        spread = max((max(va) - min(va)) / abs(best(va)),
                     (max(vb) - min(vb)) / abs(best(vb))) * 100
        delta = 100.0 * (mb - ma) / ma
        print(f"\n{label}")
        print(f"  {args.ref_a:26} {ma:8.2f}   (n={len(va)}, "
              f"all: {', '.join(f'{v:.2f}' for v in va)})")
        print(f"  {args.ref_b:26} {mb:8.2f}   (n={len(vb)}, "
              f"all: {', '.join(f'{v:.2f}' for v in vb)})")
        print(f"  {'delta':26} {delta:+8.2f}%  run-to-run spread {spread:.2f}%")
        if abs(delta) < spread:
            print("  UNRESOLVED: smaller than one build's own run-to-run "
                  "spread. Not measured, not 'no change'.")
        return {"a": va, "b": vb, "best_a": ma, "best_b": mb,
                "delta_pct": round(delta, 3), "spread_pct": round(spread, 3),
                "resolved": abs(delta) >= spread}

    # fps: higher is better, so the best run is the max. CPU: lower is better,
    # and the figure of merit for a core-release change is CPU at equal fps.
    fps_r = report("frames per second (higher is better)", 0, max)
    cpu_r = report("CPU percent (lower is better)", 1, min)
    fps_p = paired("frames per second", 0, False)
    cpu_p = paired("CPU percent", 1, True)
    if args.out:
        json.dump({"ref_a": args.ref_a, "ref_b": args.ref_b,
                   "fps": fps_r, "cpu": cpu_r,
                   "fps_paired": fps_p, "cpu_paired": cpu_p,
                   "warmup": args.warmup, "window": args.window},
                  open(args.out, "w"), indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
