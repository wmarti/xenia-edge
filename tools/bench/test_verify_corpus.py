#!/usr/bin/env python3
"""Focused tests for the PPC corpus subprocess driver."""

import importlib.util
import pathlib
import subprocess
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("verify_corpus.py")
SPEC = importlib.util.spec_from_file_location("verify_corpus", MODULE_PATH)
VERIFY_CORPUS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY_CORPUS)


class RunSuiteTest(unittest.TestCase):

    @mock.patch.object(VERIFY_CORPUS.subprocess, "run")
    def test_forwards_runner_arguments_before_test_selection(self, run):
        run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0,
            stderr="Total tests: 7\nPassed: 7\nFailed: 0\n",
        )

        row = VERIFY_CORPUS.run_suite(
            "/tmp/ppc-tests", "instr_add", "/tmp/corpus",
            "/tmp/testing", "/tmp/skip.txt",
            ["--guest_scheduler=false", "--log_level=0"], 30.0,
        )

        command = run.call_args.args[0]
        self.assertEqual(
            command,
            [
                "/tmp/ppc-tests",
                "--guest_scheduler=false",
                "--log_level=0",
                "--test_path=/tmp/testing/",
                "--test_bin_path=/tmp/corpus/",
                "instr_add",
                "--test_skip_file=/tmp/skip.txt",
            ],
        )
        self.assertEqual(row["verdict"], "pass")
        self.assertEqual(row["total"], 7)
        self.assertEqual(row["passed"], 7)


if __name__ == "__main__":
    unittest.main()
