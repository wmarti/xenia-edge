"""Count how many HIR blocks qualify as extended-block continuations.

Reads a --dump_translated_hir_functions corpus. Each block prints its incoming
edges as '; in: labelN, ...' lines, then its instructions. A block continues the
chain when it has exactly one incoming edge, that edge comes from the
layout-previous block, and -- the condition the miscompile was about -- the
branch reaching it sits in the predecessor's TRAILING run of branches, so the
predecessor provably ran to completion.
"""
import os, re, sys, collections

IN_RE = re.compile(r'^\s*; in: (label\d+)')
LBL_RE = re.compile(r'^(label\d+):')
BR_RE  = re.compile(r'^\s*(branch|branch_true|branch_false)\b.*?(label\d+)\s*$')

def parse(path):
    blocks = []      # (label, [incoming], [instr lines])
    cur = None
    for line in open(path, errors='replace'):
        m = LBL_RE.match(line)
        if m:
            cur = [m.group(1), [], []]
            blocks.append(cur)
            continue
        if line.startswith('<entry>'):
            cur = ['<entry>', [], []]
            blocks.append(cur)
            continue
        if cur is None:
            continue
        m = IN_RE.match(line)
        if m:
            cur[1].append(m.group(1)); continue
        if line.startswith('  ;'):
            continue
        if line.strip():
            cur[2].append(line.rstrip())
    return blocks

def trailing_branch_targets(instrs):
    """Targets of the trailing contiguous run of branch instructions."""
    out = []
    for line in reversed(instrs):
        m = BR_RE.match(line)
        if not m:
            break
        out.append(m.group(2))
    return out

def main():
    root = sys.argv[1]
    naive = strict = total = 0
    files = os.listdir(root)
    for fn in files:
        blocks = parse(os.path.join(root, fn))
        for idx in range(1, len(blocks)):
            label, incoming, _ = blocks[idx]
            prev_label, _, prev_instrs = blocks[idx-1]
            total += 1
            if len(incoming) != 1 or incoming[0] != prev_label:
                continue
            naive += 1                       # what the buggy predicate accepted
            if label in trailing_branch_targets(prev_instrs):
                strict += 1                  # reached from prev's tail run
            elif not prev_instrs or not BR_RE.match(prev_instrs[-1]) or \
                 not prev_instrs[-1].strip().startswith('branch '):
                strict += 1                  # fall-through
    print(f"{len(files)} functions, {total} non-head blocks")
    print(f"  single edge from layout-prev (old predicate): {naive:7d}  {100*naive/total:5.2f}%")
    print(f"  ... and prev provably ran to completion      : {strict:7d}  {100*strict/total:5.2f}%")
    print(f"  unsafe continuations the old predicate took  : {naive-strict:7d}  {100*(naive-strict)/total:5.2f}%")
main()
