#!/usr/bin/env python3
"""Focused unit tests for warmed guest-invocation benchmark analysis."""

import io
import json
import pathlib
import tempfile
import types
import unittest
from unittest import mock

from tools.bench import bench_guest_invocation as benchmark


SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64
SHA_D = "d" * 64
SHA_E = "e" * 64


def metric_values(**overrides):
    values = {
        "artifact_sha256": SHA_A,
        "corpus_sha256": SHA_B,
        "capture_build_sha256": SHA_C,
        "candidate_build_sha256": SHA_D,
        "config_sha256": SHA_E,
        "iterations": 100,
        "reset_pages": 2,
        "reset_bytes_per_iteration": 8192,
        "thread_cpu_ns": 100000,
        "uptime_raw_ns": 120000,
        "reset_only_thread_cpu_ns": 20000,
        "reset_only_uptime_raw_ns": 30000,
        "placement_generation_before": 7,
        "placement_generation_after": 7,
        "warm_verified": 1,
        "timed_exit_verified": 1,
        "final_verified": 1,
    }
    values.update(overrides)
    return values


def metric_line(**overrides):
    values = metric_values(**overrides)
    fields = "\t".join(
        f"{field}={values[field]}" for field in benchmark.METRIC_FIELDS)
    return f"{benchmark.METRIC_PREFIX}\t{fields}"


def expected(**overrides):
    values = {
        "artifact_sha256": SHA_A,
        "corpus_sha256": SHA_B,
        "capture_build_sha256": SHA_C,
        "candidate_build_sha256": SHA_D,
        "config_sha256": SHA_E,
        "iterations": 100,
        "reset_pages": 2,
    }
    values.update(overrides)
    return values


def timed_metric(ns_per_invocation):
    values = metric_values(
        thread_cpu_ns=int(ns_per_invocation * 100),
        uptime_raw_ns=int(ns_per_invocation * 120),
    )
    values["thread_cpu_ns_per_invocation"] = ns_per_invocation
    values["uptime_raw_ns_per_invocation"] = ns_per_invocation * 1.2
    values["reset_only_thread_cpu_ns_per_invocation"] = 200.0
    values["reset_only_uptime_raw_ns_per_invocation"] = 300.0
    return values


def sample(first, second, first_role="a", second_role="b"):
    return {
        "pair": 1,
        "first_role": first_role,
        "second_role": second_role,
        "first": timed_metric(first),
        "second": timed_metric(second),
    }


def phase_samples(aa, bb, ab, ba):
    return {
        "aa": [sample(*values, "a", "a") for values in aa],
        "bb": [sample(*values, "b", "b") for values in bb],
        "ab": [sample(*values, "a", "b") for values in ab],
        "ba": [sample(*values, "b", "a") for values in ba],
    }


class MarkerParserTest(unittest.TestCase):
    def test_accepts_exact_canonical_marker(self):
        parsed = benchmark.parse_metric(
            "diagnostic\n" + metric_line() + "\nmore")
        self.assertEqual(parsed, metric_values())

    def test_rejects_superseded_v1_marker(self):
        legacy = metric_line().replace(
            benchmark.METRIC_PREFIX,
            "XENIA_GUEST_INVOCATION_BENCHMARK_V1",
            1,
        )
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.parse_metric(legacy)

    def test_rejects_missing_duplicate_and_unknown_marker_fields(self):
        canonical = metric_line()
        parts = canonical.split("\t")
        invalid = (
            "no marker",
            canonical + "\n" + canonical,
            "\t".join(parts[:-1]),
            canonical + "\tunknown=1",
            "\t".join(parts[:-1] + [parts[-2]]),
            "\t".join(parts[:2] + [parts[3], parts[2]] + parts[4:]),
        )
        for output in invalid:
            with self.subTest(output=output):
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.parse_metric(output)

    def test_rejects_noncanonical_values_and_failed_verification(self):
        invalid = (
            metric_line(iterations=0),
            metric_line(thread_cpu_ns=0),
            metric_line(uptime_raw_ns=0),
            metric_line(reset_only_uptime_raw_ns=0),
            metric_line(iterations="0100"),
            metric_line(artifact_sha256="A" * 64),
            metric_line(warm_verified=0),
            metric_line(timed_exit_verified=2),
            metric_line(final_verified="true"),
        )
        for output in invalid:
            with self.subTest(output=output):
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.parse_metric(output)


class MetricValidationTest(unittest.TestCase):
    def test_accepts_matching_provenance_and_reports_per_invocation(self):
        parsed = benchmark.validate_metric(
            benchmark.parse_metric(metric_line()), expected(), 50000)
        self.assertEqual(parsed["thread_cpu_ns_per_invocation"], 1000.0)
        self.assertEqual(parsed["uptime_raw_ns_per_invocation"], 1200.0)
        self.assertEqual(
            parsed["reset_only_thread_cpu_ns_per_invocation"], 200.0)
        self.assertEqual(
            parsed["reset_only_uptime_raw_ns_per_invocation"], 300.0)

        zero_resolution_reset = benchmark.validate_metric(
            benchmark.parse_metric(metric_line(reset_only_thread_cpu_ns=0)),
            expected(),
            50000,
        )
        self.assertEqual(
            zero_resolution_reset["reset_only_thread_cpu_ns_per_invocation"],
            0.0,
        )

    def test_rejects_identity_iteration_and_reset_mismatches(self):
        cases = (
            (metric_values(artifact_sha256="f" * 64), expected()),
            (metric_values(corpus_sha256="f" * 64), expected()),
            (metric_values(capture_build_sha256="f" * 64), expected()),
            (metric_values(candidate_build_sha256="f" * 64), expected()),
            (metric_values(config_sha256="f" * 64), expected()),
            (metric_values(iterations=99), expected()),
            (metric_values(reset_pages=3), expected()),
            (metric_values(reset_bytes_per_iteration=4096), expected()),
        )
        for metric, wanted in cases:
            with self.subTest(metric=metric):
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.validate_metric(metric, wanted, 1)

    def test_allows_ab_builds_to_differ_but_pins_each_role(self):
        expected_a = expected(candidate_build_sha256=SHA_D)
        expected_b = expected(candidate_build_sha256="f" * 64)
        metric_a = metric_values(candidate_build_sha256=SHA_D)
        metric_b = metric_values(candidate_build_sha256="f" * 64)

        benchmark.validate_metric(metric_a, expected_a, 1)
        benchmark.validate_metric(metric_b, expected_b, 1)
        with self.assertRaisesRegex(
                benchmark.BenchmarkError, "candidate_build_sha256 mismatch"):
            benchmark.validate_metric(metric_b, expected_a, 1)

    def test_rejects_placement_change_and_short_cpu_interval(self):
        with self.assertRaisesRegex(
                benchmark.BenchmarkError, "placement generation changed"):
            benchmark.validate_metric(
                metric_values(placement_generation_after=8), expected(), 1)
        with self.assertRaisesRegex(
                benchmark.BenchmarkError, "too short"):
            benchmark.validate_metric(metric_values(), expected(), 100001)


class RunnerTest(unittest.TestCase):
    def _main_args(self):
        return [
            "--exe-a", "missing-a",
            "--exe-b", "missing-b",
            "--artifact", "missing-artifact",
            "--corpus", "missing-corpus",
            "--capture-build-sha256", SHA_C,
            "--config-sha256", SHA_E,
            "--iterations", "1",
            "--reset-pages", "0",
        ]

    def test_main_rejects_nonfinite_float_before_inputs(self):
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr):
            result = benchmark.main(
                self._main_args() + ["--max-control-noise-pct", "nan"])
        self.assertEqual(result, 2)
        self.assertIn("finite and positive", stderr.getvalue())

    def test_main_rejects_excessive_pairs_before_work(self):
        stderr = io.StringIO()
        with mock.patch.object(benchmark, "file_sha256") as file_sha256, \
                mock.patch.object(benchmark, "collect_phases") as collect, \
                mock.patch("sys.stderr", stderr):
            result = benchmark.main(
                self._main_args() + ["--pairs", str(benchmark.MAX_PAIRS + 4)])
        self.assertEqual(result, 2)
        self.assertIn("between 4 and", stderr.getvalue())
        file_sha256.assert_not_called()
        collect.assert_not_called()

    def test_main_refuses_output_alias_before_writing(self):
        with tempfile.TemporaryDirectory(dir=".") as directory:
            alias = pathlib.Path(directory) / "never-created"
            args = self._main_args()
            artifact_index = args.index("--artifact") + 1
            args[artifact_index] = str(alias)
            stderr = io.StringIO()
            with mock.patch("sys.stderr", stderr):
                result = benchmark.main(args + ["--out", str(alias)])
            self.assertEqual(result, 2)
            self.assertIn("aliases benchmark input", stderr.getvalue())
            self.assertFalse(alias.exists())

    def test_main_rejects_identical_candidate_binaries(self):
        def fake_sha256(path):
            if str(path) in ("missing-a", "missing-b"):
                return SHA_D
            return SHA_A

        stderr = io.StringIO()
        with mock.patch.object(
                pathlib.Path, "is_file", return_value=True), \
                mock.patch.object(benchmark.os, "access", return_value=True), \
                mock.patch.object(
                    benchmark, "file_sha256", side_effect=fake_sha256), \
                mock.patch.object(benchmark, "collect_phases") as collect, \
                mock.patch("sys.stderr", stderr):
            result = benchmark.main(self._main_args())

        self.assertEqual(result, 2)
        self.assertIn("identical SHA-256", stderr.getvalue())
        collect.assert_not_called()

    def test_main_writes_successful_improvement_report(self):
        samples = phase_samples(
            aa=[(100.0, 100.0)] * 4,
            bb=[(95.0, 95.0)] * 4,
            ab=[(100.0, 95.0)] * 4,
            ba=[(95.0, 100.0)] * 4,
        )
        hashes = {
            "missing-artifact": SHA_A,
            "missing-corpus": SHA_B,
            "missing-a": SHA_D,
            "missing-b": "f" * 64,
        }

        def fake_sha256(path):
            return hashes.get(str(path), "9" * 64)

        with tempfile.TemporaryDirectory(dir=".") as directory:
            output = pathlib.Path(directory) / "result.json"
            with mock.patch.object(
                    pathlib.Path, "is_file", return_value=True), \
                    mock.patch.object(benchmark.os, "access", return_value=True), \
                    mock.patch.object(
                        benchmark, "file_sha256", side_effect=fake_sha256), \
                    mock.patch.object(
                        benchmark, "collect_phases", return_value=samples), \
                    mock.patch("builtins.print"):
                result = benchmark.main(
                    self._main_args() + ["--out", str(output)])

            report = json.loads(output.read_text())
            self.assertEqual(result, 0)
            self.assertEqual(
                report["schema"], "xenia-guest-invocation-result-v2")
            self.assertEqual(report["verdict"], "improvement")
            self.assertEqual(
                report["inputs"]["executables"]["a"]
                ["candidate_build_sha256"],
                SHA_D,
            )
            self.assertIn("driver_sha256", report["environment"])
            self.assertFalse(
                report["measurement"]["reset_only_subtracted_from_primary"])

    def test_role_mapping_pins_each_executable_hash(self):
        common = expected()
        common.pop("candidate_build_sha256")
        hashes = {"a": SHA_D, "b": "f" * 64}

        expected_a = benchmark.expected_for_role(common, hashes, "a")
        expected_b = benchmark.expected_for_role(common, hashes, "b")

        self.assertEqual(expected_a["candidate_build_sha256"], SHA_D)
        self.assertEqual(expected_b["candidate_build_sha256"], "f" * 64)
        self.assertEqual(
            expected_a["capture_build_sha256"],
            expected_b["capture_build_sha256"],
        )

    def test_fixed_identity_and_work_flags_follow_user_extras(self):
        process = types.SimpleNamespace(
            stdout=metric_line(), stderr="", returncode=0)
        with mock.patch.object(
                benchmark.subprocess, "run", return_value=process) as run:
            result = benchmark.run_once(
                "/tmp/a", [
                    "--guest_invocation_iterations=1",
                    "--guest_scheduler=true",
                    "--log_safepoint_pc=true",
                    "--emit_mmio_aware_stores_for_recorded_exception_addresses=true",
                    "--enable_early_precompilation=true",
                    "--fold_readonly_guest_memory_loads=true",
                    "--inline_mmio_access=true",
                    "--serialize_guest_function_definitions=false",
                ],
                "/tmp/input.xinv", "/tmp/input.jcorpus", expected(),
                10.0, 1)

        command = run.call_args.args[0]
        self.assertGreater(
            command.index("--guest_invocation_iterations=100"),
            command.index("--guest_invocation_iterations=1"))
        self.assertGreater(
            command.index("--guest_scheduler=false"),
            command.index("--guest_scheduler=true"))
        self.assertGreater(
            command.index(
                "--emit_mmio_aware_stores_for_recorded_exception_addresses=false"),
            command.index(
                "--emit_mmio_aware_stores_for_recorded_exception_addresses=true"),
        )
        for fixed, user in (
                ("--enable_early_precompilation=false",
                 "--enable_early_precompilation=true"),
                ("--fold_readonly_guest_memory_loads=false",
                 "--fold_readonly_guest_memory_loads=true"),
                ("--inline_mmio_access=false", "--inline_mmio_access=true"),
                ("--log_safepoint_pc=false", "--log_safepoint_pc=true"),
                ("--serialize_guest_function_definitions=true",
                 "--serialize_guest_function_definitions=false"),
        ):
            with self.subTest(fixed=fixed):
                self.assertGreater(command.index(fixed), command.index(user))
        self.assertEqual(command[-3:], [
            "--guest_invocation_in=/tmp/input.xinv",
            "--jit_corpus_in=/tmp/input.jcorpus",
            "--guest_invocation_iterations=100",
        ])
        self.assertEqual(result["thread_cpu_ns_per_invocation"], 1000.0)

    def test_rejects_nonzero_exit_signal_missing_marker_and_timeout(self):
        failed = types.SimpleNamespace(
            stdout="", stderr="failed", returncode=3)
        with mock.patch.object(
                benchmark.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(
                    benchmark.BenchmarkError, "exited 3"):
                benchmark.run_once(
                    "/tmp/a", [], "/tmp/a.xinv", "/tmp/a.jcorpus",
                    expected(), 1.0, 1)
        signaled = types.SimpleNamespace(
            stdout=metric_line(), stderr="faulted", returncode=-11)
        with mock.patch.object(
                benchmark.subprocess, "run", return_value=signaled):
            with self.assertRaisesRegex(
                    benchmark.BenchmarkError,
                    "subprocess terminated by SIGSEGV \\(11\\)"):
                benchmark.run_once(
                    "/tmp/a", [], "/tmp/a.xinv", "/tmp/a.jcorpus",
                    expected(), 1.0, 1)
        missing_marker = types.SimpleNamespace(
            stdout="completed without proof", stderr="", returncode=0)
        with mock.patch.object(
                benchmark.subprocess, "run", return_value=missing_marker):
            with self.assertRaisesRegex(
                    benchmark.BenchmarkError, "expected exactly one"):
                benchmark.run_once(
                    "/tmp/a", [], "/tmp/a.xinv", "/tmp/a.jcorpus",
                    expected(), 1.0, 1)
        with mock.patch.object(
                benchmark.subprocess, "run",
                side_effect=benchmark.subprocess.TimeoutExpired("cmd", 1)):
            with self.assertRaisesRegex(benchmark.BenchmarkError, "timeout"):
                benchmark.run_once(
                    "/tmp/a", [], "/tmp/a.xinv", "/tmp/a.jcorpus",
                    expected(), 1.0, 1)


class PhaseOrderTest(unittest.TestCase):
    def test_four_pair_block_balances_every_temporal_position(self):
        positions = [set() for _ in range(4)]
        for pair_index in range(4):
            for position, phase in enumerate(
                    benchmark.phase_order(pair_index)):
                positions[position].add(phase)
        self.assertEqual(
            positions, [set(benchmark.PHASE_ROLES)] * 4)

    def test_four_pair_block_balances_predecessor_transitions(self):
        transitions = set()
        for pair_index in range(4):
            order = benchmark.phase_order(pair_index)
            transitions.update(zip(order, order[1:]))
        phases = set(benchmark.PHASE_ROLES)
        expected_transitions = {
            (first, second)
            for first in phases
            for second in phases
            if first != second
        }
        self.assertEqual(transitions, expected_transitions)

    def test_collection_runs_all_four_explicit_pair_orders(self):
        roles = []

        def run_role(role):
            roles.append(role)
            return timed_metric(100.0 if role == "a" else 90.0)

        with mock.patch("builtins.print"):
            samples = benchmark.collect_phases(run_role, 4)
        self.assertEqual({name: len(rows) for name, rows in samples.items()}, {
            "aa": 4,
            "bb": 4,
            "ab": 4,
            "ba": 4,
        })
        self.assertEqual(len(roles), 32)
        for phase, phase_roles in benchmark.PHASE_ROLES.items():
            self.assertTrue(all(
                (row["first_role"], row["second_role"]) == phase_roles
                for row in samples[phase]))


class AnalysisTest(unittest.TestCase):
    def test_accepts_sign_consistent_improvement_beyond_controls(self):
        samples = phase_samples(
            aa=[(100.0, 100.1), (100.2, 100.1)],
            bb=[(90.0, 90.1), (90.2, 90.1)],
            ab=[(100.0, 95.0), (100.1, 95.1)],
            # B first, then A.
            ba=[(95.0, 100.0), (95.1, 100.1)],
        )
        analysis = benchmark.analyze(samples, 1.0)
        self.assertEqual(analysis["verdict"], "improvement")
        self.assertLess(analysis["mean_cross_order_b_vs_a_delta_pct"], 0)
        self.assertIn("median_a_thread_cpu_ns_per_invocation", analysis["ab"])

    def test_accepts_sign_consistent_regression_beyond_controls(self):
        samples = phase_samples(
            aa=[(100.0, 100.1), (100.2, 100.1)],
            bb=[(105.0, 105.1), (105.2, 105.1)],
            ab=[(100.0, 105.0), (100.1, 105.1)],
            ba=[(105.0, 100.0), (105.1, 100.1)],
        )
        self.assertEqual(
            benchmark.analyze(samples, 1.0)["verdict"], "regression")

    def test_rejects_noisy_controls(self):
        samples = phase_samples(
            aa=[(100.0, 102.0), (100.0, 100.0)],
            bb=[(100.0, 100.0), (100.0, 100.0)],
            ab=[(100.0, 95.0), (100.0, 95.0)],
            ba=[(95.0, 100.0), (95.0, 100.0)],
        )
        with self.assertRaisesRegex(
                benchmark.BenchmarkError, "control noise"):
            benchmark.analyze(samples, 1.0)

    def test_rejects_unstable_cross_order_phase(self):
        samples = phase_samples(
            aa=[(100.0, 100.0), (100.0, 100.0)],
            bb=[(95.0, 95.0), (95.0, 95.0)],
            ab=[(100.0, 95.0), (120.0, 114.0)],
            ba=[(95.0, 100.0), (95.0, 100.0)],
        )
        with self.assertRaisesRegex(
                benchmark.BenchmarkError, "pair-mean drift"):
            benchmark.analyze(samples, 1.0)

    def test_rejects_cross_order_sign_disagreement(self):
        samples = phase_samples(
            aa=[(100.0, 100.0), (100.0, 100.0)],
            bb=[(100.0, 100.0), (100.0, 100.0)],
            ab=[(100.0, 95.0), (100.0, 95.0)],
            # Canonically B is slower in this order.
            ba=[(105.0, 100.0), (105.0, 100.0)],
        )
        with self.assertRaisesRegex(
                benchmark.BenchmarkError, "disagree"):
            benchmark.analyze(samples, 1.0)

    def test_sign_consistent_result_inside_control_floor_is_unresolved(self):
        samples = phase_samples(
            aa=[(100.0, 100.4), (100.0, 99.6)],
            bb=[(100.0, 100.3), (100.0, 99.7)],
            ab=[(100.0, 99.8), (100.0, 99.8)],
            ba=[(99.8, 100.0), (99.8, 100.0)],
        )
        self.assertEqual(
            benchmark.analyze(samples, 1.0)["verdict"], "unresolved")

    def test_outlier_cannot_pull_weak_pairs_past_noise_floor(self):
        weak_a = 200.0 / (2.0 - 0.001)
        weak_b = 200.0 - weak_a
        strong_a = 200.0 / (2.0 - 0.08)
        strong_b = 200.0 - strong_a
        samples = phase_samples(
            aa=[(100.0, 100.5), (100.0, 99.5)] * 2,
            bb=[(100.0, 100.5), (100.0, 99.5)] * 2,
            ab=[(weak_a, weak_b)] * 3 + [(strong_a, strong_b)],
            ba=[(weak_b, weak_a)] * 3 + [(strong_b, strong_a)],
        )
        analysis = benchmark.analyze(samples, 1.0)
        self.assertLess(analysis["mean_cross_order_b_vs_a_delta_pct"], -1.0)
        self.assertEqual(analysis["verdict"], "unresolved")


if __name__ == "__main__":
    unittest.main()
