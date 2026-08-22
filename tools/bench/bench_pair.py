#!/usr/bin/env python3
"""Wall-clock A/B for two builds, with the noise floor measured, not assumed.

bench_macos.sh does this for the Mac but refuses to run anywhere else, and the
x64 side needs the same treatment on an exclusively allocated node. The
methodology is the one that survived being wrong earlier:

  - The minimum of N runs, not the mean. The minimum rejects scheduler
    migration and thermal excursions; a mean absorbs them and reports them as
    a difference between the refs.
  - A and B interleaved with the order flipped each iteration, so drift over
    the run is not charged to whichever ref happened to go first.
  - The floor measured by running ref A against *itself*. A control suite only
    works if the change cannot reach it, and for a change touching addressing
    or context layout no such suite exists — on a64 the "control" moved -9.2%
    against a real total of -8.5%, which would have discarded the result.
    Two byte-identical binaries cannot differ by anything but measurement
    error. It has to run on the *longest* suite: calibrating on a short one
    measures how reproducibly the process starts, which is very reproducible,
    and says nothing about a six-second suite. Doing that reported a 0.07%
    measurement error next to real spreads of 18-31%, and dressed up an
    unresolved x64 result as a measured -7.8%.
  - Suites that finish at the process-startup floor are excluded from the
    total. Eight of the corpus's thirteen suites land within a few ms of
    startup, so their "delta" is the difference between two binaries' load
    time - a fixed ~50 ms that reads as +15.9% and is not about guest code
    at all.
"""
import argparse
import json
import pathlib
import platform
import statistics
import subprocess
import sys
import time

# A suite finishing within this multiple of bare process startup has not run
# enough guest code to measure. On the PPC corpus eight of thirteen suites sit
# here, all at the same 0.314s, and their deltas are load-time differences.
STARTUP_MARGIN = 1.25


def time_once(exe, suite, corpus, timeout):
    """Elapsed seconds for one suite run, or None if it did not complete."""
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            [exe, f"--test_bin_path={corpus}/", suite],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        # A suite that fails still takes time, and timing a crash is
        # meaningless, so it is dropped rather than folded into the minimum.
        return None
    return time.perf_counter() - t0


def time_startup(exe, corpus, runs, timeout):
    """Seconds the process costs before any guest code runs.

    Asking for a suite name that matches nothing loads the binary, brings up
    the runtime and reports zero tests. Whatever a real suite costs, it costs
    this much first.
    """
    samples = []
    for _ in range(runs):
        t0 = time.perf_counter()
        try:
            subprocess.run(
                [exe, f"--test_bin_path={corpus}/", "__no_such_suite__"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        samples.append(time.perf_counter() - t0)
    return min(samples) if samples else None


def measure(exe_a, exe_b, suite, corpus, runs, timeout):
    """Interleaved min-of-N for both refs. Returns (best_a, best_b, spread)."""
    a_samples, b_samples = [], []
    for i in range(runs):
        # Flip the order every iteration.
        if i % 2 == 0:
            ta = time_once(exe_a, suite, corpus, timeout)
            tb = time_once(exe_b, suite, corpus, timeout)
        else:
            tb = time_once(exe_b, suite, corpus, timeout)
            ta = time_once(exe_a, suite, corpus, timeout)
        if ta is not None:
            a_samples.append(ta)
        if tb is not None:
            b_samples.append(tb)
    if not a_samples or not b_samples:
        return None, None, None
    best_a, best_b = min(a_samples), min(b_samples)
    # The spread between a ref's fastest and slowest sample is the noise this
    # machine actually produced during this run.
    spread = max(
        (max(a_samples) - best_a) / best_a if best_a else 0,
        (max(b_samples) - best_b) / best_b if best_b else 0)
    return best_a, best_b, spread * 100.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-a", required=True)
    ap.add_argument("--exe-b", required=True)
    ap.add_argument("--ref-a", default="A")
    ap.add_argument("--ref-b", default="B")
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--suites", nargs="+", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=1800.0)
    ap.add_argument("--out", default="")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    # What the process costs before any guest code runs. Every suite pays it,
    # and the suites that pay nothing else cannot measure anything.
    startup = time_startup(args.exe_a, args.corpus, 3, args.timeout)
    if startup is not None:
        print(f"process startup: {startup:.3f}s", flush=True)

    # One probe run per suite, to find the longest. Calibrating on a short
    # suite measures startup reproducibility, not the noise that acts on the
    # suites carrying the signal.
    probes = {}
    for suite in args.suites:
        t = time_once(args.exe_a, suite, args.corpus, args.timeout)
        if t is not None:
            probes[suite] = t
    def is_startup_bound(seconds):
        return startup is not None and seconds < startup * STARTUP_MARGIN

    startup_bound = set()

    # Calibrate on the longest suite that is not startup-bound.
    usable = {s: t for s, t in probes.items() if s not in startup_bound}
    cal_suite = (max(usable, key=usable.get) if usable
                 else (max(probes, key=probes.get) if probes
                       else args.suites[0]))
    print(f"calibrating on {cal_suite}: {args.ref_a} against itself, "
          f"min of {args.runs}", flush=True)
    ca, cb, cspread = measure(args.exe_a, args.exe_a, cal_suite,
                              args.corpus, args.runs, args.timeout)
    floor = abs(100.0 * (cb - ca) / ca) if ca else None
    if floor is None:
        print("  calibration failed; treat every result below as unqualified")
    else:
        print(f"  measurement error: {floor:.2f}%  (spread {cspread:.1f}%)",
              flush=True)

    rows = []
    width = max([len(s) for s in args.suites] + [16])
    print(f"\n{'suite':{width}} {args.ref_a:>12} {args.ref_b:>12} "
          f"{'delta':>9} {'spread':>8}")
    ta = tb = 0.0
    for suite in args.suites:
        a, b, spread = measure(args.exe_a, args.exe_b, suite, args.corpus,
                               args.runs, args.timeout)
        if a is None:
            print(f"{suite:{width}} skipped (did not complete)")
            rows.append({"suite": suite, "error": "did not complete"})
            continue
        delta = 100.0 * (b - a) / a
        bound = is_startup_bound(min(a, b))
        if bound:
            startup_bound.add(suite)
        # A delta smaller than the run's own sample-to-sample variation has not
        # been separated from that variation. Sub-second suites are dominated
        # by process startup and routinely show 50-90% spread, which is how the
        # same suite came out +3.6% on one machine and -8.1% on another.
        # A startup-bound suite is never resolved whatever the arithmetic says:
        # two binaries that differ by 50 ms of load time show a tidy 0.2%
        # spread and a confident +15.9%, and none of it is guest code.
        resolved = abs(delta) >= spread and not bound
        mark = "  startup-bound" if bound else ("" if resolved
                                                else "  unresolved")
        if not bound:
            ta += a
            tb += b
        print(f"{suite:{width}} {a:12.3f} {b:12.3f} {delta:+8.2f}% "
              f"{spread:7.1f}%{mark}", flush=True)
        rows.append({"suite": suite, "a_seconds": round(a, 4),
                     "b_seconds": round(b, 4), "delta_pct": round(delta, 3),
                     "spread_pct": round(spread, 2), "resolved": resolved,
                     "startup_bound": bound})
    total_delta = 100.0 * (tb - ta) / ta if ta else None
    if ta:
        # The total covers only the suites that run guest code; folding in the
        # startup-bound ones would average real work against load time.
        print(f"{'TOTAL (measurable)':{width}} {ta:12.3f} {tb:12.3f} "
              f"{total_delta:+8.2f}%")

    good = [r for r in rows if r.get("resolved")]
    weak = [r for r in rows if "delta_pct" in r and not r.get("resolved")]
    if good:
        ga = sum(r["a_seconds"] for r in good)
        gb = sum(r["b_seconds"] for r in good)
        print(f"{'resolved only':{width}} {ga:12.3f} {gb:12.3f} "
              f"{100.0*(gb-ga)/ga:+8.2f}%  ({len(good)} of {len(rows)} suites)")
    if startup_bound:
        print(f"\n{len(startup_bound)} suite(s) finish within "
              f"{STARTUP_MARGIN:g}x of bare process startup ({startup:.3f}s) "
              f"and run\nalmost no guest code, so they are excluded from the "
              f"total:")
        print("  " + ", ".join(sorted(startup_bound)))
    if floor is not None:
        print(f"\nmeasurement error, from running {args.ref_a} against "
              f"itself on {cal_suite}: {floor:.2f}%.")
    if weak:
        print(f"{len(weak)} suite(s) unresolved — their delta is smaller than "
              f"their own sample spread:")
        print("  " + ", ".join(r["suite"] for r in weak))
        print("Report those as 'not measured', not as 'no change'. Running a "
              "sub-second\nsuite more times does not help much; the variation "
              "is process startup,\nnot the work being timed.")

    if args.out:
        pathlib.Path(args.out).write_text(json.dumps({
            "label": args.label,
            "ref_a": args.ref_a, "ref_b": args.ref_b,
            "metric": "wall_clock_min_of_n",
            "runs": args.runs,
            "host": platform.node(),
            "machine": platform.machine(),
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "floor_pct": round(floor, 3) if floor is not None else None,
            "total_a": round(ta, 4), "total_b": round(tb, 4),
            "total_delta_pct": round(total_delta, 3) if total_delta else None,
            "startup_seconds": round(startup, 4) if startup else None,
            "calibration_suite": cal_suite,
            "startup_bound_suites": sorted(startup_bound),
            "resolved_suites": len(good),
            "unresolved_suites": [r["suite"] for r in weak],
            "suites": rows,
        }, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
