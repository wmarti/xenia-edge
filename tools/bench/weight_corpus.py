#!/usr/bin/env python3
"""Weight the emitted code of each guest function by how often it actually ran.

The corpus replay measures what the backend emits: host bytes, wide-move
chains, per function. It cannot say whether any of it runs. Every ranking built
on it alone is a static one, and static rankings have been wrong here before -
the generated PPC corpus's "hot" cost turned out to be a page-table rebuild in
the harness, not guest code at all.

This joins the replay CSV against the execution counts a real run produced:

  profile.csv    from Profiler::Dump on a --trace_function_coverage build,
                 `guestcoverage` section - guest instructions retired per
                 function, and the single hottest instruction in it.
  corpus CSV     from xenia-cpu-ppc-tests --jit_corpus_csv on an ordinary
                 build - host bytes and wide-move chains per function.

The two must come from SEPARATE runs. Coverage inlines a counter per guest
instruction, so a corpus captured with it on records instrumented sizes; that
mistake once put the recorded sizes 46% above what the replay could reproduce.

WARNING - the per-function weighting below is NOT sound, and the numbers it
prints must not be used to pick an optimization. It spreads a function's whole
emitted body uniformly over its guest instructions, so anything the backend put
in a cold tail is charged at the rate of the hot path that shares the function.
That is not a small effect here:

  - 82103AD8 and 82080608 hold ordinary loads and a return, one static chain
    each, and come out with billions of attributed chain executions. That chain
    is the preempt_yield_handler materialization in the preemption tail at
    a64_emitter.cc:1200, which is explicitly the cold side of a branch.
  - 825A7EC8 spends its count in an inner loop of eight db16cyc. Its
    materializations sit at calls and in tails outside that loop; smearing the
    loop's count over all five produced an apparent 92.25B hotspot.

Both were read as top optimization targets before the smear was spotted.

It also over-reports what is removable: replacing a chain with a single ldr
saves (chain instructions - chains), not the chain instruction total. On the
Halo 3 menu capture that is 167.266B of 284.845B, 2.36% of the modelled host
instructions rather than 4.02%.

The aggregate share is the only figure here with any standing, and only as an
order of magnitude. Per-guest-instruction attribution is what settles this, and
it lives in the guestsequences table instead - see DumpSequences in
processor.cc, whose counts are tied to individual guest instructions.
"""
import argparse
import csv
import pathlib
import sys


def read_coverage(path):
    """{guest_address: (executed, hottest_address, hottest_count)} from profile.csv."""
    rows = {}
    total = None
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("guestcoverage,"):
            total = int(lines[i].split(",")[1])
            i += 2  # skip the header row
            while i < len(lines) and lines[i].strip():
                parts = next(csv.reader([lines[i]]))
                if len(parts) < 6:
                    break
                rows[int(parts[0], 16)] = (int(parts[2]), int(parts[4], 16),
                                           int(parts[5]))
                i += 1
            break
        i += 1
    return rows, total


def read_sequences(path):
    """The guestsequences table: which backend emitter the executions land in."""
    rows, total = [], None
    lines = pathlib.Path(path).read_text().splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("guestsequences,"):
            total = int(lines[i].split(",")[1])
            i += 2
            while i < len(lines) and lines[i].strip():
                p = next(csv.reader([lines[i]]))
                if len(p) < 6:
                    break
                rows.append({"key": p[0], "sequence": p[1], "executed": int(p[2]),
                             "share": float(p[3]), "occurrences": int(p[4]),
                             "avgbytes": float(p[5])})
                i += 1
            break
        i += 1
    return rows, total


def read_corpus(path):
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["guest_address"], 16)] = {
                k: int(r[k]) for k in r if k != "guest_address"}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coverage", required=True, help="profile.csv")
    ap.add_argument("--corpus", required=True, help="jit_corpus_csv output")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    cov, grand_total = read_coverage(args.coverage)
    corp = read_corpus(args.corpus)
    if not cov:
        sys.exit(f"no guestcoverage section in {args.coverage}")

    joined, cov_only, corp_only = [], 0, 0
    for addr, (executed, hot_addr, hot_count) in cov.items():
        c = corp.get(addr)
        if c is None:
            cov_only += 1
            continue
        gi = c["guest_instructions"] or 1
        joined.append({
            "address": addr,
            "executed": executed,
            "guest_instructions": c["guest_instructions"],
            "host_bytes": c["host_bytes"],
            "hot_addr": hot_addr,
            "hot_count": hot_count,
            # Emitted quantities scaled by how many of this function's guest
            # instructions actually retired.
            "dyn_host_insts": c["host_bytes"] / 4.0 * executed / gi,
            "dyn_chains": c.get("address_chains", 0) * executed / gi,
            "dyn_chain_insts": c.get("address_chain_instructions", 0)
                               * executed / gi,
            "chains": c.get("address_chains", 0),
        })
    corp_only = len(set(corp) - set(cov))

    tot_exec = sum(j["executed"] for j in joined)
    tot_host = sum(j["dyn_host_insts"] for j in joined)
    tot_ci = sum(j["dyn_chain_insts"] for j in joined)
    tot_ch = sum(j["dyn_chains"] for j in joined)

    print(f"functions   {len(joined)} joined, {cov_only} ran but not in corpus, "
          f"{corp_only} in corpus but never ran")
    print(f"guest insts {tot_exec:,} retired "
          f"({100.0*tot_exec/grand_total:.1f}% of the run's {grand_total:,})")
    print(f"host insts  {tot_host:,.0f} executed (weighted)")
    print(f"  of which wide-move chain instructions: {tot_ci:,.0f} "
          f"({100.0*tot_ci/tot_host if tot_host else 0:.2f}%)")
    print(f"  materialization sites reached:         {tot_ch:,.0f}")
    if tot_ch:
        print(f"  average chain length: {tot_ci/tot_ch:.2f} instructions")

    # Static share, for the contrast that is the whole point of this script.
    s_host = sum(c["host_bytes"] / 4.0 for c in corp.values())
    s_ci = sum(c.get("address_chain_instructions", 0) for c in corp.values())
    print(f"\nstatic, for comparison: {s_ci:,.0f} of {s_host:,.0f} "
          f"({100.0*s_ci/s_host if s_host else 0:.2f}%) - what the replay alone "
          f"reports")

    joined.sort(key=lambda j: j["dyn_host_insts"], reverse=True)
    print(f"\ntop {args.top} functions by executed host instructions")
    print(f"{'address':>10} {'executed':>14} {'share':>7} {'hostB':>7} "
          f"{'dyn host I':>14} {'dyn chain I':>13} {'chain%':>7} {'hottest':>10}")
    for j in joined[:args.top]:
        print(f"{j['address']:010X} {j['executed']:14,} "
              f"{100.0*j['executed']/tot_exec:6.2f}% {j['host_bytes']:7,} "
              f"{j['dyn_host_insts']:14,.0f} {j['dyn_chain_insts']:13,.0f} "
              f"{100.0*j['dyn_chain_insts']/j['dyn_host_insts'] if j['dyn_host_insts'] else 0:6.2f}% "
              f"{j['hot_addr']:010X}")

    ranked = sorted(joined, key=lambda j: j["dyn_chain_insts"], reverse=True)
    print(f"\ntop {args.top} functions by executed chain instructions "
          f"(where fixing chains would actually pay)")
    print(f"{'address':>10} {'dyn chain I':>13} {'cum%':>7} {'chains':>7} "
          f"{'executed':>14}")
    cum = 0.0
    for j in ranked[:args.top]:
        cum += j["dyn_chain_insts"]
        print(f"{j['address']:010X} {j['dyn_chain_insts']:13,.0f} "
              f"{100.0*cum/tot_ci if tot_ci else 0:6.2f}% {j['chains']:7,} "
              f"{j['executed']:14,}")

    seqs, seq_total = read_sequences(args.coverage)
    if seqs:
        print(f"\ntop {args.top} backend sequences by executions "
              f"(of {seq_total:,} sampled)")
        print(f"{'share':>7} {'cum':>7} {'executed':>15} {'sites':>8} "
              f"{'avgB':>7}  sequence")
        cum = 0.0
        for r in seqs[:args.top]:
            cum += r["share"]
            print(f"{100*r['share']:6.2f}% {100*cum:6.2f}% {r['executed']:15,} "
                  f"{r['occurrences']:8,} {r['avgbytes']:7.1f}  {r['sequence']}")

    if args.out:
        with open(args.out, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["guest_address", "executed", "guest_instructions",
                        "host_bytes", "address_chains", "dyn_host_insts",
                        "dyn_chain_insts", "hottest_address", "hottest_count"])
            for j in joined:
                w.writerow([f"{j['address']:08X}", j["executed"],
                            j["guest_instructions"], j["host_bytes"],
                            j["chains"], round(j["dyn_host_insts"], 1),
                            round(j["dyn_chain_insts"], 1),
                            f"{j['hot_addr']:08X}", j["hot_count"]])
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
