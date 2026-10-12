#!/usr/bin/env python3
"""show.py - source lines next to the nodes compiled from them.

    python -I show.py ENTRY LINE [LINE2]        e.g.  show.py game/City 1648
    python -I show.py --grep REGEX [--max N] [--pair940]

ENTRY is anything oracle.entry() accepts.  Prints the source lines LINE..LINE2
and every node whose line number lies in that range, in tree order, using the
foundation disassembler's rendering.  --grep searches all pair sources for a
regular expression (comments included) and shows the first N hits that way.
"""
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, '..', '..', 'lib')))
import disasm   # noqa: E402
import oracle   # noqa: E402
import tufa     # noqa: E402


def show(e, lo, hi, out=sys.stdout, context=0):
    text = oracle.load_source(e)
    lines = text.split('\n')
    out.write('== %s  lines %d-%d\n' % (e.source, lo, hi))
    for ln in range(max(1, lo - context), min(len(lines), hi + context) + 1):
        out.write('%6d  %s\n' % (ln, lines[ln - 1].rstrip('\r').replace('\t', '    ')))
    blobs = oracle.load_class(e)
    for bi, b in enumerate(blobs):
        owners = b.tree_owners()
        for ti, t in enumerate(b.trees):
            if not any(lo <= nd.line <= hi for nd in t):
                continue
            idx = [k for k, nd in enumerate(t) if lo <= nd.line <= hi]
            a, z = idx[0], idx[-1]
            out.write('-- blob %d %s tree %d (%s) nodes %d-%d\n' % (
                bi, b.name, ti, '; '.join(owners.get(ti, ['?'])), a, z))
            rows = list(disasm.tree_lines(b, ti))
            # tree_lines yields one text line per node, in order
            for k in range(a, z + 1):
                out.write(rows[k] + '\n')


def main(argv):
    if len(argv) >= 2 and argv[1] == '--grep':
        rx = re.compile(argv[2])
        mx = 5
        if '--max' in argv:
            mx = int(argv[argv.index('--max') + 1])
        hits = 0
        for e in oracle.iter_pairs(build=None, consistent=True):
            text = oracle.load_source(e)
            for ln, line in enumerate(text.split('\n'), 1):
                if rx.search(line):
                    show(e, ln, ln)
                    hits += 1
                    if hits >= mx:
                        return 0
                    break
        return 0
    e = oracle.entry(argv[1])
    lo = int(argv[2])
    hi = int(argv[3]) if len(argv) > 3 else lo
    show(e, lo, hi)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
