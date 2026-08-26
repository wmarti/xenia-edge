#!/usr/bin/env python3
"""Price a codegen change before writing it.

Joins a JIT corpus (which guest functions compiled, and their instruction
bytes) against --trace_function_coverage output (how many instructions each
function actually executed). Answers two questions a static count cannot:

  1. Which PPC opcodes dominate EXECUTED code, not emitted code.
  2. What share of execution a given set of functions accounts for.

Both matter because this title's execution is extremely concentrated: at the
GTA IV docks two functions are 73.5% of every guest instruction executed. Any
saving outside them is diluted by ~4x before it reaches a whole-program number,
which is what sank two candidate optimisations that looked worthwhile on a
static histogram.

Usage:
  weight_opcodes.py <corpus.bin> <coverage.csv> [--exclude ADDR[,ADDR...]]

The per-function attribution assumes instructions inside a function execute
uniformly. That is wrong in detail -- coverage.csv names a `hottest` address
per function precisely because they do not -- so treat opcode shares as
first-order. The function-level shares it prints are exact.
"""
import collections
import struct
import sys

PAGE = 0x1000

# PPC primary opcode (bits 0-5) -> name. Only the ones that show up hot; the
# rest print as their number, which is enough to go look them up.
NAMES = {
    7: "mulli", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 18: "b", 19: "CR ops / bclr / bcctr",
    20: "rlwimi", 21: "rlwinm", 23: "rlwnm", 24: "ori", 25: "oris",
    26: "xori", 28: "andi.", 29: "andis.", 30: "MD-form rld*",
    31: "X-form (add/and/or/cmp/shift)", 32: "lwz", 33: "lwzu", 34: "lbz",
    36: "stw", 37: "stwu", 38: "stb", 40: "lhz", 42: "lha", 44: "sth",
    48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd",
}


def read_corpus(path):
    """-> (pages: addr -> bytes, funcs: addr -> end_addr)."""
    data = open(path, "rb").read()
    magic, version, page_size, _ = struct.unpack_from("<4I", data, 0)
    if magic != 0x3143584A:
        raise SystemExit(f"{path}: not a corpus (magic {magic:08X})")
    off, pages, funcs = 16, {}, {}
    while off + 4 <= len(data):
        tag = struct.unpack_from("<I", data, off)[0]
        off += 4
        if tag == 1:
            if off + 4 + page_size > len(data):
                break  # truncated capture; tolerate
            addr = struct.unpack_from("<I", data, off)[0]
            off += 4
            pages[addr] = data[off:off + page_size]
            off += page_size
        elif tag == 2:
            if off + 16 > len(data):
                break
            a, e, _hs, _fl = struct.unpack_from("<4I", data, off)
            off += 16
            funcs[a] = e
        else:
            break
    return pages, funcs, version


def read_coverage(path):
    """-> (rows: addr -> (static_instrs, executed_instrs), total_executed)."""
    rows, total = {}, 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("guestcoverage,"):
                total = int(line.split(",")[1])
                continue
            parts = [p.strip('"') for p in line.split(",")]
            if len(parts) < 4 or parts[0] == "address":
                continue
            try:
                rows[int(parts[0], 16)] = (int(parts[1]), int(parts[2]))
            except ValueError:
                continue
    return rows, total


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    corpus_path, coverage_path = argv[1], argv[2]
    exclude = set()
    if len(argv) > 3 and argv[3] == "--exclude" and len(argv) > 4:
        exclude = {int(x, 16) for x in argv[4].split(",")}

    pages, funcs, version = read_corpus(corpus_path)
    cov, total = read_coverage(coverage_path)
    print(f"corpus v{version}: {len(funcs)} functions, {len(pages)} pages")
    print(f"coverage: {len(cov)} rows, {total:,} guest instructions executed")

    def word(addr):
        page = addr & ~(PAGE - 1)
        blob = pages.get(page)
        if blob is None:
            return None
        return struct.unpack_from(">I", blob, addr - page)[0]

    # Function-level concentration first: this is exact, no uniformity
    # assumption, and it is usually the number that decides the tick.
    ranked = sorted(((c[1], a) for a, c in cov.items()), reverse=True)
    print("\nmost-executed functions")
    print(f"  {'address':>10} {'executed':>18} {'share':>8}  cumulative")
    cum = 0
    for executed, addr in ranked[:8]:
        cum += executed
        print(f"  {addr:>10X} {executed:>18,} {100*executed/total:7.2f}% "
              f"{100*cum/total:9.2f}%")

    if exclude:
        print(f"\nexcluding {', '.join(f'{a:08X}' for a in sorted(exclude))}")

    weighted = collections.Counter()
    attributed = 0.0
    missing_pages = 0
    for addr, end in funcs.items():
        if addr in exclude:
            continue
        entry = cov.get(addr)
        if entry is None:
            continue  # compiled but never executed in the traced window
        words = [word(addr + i * 4) for i in range((end - addr) // 4 + 1)]
        present = [w for w in words if w is not None]
        if not present:
            missing_pages += 1
            continue
        per_instruction = entry[1] / len(present)
        for w in present:
            weighted[w >> 26] += per_instruction
            attributed += per_instruction

    if not attributed:
        raise SystemExit("nothing attributed -- do the corpus and coverage "
                         "come from the same run?")
    print(f"\nattributed {attributed:,.0f} executed instructions "
          f"({100*attributed/total:.1f}% of the total)")
    if missing_pages:
        print(f"  ({missing_pages} functions skipped: code pages not captured)")
    print("\nopcodes by executed share")
    print(f"  {'op':>3} {'name':<32} {'share':>8}")
    for op, count in weighted.most_common(15):
        print(f"  {op:>3} {NAMES.get(op, str(op)):<32} "
              f"{100*count/attributed:7.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
