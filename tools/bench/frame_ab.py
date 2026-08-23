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


def run_once(app, work, warmup_s, window_s, timeout, extra=()):
    """(fps, cpu_percent) over `window_frames` after warmup, or (None, None).

    CPU percent is the metric the db16cyc core-release work moved: it releases
    the core from a guest spin loop, so the frame rate is meant to stay put
    while the machine stops burning cores on a wait. Reporting only fps would
    score that change as no change.
    """
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "storage", "content"), exist_ok=True)
    subprocess.run(["cp", "-R", CONTENT,
                    os.path.join(work, "storage", "content")],
                   stderr=subprocess.DEVNULL)
    log = os.path.join(work, "run.log")
    out = open(os.path.join(work, "stdout.log"), "wb")
    # No --apu=nop: it fails CreateDriver and the guest dies at boot.
    p = subprocess.Popen(
        [app, f"--storage_root={work}/storage", f"--log_file={log}",
         "--log_level=2", "--hid=nop", "--discord=false", *extra, GAME],
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
    ap.add_argument("--warmup", type=float, default=180.0,
                    help="seconds to run before measuring")
    ap.add_argument("--window", type=float, default=120.0,
                    help="seconds to measure over")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--work", default="/private/tmp/xenia-bench/frameab")
    ap.add_argument("--out", default="")
    # Flags applied to one ref only. With the same binary on both sides this
    # turns the harness into a cvar A/B, which isolates one behaviour without a
    # rebuild and without any chance of an unrelated code difference leaking in.
    # Repeatable rather than nargs="*": the values are themselves flags, and
    # argparse hands a leading "--" to the option parser instead of the list.
    ap.add_argument("--extra-a", action="append", default=[])
    ap.add_argument("--extra-b", action="append", default=[])
    args = ap.parse_args()

    a_fps, b_fps = [], []
    for i in range(args.runs):
        order = [("a", args.app_a, a_fps, args.extra_a),
                 ("b", args.app_b, b_fps, args.extra_b)]
        if i % 2:
            order.reverse()
        for tag, app, acc, extra in order:
            fps, cpu = run_once(app, f"{args.work}-{tag}", args.warmup,
                                args.window, args.timeout, extra)
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
    if args.out:
        json.dump({"ref_a": args.ref_a, "ref_b": args.ref_b,
                   "fps": fps_r, "cpu": cpu_r,
                   "warmup": args.warmup, "window": args.window},
                  open(args.out, "w"), indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
