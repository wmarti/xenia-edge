#!/usr/bin/env python3
"""Fail-closed paired benchmark for the warmed PPC call/return kernel.

The driver consumes the integrity manifest written by gen_loop_bench.py
--callret, runs the linked-call suite and its matched empty-loop control in
balanced interleaved order, and scores control-subtracted current-thread CPU
nanoseconds per guest call. Each subprocess must emit exactly one strict
in-process metric proving that one full workload invocation was warmed first
and that no code placement occurred in the timed invocation.

A/A and B/B calibrations measure each executable against itself. The effective
floor is the largest of either calibration's worst paired delta or pair-mean
drift and the A/B phase's own pair-mean drift. A/B is an improvement only when
all paired deltas are negative and their mean magnitude clears that floor.
Mixed signs, a result inside the floor, a short net CPU interval, timeout,
failed or zero-test output, malformed or duplicate metrics, code-placement
change, and artifact mismatch all fail closed.

CLOCK_UPTIME_RAW wall nanoseconds and external process wall time are retained
as diagnostics only. Startup, JIT compilation, and the full warm invocation
are never scored. This remains a synthetic call-path measurement, not a
game-speed claim.
"""

import argparse
import hashlib
import json
import pathlib
import platform
import re
import statistics
import subprocess
import sys
import time


SCHEMA = "xenia-callret-bench-v2"
RESULT_SCHEMA = "xenia-callret-result-v2"
METRIC_PREFIX = "XENIA_PPC_BENCHMARK_V1"
EXPECTED_MEASUREMENT_BOUNDARY = {
    "timer": "in_process_warmed_invocation",
    "primary_metric": "current_thread_cpu_ns",
    "diagnostic_metric": "clock_uptime_raw_ns",
    "control_subtracted": True,
    "full_workload_warmup_invocations": 1,
    "warmup_inside_timed_region": False,
    "root_resolve_inside_timed_region": False,
    "one_time_jit_cost_inside_timed_region": False,
    "reject_code_placement_during_timed_region": True,
    "metric_prefix": METRIC_PREFIX,
}
PASS_PATTERNS = (
    re.compile(r"Running\s+1\s+test suites,\s+1\s+test cases"),
    re.compile(r"Total tests:\s*1(?:\D|$)"),
    re.compile(r"Passed:\s*1(?:\D|$)"),
    re.compile(r"Failed:\s*0(?:\D|$)"),
)
METRIC_RE = re.compile(
    re.escape(METRIC_PREFIX)
    + r"\tthread_cpu_ns=([1-9][0-9]*)"
    + r"\tuptime_raw_ns=([1-9][0-9]*)"
    + r"\tplacement_generation_before=([0-9]+)"
    + r"\tplacement_generation_after=([0-9]+)"
)
FIXED_RUNNER_FLAGS = (
    "--guest_scheduler=false",
    "--test_only_skipped=false",
    "--test_benchmark_warmed=true",
    "--count_call_paths=false",
    "--count_physical_remap_hits=false",
    "--trace_function_coverage=false",
    "--cpu_trace_mask=0",
)


class BenchmarkError(RuntimeError):
    pass


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_metric(output):
    """Parse exactly one canonical benchmark metric line."""
    lines = [
        line for line in output.splitlines()
        if line.startswith(METRIC_PREFIX)
    ]
    if len(lines) != 1:
        raise BenchmarkError(
            f"expected exactly one {METRIC_PREFIX} line, got {len(lines)}")
    match = METRIC_RE.fullmatch(lines[0])
    if not match:
        raise BenchmarkError("malformed in-process benchmark metric")
    thread_cpu_ns, uptime_raw_ns, generation_before, generation_after = (
        int(value) for value in match.groups()
    )
    if generation_before != generation_after:
        raise BenchmarkError(
            "code placement generation changed in timed region: "
            f"{generation_before} -> {generation_after}")
    return {
        "thread_cpu_ns": thread_cpu_ns,
        "uptime_raw_ns": uptime_raw_ns,
        "placement_generation_before": generation_before,
        "placement_generation_after": generation_after,
    }


def validate_suite(manifest_suite, corpus, source):
    required = {
        "name",
        "test_label",
        "binary_bytes",
        "binary_sha256",
        "map_sha256",
        "source_sha256",
        "expected_test_cases",
    }
    missing = required - manifest_suite.keys()
    if missing:
        raise BenchmarkError(
            "manifest suite is missing: " + ", ".join(sorted(missing)))
    if manifest_suite["expected_test_cases"] != 1:
        raise BenchmarkError("callret suites must contain exactly one test case")

    name = manifest_suite["name"]
    test_label = manifest_suite["test_label"]
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise BenchmarkError("manifest suite name is not a safe filename")
    if (not isinstance(test_label, str)
            or not re.fullmatch(r"[A-Za-z0-9_.-]+", test_label)):
        raise BenchmarkError("manifest test label is malformed")
    if (type(manifest_suite["binary_bytes"]) is not int
            or manifest_suite["binary_bytes"] <= 0):
        raise BenchmarkError("manifest binary size must be a positive integer")

    paths = {
        "binary": corpus / f"{name}.bin",
        "map": corpus / f"{name}.map",
        "source": source / f"{name}.s",
    }
    for kind, artifact_path in paths.items():
        if not artifact_path.is_file():
            raise BenchmarkError(f"missing {kind} artifact: {artifact_path}")

    expected = {
        "binary": manifest_suite["binary_sha256"],
        "map": manifest_suite["map_sha256"],
        "source": manifest_suite["source_sha256"],
    }
    for kind, artifact_path in paths.items():
        actual = file_sha256(artifact_path)
        if actual != expected[kind]:
            raise BenchmarkError(
                f"{kind} integrity mismatch for {name}: "
                f"expected {expected[kind]}, got {actual}")
    if paths["binary"].stat().st_size != manifest_suite["binary_bytes"]:
        raise BenchmarkError(f"binary size mismatch for {name}")
    return name


def load_manifest(path, corpus, source):
    try:
        manifest = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise BenchmarkError(
            f"unable to read manifest {path}: {error}") from error
    if manifest.get("schema") != SCHEMA:
        raise BenchmarkError(
            f"unsupported manifest schema: {manifest.get('schema')!r}")

    execution = manifest.get("execution", {})
    for key in (
            "iterations", "width", "preloop_calls", "loop_calls",
            "calls_per_invocation"):
        if type(execution.get(key)) is not int or execution[key] <= 0:
            raise BenchmarkError(f"invalid execution.{key}")
    expected_calls = execution["width"] * (execution["iterations"] + 1)
    if execution["calls_per_invocation"] != expected_calls:
        raise BenchmarkError("manifest call count is internally inconsistent")
    if execution["loop_calls"] != execution["width"] * execution["iterations"]:
        raise BenchmarkError(
            "manifest loop call count is internally inconsistent")
    if execution["preloop_calls"] != execution["width"]:
        raise BenchmarkError(
            "manifest pre-loop call count is internally inconsistent")

    boundary = manifest.get("measurement_boundary")
    if boundary != EXPECTED_MEASUREMENT_BOUNDARY:
        raise BenchmarkError(
            "manifest measurement boundary is not the strict warmed "
            "in-process v2 boundary")

    suites = manifest.get("suites", {})
    if set(suites) != {"workload", "control"}:
        raise BenchmarkError("manifest must contain workload and control suites")
    workload = validate_suite(suites["workload"], corpus, source)
    control = validate_suite(suites["control"], corpus, source)
    if workload == control:
        raise BenchmarkError("workload and control suite names must differ")
    return manifest, workload, control


def run_suite(exe, extra, suite, corpus, source, timeout):
    command = [
        str(exe),
        *extra,
        # Keep identity and instrumentation controls after user-provided
        # options so an accidental duplicate flag cannot change the scored
        # artifacts or re-enable hot-path counters.
        f"--test_bin_path={corpus}/",
        f"--test_path={source}/",
        *FIXED_RUNNER_FLAGS,
        suite,
    ]
    process_start = time.perf_counter_ns()
    try:
        process = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise BenchmarkError(f"timeout running {suite} with {exe}") from error
    except OSError as error:
        raise BenchmarkError(
            f"unable to launch {suite} with {exe}: {error}") from error
    process_wall_ns = time.perf_counter_ns() - process_start
    output = process.stdout + "\n" + process.stderr
    if process.returncode != 0:
        tail = "\n".join(output.splitlines()[-20:])
        raise BenchmarkError(
            f"{suite} exited {process.returncode} with {exe}:\n{tail}")

    missing = [
        pattern.pattern for pattern in PASS_PATTERNS
        if not pattern.search(output)
    ]
    if missing:
        tail = "\n".join(output.splitlines()[-20:])
        raise BenchmarkError(
            f"{suite} did not prove exactly one passing case with {exe}; "
            f"missing {missing}:\n{tail}")

    metric = parse_metric(output)
    metric["process_wall_ns"] = process_wall_ns
    return metric


def leg_order(pair_index):
    """Four orders balancing every leg across all temporal positions."""
    patterns = (
        (("a", "control"), ("a", "workload"),
         ("b", "control"), ("b", "workload")),
        (("b", "workload"), ("b", "control"),
         ("a", "workload"), ("a", "control")),
        (("a", "workload"), ("a", "control"),
         ("b", "workload"), ("b", "control")),
        (("b", "control"), ("b", "workload"),
         ("a", "control"), ("a", "workload")),
    )
    return patterns[pair_index % len(patterns)]


def run_phase(exes, extras, suites, corpus, source, calls, runs, timeout,
              min_net_cpu_seconds, label):
    minimum_cpu_ns = int(min_net_cpu_seconds * 1e9)
    rows = []
    for pair_index in range(runs):
        legs = {"a": {}, "b": {}}
        for role, kind in leg_order(pair_index):
            legs[role][kind] = run_suite(
                exes[role], extras[role], suites[kind], corpus, source, timeout)

        row = {"pair": pair_index + 1, "arms": {}}
        for role in ("a", "b"):
            workload = legs[role]["workload"]
            control = legs[role]["control"]
            net_cpu_ns = (
                workload["thread_cpu_ns"] - control["thread_cpu_ns"])
            if net_cpu_ns < minimum_cpu_ns:
                raise BenchmarkError(
                    f"{label} pair {pair_index + 1} arm {role}: net "
                    f"current-thread CPU interval {net_cpu_ns / 1e9:.6f}s is "
                    f"below --min-net-cpu-seconds "
                    f"{min_net_cpu_seconds:.6f}; increase --iters")
            net_uptime_raw_ns = (
                workload["uptime_raw_ns"] - control["uptime_raw_ns"])
            row["arms"][role] = {
                "workload": workload,
                "control": control,
                "net_thread_cpu_ns": net_cpu_ns,
                "net_thread_cpu_ns_per_call": net_cpu_ns / calls,
                "net_uptime_raw_ns": net_uptime_raw_ns,
                "net_uptime_raw_ns_per_call": net_uptime_raw_ns / calls,
            }

        ns_a = row["arms"]["a"]["net_thread_cpu_ns_per_call"]
        ns_b = row["arms"]["b"]["net_thread_cpu_ns_per_call"]
        row["delta_pct"] = 100.0 * (ns_b - ns_a) / ns_a
        rows.append(row)
        print(
            f"{label} pair {pair_index + 1}/{runs}: "
            f"{ns_a:.3f} -> {ns_b:.3f} thread CPU ns/call "
            f"({row['delta_pct']:+.3f}%)",
            flush=True,
        )
    return rows


def phase_summary(rows):
    if not rows:
        raise BenchmarkError("phase contains no pairs")
    ns_a = [
        row["arms"]["a"]["net_thread_cpu_ns_per_call"] for row in rows
    ]
    ns_b = [
        row["arms"]["b"]["net_thread_cpu_ns_per_call"] for row in rows
    ]
    deltas = [row["delta_pct"] for row in rows]
    pair_means = [(a + b) / 2.0 for a, b in zip(ns_a, ns_b)]
    if min(pair_means) <= 0:
        raise BenchmarkError("phase contains a nonpositive pair mean")
    drift = (
        100.0 * (max(pair_means) - min(pair_means)) / min(pair_means)
    )
    return {
        "a_min_thread_cpu_ns_per_call": min(ns_a),
        "b_min_thread_cpu_ns_per_call": min(ns_b),
        "a_median_thread_cpu_ns_per_call": statistics.median(ns_a),
        "b_median_thread_cpu_ns_per_call": statistics.median(ns_b),
        "mean_paired_delta_pct": statistics.mean(deltas),
        "max_abs_paired_delta_pct": max(abs(delta) for delta in deltas),
        "pair_mean_drift_pct": drift,
        "paired_deltas_pct": deltas,
    }


def phase_noise_floor(summary):
    return max(
        summary["max_abs_paired_delta_pct"],
        summary["pair_mean_drift_pct"],
    )


def classify(ab_rows, effective_floor):
    deltas = [row["delta_pct"] for row in ab_rows]
    mean_delta = statistics.mean(deltas)
    if all(delta < 0 for delta in deltas) and -mean_delta > effective_floor:
        return "improvement"
    if all(delta > 0 for delta in deltas) and mean_delta > effective_floor:
        return "regression"
    return "unresolved"


def write_report(path, report):
    if path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-a", required=True, type=pathlib.Path)
    parser.add_argument("--exe-b", required=True, type=pathlib.Path)
    parser.add_argument("--ref-a", default="A")
    parser.add_argument("--ref-b", default="B")
    parser.add_argument("--corpus", required=True, type=pathlib.Path)
    parser.add_argument("--test-src", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=1800.0)
    parser.add_argument(
        "--min-net-cpu-seconds", "--min-net-seconds",
        dest="min_net_cpu_seconds", type=float, default=1.0,
        help="minimum control-subtracted current-thread CPU interval per arm")
    parser.add_argument("--extra-a", action="append", default=[])
    parser.add_argument("--extra-b", action="append", default=[])
    parser.add_argument("--label", default="")
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()

    manifest_path = args.manifest or args.corpus / "callret_bench_manifest.json"
    report = {
        "schema": RESULT_SCHEMA,
        "label": args.label,
        "ref_a": args.ref_a,
        "ref_b": args.ref_b,
        "host": platform.node(),
        "machine": platform.machine(),
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "runs": args.runs,
        "min_net_thread_cpu_seconds": args.min_net_cpu_seconds,
        "aborted": None,
    }
    try:
        if args.runs < 4 or args.runs % 4:
            raise BenchmarkError(
                "--runs must be a multiple of 4 and at least 4 so temporal "
                "positions are balanced")
        if args.timeout <= 0 or args.min_net_cpu_seconds <= 0:
            raise BenchmarkError(
                "timeout and minimum net CPU interval must be positive")
        for exe in (args.exe_a, args.exe_b):
            if not exe.is_file():
                raise BenchmarkError(f"missing executable: {exe}")

        manifest, workload, control = load_manifest(
            manifest_path, args.corpus, args.test_src)
        calls = manifest["execution"]["calls_per_invocation"]
        report.update({
            "manifest": str(manifest_path),
            "manifest_sha256": file_sha256(manifest_path),
            "calls_per_invocation": calls,
            "measurement_boundary": manifest["measurement_boundary"],
            "executables": {
                "a": {
                    "path": str(args.exe_a),
                    "sha256": file_sha256(args.exe_a),
                    "extra": args.extra_a,
                },
                "b": {
                    "path": str(args.exe_b),
                    "sha256": file_sha256(args.exe_b),
                    "extra": args.extra_b,
                },
            },
        })
        suites = {"workload": workload, "control": control}

        print(
            f"A/A calibration: {args.ref_a} against identical binary",
            flush=True)
        aa_rows = run_phase(
            {"a": args.exe_a, "b": args.exe_a},
            {"a": args.extra_a, "b": args.extra_a},
            suites, args.corpus, args.test_src, calls, args.runs,
            args.timeout, args.min_net_cpu_seconds, "A/A")
        aa_summary = phase_summary(aa_rows)
        aa_noise_floor = phase_noise_floor(aa_summary)

        print(
            f"B/B calibration: {args.ref_b} against identical binary",
            flush=True)
        bb_rows = run_phase(
            {"a": args.exe_b, "b": args.exe_b},
            {"a": args.extra_b, "b": args.extra_b},
            suites, args.corpus, args.test_src, calls, args.runs,
            args.timeout, args.min_net_cpu_seconds, "B/B")
        bb_summary = phase_summary(bb_rows)
        bb_noise_floor = phase_noise_floor(bb_summary)

        print(f"A/B: {args.ref_a} against {args.ref_b}", flush=True)
        ab_rows = run_phase(
            {"a": args.exe_a, "b": args.exe_b},
            {"a": args.extra_a, "b": args.extra_b},
            suites, args.corpus, args.test_src, calls, args.runs,
            args.timeout, args.min_net_cpu_seconds, "A/B")
        ab_summary = phase_summary(ab_rows)
        ab_pair_mean_drift = ab_summary["pair_mean_drift_pct"]
        effective_floor = max(
            aa_noise_floor, bb_noise_floor, ab_pair_mean_drift)
        verdict = classify(ab_rows, effective_floor)

        report.update({
            "aa": {"samples": aa_rows, "summary": aa_summary},
            "bb": {"samples": bb_rows, "summary": bb_summary},
            "ab": {"samples": ab_rows, "summary": ab_summary},
            "aa_noise_floor_pct": aa_noise_floor,
            "bb_noise_floor_pct": bb_noise_floor,
            "ab_pair_mean_drift_pct": ab_pair_mean_drift,
            "effective_floor_pct": effective_floor,
            "verdict": verdict,
        })
        write_report(args.out, report)
        print(
            f"effective floor {effective_floor:.3f}% "
            f"(A/A {aa_noise_floor:.3f}%, B/B {bb_noise_floor:.3f}%, "
            f"A/B drift {ab_pair_mean_drift:.3f}%); mean paired A/B "
            f"{ab_summary['mean_paired_delta_pct']:+.3f}%; {verdict}",
            flush=True,
        )
        return 0 if verdict == "improvement" else 1
    except BenchmarkError as error:
        report["aborted"] = str(error)
        report["verdict"] = "invalid"
        write_report(args.out, report)
        print(f"benchmark invalid: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
