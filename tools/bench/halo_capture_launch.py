#!/usr/bin/env python3
"""Fail-closed launch of one retained Halo 3 capture fixture.

The launch refuses to start unless every precondition below holds, and it
prints every precondition that failed. Nothing is taken from defaults: the
executable, title, input script, storage root, capture root, scheduler lane,
fixture and build commit are explicit arguments. The app is never launched
without a title, because that only shows the usage dialog.

Preconditions:
  1. Apple Silicon macOS host; the retained lane is A64 on macOS.
  2. Every path argument is absolute.
  3. --exe is a regular executable file, not an .app directory.
  4. --exe embeds the capture runtime's arm banner, so it is a capture build.
  5. --exe embeds --build-commit, so its version stamp names that commit.
  6. --expected-exe-sha256, when given, matches the executable.
  7. --scheduler on requires the scheduler-capture capability literal in the
     executable and rejects one that still carries the legacy
     "--guest_scheduler=false" rejection. --scheduler off is the secondary
     diagnostic lane and is recorded as such.
  8. Fixtures beyond the implemented one-segment primitive require the
     session-capture capability literal in the executable.
  9. --title exists: a regular file, or a directory holding default.xex.
 10. --input-script is a nonempty regular file.
 11. --storage-root is an existing directory outside $TMPDIR, /tmp,
     /private/tmp, /var/folders and outside every Git worktree; its content/
     and cache/ hold title-derived data.
 12. --capture-root is an existing directory under the same exclusions.
 13. Free space on /System/Volumes/Data and on the capture volume is at least
     the bundle cap plus the reserve (1 GiB + 8 GiB; the reserve has an
     8 GiB floor).
 14a. --extra may not name any flag the harness sets, any capture flag, or a
     path/logging flag that would bypass these guards.
 14. The target, its .part staging sibling and every launch sibling (.log,
     .stdout.log, .presents.txt, .launch.json, .stop) do not exist.
 15. No other process running the same executable name exists.
 16. Warmup, budget, timeout and grace are positive. The ten-second and
     manual fixtures need an explicit --bundle-cap-bytes and an accepted
     --prior-fixture-report from a smaller fixture.

Acceptance for the segment fixture: the capture log's "published" line appears
after the warmup, the present counter advanced during the warmup, the bundle
holds manifest.json, invocation.jcorpus and invocation.guest_invocation, the
manifest names this executable's SHA-256 and the requested guest_scheduler
value, the corpus/artifact digests match, no .part sibling remains, the bundle
fits the cap, and no input changed. Anything else is not accepted.

This is a launch harness. It proves that a capture bundle was published under
the recorded configuration; it proves nothing about replay fidelity, title
speed or host presentation.

Exit codes: 0 accepted (or --dry-run with every precondition satisfied),
1 launched but not accepted, 2 precondition failure with nothing launched.
"""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import signal
import subprocess
import sys
import tempfile
import time


SCHEMA = "xenia-halo-capture-launch-v1"
DATA_VOLUME = "/System/Volumes/Data"
GIB = 1024 ** 3
DEFAULT_BUNDLE_CAP_BYTES = 1 * GIB
DEFAULT_RESERVE_BYTES = 8 * GIB
PROTOCOL_WARMUP_SECONDS = 100.0
PROTOCOL_MARKER = "pm4_swap"

# Literals the executable must carry. The first two are strings the runtime
# itself formats, so their presence proves which code was linked in.
CAPTURE_BUILD_MARKER = (
    b"Guest invocation capture armed before title translation")
LEGACY_SCHEDULER_REJECTION = (
    b"capture requires explicit --guest_scheduler=false")
# Capability literals a production runtime embeds in its arm banner once the
# capability exists. Until it does, the lanes needing them are refused here.
SCHEDULER_CAPTURE_CAPABILITY = b"XENIA_GUEST_EXECUTION_SCHEDULER_CAPTURE_V1"
SESSION_CAPTURE_CAPABILITY = b"XENIA_GUEST_EXECUTION_SESSION_CAPTURE_V1"

# Log lines emitted by the implemented one-segment runtime.
ARMED_LINE = "Guest invocation capture armed before title translation"
PUBLISHED_LINE = "Guest invocation capture published:"
REJECTED_LINE = "Guest invocation capture initialization rejected:"
FINALIZED_PUBLISHED_LINE = (
    "Guest invocation capture finalized: one segment published")
FAILURE_LINES = (
    REJECTED_LINE,
    "Guest invocation capture finalized without output",
    "Guest invocation capture finalized incomplete",
    "Guest invocation capture finalized in an invalid state",
    "Guest invocation capture sink changed before runtime shutdown",
)
BUNDLE_FILES = (
    "manifest.json",
    "invocation.jcorpus",
    "invocation.guest_invocation",
)

# Values the capture runtime requires in the effective configuration. They
# are passed explicitly so a per-title config cannot leave one to a default.
CANONICAL_REPLAY_CONTROLS = (
    "--break_on_instruction=0",
    "--count_call_paths=false",
    "--count_physical_remap_hits=false",
    "--cpu_trace_mask=0",
    "--debug=false",
    "--emit_mmio_aware_stores_for_recorded_exception_addresses=false",
    "--enable_early_precompilation=false",
    "--fold_readonly_guest_memory_loads=false",
    "--inline_mmio_access=false",
    "--log_safepoint_pc=false",
    "--serialize_guest_function_definitions=true",
    "--trace_function_coverage=false",
)

# Flags an --extra value may never set. cvars take the last value given and
# extras are appended after the harness's own flags, so any of these would
# silently redirect output, logging or configuration around the guards.
RESERVED_EXTRA_PREFIXES = ("--guest_invocation_capture_",
                           "--guest_execution_capture_")
RESERVED_EXTRA_FLAGS = frozenset({
    "--cache_root", "--config", "--content_root", "--discord",
    "--guest_scheduler", "--hid", "--input_script", "--log_append",
    "--log_file", "--log_level", "--present_count_file", "--storage_root",
    "--target",
} | {flag.split("=", 1)[0] for flag in CANONICAL_REPLAY_CONTROLS})

SCHEDULER_LANES = {
    "on": {"flag": "--guest_scheduler=true", "lane": "primary"},
    "off": {"flag": "--guest_scheduler=false", "lane": "secondary-diagnostic"},
}

# Progressive retained fixtures. The segment fixture uses the implemented
# one-segment primitive. The others name the session-capture flag contract
# and stay refused until the executable carries SESSION_CAPTURE_CAPABILITY.
FIXTURES = {
    "segment": {
        "session": False,
        "boundary": None,
        "budget_seconds": 60.0,
        "explicit_cap": False,
    },
    "marker": {
        "session": True,
        "boundary": "guest_marker_count:1",
        "budget_seconds": 60.0,
        "explicit_cap": False,
    },
    "second": {
        "session": True,
        "boundary": "capture_duration_ns:1000000000",
        "budget_seconds": 60.0,
        "explicit_cap": False,
    },
    "ten-seconds": {
        "session": True,
        "boundary": "capture_duration_ns:10000000000",
        "budget_seconds": 120.0,
        "explicit_cap": True,
    },
    "manual": {
        "session": True,
        "boundary": "manual",
        "budget_seconds": 600.0,
        "explicit_cap": True,
    },
}
FIXTURE_ORDER = ("segment", "marker", "second", "ten-seconds", "manual")
TEMP_ROOTS = ("/tmp", "/private/tmp", "/var/folders", "/private/var/folders")
LAUNCH_SIBLINGS = (".part", ".log", ".stdout.log", ".presents.txt",
                   ".launch.json", ".stop")
HEX_ADDRESS = re.compile(r"(0[xX])?[0-9a-fA-F]{8}")
COMMIT = re.compile(r"[0-9a-f]{40}")
SHA256 = re.compile(r"[0-9a-f]{64}")


class LaunchError(RuntimeError):
    pass


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def scan_executable(path, needles):
    """SHA-256 and needle presence in one pass over the executable."""
    digest = hashlib.sha256()
    found = {needle: False for needle in needles}
    overlap = max(len(needle) for needle in needles) - 1
    tail = b""
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
            window = tail + chunk
            for needle in needles:
                if not found[needle] and needle in window:
                    found[needle] = True
            tail = window[-overlap:] if overlap else b""
    return digest.hexdigest(), found


def title_fingerprint(path, full):
    """Size, mtime and edge digests; a full digest only on request."""
    stat = path.stat()
    record = {
        "path": str(path),
        "kind": "directory" if path.is_dir() else "file",
        "size_bytes": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
    }
    if path.is_dir():
        record["default_xex_sha256"] = file_sha256(path / "default.xex")
        return record
    edge = 4 * 1024 * 1024
    with open(path, "rb") as stream:
        head = hashlib.sha256(stream.read(edge)).hexdigest()
        stream.seek(max(0, stat.st_size - edge))
        tail = hashlib.sha256(stream.read(edge)).hexdigest()
    record["head_4mib_sha256"] = head
    record["tail_4mib_sha256"] = tail
    if full:
        record["sha256"] = file_sha256(path)
    return record


def free_bytes(path):
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_frsize


def git_output(directory, *args):
    try:
        result = subprocess.run(
            ["git", "-C", str(directory), *args],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=20)
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def tool_provenance():
    tool = pathlib.Path(__file__).resolve()
    return {
        "path": str(tool),
        "sha256": file_sha256(tool),
        "commit": git_output(tool.parent, "rev-parse", "HEAD"),
        "branch": git_output(tool.parent, "rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(git_output(tool.parent, "status", "--porcelain",
                                 "--", str(tool))),
    }


def inside_git_worktree(path):
    for ancestor in (path, *path.parents):
        if (ancestor / ".git").exists():
            return True
    return False


def under_any(path, roots):
    for root in roots:
        try:
            real_root = pathlib.Path(root).resolve(strict=True)
        except OSError:
            continue
        if path == real_root or real_root in path.parents:
            return str(real_root)
    return None


def running_emulators(exe_name):
    """(pid, path) for every process running an executable of that name."""
    try:
        listing = subprocess.run(
            ["ps", "-axo", "pid=,comm="], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError) as error:
        raise LaunchError(f"unable to list processes: {error}") from error
    matches = []
    for line in listing.splitlines():
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        # comm is the executable path alone, so a space inside it is kept.
        executable = parts[1].strip()
        if pathlib.Path(executable).name == exe_name:
            matches.append((int(parts[0]), executable))
    return matches


def read_present_counter(path):
    try:
        with open(path, "r", encoding="ascii") as stream:
            parts = stream.read().split()
        if len(parts) >= 2:
            return int(parts[0]), int(parts[1])
    except (OSError, ValueError):
        pass
    return None


def write_json_atomically(path, payload):
    text = json.dumps(payload, allow_nan=False, indent=2, sort_keys=True) + "\n"
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=path.parent,
                prefix=f".{path.name}.", suffix=".tmp",
                delete=False) as stream:
            temporary = pathlib.Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Fail-closed launch of one retained Halo 3 capture fixture")
    parser.add_argument("--exe", required=True, type=pathlib.Path,
                        help="capture-build executable inside the .app bundle")
    parser.add_argument("--title", required=True, type=pathlib.Path,
                        help="title file, or a directory holding default.xex")
    parser.add_argument("--input-script", required=True, type=pathlib.Path)
    parser.add_argument("--storage-root", required=True, type=pathlib.Path)
    parser.add_argument("--capture-root", required=True, type=pathlib.Path,
                        help="stable local bundle root outside temp and Git")
    parser.add_argument("--run-name", required=True,
                        help="bundle directory name created under the root")
    parser.add_argument("--fixture", required=True, choices=FIXTURE_ORDER)
    parser.add_argument("--scheduler", required=True,
                        choices=sorted(SCHEDULER_LANES))
    parser.add_argument("--build-commit", required=True,
                        help="40-hex commit the executable was stamped with")
    parser.add_argument("--expected-exe-sha256", default=None)
    parser.add_argument("--root-address", default=None,
                        help="segment fixture: eight-hex-digit root address")
    parser.add_argument("--root-end-address", default=None)
    parser.add_argument("--occurrence", type=int, default=None,
                        help="segment fixture: 1-based root occurrence")
    parser.add_argument("--warmup-seconds", type=float,
                        default=PROTOCOL_WARMUP_SECONDS)
    parser.add_argument("--budget-seconds", type=float, default=None,
                        help="time allowed after the warmup for publication")
    parser.add_argument("--timeout-seconds", type=float, default=None,
                        help="hard wall limit (default warmup+budget+300)")
    parser.add_argument("--shutdown-grace-seconds", type=float, default=30.0)
    parser.add_argument("--bundle-cap-bytes", type=int, default=None)
    parser.add_argument("--reserve-bytes", type=int,
                        default=DEFAULT_RESERVE_BYTES)
    parser.add_argument("--prior-fixture-report", type=pathlib.Path,
                        default=None,
                        help="accepted report of a smaller fixture, required "
                             "before raising the cap")
    parser.add_argument("--full-title-sha256", action="store_true",
                        help="hash the whole title instead of its edges")
    parser.add_argument("--extra", action="append", default=[],
                        help="additional emulator flag; may repeat")
    parser.add_argument("--dry-run", action="store_true",
                        help="check every precondition and print the command "
                             "without launching or writing anything")
    return parser.parse_args(argv)


def check_preconditions(args):
    """Return the launch plan, or raise LaunchError listing every failure."""
    failures = []
    plan = {}

    if platform.system() != "Darwin" or platform.machine() != "arm64":
        failures.append(
            "host must be Apple Silicon macOS, got "
            f"{platform.system()}/{platform.machine()}")

    paths = {
        "--exe": args.exe,
        "--title": args.title,
        "--input-script": args.input_script,
        "--storage-root": args.storage_root,
        "--capture-root": args.capture_root,
    }
    if args.prior_fixture_report is not None:
        paths["--prior-fixture-report"] = args.prior_fixture_report
    for name, path in paths.items():
        if not path.is_absolute():
            failures.append(f"{name} must be absolute: {path}")

    fixture = FIXTURES[args.fixture]
    lane = SCHEDULER_LANES[args.scheduler]
    budget = (fixture["budget_seconds"] if args.budget_seconds is None
              else args.budget_seconds)
    timeout = (args.warmup_seconds + budget + 300.0
               if args.timeout_seconds is None else args.timeout_seconds)
    for name, value in (("--warmup-seconds", args.warmup_seconds),
                        ("--budget-seconds", budget),
                        ("--timeout-seconds", timeout),
                        ("--shutdown-grace-seconds",
                         args.shutdown_grace_seconds)):
        if not value > 0:
            failures.append(f"{name} must be positive, got {value}")
    if timeout <= args.warmup_seconds + budget:
        failures.append(
            f"--timeout-seconds {timeout} must exceed warmup "
            f"{args.warmup_seconds} plus budget {budget}")
    if args.warmup_seconds != PROTOCOL_WARMUP_SECONDS:
        failures.append(
            f"--warmup-seconds must be {PROTOCOL_WARMUP_SECONDS:g}; the "
            f"retained protocol warms for exactly that long")

    if not COMMIT.fullmatch(args.build_commit):
        failures.append("--build-commit must be 40 lowercase hex digits")
    if (args.expected_exe_sha256 is not None and
            not SHA256.fullmatch(args.expected_exe_sha256)):
        failures.append("--expected-exe-sha256 must be 64 lowercase hex digits")

    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", args.run_name):
        failures.append(
            "--run-name must be 1-128 characters of [A-Za-z0-9._-] and not "
            "begin with a dot")

    cap = (DEFAULT_BUNDLE_CAP_BYTES if args.bundle_cap_bytes is None
           else args.bundle_cap_bytes)
    if cap <= 0:
        failures.append(f"--bundle-cap-bytes must be positive, got {cap}")
    if args.reserve_bytes < DEFAULT_RESERVE_BYTES:
        failures.append(
            f"--reserve-bytes must be at least {DEFAULT_RESERVE_BYTES} "
            f"(8 GiB), got {args.reserve_bytes}")
    if fixture["explicit_cap"]:
        if args.bundle_cap_bytes is None:
            failures.append(
                f"fixture {args.fixture} needs an explicit --bundle-cap-bytes "
                "recorded from a measured smaller fixture")
        if args.prior_fixture_report is None:
            failures.append(
                f"fixture {args.fixture} needs --prior-fixture-report naming "
                "an accepted smaller fixture")
    elif cap > DEFAULT_BUNDLE_CAP_BYTES:
        failures.append(
            f"fixture {args.fixture} is capped at {DEFAULT_BUNDLE_CAP_BYTES} "
            f"bytes; got {cap}")
    if args.prior_fixture_report is not None:
        plan["prior_fixture"] = check_prior_report(
            args.prior_fixture_report, args.fixture, cap, failures)

    # Executable.
    exe = args.exe
    if not exe.is_file():
        failures.append(
            f"--exe must be a regular file, not an .app directory: {exe}")
    elif not os.access(exe, os.X_OK):
        failures.append(f"--exe is not executable: {exe}")
    else:
        needles = (CAPTURE_BUILD_MARKER, LEGACY_SCHEDULER_REJECTION,
                   SCHEDULER_CAPTURE_CAPABILITY, SESSION_CAPTURE_CAPABILITY,
                   args.build_commit.encode("ascii"))
        exe_sha256, found = scan_executable(exe, needles)
        plan["exe_sha256"] = exe_sha256
        plan["exe_markers"] = {
            "capture_build": found[CAPTURE_BUILD_MARKER],
            "legacy_scheduler_rejection": found[LEGACY_SCHEDULER_REJECTION],
            "scheduler_capture_capability":
                found[SCHEDULER_CAPTURE_CAPABILITY],
            "session_capture_capability": found[SESSION_CAPTURE_CAPABILITY],
            "build_commit": found[args.build_commit.encode("ascii")],
        }
        if not found[CAPTURE_BUILD_MARKER]:
            failures.append(
                f"{exe} is not a capture build: it does not contain "
                f"{CAPTURE_BUILD_MARKER.decode()!r}")
        if not found[args.build_commit.encode("ascii")]:
            failures.append(
                f"{exe} does not embed build commit {args.build_commit}; "
                "its version stamp names another commit")
        if (args.expected_exe_sha256 is not None and
                args.expected_exe_sha256 != exe_sha256):
            failures.append(
                f"--expected-exe-sha256 {args.expected_exe_sha256} does not "
                f"match {exe} ({exe_sha256})")
        if args.scheduler == "on":
            if found[LEGACY_SCHEDULER_REJECTION]:
                failures.append(
                    f"scheduler-on capture is refused: {exe} still rejects "
                    "guest_scheduler=true with "
                    f"{LEGACY_SCHEDULER_REJECTION.decode()!r} "
                    "(src/xenia/cpu/guest_invocation_capture_runtime.cc)")
            if not found[SCHEDULER_CAPTURE_CAPABILITY]:
                failures.append(
                    f"scheduler-on capture is refused: {exe} does not carry "
                    f"{SCHEDULER_CAPTURE_CAPABILITY.decode()}; the scheduler "
                    "event observer is not integrated into this build")
        if fixture["session"] and not found[SESSION_CAPTURE_CAPABILITY]:
            failures.append(
                f"fixture {args.fixture} is refused: {exe} does not carry "
                f"{SESSION_CAPTURE_CAPABILITY.decode()}; only the segment "
                "fixture is implemented")

    # Segment selection.
    if not fixture["session"]:
        for name, value in (("--root-address", args.root_address),
                            ("--root-end-address", args.root_end_address)):
            if value is None or not HEX_ADDRESS.fullmatch(value):
                failures.append(
                    f"segment fixture needs {name} as exactly eight "
                    f"hexadecimal digits, got {value!r}")
        if args.occurrence is None or args.occurrence < 1:
            failures.append(
                "segment fixture needs --occurrence >= 1, got "
                f"{args.occurrence!r}")
    else:
        for name, value in (("--root-address", args.root_address),
                            ("--root-end-address", args.root_end_address),
                            ("--occurrence", args.occurrence)):
            if value is not None:
                failures.append(
                    f"{name} applies only to the segment fixture")

    # Title, script, storage.
    title = args.title
    if title.is_dir():
        if not (title / "default.xex").is_file():
            failures.append(f"--title directory holds no default.xex: {title}")
    elif not title.is_file():
        failures.append(f"--title does not exist: {title}")
    script = args.input_script
    if not script.is_file():
        failures.append(f"--input-script is not a file: {script}")
    elif script.stat().st_size == 0:
        failures.append(f"--input-script is empty: {script}")
    temp_roots = list(TEMP_ROOTS)
    if os.environ.get("TMPDIR"):
        temp_roots.append(os.environ["TMPDIR"])

    def check_title_data_root(name, directory):
        """content/ and cache/ under storage, and every bundle, are
        title-derived; none may sit under temporary storage or Git."""
        real = directory.resolve()
        temp_hit = under_any(real, temp_roots)
        if temp_hit:
            failures.append(
                f"{name} {directory} is under temporary storage {temp_hit}; "
                "the trace-temp cleaner may reclaim title-derived data")
        if inside_git_worktree(real):
            failures.append(
                f"{name} {directory} is inside a Git worktree; title-derived "
                "data never enters Git")
        return real

    if not args.storage_root.is_dir():
        failures.append(
            f"--storage-root is not a directory: {args.storage_root}")
    else:
        check_title_data_root("--storage-root", args.storage_root)

    # Capture root, target and siblings.
    root = args.capture_root
    if not root.is_dir():
        failures.append(f"--capture-root is not a directory: {root}")
    else:
        real_root = check_title_data_root("--capture-root", root)
        target = root / args.run_name
        plan["target"] = target
        existing = []
        for suffix in ("", *LAUNCH_SIBLINGS):
            sibling = target.with_name(target.name + suffix)
            if sibling.exists() or sibling.is_symlink():
                existing.append(str(sibling))
        if existing:
            failures.append(
                "target or launch sibling already exists; choose a new "
                "--run-name rather than replacing data: " + ", ".join(existing))
        required = cap + args.reserve_bytes
        plan["disk"] = {}
        for label, volume in (("data_volume", DATA_VOLUME),
                              ("capture_volume", str(real_root))):
            try:
                free = free_bytes(volume)
            except OSError as error:
                failures.append(f"unable to query free space on {volume}: "
                                f"{error}")
                continue
            plan["disk"][label] = {"path": volume, "free_bytes": free}
            if free < required:
                failures.append(
                    f"{volume} has {free} bytes free; the {args.fixture} "
                    f"fixture needs cap {cap} + reserve {args.reserve_bytes} "
                    f"= {required}")

    # Other emulators.
    try:
        others = running_emulators(exe.name)
    except LaunchError as error:
        failures.append(str(error))
        others = []
    if others:
        failures.append(
            "another emulator process is running; a shared machine "
            "contaminates the capture: " +
            ", ".join(f"pid {pid} {argv0}" for pid, argv0 in others))

    seen_extra = set()
    for flag in args.extra:
        name = flag.split("=", 1)[0]
        if not flag.startswith("--") or len(name) < 3:
            failures.append(
                f"--extra values must be --flag=value, got {flag!r}")
        elif name in RESERVED_EXTRA_FLAGS or \
                name.startswith(RESERVED_EXTRA_PREFIXES):
            failures.append(
                f"--extra may not set {name}; it would bypass the output, "
                "logging or configuration guards")
        elif name in seen_extra:
            failures.append(f"--extra names {name} more than once")
        seen_extra.add(name)

    if failures:
        raise LaunchError(failures)

    plan.update({
        "fixture": args.fixture,
        "boundary": fixture["boundary"],
        "session": fixture["session"],
        "scheduler_flag": lane["flag"],
        "lane": lane["lane"],
        "warmup_seconds": args.warmup_seconds,
        "budget_seconds": budget,
        "timeout_seconds": timeout,
        "bundle_cap_bytes": cap,
        "reserve_bytes": args.reserve_bytes,
    })
    return plan


def check_prior_report(path, fixture, cap, failures):
    """The cap may rise only after a smaller fixture was accepted."""
    if not path.is_file():
        failures.append(f"--prior-fixture-report is not a file: {path}")
        return None
    try:
        with open(path, "r", encoding="utf-8") as stream:
            report = json.load(stream)
    except (OSError, ValueError) as error:
        failures.append(f"--prior-fixture-report is unreadable: {error}")
        return None
    prior_fixture = report.get("fixture")
    summary = {
        "path": str(path),
        "fixture": prior_fixture,
        "verdict": report.get("verdict"),
        "bundle_bytes": report.get("bundle", {}).get("byte_size"),
    }
    if report.get("schema") != SCHEMA:
        failures.append(f"--prior-fixture-report has schema "
                        f"{report.get('schema')!r}, expected {SCHEMA}")
    if report.get("verdict") != "accepted":
        failures.append("--prior-fixture-report was not accepted: "
                        f"{report.get('verdict')!r}")
    if (prior_fixture not in FIXTURE_ORDER or
            FIXTURE_ORDER.index(prior_fixture) >= FIXTURE_ORDER.index(fixture)):
        failures.append(
            f"--prior-fixture-report must come from a fixture before "
            f"{fixture}, got {prior_fixture!r}")
    if not isinstance(summary["bundle_bytes"], int):
        failures.append("--prior-fixture-report records no bundle byte size")
    elif summary["bundle_bytes"] > cap:
        failures.append(
            f"--bundle-cap-bytes {cap} is below the prior fixture's measured "
            f"{summary['bundle_bytes']} bytes")
    return summary


def build_command(args, plan):
    target = plan["target"]
    log_path = target.with_name(target.name + ".log")
    counter_path = target.with_name(target.name + ".presents.txt")
    command = [
        str(args.exe),
        f"--storage_root={args.storage_root}",
        f"--log_file={log_path}",
        "--log_level=2",
        "--hid=nop",
        "--discord=false",
        f"--present_count_file={counter_path}",
        f"--input_script={args.input_script}",
        plan["scheduler_flag"],
        *CANONICAL_REPLAY_CONTROLS,
    ]
    if not plan["session"]:
        deadline_ms = int((plan["warmup_seconds"] + plan["budget_seconds"]) *
                          1000)
        command += [
            f"--guest_invocation_capture_output={target}",
            f"--guest_invocation_capture_root_address={args.root_address}",
            f"--guest_invocation_capture_root_end_address="
            f"{args.root_end_address}",
            f"--guest_invocation_capture_occurrence={args.occurrence}",
            f"--guest_invocation_capture_max_duration_ms={deadline_ms}",
        ]
    else:
        # Proposed session-capture contract; refused until the executable
        # carries SESSION_CAPTURE_CAPABILITY.
        command += [
            f"--guest_execution_capture_output={target}",
            f"--guest_execution_capture_marker_source={PROTOCOL_MARKER}",
            f"--guest_execution_capture_warmup_ms="
            f"{int(plan['warmup_seconds'] * 1000)}",
            f"--guest_execution_capture_boundary={plan['boundary']}",
            f"--guest_execution_capture_bundle_cap_bytes="
            f"{plan['bundle_cap_bytes']}",
        ]
        if plan["boundary"] == "manual":
            command.append(
                f"--guest_execution_capture_stop_file="
                f"{target.with_name(target.name + '.stop')}")
    command += args.extra
    names = [entry.split("=", 1)[0] for entry in command[1:]]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise LaunchError(
            [f"command names a flag more than once: {duplicates}"])
    command.append(str(args.title))
    return command


def terminate(process, grace_seconds):
    if process.poll() is not None:
        return "exited"
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=grace_seconds)
        return "sigterm"
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        return "sigkill"


class LogTail:
    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.partial = ""

    def lines(self):
        try:
            with open(self.path, "rb") as stream:
                stream.seek(self.offset)
                data = stream.read()
                self.offset = stream.tell()
        except OSError:
            return []
        text = self.partial + data.decode("utf-8", errors="replace")
        pieces = text.split("\n")
        self.partial = pieces.pop()
        return pieces


def monitor(process, args, plan, run):
    """Watch the log and present counter; return the observation record."""
    target = plan["target"]
    log = LogTail(target.with_name(target.name + ".log"))
    counter_path = target.with_name(target.name + ".presents.txt")
    started = time.monotonic()
    observed = {
        "armed": None,
        "published": None,
        "published_after_warmup": None,
        "finalized_published": None,
        "failure_lines": [],
        "counter_samples": [],
        "counter_advanced_in_warmup": None,
        "exit_status": None,
        "termination": None,
        "reason": None,
    }
    warmup_checked = False
    last_sample = -10.0
    success = False

    def take_sample(elapsed):
        sample = read_present_counter(counter_path)
        if sample is not None:
            observed["counter_samples"].append(
                {"elapsed_seconds": round(elapsed, 3),
                 "count": sample[0], "monotonic_ns": sample[1]})

    while True:
        elapsed = time.monotonic() - started
        for line in log.lines():
            if ARMED_LINE in line and observed["armed"] is None:
                observed["armed"] = elapsed
            if PUBLISHED_LINE in line and observed["published"] is None:
                observed["published"] = elapsed
                observed["published_after_warmup"] = (
                    elapsed >= plan["warmup_seconds"])
            if FINALIZED_PUBLISHED_LINE in line:
                observed["finalized_published"] = elapsed
            for marker in FAILURE_LINES:
                if marker in line:
                    observed["failure_lines"].append(line.strip())
        if elapsed - last_sample >= 5.0:
            last_sample = elapsed
            take_sample(elapsed)
        if observed["failure_lines"]:
            observed["reason"] = "capture runtime reported a failure"
            break
        if elapsed >= plan["warmup_seconds"] and not warmup_checked:
            warmup_checked = True
            # Liveness is judged at the end of the warmup, not anywhere in
            # it: a guest that presented early and then parked must not
            # count. The counter file is rewritten every 64 presents, so two
            # fresh samples 5 s apart differ only while swaps keep coming.
            take_sample(elapsed)
            recent = [s for s in observed["counter_samples"]
                      if s["elapsed_seconds"] >= plan["warmup_seconds"] - 15.0]
            advanced = (len(recent) >= 2 and
                        recent[-1]["count"] > recent[-2]["count"] > 0)
            observed["counter_advanced_in_warmup"] = advanced
            if not advanced:
                observed["reason"] = (
                    "present counter did not advance in the last 15 s of the "
                    "warmup; the title is not treated as live")
                break
        if observed["published"] is not None:
            if not observed["published_after_warmup"]:
                observed["reason"] = (
                    "segment published before the warmup ended; choose a "
                    "later --occurrence")
                break
            success = True
            break
        status = process.poll()
        if status is not None:
            observed["exit_status"] = status
            observed["reason"] = f"emulator exited early with status {status}"
            break
        if elapsed >= plan["timeout_seconds"]:
            observed["reason"] = (
                f"timeout after {plan['timeout_seconds']} s without a "
                "published bundle")
            break
        time.sleep(1.0)
    observed["termination"] = terminate(process, args.shutdown_grace_seconds)
    if observed["exit_status"] is None:
        observed["exit_status"] = process.returncode
    for line in log.lines():
        if FINALIZED_PUBLISHED_LINE in line:
            observed["finalized_published"] = time.monotonic() - started
        for marker in FAILURE_LINES:
            if marker in line:
                observed["failure_lines"].append(line.strip())
    observed["elapsed_seconds"] = round(time.monotonic() - started, 3)
    observed["success_marker"] = success and not observed["failure_lines"]
    run["observed"] = observed
    return observed


def verify_bundle(plan, run):
    """Every acceptance condition for the published bundle, or the reasons."""
    target = plan["target"]
    problems = []
    if plan["session"]:
        problems.append(
            "session bundle verification is not implemented; the session "
            "runner must validate the manifest, chunks and checkpoints")
    staging = target.with_name(target.name + ".part")
    if staging.exists():
        problems.append(f"staging directory remains: {staging}")
    if not target.is_dir():
        problems.append(f"published bundle is missing: {target}")
        return problems
    bundle = {"path": str(target), "files": {}, "byte_size": 0}
    for name in BUNDLE_FILES:
        path = target / name
        if not path.is_file():
            problems.append(f"bundle file is missing: {path}")
            continue
        size = path.stat().st_size
        bundle["files"][name] = {"byte_size": size,
                                 "sha256": file_sha256(path)}
        bundle["byte_size"] += size
    extra = sorted(p.name for p in target.iterdir()
                   if p.name not in BUNDLE_FILES)
    if extra:
        problems.append(f"bundle holds unexpected entries: {extra}")
    if bundle["byte_size"] > plan["bundle_cap_bytes"]:
        problems.append(
            f"bundle is {bundle['byte_size']} bytes, above the cap "
            f"{plan['bundle_cap_bytes']}")
    run["bundle"] = bundle
    if problems:
        return problems
    try:
        with open(target / "manifest.json", "r", encoding="utf-8") as stream:
            manifest = json.load(stream)
    except (OSError, ValueError) as error:
        return [f"manifest.json is unreadable: {error}"]
    if manifest.get("capture_build_sha256") != plan["exe_sha256"]:
        problems.append(
            "manifest capture_build_sha256 "
            f"{manifest.get('capture_build_sha256')!r} is not the launched "
            f"executable {plan['exe_sha256']}")
    entries = {entry.get("name"): entry.get("value")
               for entry in manifest.get("replay_config", {}).get("entries", [])
               if isinstance(entry, dict)}
    expected_scheduler = plan["scheduler_flag"].split("=", 1)[1]
    if entries.get("guest_scheduler") != expected_scheduler:
        problems.append(
            f"manifest guest_scheduler={entries.get('guest_scheduler')!r} "
            f"does not match the requested lane ({expected_scheduler})")
    for section, name in (("corpus", "invocation.jcorpus"),
                          ("artifact", "invocation.guest_invocation")):
        record = manifest.get(section, {})
        actual = bundle["files"][name]
        if record.get("file") != name:
            problems.append(
                f"manifest {section}.file is {record.get('file')!r}")
        if record.get("sha256") != actual["sha256"]:
            problems.append(f"manifest {section}.sha256 does not match {name}")
        if record.get("byte_size") != actual["byte_size"]:
            problems.append(
                f"manifest {section}.byte_size does not match {name}")
    run["manifest"] = {
        "schema": manifest.get("schema"),
        "capture_build_sha256": manifest.get("capture_build_sha256"),
        "replay_config_sha256": manifest.get("replay_config_sha256"),
        "guest_scheduler": entries.get("guest_scheduler"),
        "root": manifest.get("root"),
        "counts": manifest.get("counts"),
    }
    return problems


def main(argv=None):
    args = parse_args(argv)
    try:
        plan = check_preconditions(args)
        command = build_command(args, plan)
    except LaunchError as error:
        failures = error.args[0]
        print(f"launch refused: {len(failures)} precondition(s) failed",
              file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 2

    target = plan["target"]
    run = {
        "schema": SCHEMA,
        "fixture": args.fixture,
        "lane": plan["lane"],
        "scheduler_flag": plan["scheduler_flag"],
        "boundary": plan["boundary"],
        "verdict": "not-launched",
        "command": command,
        "target": str(target),
        "launch_siblings": {
            suffix.lstrip("."): str(target.with_name(target.name + suffix))
            for suffix in LAUNCH_SIBLINGS},
        "executable": {
            "path": str(args.exe),
            "sha256_before": plan["exe_sha256"],
            "build_commit": args.build_commit,
            "markers": plan["exe_markers"],
        },
        "input_script": {
            "path": str(args.input_script),
            "sha256": file_sha256(args.input_script),
        },
        "storage_root": str(args.storage_root),
        "limits": {
            "warmup_seconds": plan["warmup_seconds"],
            "budget_seconds": plan["budget_seconds"],
            "timeout_seconds": plan["timeout_seconds"],
            "shutdown_grace_seconds": args.shutdown_grace_seconds,
            "bundle_cap_bytes": plan["bundle_cap_bytes"],
            "reserve_bytes": plan["reserve_bytes"],
        },
        "disk": plan["disk"],
        "prior_fixture": plan.get("prior_fixture"),
        "environment": {
            "host": platform.node(),
            "machine": platform.machine(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "tmpdir": os.environ.get("TMPDIR", ""),
            "working_directory": str(pathlib.Path.cwd()),
        },
        "tool": tool_provenance(),
    }
    if args.dry_run:
        run["verdict"] = "dry-run"
        print(json.dumps(run, allow_nan=False, indent=2, sort_keys=True))
        print("dry run: every precondition passed; nothing was launched")
        return 0

    run["title"] = title_fingerprint(args.title, args.full_title_sha256)
    run["started_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    report_path = target.with_name(target.name + ".launch.json")
    write_json_atomically(report_path, run)

    stdout_path = target.with_name(target.name + ".stdout.log")
    try:
        with open(stdout_path, "xb") as stdout_stream:
            process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=stdout_stream,
                stderr=subprocess.STDOUT)
    except OSError as error:
        run["verdict"] = "launch-failed"
        run["reason"] = f"unable to launch {args.exe}: {error}"
        write_json_atomically(report_path, run)
        print(run["reason"], file=sys.stderr)
        return 1

    try:
        observed = monitor(process, args, plan, run)
    finally:
        # monitor() terminates on every expected path; this backstop keeps
        # an unexpected exception from orphaning the emulator.
        if process.poll() is None:
            terminate(process, args.shutdown_grace_seconds)
    problems = []
    if not observed["success_marker"]:
        problems.append(observed["reason"] or "no success marker observed")
        problems.extend(observed["failure_lines"])
    problems.extend(verify_bundle(plan, run))
    exe_after = file_sha256(args.exe)
    run["executable"]["sha256_after"] = exe_after
    if exe_after != plan["exe_sha256"]:
        problems.append("executable changed during the run")
    if file_sha256(args.input_script) != run["input_script"]["sha256"]:
        problems.append("input script changed during the run")
    title_after = title_fingerprint(args.title, args.full_title_sha256)
    if title_after != run["title"]:
        problems.append("title changed during the run")

    run["problems"] = problems
    run["verdict"] = "accepted" if not problems else "not-accepted"
    run["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    write_json_atomically(report_path, run)
    if problems:
        print(f"capture not accepted ({len(problems)} problem(s)):",
              file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(f"report: {report_path}", file=sys.stderr)
        return 1
    print(f"capture accepted: {target} ({run['bundle']['byte_size']} bytes, "
          f"{plan['lane']} lane, {plan['scheduler_flag']}); "
          f"report: {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
