#!/usr/bin/env python3
"""Focused tests for the PPC corpus subprocess driver."""

import importlib.util
import pathlib
import subprocess
import tempfile
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


class DiscoverSuitesTest(unittest.TestCase):

    def test_requires_exact_runner_visible_source_closure(self):
        with tempfile.TemporaryDirectory() as root_string:
            root = pathlib.Path(root_string)
            source = root / "source"
            corpus = root / "corpus"
            source.mkdir()
            corpus.mkdir()
            for suite in ("instr_z", "instr_a"):
                (source / f"{suite}.s").touch()
                (corpus / f"{suite}.map").touch()
                (corpus / f"{suite}.bin").touch()
            # seq_ files are helpers that the PPC runner deliberately does not
            # discover, even though the assembler also emits them.
            (source / "seq_helper.s").touch()
            (corpus / "seq_helper.map").touch()
            (corpus / "seq_helper.bin").touch()

            self.assertEqual(
                VERIFY_CORPUS.discover_suites(corpus, source),
                ["instr_a", "instr_z"],
            )

    def test_rejects_stale_or_incomplete_corpus(self):
        with tempfile.TemporaryDirectory() as root_string:
            root = pathlib.Path(root_string)
            source = root / "source"
            corpus = root / "corpus"
            source.mkdir()
            corpus.mkdir()
            (source / "instr_current.s").touch()
            (corpus / "instr_stale.map").touch()
            (corpus / "instr_stale.bin").touch()

            with self.assertRaisesRegex(ValueError, "missing .map suites"):
                VERIFY_CORPUS.discover_suites(corpus, source)

    def test_restricted_run_still_requires_source_map_and_binary(self):
        with tempfile.TemporaryDirectory() as root_string:
            root = pathlib.Path(root_string)
            source = root / "source"
            corpus = root / "corpus"
            source.mkdir()
            corpus.mkdir()
            (source / "instr_one.s").touch()
            (corpus / "instr_one.map").touch()

            with self.assertRaisesRegex(ValueError, "missing .bin suites"):
                VERIFY_CORPUS.discover_suites(
                    corpus, source, ["instr_one"]
                )


if __name__ == "__main__":
    unittest.main()
