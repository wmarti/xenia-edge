#!/usr/bin/env python3
"""Ask a running emulator what is limiting it, without stopping it.

Everything else in tools/bench answers "is B faster than A" after the fact.
That is the right question for a change that already exists and the wrong one
for deciding what to write next, which needs to know where the time is going
right now: whether the frame is waiting on the GPU, on one guest thread, or on
a lock, and which guest function is on top when it is the CPU.

Three sources, none of which need sudo or Accessibility:

  fps          the frame counter the emulator already prints to its log
  GPU          IOAccelerator's PerformanceStatistics, which carries device,
               renderer and tiler utilisation as plain integers
  CPU          /usr/bin/sample, which walks every thread and names it

The last one is what makes this worth having. Guest code is JIT'd and has no
symbols, so a host profiler drops those samples on bare addresses and the
profile stops one level short of the answer. --jit_perf_map writes
`<host_hex> <size_hex> <name>` per translated function, so those addresses can
be turned back into guest functions and the time profile can be checked
against a codegen ranking rather than trusted on its own.

Caveat worth stating plainly: `sample` suspends the target to walk its stacks.
Use `status`/`watch` next to a measurement and `profile` only when nothing is
being timed, or the profiler becomes part of what you measured.
"""
import argparse, os, re, subprocess, sys, time

APP_RE = re.compile(r"Contents/MacOS/Xenia-edge$")
FRAME_RE = re.compile(rb"^.> f:(\d+)", re.M)
# `sample` indents by two spaces per level and puts the sample count first.
NODE_RE = re.compile(r"^(\s*)(\d+)\s+(.*?)\s*$")
THREAD_RE = re.compile(r"^\s*(\d+)\s+Thread_(\w+)(?::\s*(.*?))?\s*$")


def emulators():
    """Running emulator processes, as (pid, argv)."""
    out = subprocess.run(["ps", "-Ao", "pid=,command="], capture_output=True,
                         text=True).stdout
    found = []
    for line in out.splitlines():
        pid, _, cmd = line.strip().partition(" ")
        if "Contents/MacOS/Xenia-edge" in cmd and "--storage_root" in cmd:
            found.append((int(pid), cmd))
    return found


def pick_pid(pid):
    procs = emulators()
    if pid:
        return pid, dict(procs).get(pid, "")
    if not procs:
        sys.exit("no emulator running (looked for Xenia-edge with --storage_root)")
    if len(procs) > 1:
        print(f"note: {len(procs)} emulators running, taking the first",
              file=sys.stderr)
    return procs[0]


def arg_value(cmd, flag):
    m = re.search(re.escape(flag) + r"=(\S+)", cmd)
    return m.group(1) if m else None


def frame_of(log):
    try:
        with open(log, "rb") as f:
            f.seek(0, os.SEEK_END)
            back = min(f.tell(), 65536)
            f.seek(-back, os.SEEK_END)
            hits = FRAME_RE.findall(f.read())
        return int(hits[-1]) if hits else 0
    except (OSError, TypeError):
        return 0


def cpu_seconds(pid):
    out = subprocess.run(["ps", "-o", "cputime=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        return None
    days = 0
    if "-" in out:
        d, out = out.split("-", 1)
        days = int(d)
    sec = 0.0
    for part in out.split(":"):
        sec = sec * 60 + float(part)
    return sec + days * 86400


def gpu():
    """Device/renderer/tiler utilisation, straight out of IOAccelerator.

    There is more than one IOAccelerator node -- taking the first match
    reported a flat 0% while the GPU was plainly working, because the first
    one to appear is not the one doing the drawing. Every block is read and
    the busiest wins.
    """
    out = subprocess.run(["ioreg", "-r", "-d", "1", "-w", "0",
                          "-c", "IOAccelerator"], capture_output=True,
                         text=True).stdout
    best = {}
    for block in re.findall(r'"PerformanceStatistics"\s*=\s*\{(.*?)\}', out,
                            re.S):
        stats = {}
        for key in ("Device Utilization %", "Renderer Utilization %",
                    "Tiler Utilization %", "In use system memory"):
            m = re.search(rf'"{re.escape(key)}"=(\d+)', block)
            if m:
                stats[key] = int(m.group(1))
        if stats.get("Device Utilization %", -1) > \
           best.get("Device Utilization %", -1):
            best = stats
    return best


def cores():
    return int(subprocess.run(["sysctl", "-n", "hw.ncpu"],
                              capture_output=True, text=True).stdout or 1)


def load_perf_map(path):
    """[(start, end, name)] sorted by start, for bisecting a sample address."""
    syms = []
    if not path or not os.path.exists(path):
        return syms
    for line in open(path, errors="replace"):
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        try:
            start, size = int(parts[0], 16), int(parts[1], 16)
        except ValueError:
            continue
        syms.append((start, start + size, parts[2].strip()))
    syms.sort()
    return syms


def guest_symbol(syms, addr):
    lo, hi = 0, len(syms) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        s, e, name = syms[mid]
        if addr < s:
            hi = mid - 1
        elif addr >= e:
            lo = mid + 1
        else:
            return name
    return None


def snapshot(pid, cmd, window):
    log = arg_value(cmd, "--log_file")
    f0, c0, t0 = frame_of(log), cpu_seconds(pid), time.perf_counter()
    time.sleep(window)
    f1, c1, t1 = frame_of(log), cpu_seconds(pid), time.perf_counter()
    el = t1 - t0
    g = gpu()
    return {
        "fps": (f1 - f0) / el if f1 > f0 else None,
        "cpu_pct": 100.0 * (c1 - c0) / el if c0 is not None and c1 is not None
                   else None,
        "gpu_pct": g.get("Device Utilization %"),
        "renderer_pct": g.get("Renderer Utilization %"),
        "tiler_pct": g.get("Tiler Utilization %"),
        "frames": (f0, f1),
    }


def verdict(s, ncores):
    """What is holding the frame back, stated only when the data says so."""
    gpu_pct, cpu_pct, fps = s["gpu_pct"], s["cpu_pct"], s["fps"]
    if gpu_pct is not None and gpu_pct >= 85:
        return f"GPU-bound: device utilisation {gpu_pct}%"
    bits = []
    if gpu_pct is not None:
        bits.append(f"GPU idle enough to rule it out ({gpu_pct}%)")
    if cpu_pct is not None:
        busy = cpu_pct / (ncores * 100.0)
        bits.append(f"CPU {cpu_pct:.0f}% of {ncores} cores ({busy:.0%})")
    if fps is not None and 28.5 <= fps <= 31.0:
        # Reach and Halo 3 both lock to 30. At the cap the limiter question is
        # not "what is slow" but "what is burning cores while capped".
        bits.append(f"sitting on the 30 fps cap ({fps:.1f}), so CPU percent is "
                    f"the figure of merit and frame rate is only a guard")
    elif fps is not None and fps > 31.0:
        # Frame numbers are absolute, so a high rate is real advance, not a
        # logging artefact -- it means the title is not capped here (a load
        # screen, usually), and this is not a steady-state reading.
        bits.append(f"{fps:.1f} fps, above the 30 cap: not steady state, "
                    f"do not read this as a measurement")
    return "; ".join(bits) if bits else "not enough signal"


def do_status(a):
    pid, cmd = pick_pid(a.pid)
    s = snapshot(pid, cmd, a.window)
    print(f"pid {pid}  {os.path.basename(arg_value(cmd, '--storage_root') or '?')}")
    fps = f"{s['fps']:.2f}" if s["fps"] is not None else "n/a"
    cpu = f"{s['cpu_pct']:.1f}%" if s["cpu_pct"] is not None else "n/a"
    print(f"  fps {fps}   CPU {cpu}   GPU {s['gpu_pct']}% "
          f"(renderer {s['renderer_pct']}%, tiler {s['tiler_pct']}%)")
    print(f"  -> {verdict(s, cores())}")
    return s


def do_watch(a):
    while True:
        do_status(a)
        sys.stdout.flush()


def do_profile(a):
    pid, cmd = pick_pid(a.pid)
    syms = load_perf_map(a.map or arg_value(cmd, "--jit_perf_map"))
    out = f"/tmp/xe-sample-{pid}.txt"
    print(f"sampling pid {pid} for {a.secs}s"
          + (f" ({len(syms)} guest symbols)" if syms else
             " (no JIT symbol map: guest frames will stay as addresses)"),
          flush=True)
    r = subprocess.run(["sample", str(pid), str(a.secs), "-mayDie", "-f", out],
                       capture_output=True, text=True)
    if not os.path.exists(out):
        sys.exit(f"sample failed: {(r.stderr or r.stdout).strip()[:400]}")
    text = open(out, errors="replace").read()

    # Per-thread totals, then self time per symbol. `sample` prints a call
    # graph where a node's count includes its children, so self time is the
    # node minus the children directly under it.
    threads, cur, lines = {}, None, text.splitlines()
    nodes = []
    for ln in lines:
        mt = THREAD_RE.match(ln)
        if mt and "Thread_" in ln:
            cur = (mt.group(3) or f"tid {mt.group(2)}").strip()
            threads[cur] = threads.get(cur, 0) + int(mt.group(1))
            continue
        m = NODE_RE.match(ln)
        if m and cur is not None and "Binary Images" not in ln:
            nodes.append((len(m.group(1)), int(m.group(2)), m.group(3), cur))

    self_time = {}
    for i, (depth, count, label, thread) in enumerate(nodes):
        child = 0
        for d2, c2, _, _ in nodes[i + 1:]:
            if d2 <= depth:
                break
            if d2 == depth + 2:
                child += c2
        s = count - child
        if s <= 0:
            continue
        name = label
        addr = re.search(r"\b0x([0-9a-f]{6,})\b", label)
        if syms and addr:
            g = guest_symbol(syms, int(addr.group(1), 16))
            if g:
                name = f"[guest] {g}"
        self_time[name] = self_time.get(name, 0) + s

    total = sum(threads.values()) or 1
    print(f"\nthreads by samples ({total} total)")
    for name, n in sorted(threads.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  {100.0*n/total:5.1f}%  {n:>6}  {name}")
    print(f"\nhottest by self time")
    for name, n in sorted(self_time.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  {100.0*n/total:5.1f}%  {n:>6}  {name[:110]}")
    guest = sum(n for k, n in self_time.items() if k.startswith("[guest] "))
    if syms:
        print(f"\nguest JIT code accounts for {100.0*guest/total:.1f}% of samples")
    print(f"\nfull sample: {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("status", "watch"):
        p = sub.add_parser(name)
        p.add_argument("--pid", type=int)
        p.add_argument("--window", type=float, default=5.0,
                       help="seconds to measure fps and CPU over")
    p = sub.add_parser("profile")
    p.add_argument("--pid", type=int)
    p.add_argument("--secs", type=int, default=8)
    p.add_argument("--map", default="", help="JIT symbol map from --jit_perf_map")
    sub.add_parser("list")
    a = ap.parse_args()
    if a.cmd == "list":
        for pid, cmd in emulators():
            print(pid, arg_value(cmd, "--storage_root"))
    elif a.cmd == "status":
        do_status(a)
    elif a.cmd == "watch":
        do_watch(a)
    elif a.cmd == "profile":
        do_profile(a)


if __name__ == "__main__":
    main()
