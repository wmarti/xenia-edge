#!/usr/bin/env python3
"""Compare two builds by counting instructions instead of timing them.

The x64 side runs on a shared cluster where `perf_event_paranoid` is 4, so
hardware counters are unavailable, and where wall clock is only trustworthy on
an exclusively allocated node. Callgrind sidesteps both: it counts instructions
deterministically, so the same binary produces the same number regardless of
what else the machine is doing, and results stay comparable across weeks.

The cost is roughly a 50x slowdown, so this is aimed at the benchmark suites
rather than the whole corpus. Wall clock on an exclusive node remains the
secondary confirmation, and a disagreement between the two is a finding in its
own right.
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
import pathlib

# callgrind_annotate is not always installed; the totals line in the output
# file is easy enough to read directly.
SUMMARY_RE = re.compile(r"^summary:\s+(\d+)", re.M)
TOTALS_RE = re.compile(r"^totals:\s+(\d+)", re.M)
# Starting the process and discovering the corpus alone costs ~86M
# instructions, so anything near that never reached the guest code.
MIN_PLAUSIBLE = 150_000_000


def count(exe, suite, corpus, timeout):
    """Return instructions retired for one suite, or None if it did not run."""
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / "cg.out"
        cmd = [
            "valgrind", "--tool=callgrind",
            # The JIT writes code and then executes it; without this callgrind
            # keeps running stale translations and the counts are fiction.
            "--smc-check=all-non-file",
            "--collect-systime=no", "--cache-sim=no", "--branch-sim=no",
            f"--callgrind-out-file={out}",
            exe, f"--test_bin_path={corpus}/", suite,
        ]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=timeout)
        except subprocess.TimeoutExpired:
            return None, "timeout"
        if not out.exists():
            return None, f"no output (rc={proc.returncode})"
        if proc.returncode != 0:
            # valgrind exits 0 even when the program under it dies, and it
            # still writes an output file, so a process that failed at startup
            # reports a small, perfectly stable, entirely meaningless count.
            # A stale container image missing liblz4 produced ~100k
            # instructions per suite and a tidy -0.5% table.
            tail = (proc.stderr or "").strip().splitlines()[-1:] or ["?"]
            return None, f"program failed (rc={proc.returncode}): {tail[0][:120]}"
        text = out.read_text()
        m = SUMMARY_RE.search(text) or TOTALS_RE.search(text)
        if not m:
            return None, "no summary line"
        count = int(m.group(1))
        if count < MIN_PLAUSIBLE:
            return None, (f"only {count:,} instructions — the program cannot "
                          f"have run the suite")
        return count, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-a", required=True)
    ap.add_argument("--exe-b", required=True)
    ap.add_argument("--ref-a", default="A")
    ap.add_argument("--ref-b", default="B")
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--suites", nargs="+", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--timeout", type=float, default=3600.0)
    args = ap.parse_args()

    rows = []
    width = max([len(s) for s in args.suites] + [16])
    print(f"{'suite':{width}} {args.ref_a:>16} {args.ref_b:>16} {'delta':>9}")
    ta = tb = 0
    for suite in args.suites:
        a, erra = count(args.exe_a, suite, args.corpus, args.timeout)
        b, errb = count(args.exe_b, suite, args.corpus, args.timeout)
        if a is None or b is None:
            print(f"{suite:{width}} skipped ({erra or errb})")
            rows.append({"suite": suite, "a": a, "b": b,
                         "error": erra or errb})
            continue
        ta += a
        tb += b
        delta = 100.0 * (b - a) / a
        print(f"{suite:{width}} {a:16,} {b:16,} {delta:+8.2f}%")
        rows.append({"suite": suite, "a": a, "b": b,
                     "delta_pct": round(delta, 3)})
    if ta:
        print(f"{'TOTAL':{width}} {ta:16,} {tb:16,} "
              f"{100.0*(tb-ta)/ta:+8.2f}%")
    print("\nInstruction counts are deterministic: a repeat run on this "
          "machine\nreproduces them exactly, so there is no noise floor to "
          "clear.")

    if args.out:
        pathlib.Path(args.out).write_text(json.dumps({
            "ref_a": args.ref_a, "ref_b": args.ref_b,
            "metric": "callgrind_instructions",
            "total_a": ta, "total_b": tb,
            "delta_pct": round(100.0*(tb-ta)/ta, 3) if ta else None,
            "suites": rows,
        }, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
