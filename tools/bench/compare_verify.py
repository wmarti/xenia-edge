#!/usr/bin/env python3
"""Diff two verify_corpus.py runs and decide whether the change is admissible.

Two different comparisons use this. A/B on one backend answers "did this ref
break anything the previous ref got right"; A64-vs-x64 at the same ref answers
"do the two backends still agree", which is the question the shared PPC
frontend commits actually put at risk.

Exit status is the gate: 0 when nothing regressed, 1 when something did.
"""
import argparse
import json
import sys

# Ranked worst-first so a suite that changes verdict is described by the more
# serious of the two states.
SEVERITY = {"pass": 0, "fail": 1, "timeout": 2, "crash": 3}


def load(path):
    with open(path) as f:
        doc = json.load(f)
    return doc, {r["suite"]: r for r in doc["results"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("candidate")
    ap.add_argument("--json-out", default="")
    ap.add_argument("--allow-new-failures", action="store_true",
                    help="report regressions but still exit 0")
    args = ap.parse_args()

    base_doc, base = load(args.baseline)
    cand_doc, cand = load(args.candidate)

    regressions, fixes, changed_counts = [], [], []
    for suite in sorted(set(base) | set(cand)):
        b, c = base.get(suite), cand.get(suite)
        if b is None:
            # Only the candidate ran it. Not a regression, but worth surfacing
            # so a corpus that silently grew doesn't look like a clean run.
            fixes.append((suite, "absent", c["verdict"]))
            continue
        if c is None:
            regressions.append((suite, b["verdict"], "absent"))
            continue
        sb, sc = SEVERITY[b["verdict"]], SEVERITY[c["verdict"]]
        if sc > sb:
            regressions.append((suite, b["verdict"], c["verdict"]))
        elif sc < sb:
            fixes.append((suite, b["verdict"], c["verdict"]))
        elif b["failed"] != c["failed"]:
            # Same verdict, different number of failing cases inside it.
            (regressions if c["failed"] > b["failed"] else fixes).append(
                (suite, f"{b['failed']} failed", f"{c['failed']} failed"))
        elif b["total"] != c["total"]:
            changed_counts.append((suite, b["total"], c["total"]))

    def show(title, rows):
        if not rows:
            return
        print(f"\n{title} ({len(rows)}):")
        for suite, was, now in rows:
            print(f"  {suite}: {was} -> {now}")

    print(f"baseline : {base_doc.get('label') or args.baseline}")
    print(f"candidate: {cand_doc.get('label') or args.candidate}")
    print(f"  suites {base_doc['suites']} -> {cand_doc['suites']}, "
          f"cases {base_doc['cases']} -> {cand_doc['cases']}")
    show("REGRESSIONS", regressions)
    show("fixes", fixes)
    if changed_counts:
        print(f"\ncase-count changes ({len(changed_counts)}):")
        for suite, was, now in changed_counts:
            print(f"  {suite}: {was} -> {now} cases")
    if not regressions:
        print("\nno regressions")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({
                "baseline": base_doc.get("label") or args.baseline,
                "candidate": cand_doc.get("label") or args.candidate,
                "regressions": [
                    {"suite": s, "was": w, "now": n} for s, w, n in regressions],
                "fixes": [
                    {"suite": s, "was": w, "now": n} for s, w, n in fixes],
                "case_count_changes": [
                    {"suite": s, "was": w, "now": n}
                    for s, w, n in changed_counts],
            }, f, indent=2)
            f.write("\n")

    return 1 if regressions and not args.allow_new_failures else 0


if __name__ == "__main__":
    sys.exit(main())
