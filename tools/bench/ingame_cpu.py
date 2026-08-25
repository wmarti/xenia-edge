#!/usr/bin/env python3
"""Measure CPU percent of an already-running emulator over a fixed window.

The menu A/B can be automated because the harness never has to touch the UI.
Getting into a campaign cannot: this build offers only [sdl, nop] for --hid,
there is no keyboard backend, and the tree has neither save states nor input
replay. So a person navigates, and this measures.

Usage:
  ingame_cpu.py launch --app <binary> --game <path> --work <dir> --label <name>
  ingame_cpu.py measure --work <dir> [--window 120]

Launch, play until gameplay is actually running, then measure. The window is
wall-clock and samples the process's own CPU time, the same figure frame_ab.py
uses, so the numbers are comparable across the two harnesses.
"""
import argparse, json, os, subprocess, sys, time, shutil

CONTENT = "/Users/admin/.local/share/Xenia/content/E03000006470AD12"


def cpu_seconds(pid):
    try:
        out = subprocess.run(["ps", "-o", "cputime=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    t = out.strip()
    if not t:
        return None
    parts = t.split(":")
    try:
        sec = float(parts[-1])
        if len(parts) >= 2:
            sec += 60 * int(parts[-2])
        if len(parts) >= 3:
            sec += 3600 * int(parts[-3])
        return sec
    except ValueError:
        return None


def launch(args):
    os.makedirs(os.path.join(args.work, "storage", "content"), exist_ok=True)
    if not os.path.isdir(os.path.join(args.work, "storage", "content",
                                      os.path.basename(CONTENT))):
        subprocess.run(["cp", "-R", CONTENT,
                        os.path.join(args.work, "storage", "content")],
                       stderr=subprocess.DEVNULL)
    out = open(os.path.join(args.work, "stdout.log"), "wb")
    # --hid=sdl, not nop: a controller has to reach the guest. No --apu=nop.
    p = subprocess.Popen(
        [args.app, f"--storage_root={args.work}/storage",
         f"--log_file={args.work}/run.log", "--log_level=2",
         "--discord=false", "--hid=sdl", args.game],
        cwd=args.work, stdin=subprocess.DEVNULL, stdout=out,
        stderr=subprocess.STDOUT, start_new_session=True)
    with open(os.path.join(args.work, "pid"), "w") as f:
        f.write(str(p.pid))
    with open(os.path.join(args.work, "label"), "w") as f:
        f.write(args.label)
    print(f"launched {args.label} pid={p.pid}")


def measure(args):
    pid = int(open(os.path.join(args.work, "pid")).read().strip())
    label = open(os.path.join(args.work, "label")).read().strip()
    t0 = time.time()
    c0 = cpu_seconds(pid)
    if c0 is None:
        sys.exit(f"pid {pid} is not running")
    time.sleep(args.window)
    c1 = cpu_seconds(pid)
    t1 = time.time()
    if c1 is None:
        sys.exit(f"pid {pid} exited during the window")
    pct = (c1 - c0) / (t1 - t0) * 100
    rec = {"label": label, "pid": pid, "window_s": round(t1 - t0, 2),
           "cpu_pct": round(pct, 2)}
    print(f"{label}: {pct:.1f}% CPU over {t1 - t0:.0f}s")
    path = os.path.join(args.work, "result.json")
    with open(path, "w") as f:
        json.dump(rec, f, indent=2)
    print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("launch")
    l.add_argument("--app", required=True)
    l.add_argument("--game", required=True)
    l.add_argument("--work", required=True)
    l.add_argument("--label", required=True)
    l.set_defaults(func=launch)
    m = sub.add_parser("measure")
    m.add_argument("--work", required=True)
    m.add_argument("--window", type=float, default=120.0)
    m.set_defaults(func=measure)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
