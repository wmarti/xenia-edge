#!/usr/bin/env python3
"""Focused static/unit checks for the warmed PPC call/return benchmark lane."""

import json
import pathlib
import tempfile
import types
import unittest
from unittest import mock

from tools.bench import bench_callret
from tools.bench import gen_loop_bench


def relative_branch_target(words, index):
    displacement = words[index] & 0x03FFFFFC
    if displacement & 0x02000000:
        displacement -= 0x04000000
    return index + displacement // 4


def metric_line(cpu=1234567, wall=2345678, before=9, after=9):
    return (
        f"{bench_callret.METRIC_PREFIX}\tthread_cpu_ns={cpu}"
        f"\tuptime_raw_ns={wall}"
        f"\tplacement_generation_before={before}"
        f"\tplacement_generation_after={after}"
    )


def passing_output(metric=None):
    metric = metric or metric_line()
    return (
        "Running 1 test suites, 1 test cases...\n"
        f"{metric}\n"
        "Total tests: 1\n"
        "Passed: 1\n"
        "Failed: 0\n"
    )


def result_row(a_ns, b_ns):
    return {
        "arms": {
            "a": {"net_thread_cpu_ns_per_call": a_ns},
            "b": {"net_thread_cpu_ns_per_call": b_ns},
        },
        "delta_pct": 100.0 * (b_ns - a_ns) / a_ns,
    }


class CallretGeneratorTest(unittest.TestCase):
    def test_linked_calls_target_leaf_and_preserve_outer_lr(self):
        width = 3
        words, execution = gen_loop_bench.build_callret(width, 2)
        leaf_index = len(words) - 1

        self.assertEqual(words[0], gen_loop_bench.MFLR_R12)
        self.assertEqual(words[-3], gen_loop_bench.MTLR_R12)
        self.assertEqual(words[-2], gen_loop_bench.BLR)
        self.assertEqual(words[-1], gen_loop_bench.BLR)
        for index in range(1, 1 + 2 * width):
            self.assertEqual(words[index] & 1, 1)
            self.assertEqual(relative_branch_target(words, index), leaf_index)

        self.assertEqual(execution["preloop_calls"], 3)
        self.assertEqual(execution["loop_calls"], 6)
        self.assertEqual(execution["calls_per_invocation"], 9)

    def test_control_has_same_outer_shape_and_no_linked_branch(self):
        width = 4
        workload, _ = gen_loop_bench.build_callret(width, 7)
        control = gen_loop_bench.build_callret_control(width, 7)

        self.assertEqual(control[0], gen_loop_bench.MFLR_R12)
        self.assertEqual(
            control[-2:],
            [gen_loop_bench.MTLR_R12, gen_loop_bench.BLR],
        )
        self.assertEqual(
            control[1:1 + 2 * width],
            [gen_loop_bench.NOP] * (2 * width),
        )
        self.assertEqual(len(workload), len(control) + 1)

    def test_branch_rejects_unaligned_and_out_of_range(self):
        with self.assertRaises(ValueError):
            gen_loop_bench.branch(2, link=True)
        with self.assertRaises(ValueError):
            gen_loop_bench.branch(1 << 25, link=True)
        self.assertEqual(gen_loop_bench.branch(-4, link=True), 0x4BFFFFFD)

    def test_manifest_integrity_and_warmed_boundary_round_trip(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            manifest_path, generated = gen_loop_bench.generate_callret(
                output, [output], iters=5, width=3)
            loaded, workload, control = bench_callret.load_manifest(
                manifest_path, output, output)

            self.assertEqual(loaded, generated)
            self.assertEqual(workload, "instr_bench_callret")
            self.assertEqual(control, "instr_bench_callret_control")
            self.assertEqual(
                loaded["execution"]["calls_per_invocation"], 18)
            self.assertEqual(
                loaded["measurement_boundary"],
                bench_callret.EXPECTED_MEASUREMENT_BOUNDARY,
            )

            (output / "instr_bench_callret.bin").write_bytes(b"corrupt")
            with self.assertRaises(bench_callret.BenchmarkError):
                bench_callret.load_manifest(manifest_path, output, output)

    def test_manifest_rejects_measurement_boundary_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            manifest_path, generated = gen_loop_bench.generate_callret(
                output, [output], iters=5, width=3)
            generated["measurement_boundary"]["timer"] = (
                "external_process_wall_clock")
            manifest_path.write_text(json.dumps(generated))

            with self.assertRaises(bench_callret.BenchmarkError):
                bench_callret.load_manifest(manifest_path, output, output)


class CallretMetricParserTest(unittest.TestCase):
    def test_accepts_one_exact_positive_metric(self):
        parsed = bench_callret.parse_metric(
            "unrelated\n" + metric_line(cpu=111, wall=222, before=7, after=7)
            + "\nmore")
        self.assertEqual(parsed, {
            "thread_cpu_ns": 111,
            "uptime_raw_ns": 222,
            "placement_generation_before": 7,
            "placement_generation_after": 7,
        })

    def test_rejects_missing_duplicate_and_malformed_metric(self):
        invalid_outputs = (
            "no metric",
            metric_line() + "\n" + metric_line(),
            metric_line() + "\textra=1",
            " " + metric_line(),
            metric_line(cpu=0),
        )
        for output in invalid_outputs:
            with self.subTest(output=output):
                with self.assertRaises(bench_callret.BenchmarkError):
                    bench_callret.parse_metric(output)

    def test_rejects_code_placement_generation_change(self):
        with self.assertRaisesRegex(
                bench_callret.BenchmarkError, "generation changed"):
            bench_callret.parse_metric(metric_line(before=3, after=4))


class CallretDriverTest(unittest.TestCase):
    def test_run_suite_enforces_noninstrumented_warmed_flags_last(self):
        process = types.SimpleNamespace(
            stdout=passing_output(),
            stderr="",
            returncode=0,
        )
        with mock.patch.object(
                bench_callret.subprocess, "run", return_value=process) as run:
            parsed = bench_callret.run_suite(
                pathlib.Path("/tmp/fake-ppc-test"),
                ["--guest_scheduler=true", "--count_call_paths=true",
                 "--test_path=/tmp/wrong-source/"],
                "instr_bench_callret",
                pathlib.Path("/tmp/corpus"),
                pathlib.Path("/tmp/source"),
                10.0,
            )

        command = run.call_args.args[0]
        self.assertGreater(
            command.index("--guest_scheduler=false"),
            command.index("--guest_scheduler=true"),
        )
        self.assertGreater(
            command.index("--count_call_paths=false"),
            command.index("--count_call_paths=true"),
        )
        self.assertGreater(
            command.index("--test_path=/tmp/source/"),
            command.index("--test_path=/tmp/wrong-source/"),
        )
        self.assertIn("--test_benchmark_warmed=true", command)
        self.assertEqual(command[-1], "instr_bench_callret")
        self.assertEqual(parsed["thread_cpu_ns"], 1234567)
        self.assertGreater(parsed["process_wall_ns"], 0)

    def test_run_suite_rejects_pass_without_metric(self):
        process = types.SimpleNamespace(
            stdout=passing_output(metric="not a metric"),
            stderr="",
            returncode=0,
        )
        with mock.patch.object(
                bench_callret.subprocess, "run", return_value=process):
            with self.assertRaises(bench_callret.BenchmarkError):
                bench_callret.run_suite(
                    pathlib.Path("/tmp/fake-ppc-test"),
                    [],
                    "instr_bench_callret",
                    pathlib.Path("/tmp/corpus"),
                    pathlib.Path("/tmp/source"),
                    10.0,
                )

    def test_leg_order_balances_every_pair(self):
        for pair_index in range(8):
            order = bench_callret.leg_order(pair_index)
            self.assertEqual(set(order), {
                ("a", "workload"),
                ("a", "control"),
                ("b", "workload"),
                ("b", "control"),
            })

    def test_four_pair_block_balances_every_temporal_position(self):
        positions = [set() for _ in range(4)]
        for pair_index in range(4):
            for position, leg in enumerate(bench_callret.leg_order(pair_index)):
                positions[position].add(leg)
        expected = {
            ("a", "workload"),
            ("a", "control"),
            ("b", "workload"),
            ("b", "control"),
        }
        self.assertEqual(positions, [expected] * 4)

    def test_classifier_requires_consistent_sign_and_floor_clearance(self):
        improvement = [
            result_row(10.0, 9.8),
            result_row(10.1, 9.85),
            result_row(9.9, 9.7),
        ]
        regression = [
            result_row(10.0, 10.2),
            result_row(10.1, 10.35),
            result_row(9.9, 10.1),
        ]
        mixed = [
            result_row(10.0, 9.8),
            result_row(10.0, 10.1),
            result_row(10.0, 9.7),
        ]
        inside_floor = [
            result_row(10.0, 9.98),
            result_row(10.0, 9.97),
            result_row(10.0, 9.96),
        ]

        self.assertEqual(
            bench_callret.classify(improvement, 1.0), "improvement")
        self.assertEqual(
            bench_callret.classify(regression, 1.0), "regression")
        self.assertEqual(
            bench_callret.classify(mixed, 1.0), "unresolved")
        self.assertEqual(
            bench_callret.classify(inside_floor, 1.0), "unresolved")
        self.assertEqual(
            bench_callret.classify(improvement, 3.0), "unresolved")

    def test_phase_summary_exposes_pair_drift_and_noise_floor(self):
        rows = [
            result_row(10.0, 9.8),
            result_row(10.2, 10.0),
            result_row(9.9, 9.7),
        ]
        summary = bench_callret.phase_summary(rows)
        self.assertGreater(summary["pair_mean_drift_pct"], 0)
        self.assertEqual(len(summary["paired_deltas_pct"]), 3)
        self.assertEqual(
            bench_callret.phase_noise_floor(summary),
            max(
                summary["max_abs_paired_delta_pct"],
                summary["pair_mean_drift_pct"],
            ),
        )


if __name__ == "__main__":
    unittest.main()
