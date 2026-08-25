#!/usr/bin/env python3
"""Count HIR ops and provably-dead context stores over a frozen dump.

Written because the first version of this analysis was a throwaway heredoc run
twice, minutes apart, against a directory the emulator was still writing into.
The two runs saw 14,057 and 18,362 functions and their numbers were published
side by side as though they came from one corpus. They did not, and with the
dump deleted neither could be audited. So: one script, in the repo, that prints
its own denominators and refuses to run against a directory still being
written.

Input is a directory of files written by --dump_translated_hir_functions, one
per translated function, named by guest address in hex. The dump is taken after
every pass including register allocation, so what it holds is the HIR that was
actually handed to the backend.

Two questions:

  1. Op mix. How many HIR ops per guest instruction, and what share of them is
     context traffic. This is the number that says whether the frontend is fat
     or whether the cost is somewhere else.

  2. How many store_context ops are provably dead, split by what has to be
     assumed:
       A) overwritten before any read within one block -- no assumption at all
       B) a caller-volatile GPR still pending at a return -- needs the guest to
          honour the PowerPC ABI, which hand-written assembly need not

     r3/r4 are excluded from B: they carry return values, so a store to them
     before a blr is exactly what the caller reads.

   context_barrier is NOT a barrier here. Its opcode flags are 0 and IsFake()
   returns true, so it blocks nothing -- treating it as a barrier was what made
   an earlier version of this count report a flat zero.
"""
import argparse, collections, os, re, sys, time

R_BASE = 40           # offsetof(PPCContext, r)
GPR = {R_BASE + 8 * n: f"r{n}" for n in range(32)}
# Caller-volatile GPRs, minus r3/r4 which carry return values.
VOLATILE_NO_RETVAL = {R_BASE + 8 * n for n in [0] + list(range(5, 13))}
BARRIER = re.compile(r"^(call|call_indirect|call_extern|call_true|branch|"
                     r"branch_true|branch_false|return|check_preempt|trap|"
                     r"memory_barrier)")
STORE = re.compile(r"^store_context \+(\d+),")
LOAD = re.compile(r"^(?:v\d+\.\w+(?:<\w+>)? = )?load_context \+(\d+)")
GUEST = re.compile(r"^; [0-9A-F]{8} [0-9A-F]{8} ")
OP = re.compile(r"^(?:v\d+\.\w+(?:<\w+>)? = )?([a-z_0-9]+)")
NAME = re.compile(r"^[0-9A-F]{8}$")


def settled(path, seconds=5.0):
    """Refuse to count a directory something is still writing into."""
    a = len(os.listdir(path))
    time.sleep(seconds)
    b = len(os.listdir(path))
    return a == b, a, b


def analyse(path):
    ops = collections.Counter()
    guest_insns = hir_ops = 0
    total_store = dead_overwrite = dead_at_return = 0
    by_reg = collections.Counter()
    files = 0
    for name in os.listdir(path):
        if not NAME.match(name):
            continue
        files += 1
        pending = {}
        for raw in open(os.path.join(path, name), errors="replace"):
            s = raw.strip()
            if s.startswith(";"):
                if GUEST.match(s):
                    guest_insns += 1
                continue
            if not s or s.startswith("<"):
                continue
            if s.endswith(":"):
                pending.clear()       # a label is a new block
                continue
            hir_ops += 1
            m = OP.match(s)
            if m:
                ops[m.group(1)] += 1
            body = re.sub(r"^v\d+\.\w+(?:<\w+>)? = ", "", s)
            if body.startswith("context_barrier"):
                continue
            if BARRIER.match(body):
                if body.startswith("call_indirect"):
                    for off in pending:
                        if off in VOLATILE_NO_RETVAL:
                            dead_at_return += 1
                            by_reg[GPR.get(off, off)] += 1
                pending.clear()
                continue
            m = STORE.match(s)
            if m:
                off = int(m.group(1))
                total_store += 1
                if off in pending:
                    dead_overwrite += 1
                pending[off] = True
                continue
            m = LOAD.match(s)
            if m:
                pending.pop(int(m.group(1)), None)
    return dict(ops=ops, files=files, guest_insns=guest_insns, hir_ops=hir_ops,
                total_store=total_store, dead_overwrite=dead_overwrite,
                dead_at_return=dead_at_return, by_reg=by_reg)


def report(r, label):
    print(f"=== {label} ===")
    print(f"{r['files']} functions, {r['guest_insns']} guest instructions, "
          f"{r['hir_ops']} HIR ops")
    if r["guest_insns"]:
        print(f"HIR ops per guest instruction: "
              f"{r['hir_ops']/r['guest_insns']:.2f}")
    tot = r["hir_ops"] or 1
    sc, lc = r["ops"]["store_context"], r["ops"]["load_context"]
    print(f"\nstore_context {sc:>9}  {100.0*sc/tot:5.1f}% of HIR ops")
    print(f"load_context  {lc:>9}  {100.0*lc/tot:5.1f}%")
    print(f"context total {sc+lc:>9}  {100.0*(sc+lc)/tot:5.1f}%")
    # Cross-check: the store count used as the dead-store denominator must be
    # the same number as the op-mix store count. They were not, once.
    assert sc == r["total_store"], (
        f"INCONSISTENT: op mix counted {sc} store_context, dead-store pass "
        f"counted {r['total_store']}")
    print(f"\nof {r['total_store']} store_context:")
    print(f"  overwritten before any read, same block  "
          f"{r['dead_overwrite']:>7}  "
          f"{100.0*r['dead_overwrite']/max(r['total_store'],1):5.2f}%")
    print(f"  volatile GPR pending at a return (ABI)   "
          f"{r['dead_at_return']:>7}  "
          f"{100.0*r['dead_at_return']/max(r['total_store'],1):5.2f}%")
    print("\ntop ops:")
    for op, n in r["ops"].most_common(10):
        print(f"  {op:<20} {n:>9}  {100.0*n/tot:5.1f}%")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+",
                    help="one or more frozen hirdump directories; two are "
                         "compared against each other")
    ap.add_argument("--labels", default="")
    ap.add_argument("--allow-unsettled", action="store_true")
    a = ap.parse_args()
    labels = a.labels.split(",") if a.labels else a.dirs
    results = []
    for d, label in zip(a.dirs, labels):
        ok, before, after = settled(d)
        if not ok and not a.allow_unsettled:
            sys.exit(f"{d} is still being written ({before} -> {after} files). "
                     f"Stop the emulator first, or pass --allow-unsettled and "
                     f"do not publish the number.")
        results.append((label, analyse(d)))
    for label, r in results:
        report(r, label)
    if len(results) == 2:
        (la, ra), (lb, rb) = results
        print("=== difference ===")
        if ra["files"] != rb["files"]:
            print(f"WARNING: different corpus sizes ({ra['files']} vs "
                  f"{rb['files']} functions) -- shares are comparable, "
                  f"absolute counts are not")
        for key in ("hir_ops", "total_store"):
            x, y = ra[key], rb[key]
            print(f"  {key:<14} {x:>9} -> {y:>9}   {100.0*(y-x)/max(x,1):+7.2f}%")
        for op in ("store_context", "load_context"):
            x, y = ra["ops"][op], rb["ops"][op]
            print(f"  {op:<14} {x:>9} -> {y:>9}   {100.0*(y-x)/max(x,1):+7.2f}%")


if __name__ == "__main__":
    main()
