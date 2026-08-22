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
    error.
"""
import argparse
import json
import pathlib
import platform
import statistics
import subprocess
import sys
import time


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

    # Calibrate first, on the suite that will also be reported, so the floor
    # comes from the same work the results come from.
    cal_suite = args.suites[0]
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
        ta += a
        tb += b
        delta = 100.0 * (b - a) / a
        print(f"{suite:{width}} {a:12.3f} {b:12.3f} {delta:+8.2f}% "
              f"{spread:7.1f}%", flush=True)
        rows.append({"suite": suite, "a_seconds": round(a, 4),
                     "b_seconds": round(b, 4), "delta_pct": round(delta, 3),
                     "spread_pct": round(spread, 2)})
    total_delta = 100.0 * (tb - ta) / ta if ta else None
    if ta:
        print(f"{'TOTAL':{width}} {ta:12.3f} {tb:12.3f} {total_delta:+8.2f}%")
    if floor is not None:
        print(f"\nmeasurement error, from running {args.ref_a} against "
              f"itself: {floor:.2f}%.\nAnything of that size or smaller has "
              f"not been measured.")

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
            "suites": rows,
        }, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
