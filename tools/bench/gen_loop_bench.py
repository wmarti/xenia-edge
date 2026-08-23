#!/usr/bin/env python3
"""Generate guest-side loop benchmarks for the PPC test harness.

The generated corpus cannot measure emitted code. Every case there is one guest
instruction and a blr, wrapped in a heap reset, a ThreadState teardown, a JIT
compile and a string compare - about 800,000 host cycles to execute one guest
instruction, of which a profile finds 47% in BaseHeap::RebuildFreeBlocks, 11%
in Capstone, 1.7% in code generation and none at all in guest code. A backend
change that makes an emitted sequence faster cannot move that number; only one
that makes the sequence *smaller* does, and then only because the harness
disassembles everything it emits.

These benchmarks put the opcode in a loop instead, so the emitted sequence runs
hundreds of millions of times and the per-case overhead amortises to nothing.

Two shapes per opcode:

  lat - a serial dependency chain, each instruction consuming the previous
        result. Measures latency. Nothing can be eliminated, because every
        result feeds the next instruction and the last one reaches the guest
        context.
  tp  - independent instructions reduced together at the end of the body.
        Measures throughput. The reduction is a fixed cost paid identically by
        both refs.

Two properties of the generated code, both confirmed by reading back the final
register from a short run of each ref:

  - In `lat`, the float opcodes chain into their own result, so vmaddfp and
    vnmsubfp diverge to Inf/NaN within a few iterations and stay there. Those
    suites measure the *nonfinite* path, which is what ccaa671b0, dcf981c08,
    8c59d9030 and 1dd664b3f target - useful, but it must be read as such and
    not as the ordinary-operand cost. In `tp` the opcodes only ever read the
    seeded operand registers, so the float inputs stay normal throughout.
  - In `tp` the reduction can XOR back to the accumulator's seed when the
    rotation repeats an operand triple. That leaves the accumulator
    loop-invariant. It does not affect the timing - there is no
    loop-invariant motion pass, so the body still executes every iteration -
    but it does mean the final value is not a useful checksum.

Both refs must compute the same final register for a timing comparison to mean
anything; run a short version with a deliberately wrong REGISTER_OUT and
compare the values the harness reports.

The aliasing pattern is deliberate. SELECT_V128 and the scalar float binops
have separate emitter paths for dest-aliases-src1, -src2 and -src3, and the
commits that claim to optimise them only touch the aliased cases, so the chain
rotates the destination through all three operand positions.

No assembler is required: third_party/binutils ships patches and a build.sh but
no binaries, and PPC is fixed-width, so the encodings are written out directly.

Usage:
  gen_loop_bench.py --out-bin <corpus dir> --out-src <dir>... [--iters N]
"""
import argparse
import pathlib
import struct
import sys

# Vector registers the harness seeds through REGISTER_IN. v0 is the running
# accumulator; v1..v7 are read-only operands.
ACC = 0
OPS = [1, 2, 3, 4, 5, 6, 7]

# Bit patterns chosen so the float opcodes see ordinary normal numbers. A mask
# perturbed into denormals or NaNs would measure the denormal fixup path
# instead of the opcode, and that path is itself one of the things under test.
SEEDS = {
    0: "3F800000 3F800000 3F800000 3F800000",
    1: "40000000 40000000 40000000 40000000",
    2: "40400000 40400000 40400000 40400000",
    3: "40800000 40800000 40800000 40800000",
    4: "0000FFFF FFFF0000 00FF00FF FF00FF00",
    5: "FFFFFFFF 00000000 FFFFFFFF 00000000",
    6: "3FC00000 3FC00000 3FC00000 3FC00000",
    7: "40A00000 40A00000 40A00000 40A00000",
}

# VA-form: opcode 4, XO in bits 26-31, four vector operands.
VA_FORM = {
    "vsel": 42,
    "vperm": 43,
    "vmaddfp": 46,
    "vnmsubfp": 47,
}
# VX-form: opcode 4, XO in bits 21-31, three vector operands.
VX_FORM = {
    "vaddfp": 10,
    "vand": 1028,
    "vor": 1156,
    "vxor": 1220,
    "vpkuhus": 142,
    "vavgsb": 1282,
    "vavgsw": 1410,
}


def va(xo, d, a, b, c):
    return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | xo


def vx(xo, d, a, b):
    return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | xo


def addic_dot(d, a, simm):
    """addic. rD,rA,SIMM - subtracting one and recording into CR0."""
    return (13 << 26) | (d << 21) | (a << 16) | (simm & 0xFFFF)


def bne(displacement_bytes):
    """bne <rel> - BO=4 (branch if false), BI=2 (CR0[EQ])."""
    bd = displacement_bytes >> 2
    return (16 << 26) | (4 << 21) | (2 << 16) | ((bd & 0x3FFF) << 2)


BLR = 0x4E800020


def emit_op(name, dest, srcs):
    """One instruction writing `dest`, reading `srcs` (cycled as needed)."""
    def s(i):
        return srcs[i % len(srcs)]
    if name in VA_FORM:
        return va(VA_FORM[name], dest, s(0), s(1), s(2))
    if name in VX_FORM:
        return vx(VX_FORM[name], dest, s(0), s(1))
    raise SystemExit(f"unknown opcode {name}")


def arity(name):
    return 3 if name in VA_FORM else 2


def body_lat(name, width):
    """Serial chain: every instruction reads the accumulator and writes it.

    The destination rotates through the operand positions so the emitter's
    dest-aliases-src1/2/3 paths are all exercised.
    """
    out = []
    n = arity(name)
    for i in range(width):
        srcs = list(OPS[i % len(OPS):] + OPS[:i % len(OPS)])[:n]
        # Place the accumulator in a different source slot each time.
        srcs[i % n] = ACC
        out.append(emit_op(name, ACC, srcs))
    return out


def body_tp(name, width):
    """Independent instructions into scratch registers, then reduced into ACC.

    The reduction is what keeps the results live; without it dead-code
    elimination is free to drop every one of them.
    """
    out = []
    n = arity(name)
    scratch = list(range(8, 8 + width))
    for i, d in enumerate(scratch):
        srcs = list(OPS[i % len(OPS):] + OPS[:i % len(OPS)])[:n]
        out.append(emit_op(name, d, srcs))
    for d in scratch:
        out.append(vx(VX_FORM["vxor"], ACC, ACC, d))
    return out


def build(name, shape, width, iters):
    """(machine code words, guest instructions executed per iteration)."""
    body = body_lat(name, width) if shape == "lat" else body_tp(name, width)
    words = list(body)
    words.append(addic_dot(3, 3, -1))
    # The branch sits one word past the body; it jumps back to the top.
    words.append(bne(-4 * (len(words))))
    words.append(BLR)
    return words, len(body)


def annotations(iters):
    # Decimal. The harness parses GPR values with a base-0 strtoull, so a
    # zero-padded hex string like 003D0900 is read as octal and truncated at
    # the first non-octal digit - 003D0900 becomes 3, and the loop runs three
    # times while reporting a pass.
    lines = [f"  #_ REGISTER_IN r3 {iters:d}"]
    for r in [ACC] + OPS:
        lines.append(f"  #_ REGISTER_IN v{r} [{SEEDS[r]}]")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-bin", required=True,
                    help="corpus directory for .bin/.map")
    ap.add_argument("--out-src", required=True, nargs="+",
                    help="testing source dir(s) for the .s; pass one per ref, "
                         "since the harness scans each ref's own tree")
    ap.add_argument("--iters", type=int, default=2_000_000)
    ap.add_argument("--width", type=int, default=8,
                    help="opcodes per loop iteration")
    ap.add_argument("--ops", nargs="+",
                    default=["vsel", "vperm", "vmaddfp", "vnmsubfp", "vand"])
    ap.add_argument("--shapes", nargs="+", default=["lat", "tp"])
    args = ap.parse_args()

    out_bin = pathlib.Path(args.out_bin)
    out_bin.mkdir(parents=True, exist_ok=True)
    srcs = [pathlib.Path(p) for p in args.out_src]
    for d in srcs:
        if not d.is_dir():
            sys.exit(f"not a directory: {d}")

    made = []
    for op in args.ops:
        for shape in args.shapes:
            suite = f"instr_bench_{op}_{shape}"
            label = f"test_bench_{op}_{shape}"
            words, per_iter = build(op, shape, args.width, args.iters)
            blob = b"".join(struct.pack(">I", w) for w in words)
            (out_bin / f"{suite}.bin").write_bytes(blob)
            (out_bin / f"{suite}.map").write_text(
                f"{0:016x} t {label}\n")
            total = per_iter * args.iters
            text = [
                f"# Generated by tools/bench/gen_loop_bench.py - do not edit.",
                f"# {op} x{per_iter} per iteration x {args.iters} iterations",
                f"# = {total:,} guest {op} executions per run.",
                f"# No REGISTER_OUT: the harness checks only what is asserted,",
                f"# and the point here is the time, not the value.",
                f"{label}:",
            ] + annotations(args.iters) + [""]
            for d in srcs:
                (d / f"{suite}.s").write_text("\n".join(text))
            made.append((suite, per_iter, total, len(blob)))

    print(f"{'suite':32}{'ops/iter':>9}{'total ops':>16}{'bytes':>7}")
    for suite, per_iter, total, nbytes in made:
        print(f"{suite:32}{per_iter:9d}{total:16,}{nbytes:7d}")
    print(f"\n.bin/.map -> {out_bin}")
    for d in srcs:
        print(f".s        -> {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
