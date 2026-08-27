#!/usr/bin/env python3
"""Run the PPC test corpus one suite at a time and record a per-suite verdict.

Running the whole corpus in a single process is faster, but a backend bug that
faults takes the rest of the corpus with it: the baseline `edge` binary dies of
SIGBUS on instr_seq_stacksync and never reaches the other 375 suites. Because a
regression gate has to compare complete result sets, each suite gets its own
process here, and a crash is recorded as a verdict rather than ending the run.

The output is a JSON document meant to be diffed against another one by
compare_verify.py, so both backends can be held to the same corpus.
"""
import argparse
import json
import pathlib
import re
import signal
import subprocess
import sys
import time

TOTAL_RE = re.compile(r"^Total tests: (\d+)$", re.M)
PASSED_RE = re.compile(r"^Passed: (\d+)$", re.M)
FAILED_RE = re.compile(r"^Failed: (\d+)$", re.M)


def discover_suites(corpus, test_path, requested=None):
    """Return the exact source-backed corpus closure accepted by the runner."""
    source_suites = {p.stem for p in test_path.glob("instr_*.s")}
    map_suites = {p.stem for p in corpus.glob("instr_*.map")}
    bin_suites = {p.stem for p in corpus.glob("instr_*.bin")}

    if requested is not None:
        requested_suites = set(requested)
        unknown = requested_suites - source_suites
        missing_maps = requested_suites - map_suites
        missing_bins = requested_suites - bin_suites
    else:
        requested_suites = source_suites
        unknown = set()
        missing_maps = source_suites - map_suites
        missing_bins = source_suites - bin_suites

    problems = []
    if unknown:
        problems.append("unknown source suites: " + ", ".join(sorted(unknown)))
    if missing_maps:
        problems.append("missing .map suites: " +
                        ", ".join(sorted(missing_maps)))
    if missing_bins:
        problems.append("missing .bin suites: " +
                        ", ".join(sorted(missing_bins)))
    if requested is None:
        orphan_maps = map_suites - source_suites
        orphan_bins = bin_suites - source_suites
        if orphan_maps:
            problems.append("orphan .map suites: " +
                            ", ".join(sorted(orphan_maps)))
        if orphan_bins:
            problems.append("orphan .bin suites: " +
                            ", ".join(sorted(orphan_bins)))
    if problems:
        raise ValueError("; ".join(problems))
    return sorted(requested_suites)


def describe_exit(code):
    """Turn a subprocess returncode into (verdict, signal-name-or-None)."""
    if code == 0:
        return "pass", None
    # Python reports a signal death as a negative returncode; the shell reports
    # the same thing as 128+n. Accept both so this works when the runner is
    # invoked through a wrapper.
    signo = None
    if code < 0:
        signo = -code
    elif code > 128:
        signo = code - 128
    if signo is not None:
        try:
            name = signal.Signals(signo).name
        except ValueError:
            name = f"SIG{signo}"
        return "crash", name
    return "fail", None


def run_suite(exe, suite, corpus, test_path, skip_file, runner_args, timeout):
    cmd = [
        exe,
        *runner_args,
        f"--test_path={test_path}/",
        f"--test_bin_path={corpus}/",
        suite,
    ]
    if skip_file:
        cmd.append(f"--test_skip_file={skip_file}")
    started = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        )
        code = proc.returncode
        err = proc.stderr
    except subprocess.TimeoutExpired:
        return {
            "suite": suite, "verdict": "timeout", "signal": None,
            "total": 0, "passed": 0, "failed": 0,
            "seconds": round(time.perf_counter() - started, 4),
        }
    verdict, signame = describe_exit(code)

    def grab(pattern):
        m = pattern.search(err)
        return int(m.group(1)) if m else 0

    return {
        "suite": suite, "verdict": verdict, "signal": signame,
        "total": grab(TOTAL_RE), "passed": grab(PASSED_RE),
        "failed": grab(FAILED_RE),
        "seconds": round(time.perf_counter() - started, 4),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--corpus", required=True,
                    help="directory holding the prebuilt .bin/.map corpus")
    ap.add_argument("--test-path", required=True,
                    help="directory holding the .s sources; drives discovery")
    ap.add_argument("--skip-file", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="",
                    help="free-form identity for the row, e.g. 'edge@a1b2c3 a64'")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument(
        "--runner-arg", action="append", default=[],
        help="argument forwarded to every PPC runner process; use "
             "--runner-arg=--flag=value for arguments beginning with '--'",
    )
    ap.add_argument("--suites", nargs="*", default=None,
                    help="restrict to these suites (default: the whole corpus)")
    args = ap.parse_args()

    corpus = pathlib.Path(args.corpus)
    test_path = pathlib.Path(args.test_path)
    # The PPC runner discovers only instr_*.s. Require the prebuilt .map/.bin
    # corpus to be the exact closure of those sources so a stale archive can't
    # silently omit a new suite or feed annotations to an older binary.
    try:
        suites = discover_suites(corpus, test_path, args.suites)
    except ValueError as error:
        sys.exit(f"error: corpus/source mismatch: {error}")
    if not suites:
        sys.exit(f"error: no instr_*.s suites found under {test_path}")

    results = []
    for i, suite in enumerate(suites, 1):
        row = run_suite(args.exe, suite, args.corpus, args.test_path,
                        args.skip_file, args.runner_arg, args.timeout)
        results.append(row)
        if row["verdict"] != "pass":
            marker = row["signal"] or row["verdict"]
            print(f"[{i}/{len(suites)}] {suite}: {row['verdict']} ({marker})",
                  file=sys.stderr)

    summary = {
        "label": args.label,
        "exe": args.exe,
        "corpus": str(corpus),
        "runner_args": args.runner_arg,
        "suites": len(results),
        "cases": sum(r["total"] for r in results),
        "passed": sum(r["passed"] for r in results),
        "failed": sum(r["failed"] for r in results),
        "crashed": sum(1 for r in results if r["verdict"] == "crash"),
        "timed_out": sum(1 for r in results if r["verdict"] == "timeout"),
        # Counted from verdicts rather than from the parsed totals. A binary
        # that dies before printing anything — a missing shared library, say —
        # yields "0 failed" from the totals while every suite has in fact
        # failed, which reads as a clean run.
        "not_passed": sum(1 for r in results if r["verdict"] != "pass"),
        "results": results,
    }
    pathlib.Path(args.out).write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{args.label or args.exe}: {summary['suites']} suites, "
          f"{summary['cases']} cases, {summary['failed']} failed, "
          f"{summary['crashed']} crashed, {summary['timed_out']} timed out")
    if summary["cases"] == 0:
        print("error: no test cases ran at all — treat this as a broken "
              "harness, not as a clean corpus", file=sys.stderr)
    # A clean corpus is the only success. Anything else is a finding.
    return 0 if summary["not_passed"] == 0 and summary["cases"] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
