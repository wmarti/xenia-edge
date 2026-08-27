#!/usr/bin/env python3
"""Fail-closed paired analysis for warmed guest-invocation replay.

Each subprocess must emit exactly one canonical
XENIA_GUEST_INVOCATION_BENCHMARK_V2 line. The metric is current-thread CPU
nanoseconds for a fixed invocation batch; reset work is included. A separate
post-primary reset-only batch is reported as a raw diagnostic and is never
subtracted from the primary metric.
This tool reports per-invocation CPU cost, not whole-title performance.

Four deterministic phases are collected: A/A and B/B controls, then the real
comparison in both A/B and B/A execution order. By default a phase is invalid
if its control noise or pair-mean drift exceeds 1%. The threshold is explicit
through --max-control-noise-pct. A result is resolved only when both
cross-orders agree in sign and every comparison pair clears the measured
control and drift floor in that sign.
"""

import argparse
import hashlib
import json
import math
import os
import pathlib
import platform
import re
import signal
import statistics
import subprocess
import sys
import tempfile


SCHEMA = "xenia-guest-invocation-result-v2"
METRIC_PREFIX = "XENIA_GUEST_INVOCATION_BENCHMARK_V2"
PAGE_SIZE = 4096
MAX_PAIRS = 64
METRIC_FIELDS = (
    "artifact_sha256",
    "corpus_sha256",
    "capture_build_sha256",
    "candidate_build_sha256",
    "config_sha256",
    "iterations",
    "reset_pages",
    "reset_bytes_per_iteration",
    "thread_cpu_ns",
    "uptime_raw_ns",
    "reset_only_thread_cpu_ns",
    "reset_only_uptime_raw_ns",
    "placement_generation_before",
    "placement_generation_after",
    "warm_verified",
    "timed_exit_verified",
    "final_verified",
)
SHA_FIELDS = frozenset((
    "artifact_sha256",
    "corpus_sha256",
    "capture_build_sha256",
    "candidate_build_sha256",
    "config_sha256",
))
POSITIVE_FIELDS = frozenset((
    "iterations",
    "thread_cpu_ns",
    "uptime_raw_ns",
    "reset_only_uptime_raw_ns",
))
NONNEGATIVE_FIELDS = frozenset((
    "reset_pages",
    "reset_bytes_per_iteration",
    "reset_only_thread_cpu_ns",
    "placement_generation_before",
    "placement_generation_after",
))
VERIFICATION_FIELDS = frozenset((
    "warm_verified",
    "timed_exit_verified",
    "final_verified",
))
PHASE_ROLES = {
    "aa": ("a", "a"),
    "bb": ("b", "b"),
    "ab": ("a", "b"),
    "ba": ("b", "a"),
}
PHASE_SCHEDULE = (
    ("aa", "bb", "ba", "ab"),
    ("bb", "ab", "aa", "ba"),
    ("ab", "ba", "bb", "aa"),
    ("ba", "aa", "ab", "bb"),
)
FIXED_RUNNER_FLAGS = (
    "--test_benchmark_warmed=false",
    "--guest_scheduler=false",
    "--log_safepoint_pc=false",
    "--jit_corpus_allow_incomplete=false",
    "--count_call_paths=false",
    "--count_physical_remap_hits=false",
    "--emit_mmio_aware_stores_for_recorded_exception_addresses=false",
    "--enable_early_precompilation=false",
    "--fold_readonly_guest_memory_loads=false",
    "--inline_mmio_access=false",
    "--serialize_guest_function_definitions=true",
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


def _parse_canonical_uint(field, value, positive):
    pattern = r"[1-9][0-9]*" if positive else r"0|[1-9][0-9]*"
    if not re.fullmatch(pattern, value):
        requirement = "positive" if positive else "nonnegative"
        raise BenchmarkError(f"{field} must be a canonical {requirement} integer")
    return int(value)


def parse_metric(output):
    """Parse exactly one strict, complete benchmark marker."""
    lines = [
        line for line in output.splitlines()
        if line.startswith(METRIC_PREFIX)
    ]
    if len(lines) != 1:
        raise BenchmarkError(
            f"expected exactly one {METRIC_PREFIX} line, got {len(lines)}")

    parts = lines[0].split("\t")
    if parts[0] != METRIC_PREFIX or len(parts) != len(METRIC_FIELDS) + 1:
        raise BenchmarkError("benchmark marker has missing or unknown fields")

    metric = {}
    for token, expected_field in zip(parts[1:], METRIC_FIELDS):
        field, separator, value = token.partition("=")
        if separator != "=" or field != expected_field or not value:
            raise BenchmarkError(
                "benchmark marker fields must be complete, unique, and in "
                "canonical order")
        if field in SHA_FIELDS:
            if not re.fullmatch(r"[0-9a-f]{64}", value):
                raise BenchmarkError(f"{field} must be lowercase SHA-256")
            metric[field] = value
        elif field in POSITIVE_FIELDS:
            metric[field] = _parse_canonical_uint(field, value, True)
        elif field in NONNEGATIVE_FIELDS:
            metric[field] = _parse_canonical_uint(field, value, False)
        elif field in VERIFICATION_FIELDS:
            if value != "1":
                raise BenchmarkError(f"{field} must be 1")
            metric[field] = 1
        else:  # Guard additions to METRIC_FIELDS without validation rules.
            raise BenchmarkError(f"no parser for benchmark field {field}")
    return metric


def validate_metric(metric, expected, min_thread_cpu_ns):
    for field in ("artifact_sha256", "corpus_sha256",
                  "capture_build_sha256", "candidate_build_sha256",
                  "config_sha256", "iterations", "reset_pages"):
        if metric[field] != expected[field]:
            raise BenchmarkError(
                f"{field} mismatch: expected {expected[field]}, "
                f"got {metric[field]}")

    expected_reset_bytes = expected["reset_pages"] * PAGE_SIZE
    if metric["reset_bytes_per_iteration"] != expected_reset_bytes:
        raise BenchmarkError(
            "reset accounting mismatch: expected "
            f"{expected_reset_bytes} bytes for {expected['reset_pages']} pages, "
            f"got {metric['reset_bytes_per_iteration']}")
    if (metric["placement_generation_before"] !=
            metric["placement_generation_after"]):
        raise BenchmarkError(
            "code placement generation changed in the timed region: "
            f"{metric['placement_generation_before']} -> "
            f"{metric['placement_generation_after']}")
    if metric["thread_cpu_ns"] < min_thread_cpu_ns:
        raise BenchmarkError(
            "current-thread CPU interval is too short: "
            f"{metric['thread_cpu_ns']} ns < {min_thread_cpu_ns} ns; "
            "increase --iterations")

    metric = dict(metric)
    metric["thread_cpu_ns_per_invocation"] = (
        metric["thread_cpu_ns"] / metric["iterations"])
    metric["uptime_raw_ns_per_invocation"] = (
        metric["uptime_raw_ns"] / metric["iterations"])
    metric["reset_only_thread_cpu_ns_per_invocation"] = (
        metric["reset_only_thread_cpu_ns"] / metric["iterations"])
    metric["reset_only_uptime_raw_ns_per_invocation"] = (
        metric["reset_only_uptime_raw_ns"] / metric["iterations"])
    return metric


def build_command(exe, extra, artifact, corpus, iterations):
    return [
        str(exe),
        *extra,
        *FIXED_RUNNER_FLAGS,
        f"--guest_invocation_in={artifact}",
        f"--jit_corpus_in={corpus}",
        f"--guest_invocation_iterations={iterations}",
    ]


def expected_for_role(expected_common, executable_sha256, role):
    expected = dict(expected_common)
    expected["candidate_build_sha256"] = executable_sha256[role]
    return expected


def run_once(exe, extra, artifact, corpus, expected, timeout,
             min_thread_cpu_ns):
    command = build_command(
        exe, extra, artifact, corpus, expected["iterations"])
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
        raise BenchmarkError(f"timeout running {exe}") from error
    except OSError as error:
        raise BenchmarkError(f"unable to launch {exe}: {error}") from error

    output = process.stdout + "\n" + process.stderr
    if process.returncode < 0:
        signal_number = -process.returncode
        try:
            signal_name = signal.Signals(signal_number).name
        except ValueError:
            signal_name = "unknown signal"
        tail = "\n".join(output.splitlines()[-20:])
        raise BenchmarkError(
            "guest invocation subprocess terminated by "
            f"{signal_name} ({signal_number}) with {exe}:\n{tail}")
    if process.returncode != 0:
        tail = "\n".join(output.splitlines()[-20:])
        raise BenchmarkError(
            f"guest invocation exited {process.returncode} with {exe}:\n{tail}")
    return validate_metric(
        parse_metric(output), expected, min_thread_cpu_ns)


def phase_order(pair_index):
    """Balance phase positions and predecessor transitions per four pairs."""
    return PHASE_SCHEDULE[pair_index % len(PHASE_SCHEDULE)]


def collect_phases(run_role, pairs):
    samples = {phase: [] for phase in PHASE_ROLES}
    for pair_index in range(pairs):
        for phase in phase_order(pair_index):
            first_role, second_role = PHASE_ROLES[phase]
            first = run_role(first_role)
            second = run_role(second_role)
            samples[phase].append({
                "pair": pair_index + 1,
                "first_role": first_role,
                "second_role": second_role,
                "first": first,
                "second": second,
            })
            print(
                f"{phase.upper()} pair {pair_index + 1}/{pairs}: "
                f"{first['thread_cpu_ns_per_invocation']:.3f} -> "
                f"{second['thread_cpu_ns_per_invocation']:.3f} "
                "current-thread CPU ns/invocation",
                flush=True,
            )
    return samples


def _relative_delta_pct(before, after):
    if before <= 0 or after <= 0:
        raise BenchmarkError("comparison contains nonpositive work")
    return 100.0 * (after - before) / before


def _drift_pct(values):
    if not values or min(values) <= 0:
        raise BenchmarkError("phase contains no positive work")
    return 100.0 * (max(values) - min(values)) / min(values)


def control_summary(samples):
    if not samples:
        raise BenchmarkError("control phase contains no pairs")
    first = [
        row["first"]["thread_cpu_ns_per_invocation"] for row in samples
    ]
    second = [
        row["second"]["thread_cpu_ns_per_invocation"] for row in samples
    ]
    deltas = [
        _relative_delta_pct(a_value, b_value)
        for a_value, b_value in zip(first, second)
    ]
    pair_means = [
        (a_value + b_value) / 2.0
        for a_value, b_value in zip(first, second)
    ]
    max_abs_delta = max(abs(delta) for delta in deltas)
    drift = _drift_pct(pair_means)
    return {
        "median_first_thread_cpu_ns_per_invocation": statistics.median(first),
        "median_second_thread_cpu_ns_per_invocation": statistics.median(second),
        "mean_paired_delta_pct": statistics.mean(deltas),
        "max_abs_paired_delta_pct": max_abs_delta,
        "pair_mean_drift_pct": drift,
        "noise_pct": max(max_abs_delta, drift),
        "paired_deltas_pct": deltas,
    }


def comparison_summary(samples, phase):
    if phase not in ("ab", "ba") or not samples:
        raise BenchmarkError("comparison phase must be nonempty A/B or B/A")
    a_values = []
    b_values = []
    for row in samples:
        if phase == "ab":
            a_metric, b_metric = row["first"], row["second"]
        else:
            b_metric, a_metric = row["first"], row["second"]
        a_values.append(a_metric["thread_cpu_ns_per_invocation"])
        b_values.append(b_metric["thread_cpu_ns_per_invocation"])
    deltas = [
        _relative_delta_pct(a_value, b_value)
        for a_value, b_value in zip(a_values, b_values)
    ]
    pair_means = [
        (a_value + b_value) / 2.0
        for a_value, b_value in zip(a_values, b_values)
    ]
    return {
        "a_thread_cpu_ns_per_invocation": a_values,
        "b_thread_cpu_ns_per_invocation": b_values,
        "median_a_thread_cpu_ns_per_invocation": statistics.median(a_values),
        "median_b_thread_cpu_ns_per_invocation": statistics.median(b_values),
        "mean_b_vs_a_delta_pct": statistics.mean(deltas),
        "pair_mean_drift_pct": _drift_pct(pair_means),
        "paired_b_vs_a_deltas_pct": deltas,
    }


def _sign(value):
    return (value > 0) - (value < 0)


def analyze(samples, max_control_noise_pct):
    aa = control_summary(samples["aa"])
    bb = control_summary(samples["bb"])
    for name, summary in (("A/A", aa), ("B/B", bb)):
        if summary["noise_pct"] > max_control_noise_pct:
            raise BenchmarkError(
                f"{name} control noise {summary['noise_pct']:.6f}% exceeds "
                f"threshold {max_control_noise_pct:.6f}%")

    ab = comparison_summary(samples["ab"], "ab")
    ba = comparison_summary(samples["ba"], "ba")
    for name, summary in (("A/B", ab), ("B/A", ba)):
        if summary["pair_mean_drift_pct"] > max_control_noise_pct:
            raise BenchmarkError(
                f"{name} pair-mean drift "
                f"{summary['pair_mean_drift_pct']:.6f}% exceeds threshold "
                f"{max_control_noise_pct:.6f}%")
    ab_mean = ab["mean_b_vs_a_delta_pct"]
    ba_mean = ba["mean_b_vs_a_delta_pct"]
    if _sign(ab_mean) != _sign(ba_mean):
        raise BenchmarkError(
            "A/B and B/A execution orders disagree in canonical B-vs-A sign: "
            f"{ab_mean:+.6f}% vs {ba_mean:+.6f}%")

    effective_floor = max(
        aa["noise_pct"], bb["noise_pct"],
        ab["pair_mean_drift_pct"], ba["pair_mean_drift_pct"])
    all_deltas = (
        ab["paired_b_vs_a_deltas_pct"] +
        ba["paired_b_vs_a_deltas_pct"])
    cross_order_means_clear_floor = (
        abs(ab_mean) > effective_floor and abs(ba_mean) > effective_floor)
    if (all(delta < -effective_floor for delta in all_deltas) and
            cross_order_means_clear_floor):
        verdict = "improvement"
    elif (all(delta > effective_floor for delta in all_deltas) and
          cross_order_means_clear_floor):
        verdict = "regression"
    else:
        verdict = "unresolved"

    return {
        "aa": aa,
        "bb": bb,
        "ab": ab,
        "ba": ba,
        "effective_noise_floor_pct": effective_floor,
        "median_cross_order_a_thread_cpu_ns_per_invocation": statistics.median(
            ab["a_thread_cpu_ns_per_invocation"] +
            ba["a_thread_cpu_ns_per_invocation"]),
        "median_cross_order_b_thread_cpu_ns_per_invocation": statistics.median(
            ab["b_thread_cpu_ns_per_invocation"] +
            ba["b_thread_cpu_ns_per_invocation"]),
        "mean_cross_order_b_vs_a_delta_pct": statistics.mean(
            (ab_mean, ba_mean)),
        "verdict": verdict,
    }


def write_report(path, report):
    if not path:
        return
    payload = json.dumps(
        report, allow_nan=False, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=path.parent,
                prefix=f".{path.name}.", suffix=".tmp", delete=False) as stream:
            temporary_path = pathlib.Path(stream.name)
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def validate_output_path(output, inputs):
    if not output:
        return
    try:
        resolved_output = output.resolve()
        for input_path in inputs:
            if resolved_output == input_path.resolve():
                raise BenchmarkError(
                    f"--out aliases benchmark input {input_path}")
            if (output.exists() and input_path.exists() and
                    output.samefile(input_path)):
                raise BenchmarkError(
                    f"--out aliases benchmark input {input_path}")
    except RuntimeError as error:
        raise BenchmarkError(
            f"unable to resolve --out safely: {error}") from error


def validate_positive_finite(name, value):
    if not math.isfinite(value) or value <= 0:
        raise BenchmarkError(f"{name} must be finite and positive")


def _validate_sha256_argument(name, value):
    if not re.fullmatch(r"[0-9a-f]{64}", value):
        raise BenchmarkError(
            f"{name} must be 64 lowercase hexadecimal digits")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Paired warmed guest-invocation CPU benchmark analysis")
    parser.add_argument("--exe-a", required=True, type=pathlib.Path)
    parser.add_argument("--exe-b", required=True, type=pathlib.Path)
    parser.add_argument("--ref-a", default="A")
    parser.add_argument("--ref-b", default="B")
    parser.add_argument("--artifact", required=True, type=pathlib.Path)
    parser.add_argument("--corpus", required=True, type=pathlib.Path)
    parser.add_argument("--capture-build-sha256", required=True)
    parser.add_argument("--config-sha256", required=True)
    parser.add_argument("--iterations", required=True, type=int)
    parser.add_argument("--reset-pages", required=True, type=int)
    parser.add_argument(
        "--pairs", type=int, default=4,
        help=f"pairs per phase; multiple of 4, maximum {MAX_PAIRS}")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--min-thread-cpu-seconds", type=float, default=0.05,
        help="minimum current-thread CPU time in every fixed-work batch")
    parser.add_argument(
        "--max-control-noise-pct", type=float, default=1.0,
        help="maximum control noise or phase pair-mean drift (default 1%%)")
    parser.add_argument("--extra-a", action="append", default=[])
    parser.add_argument("--extra-b", action="append", default=[])
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args(argv)

    report = {
        "schema": SCHEMA,
        "refs": {"a": args.ref_a, "b": args.ref_b},
        "aborted": None,
    }
    safe_output = None
    try:
        validate_output_path(
            args.out, (args.exe_a, args.exe_b, args.artifact, args.corpus))
        safe_output = args.out
        capture_build_sha256 = _validate_sha256_argument(
            "--capture-build-sha256", args.capture_build_sha256)
        config_sha256 = _validate_sha256_argument(
            "--config-sha256", args.config_sha256)
        if args.iterations <= 0 or args.reset_pages < 0:
            raise BenchmarkError(
                "iterations must be positive and reset pages nonnegative")
        if args.pairs < 4 or args.pairs > MAX_PAIRS or args.pairs % 4:
            raise BenchmarkError(
                "--pairs must be a multiple of 4 between 4 and "
                f"{MAX_PAIRS}")
        validate_positive_finite("--timeout", args.timeout)
        validate_positive_finite(
            "--min-thread-cpu-seconds", args.min_thread_cpu_seconds)
        validate_positive_finite(
            "--max-control-noise-pct", args.max_control_noise_pct)
        for path in (args.exe_a, args.exe_b, args.artifact, args.corpus):
            if not path.is_file():
                raise BenchmarkError(f"missing input: {path}")
        for exe in (args.exe_a, args.exe_b):
            if not os.access(exe, os.X_OK):
                raise BenchmarkError(f"executable is not runnable: {exe}")

        artifact_sha256 = file_sha256(args.artifact)
        corpus_sha256 = file_sha256(args.corpus)
        driver_path = pathlib.Path(__file__).resolve()
        driver_sha256 = file_sha256(driver_path)
        executable_sha256 = {
            "a": file_sha256(args.exe_a),
            "b": file_sha256(args.exe_b),
        }
        if executable_sha256["a"] == executable_sha256["b"]:
            raise BenchmarkError(
                "A and B executables have identical SHA-256 digests; "
                "an optimization comparison requires distinct binaries")
        expected_common = {
            "artifact_sha256": artifact_sha256,
            "corpus_sha256": corpus_sha256,
            "capture_build_sha256": capture_build_sha256,
            "config_sha256": config_sha256,
            "iterations": args.iterations,
            "reset_pages": args.reset_pages,
        }
        minimum_cpu_ns = int(args.min_thread_cpu_seconds * 1e9)
        if minimum_cpu_ns <= 0:
            raise BenchmarkError(
                "minimum CPU time rounds to a nonpositive nanosecond interval")
        exes = {"a": args.exe_a, "b": args.exe_b}
        extras = {"a": args.extra_a, "b": args.extra_b}

        def run_role(role):
            expected = expected_for_role(
                expected_common, executable_sha256, role)
            return run_once(
                exes[role], extras[role], args.artifact, args.corpus,
                expected, args.timeout, minimum_cpu_ns)

        samples = collect_phases(run_role, args.pairs)
        analysis = analyze(samples, args.max_control_noise_pct)

        if (file_sha256(args.artifact) != artifact_sha256 or
                file_sha256(args.corpus) != corpus_sha256 or
                file_sha256(args.exe_a) != executable_sha256["a"] or
                file_sha256(args.exe_b) != executable_sha256["b"] or
                file_sha256(driver_path) != driver_sha256):
            raise BenchmarkError("an input changed during the benchmark")

        report.update({
            "inputs": {
                "artifact": str(args.artifact),
                "artifact_sha256": artifact_sha256,
                "corpus": str(args.corpus),
                "corpus_sha256": corpus_sha256,
                "capture_build_sha256": capture_build_sha256,
                "config_sha256": config_sha256,
                "executables": {
                    "a": {
                        "path": str(args.exe_a),
                        "candidate_build_sha256": executable_sha256["a"],
                        "extra": args.extra_a,
                    },
                    "b": {
                        "path": str(args.exe_b),
                        "candidate_build_sha256": executable_sha256["b"],
                        "extra": args.extra_b,
                    },
                },
            },
            "environment": {
                "driver": str(driver_path),
                "driver_sha256": driver_sha256,
                "host": platform.node(),
                "machine": platform.machine(),
                "platform": platform.platform(),
                "python": platform.python_version(),
                "working_directory": str(pathlib.Path.cwd()),
            },
            "measurement": {
                "primary": "current_thread_cpu_ns_per_invocation",
                "diagnostic": "uptime_raw_ns_per_invocation",
                "iterations_per_batch": args.iterations,
                "reset_pages_per_invocation": args.reset_pages,
                "reset_bytes_per_invocation": args.reset_pages * PAGE_SIZE,
                "reset_included_in_primary": True,
                "reset_only_diagnostic": "separate_post_primary_batch",
                "reset_only_subtracted_from_primary": False,
                "clock_boundary_order": (
                    "wall_start,cpu_start,work,cpu_end,wall_end"),
                "pairs_per_phase": args.pairs,
                "min_thread_cpu_ns_per_batch": minimum_cpu_ns,
                "max_control_noise_pct": args.max_control_noise_pct,
            },
            "samples": samples,
            "analysis": analysis,
            "verdict": analysis["verdict"],
        })
        write_report(safe_output, report)
        print(
            "A median "
            f"{analysis['median_cross_order_a_thread_cpu_ns_per_invocation']:.3f}, "
            "B median "
            f"{analysis['median_cross_order_b_thread_cpu_ns_per_invocation']:.3f} "
            "current-thread CPU ns/invocation; cross-order delta "
            f"{analysis['mean_cross_order_b_vs_a_delta_pct']:+.3f}%; "
            f"effective noise floor "
            f"{analysis['effective_noise_floor_pct']:.3f}%; "
            f"{analysis['verdict']}",
            flush=True,
        )
        return 0 if analysis["verdict"] == "improvement" else 1
    except (BenchmarkError, OSError, UnicodeError,
            ArithmeticError, ValueError) as error:
        report["aborted"] = str(error)
        report["verdict"] = "invalid"
        try:
            write_report(safe_output, report)
        except (OSError, UnicodeError, ValueError) as report_error:
            print(
                f"unable to write invalid report: {report_error}",
                file=sys.stderr,
                flush=True,
            )
        print(f"benchmark invalid: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
