#!/usr/bin/env python3
"""Fail-closed A64 capture macro codegen comparison.

The pre-hook and capture-off binaries must contain this same fixture test.
Their raw emitted bytes must match exactly. The optional capture-on binary is
also required to run the focused control tests and to emit different fixture
bytes, proving that the comparison did not accidentally use another off build.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys


MARKER = re.compile(
    r"^A64_CAPTURE_CODEGEN ([a-z0-9_-]+) ([0-9a-f]+)$", re.MULTILINE
)
FIXTURE_FILTER = "[a64-capture-codegen]"
CONTROL_FILTER = "[guest-invocation-capture]"


def run_tests(binary: pathlib.Path, test_filter: str, timeout: float) -> str:
    if not binary.is_file():
        raise RuntimeError(f"test binary does not exist: {binary}")
    result = subprocess.run(
        [str(binary), test_filter],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"{binary} failed {test_filter!r} with exit {result.returncode}:\n"
            f"{result.stdout}"
        )
    return result.stdout


def collect_fixtures(binary: pathlib.Path, timeout: float) -> dict[str, bytes]:
    output = run_tests(binary, FIXTURE_FILTER, timeout)
    fixtures: dict[str, bytes] = {}
    for name, encoded_bytes in MARKER.findall(output):
        if name in fixtures:
            raise RuntimeError(f"{binary} emitted duplicate fixture {name!r}")
        if len(encoded_bytes) % 8:
            raise RuntimeError(
                f"{binary} fixture {name!r} is not whole A64 instructions"
            )
        fixtures[name] = bytes.fromhex(encoded_bytes)
    if set(fixtures) != {"empty", "scalar", "vector"}:
        raise RuntimeError(
            f"{binary} emitted fixture set {sorted(fixtures)}, expected "
            "['empty', 'scalar', 'vector']"
        )
    return fixtures


def describe(fixtures: dict[str, bytes]) -> str:
    fields = []
    for name in sorted(fixtures):
        code = fixtures[name]
        digest = hashlib.sha256(code).hexdigest()[:16]
        fields.append(f"{name}={len(code)}B/{digest}")
    return ", ".join(fields)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pre-hook", required=True, type=pathlib.Path)
    parser.add_argument("--capture-off", required=True, type=pathlib.Path)
    parser.add_argument("--capture-on", type=pathlib.Path)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    try:
        pre_hook = collect_fixtures(args.pre_hook, args.timeout)
        capture_off = collect_fixtures(args.capture_off, args.timeout)
        if pre_hook != capture_off:
            differing = sorted(
                name
                for name in set(pre_hook) | set(capture_off)
                if pre_hook.get(name) != capture_off.get(name)
            )
            raise RuntimeError(
                "capture-off emitted bytes differ from pre-hook baseline for: "
                + ", ".join(differing)
            )

        print("capture-off exactly matches pre-hook: " + describe(capture_off))

        if args.capture_on:
            capture_on = collect_fixtures(args.capture_on, args.timeout)
            if capture_on == capture_off:
                raise RuntimeError(
                    "capture-on fixtures unexpectedly match capture-off; "
                    "verify build flags"
                )
            run_tests(args.capture_on, CONTROL_FILTER, args.timeout)
            print(
                "capture-on hooks present and focused control tests passed: "
                + describe(capture_on)
            )
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
