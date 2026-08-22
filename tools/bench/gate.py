#!/usr/bin/env python3
"""Run the correctness gate for a ref pair and emit one auditable result bundle.

This is the machine-independent half of the two-machine setup: given two
already-built binaries and the corpus, it produces the same JSON shape on
Apple Silicon and on an x64 compute node, so the two can be compared to each
other rather than only to their own history.

Building and scheduling are deliberately left to the caller, because those are
where the two machines genuinely differ (a local build here, a container under
SLURM there). Everything downstream of "two binaries exist" is shared.
"""
import argparse
import json
import pathlib
import platform
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent


def sh(cmd):
    """Run cmd, returning stripped stdout or '' — conditions are best-effort."""
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              timeout=20).stdout.strip()
    except Exception:
        return ""


def conditions():
    """Record what the machine was doing, so a bad run can be spotted later.

    A number without its conditions is not evidence: measuring this project on
    a loaded shared box once produced a 47% regression that repeat measurement
    erased completely.
    """
    c = {
        "host": platform.node(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    # Which tooling produced this bundle. Several sessions edit this
    # repository, so a result that cannot be traced to a commit — and to
    # whether the tree was dirty at the time — is hard to trust later.
    here = str(HERE)
    c["tool_commit"] = sh(f"git -C {here} rev-parse --short HEAD")
    c["tool_branch"] = sh(f"git -C {here} rev-parse --abbrev-ref HEAD")
    c["tool_dirty"] = bool(sh(f"git -C {here} status --porcelain -- {here}"))
    if c["system"] == "Darwin":
        c["cpu"] = sh("sysctl -n machdep.cpu.brand_string")
        c["ncpu"] = sh("sysctl -n hw.ncpu")
        c["load"] = sh("sysctl -n vm.loadavg")
        c["thermal"] = sh("pmset -g therm")
        c["low_power"] = sh("pmset -g | grep -i lowpowermode")
        c["on_ac"] = "AC Power" in sh("pmset -g ps")
    else:
        c["cpu"] = sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-").strip()
        c["ncpu"] = sh("nproc")
        c["load"] = sh("cat /proc/loadavg")
        c["governor"] = sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        c["slurm_job"] = sh("echo $SLURM_JOB_ID")
        c["slurm_node"] = sh("echo $SLURMD_NODENAME")
        # AVX-512 presence decides which x64 codepaths the run actually
        # exercised, so it belongs in the record rather than being inferred
        # from the node name later.
        c["avx512"] = bool(sh("grep -m1 -o avx512f /proc/cpuinfo"))
    return c


def verify(exe, label, corpus, test_path, skip_file, out):
    cmd = [sys.executable, str(HERE / "verify_corpus.py"),
           "--exe", exe, "--corpus", corpus, "--test-path", test_path,
           "--label", label, "--out", out]
    if skip_file:
        cmd += ["--skip-file", skip_file]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    sys.stderr.write(proc.stderr)
    print(proc.stdout.strip())
    return json.loads(pathlib.Path(out).read_text())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-a", required=True)
    ap.add_argument("--exe-b", required=True)
    ap.add_argument("--ref-a", default="A")
    ap.add_argument("--ref-b", default="B")
    ap.add_argument("--backend", required=True, choices=["a64", "x64"])
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--test-path", required=True)
    ap.add_argument("--skip-file", default="")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    out = pathlib.Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    cond = conditions()
    print(f"== {args.backend} on {cond['host']} ({cond.get('cpu', '?')})")

    a = verify(args.exe_a, f"{args.ref_a} {args.backend}", args.corpus,
               args.test_path, args.skip_file, str(out / "verify-a.json"))
    b = verify(args.exe_b, f"{args.ref_b} {args.backend}", args.corpus,
               args.test_path, args.skip_file, str(out / "verify-b.json"))

    cmp_proc = subprocess.run(
        [sys.executable, str(HERE / "compare_verify.py"),
         str(out / "verify-a.json"), str(out / "verify-b.json"),
         "--json-out", str(out / "gate.json")],
        capture_output=True, text=True)
    print(cmp_proc.stdout)
    gate = json.loads((out / "gate.json").read_text())
    # A comparison only means something if both sides actually ran. When they
    # break the same way — the x64 binary failing to find libSDL3 outside the
    # container it was built in — the diff is empty and the gate would
    # otherwise report a pass on two piles of nothing.
    broken = [name for name, doc in (("a", a), ("b", b)) if doc["cases"] == 0]
    clean = not gate["regressions"] and not broken

    result = {
        "backend": args.backend,
        "ref_a": args.ref_a,
        "ref_b": args.ref_b,
        "conditions": cond,
        "a": {k: a[k] for k in
              ("suites", "cases", "failed", "crashed", "timed_out")},
        "b": {k: b[k] for k in
              ("suites", "cases", "failed", "crashed", "timed_out")},
        "regressions": gate["regressions"],
        "fixes": gate["fixes"],
        "gate": "pass" if clean else "fail",
        "broken_sides": broken,
    }
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    if broken:
        print(f"\nERROR: ran no test cases at all on side(s): "
              f"{', '.join(broken)}. The comparison is meaningless.")
    print(f"\ngate: {result['gate']}  "
          f"({len(gate['regressions'])} regressions, {len(gate['fixes'])} fixes)")
    print(f"bundle: {out}")
    # Non-zero means "do not go on to benchmark this pair": timing a
    # miscompile is meaningless.
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
