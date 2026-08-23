#!/usr/bin/env python3
"""Rank backend sequences by the work a title actually executes.

Reads the tables --trace_function_coverage_out writes and answers one question:
for a proposed codegen change T, how many host instructions would it actually
remove? That is

    score(T) = sum over affected sites s of  E_s * (C_old,s - C_new,s)

where E_s is the site's own execution count. Every weaker form of this has been
wrong here: a function's average smears cold tail code at the hot path's rate, a
sequence's average size assumes size and heat are uncorrelated, and a gross
instruction share is not a saving because the replacement is not free.

The columns this consumes are built to make that sum exact rather than modelled:

  exechostbytes    sum over sites of executions x THAT site's inline bytes
  exectailbytes    the same for bytes the sequence pushed into a cold tail.
                   Reported apart and never added to the hot total. It is an
                   UPPER BOUND, not a cost: it charges the tail at the
                   enclosing guest instruction's rate, and a tail is by
                   construction the side of a branch that usually is not taken.
                   Only a taken-counter on the tail itself would settle it.
  execchaininsts   executions x wide-move chain instructions at that site
  execchains       executions x chains at that site, so replacing each chain
                   with one ldr saves exactly execchaininsts - execchains

What this still cannot tell you: whether the emulator's CPU time is in JIT-ed
guest code at all. That needs an uninstrumented host-PC sample of the same
build. Rank here, then require the top targets to also show up there.
"""
import argparse
import csv
import pathlib
import sys


def section(lines, name, ncols):
    """Rows of the named table, plus its header values."""
    for i, line in enumerate(lines):
        if line.startswith(name + ","):
            totals = line.split(",")[1:]
            rows = []
            j = i + 2
            while j < len(lines) and lines[j].strip():
                parts = next(csv.reader([lines[j]]))
                if len(parts) < ncols:
                    break
                rows.append(parts)
                j += 1
            return rows, totals
    return [], []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coverage", required=True)
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    lines = pathlib.Path(args.coverage).read_text().splitlines()

    seq, totals = section(lines, "guestsequences", 13)
    if not seq:
        sys.exit("no guestsequences table with the exact columns; the capture "
                 "predates the attribution fix and cannot be ranked")
    rows = [{"key": r[0], "seq": r[1], "executed": int(r[2]),
             "occurrences": int(r[4]), "avgbytes": float(r[5]),
             "exechostbytes": int(r[6]), "exectailbytes": int(r[8]),
             "execchaininsts": int(r[9]), "execchains": int(r[10]),
             "tailbytes": int(r[11]), "chains": int(r[12])} for r in seq]

    hot = sum(r["exechostbytes"] for r in rows)
    tail = sum(r["exectailbytes"] for r in rows)
    ci = sum(r["execchaininsts"] for r in rows)
    ch = sum(r["execchains"] for r in rows)
    hot_i = hot / 4.0

    print(f"{len(rows)} distinct sequences")
    print(f"executed host instructions (inline, exact): {hot_i:,.0f}")
    print(f"  in wide-move chains:  {ci/4.0:,.0f} "
          f"({100.0*ci/hot if hot else 0:.2f}%)")
    print(f"  chain sites reached:  {ch/4.0:,.0f}")
    print(f"\nscore(chain -> one ldr) = {(ci-ch)/4.0:,.0f} instructions "
          f"({100.0*(ci-ch)/hot if hot else 0:.2f}% of executed)")
    print(f"cold tail bytes at the enclosing rate: {tail/4.0:,.0f} "
          f"instructions -- UPPER BOUND, not a cost")

    print(f"\ntop {args.top} sequences by executed host instructions")
    print(f"{'share':>7} {'cum':>7} {'exec host I':>17} {'executions':>16} "
          f"{'I/exec':>7} {'chain I':>14} {'sites':>8}  sequence")
    rows.sort(key=lambda r: r["exechostbytes"], reverse=True)
    cum = 0
    for r in rows[:args.top]:
        cum += r["exechostbytes"]
        per = r["exechostbytes"] / r["executed"] / 4.0 if r["executed"] else 0
        print(f"{100.0*r['exechostbytes']/hot:6.2f}% {100.0*cum/hot:6.2f}% "
              f"{r['exechostbytes']/4.0:17,.0f} {r['executed']:16,} {per:7.2f} "
              f"{r['execchaininsts']/4.0:14,.0f} {r['occurrences']:8,}  "
              f"{r['seq']}")

    chainy = [r for r in rows if r["execchaininsts"] > r["execchains"]]
    if chainy:
        chainy.sort(key=lambda r: r["execchaininsts"] - r["execchains"],
                    reverse=True)
        print(f"\ntop sequences by score(chain -> one ldr)")
        print(f"{'saved I':>16} {'share':>7} {'chains':>8}  sequence")
        for r in chainy[:12]:
            saved = (r["execchaininsts"] - r["execchains"]) / 4.0
            print(f"{saved:16,.0f} {100.0*saved*4/hot:6.2f}% {r['chains']:8,}  "
                  f"{r['seq']}")

    pcs, pct = section(lines, "guestpc", 2)
    if pcs:
        vals = sorted(((int(a, 16), int(e)) for a, e in pcs),
                      key=lambda t: -t[1])
        gt = sum(v for _, v in vals)
        print(f"\n{len(vals):,} guest PCs executed, {gt:,} total")
        top_n = sum(v for _, v in vals[:100])
        print(f"top 100 guest PCs are {100.0*top_n/gt:.1f}% of all guest "
              f"instruction executions")
        print(f"\ntop 15 guest PCs")
        for a, v in vals[:15]:
            print(f"  {a:08X} {v:16,} {100.0*v/gt:6.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
