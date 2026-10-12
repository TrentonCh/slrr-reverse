#!/usr/bin/env python3
"""selftest_destruct.py - is the structure check (destruct.py) sharp?

A recogniser that accepts everything proves nothing.  This script damages
trees that destruct.py accepts and counts how many damaged trees it still
accepts.  Mutations, one node at a time:

    jump     the offset of a jump, +1 and -1
    release  the index of a release, +1 and -1
    declare  the index of a declare, +1 and -1
    drop     the node removed (jump offsets across it are NOT repaired)

Known slack, which the drop mutants show: behind a `break`, `continue` or
`return` the reader does not know how many locals the compiler still counts
as live (the releases of the exit and of a block end in front of it look the
same), so it does not insist on the unreachable scope-end `release` nodes
that follow.  Removing a unary operator or a `getfield` that no jump spans
leaves another well-formed program and is accepted by design.

A mutated tree that is still accepted is not necessarily a hole: a jump moved
by one node can land on another statement boundary and give a different but
well-formed program (`if (c) a; b;` against `if (c) { a; b; }` is exactly a
jump offset).  The script therefore also reports how many accepted mutants
have that excuse: the new target of the jump is itself a statement boundary
of the original tree.

All trees of the class files without source that hold a jump are used, and
every 20th other tree.  Writes out/selftest_destruct.txt and .json.
"""
import json
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import check_destruct  # noqa: E402
import destruct        # noqa: E402
import oracle          # noqa: E402
import tufa            # noqa: E402

JUMPS = (0x1014, 0x1015, 0x1016, 0x1017)


def accepts(blob, nodes, info):
    kind, is_ctor, st, nargs, ret_void, label, row = info
    d = destruct.Destruct(blob, nodes, kind, is_ctor, st, nargs, ret_void)
    try:
        d.run()
    except destruct.Fail:
        return None
    except (IndexError, KeyError, RecursionError):
        return None
    return d


def main():
    c = Counter()
    entries = {e.cls: e for e in oracle.iter_entries(('pair_940', 'pair_other'))}
    k = 0
    for rel, blobs in oracle.iter_class_files():
        nosrc = rel not in entries
        for bi, b in enumerate(blobs):
            info = check_destruct.tree_info(b)
            for t, nodes in enumerate(b.trees):
                if not nodes:
                    continue
                has_jump = any(tufa.OP_FLAGS[x.op] in JUMPS for x in nodes)
                k += 1
                if not ((nosrc and has_jump) or k % 20 == 0):
                    continue
                d0 = accepts(b, nodes, info[t])
                if d0 is None:
                    continue
                c['trees used'] += 1
                starts = d0.starts
                for i, nd in enumerate(nodes):
                    code = tufa.OP_FLAGS[nd.op]
                    muts = []
                    if code in JUMPS:
                        muts = [('jump', tufa.Node(nd.op, nd.slot, (nd.imm + dv) & 0xFFFFFFFF), dv)
                                for dv in (1, -1)]
                    elif code == 0x1003:
                        muts = [('release', tufa.Node(nd.op, nd.slot, (nd.imm + dv) & 0xFFFFFFFF), dv)
                                for dv in (1, -1) if nd.imm + dv >= 0]
                    elif code == 0x1002:
                        muts = [('declare', tufa.Node(nd.op, nd.slot, (nd.imm + dv) & 0xFFFFFFFF), dv)
                                for dv in (1, -1) if nd.imm + dv >= 0]
                    for what, new, dv in muts:
                        m = list(nodes)
                        m[i] = new
                        c['%s mutants' % what] += 1
                        if accepts(b, m, info[t]) is not None:
                            c['%s mutants accepted' % what] += 1
                            if what == 'jump':
                                off = nd.imm - (1 << 32) if nd.imm & 0x80000000 else nd.imm
                                if i + off + dv in starts:
                                    c['jump mutants accepted: new target is a statement boundary'] += 1
                    if i % 7 == 0:
                        m = nodes[:i] + nodes[i + 1:]
                        c['drop mutants'] += 1
                        if accepts(b, m, info[t]) is not None:
                            c['drop mutants accepted'] += 1
                            if code == 0x1003:
                                c['drop accepted: release'] += 1
                            elif code in (0x1007, 0x401c):
                                c['drop accepted: unary operator or getfield'] += 1
                            else:
                                c['drop accepted: other'] += 1
    L = ['selftest_destruct.py: mutated trees against the structure check', '']
    L.append('trees used: %d' % c['trees used'])
    for what in ('jump', 'release', 'declare', 'drop'):
        n = c['%s mutants' % what]
        a = c['%s mutants accepted' % what]
        L.append('  %-8s mutants %7d   rejected %7d   accepted %5d (%.2f%%)'
                 % (what, n, n - a, a, 100.0 * a / n if n else 0))
    L.append('  accepted jump mutants whose new target is a statement boundary of the '
             'original tree: %d' % c['jump mutants accepted: new target is a statement boundary'])
    L.append('  accepted drop mutants: %d a unary operator or a getfield (another well-formed '
             'expression), %d a release, %d other'
             % (c['drop accepted: unary operator or getfield'], c['drop accepted: release'],
                c['drop accepted: other']))
    text = '\n'.join(L) + '\n'
    with open(os.path.join(_HERE, 'out', 'selftest_destruct.txt'), 'w', newline='\n') as f:
        f.write(text)
    with open(os.path.join(_HERE, 'out', 'selftest_destruct.json'), 'w', newline='\n') as f:
        json.dump(dict(c), f, indent=1, sort_keys=True)
        f.write('\n')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
