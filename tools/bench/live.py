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
# `sample` draws the call graph with +, ! and : as tree characters, so depth
# is the column the count starts at, not a run of leading spaces. Matching on
# whitespace alone parses nothing at all and reports an empty profile.
NODE_RE = re.compile(r"^([\s+!:|]*)(\d+)\s+(.*?)\s*$")
# \S+ not \w+: `sample` writes "Thread_<multiple>" when it merges several
# short-lived threads under one header, and \w+ does not match the angle
# brackets. Those headers were dropped, and every sample under them was
# folded into whichever thread was named last -- at the GTA IV docks that
# silently moved 3,657 samples of com.Metal.CommandQueueDispatch, plus
# CompletionQueueDispatch, FramePacing and libMTLHud, onto their neighbours.
THREAD_RE = re.compile(r"^\s*(\d+)\s+Thread_(\S+?):?(?:\s+(.*?))?\s*$")
# A stack sitting in one of these was not on a core. `sample` records every
# thread at every interval whether it is running or blocked, so counting all
# of them gives each thread an identical total and measures nothing.
# Truly parked: the thread is off the run queue and consuming nothing. Samples
# on these stacks are dropped from the CPU accounting entirely.
BLOCKED = (
    "__psynch_cvwait", "__psynch_mutexwait", "__psynch_rw_", "mach_msg2_trap",
    "kevent_id", "kevent", "semaphore_wait_trap", "semaphore_timedwait_trap",
    "semaphore_wait_signal_trap", "__semwait_signal", "mach_wait_until",
    "__select", "__workq_kernreturn", "poll", "__read", "__sigsuspend",
    "__ulock_wait", "start_wqthread",
    "__wait4", "__accept", "__recvfrom", "guarded_kqueue_np",
)

# NOT blocked. sched_yield() lands here via cthread_yield, and it leaves the
# thread RUNNABLE: it gives up the rest of its quantum and is immediately
# eligible again, so on a machine with a spare core it returns almost at once
# and the loop spins at syscall rate. These samples are real CPU.
#
# They were in BLOCKED, which silently deleted the largest single consumer in
# the profile. At the Halo Reach menu the NETWORK_RECEIVE thread is 5,438
# samples, 3,130 of them swtch_pri; counting those as blocked reported the
# thread at 29.4% of on-core CPU when its true share is about 2.4x that.
# Reported separately rather than merged into ordinary work, because a thread
# spinning in yield is not the same finding as a thread doing arithmetic --
# one is removable by scheduling policy, the other is not.
SPINNING = ("swtch_pri", "thread_switch", "sched_yield", "cthread_yield")


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
    return disambiguate(syms)


def disambiguate(syms):
    """Give repeated names their own identity.

    code_cache_base.h falls back to `guest_{:08X}` when a translated body has
    no function_info, and the JIT's own transition thunks are emitted before
    any guest function with a guest address of zero -- so six distinct stubs
    all arrive called guest_00000000 and collapse into one line that reads
    like a single very hot guest function. It is not one, and it is not guest
    code.
    """
    counts = {}
    for _, _, name in syms:
        counts[name] = counts.get(name, 0) + 1
    out, seen = [], {}
    for start, end, name in syms:
        if counts[name] > 1:
            seen[name] = seen.get(name, 0) + 1
            name = f"{name}#{seen[name]}@{start:x}"
        out.append((start, end, name))
    return out


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


def parse_sample(text):
    """(nodes, threads) from a `sample` call graph.

    nodes are (depth, count, label, thread); depth is the column the count
    starts at, which `sample` steps by two per level.
    """
    nodes, threads, cur = [], {}, None
    for ln in text.splitlines():
        # `sample` appends a flat "Total number in stack" table after the call
        # graph and before "Binary Images:". Parsing it as tree nodes added
        # ~2,200 phantom nodes and inflated the denominator by ~8.5%.
        if ln.startswith("Binary Images:") or ln.startswith(
                "Total number in stack"):
            break
        mt = THREAD_RE.match(ln)
        if mt and "Thread_" in ln:
            cur = (mt.group(3) or "").strip() or f"tid {mt.group(2)}"
            threads[cur] = threads.get(cur, 0) + int(mt.group(1))
            continue
        m = NODE_RE.match(ln)
        if m and cur is not None:
            nodes.append((len(m.group(1)), int(m.group(2)), m.group(3), cur))
    return nodes, threads


def self_times(nodes):
    """(work, spin, running) self-sample maps keyed by (thread, label).

    A node's count includes its children, so self time is the node minus the
    children directly beneath it. A node whose label names a blocking
    primitive consumed no CPU and is dropped.

    A node whose label names a yield trap DID consume CPU -- see SPINNING --
    and is returned separately so it can be reported as what it is rather than
    either deleted or disguised as useful work. `running` counts both.
    """
    work, spin, running = {}, {}, 0
    for i, (depth, count, label, thread) in enumerate(nodes):
        child = 0
        for j in range(i + 1, len(nodes)):
            d2, c2 = nodes[j][0], nodes[j][1]
            if d2 <= depth:
                break
            if d2 == depth + 2:
                child += c2
        self_n = count - child
        if self_n <= 0:
            continue
        # SPINNING is tested BEFORE BLOCKED, and both are substring tests.
        # A bare "read" in BLOCKED matched "cthread_yield" and "thread_switch",
        # which silently sent both back to the blocked bin and defeated half of
        # the spin accounting; it is "__read" now, but the ordering is what
        # makes that class of collision harmless rather than merely unlikely.
        if any(sp in label for sp in SPINNING):
            bucket = spin
        elif any(b in label for b in BLOCKED):
            continue
        else:
            bucket = work
        bucket[(thread, label)] = bucket.get((thread, label), 0) + self_n
        running += self_n
    return work, spin, running


def do_profile(a):
    pid, cmd = pick_pid(a.pid)
    syms = load_perf_map(a.map or arg_value(cmd, "--jit_perf_map"))
    out = f"/tmp/xe-sample-{pid}.txt"
    print(f"sampling pid {pid} for {a.secs}s"
          + (f" ({len(syms)} guest symbols)" if syms else
             " (no JIT symbol map: guest frames stay as addresses)"),
          flush=True)
    r = subprocess.run(["sample", str(pid), str(a.secs), "-mayDie", "-f", out],
                       capture_output=True, text=True)
    if not os.path.exists(out):
        sys.exit(f"sample failed: {(r.stderr or r.stdout).strip()[:400]}")
    nodes, threads = parse_sample(open(out, errors="replace").read())
    work, spin, running = self_times(nodes)
    selves = dict(work)
    for k, n in spin.items():
        selves[k] = selves.get(k, 0) + n
    if not running:
        sys.exit("no running samples: every thread was blocked")

    def pretty(label):
        """JIT frames arrive as `??? (in <unknown binary>) [0xADDR]`."""
        m = re.search(r"\[0x([0-9a-f]+)", label)
        if syms and m:
            g = guest_symbol(syms, int(m.group(1), 16))
            if g:
                return f"[guest] {g}"
        if "???" in label and m:
            return f"[jit, unmapped] 0x{m.group(1)}"
        return re.sub(r"\s+\(in .*", "", label)

    by_thread, spin_thread = {}, {}
    for (thread, label), n in selves.items():
        by_thread[thread] = by_thread.get(thread, 0) + n
    for (thread, label), n in spin.items():
        spin_thread[thread] = spin_thread.get(thread, 0) + n
    spin_total = sum(spin.values())
    print(f"\n{running} running samples of {sum(threads.values())} taken "
          f"({100.0*running/max(sum(threads.values()),1):.1f}% on a core; the "
          f"rest were parked)")
    if spin_total:
        print(f"of those, {spin_total} ({100.0*spin_total/running:.1f}%) are a "
              f"thread spinning in a yield trap -- runnable, not parked, and "
              f"burning syscalls rather than doing work")
    print("\nCPU by thread   (spin = share of that thread stuck in yield)")
    for name, n in sorted(by_thread.items(), key=lambda kv: -kv[1])[:14]:
        sp = spin_thread.get(name, 0)
        tag = f"   spin {100.0*sp/n:4.0f}%" if sp else ""
        print(f"  {100.0*n/running:5.1f}%  {n:>6}  {name}{tag}")

    merged = {}
    for (_, label), n in selves.items():
        k = pretty(label)
        merged[k] = merged.get(k, 0) + n
    print("\nhottest by self time")
    for name, n in sorted(merged.items(), key=lambda kv: -kv[1])[:25]:
        print(f"  {100.0*n/running:5.1f}%  {n:>6}  {name[:100]}")

    guest = sum(n for k, n in merged.items() if k.startswith("[guest] "))
    unmapped = sum(n for k, n in merged.items() if k.startswith("[jit, unmapped]"))
    print(f"\nguest JIT code: {100.0*guest/running:.1f}% mapped to a function, "
          f"{100.0*unmapped/running:.1f}% JIT but outside the map")
    print(f"full sample: {out}")


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
