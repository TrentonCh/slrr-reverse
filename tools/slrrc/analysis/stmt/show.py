#!/usr/bin/env python3
"""show.py - source lines next to the nodes compiled from them.

    python -I show.py <entry> <first line> <last line> [--ctx N]
    python -I show.py <entry> --tree N [--blob B]

<entry> is anything oracle.entry() accepts ('game/Catalog', 'Trigger').
Prints the source lines of the range, then every tree that owns a node whose
line lies in the range, restricted to the node span that covers the range
(plus --ctx nodes on each side).
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, '..', '..', 'lib')))

import disasm  # noqa: E402
import oracle  # noqa: E402
import tufa    # noqa: E402


def owners(blob):
    own = {}
    for st, i, f in blob.fields():
        if f.init != tufa.NONE:
            own[f.init] = ('field', st, f)
    for st, i, m in blob.methods():
        if m.tree != tufa.NONE:
            own[m.tree] = ('method', st, m)
    return own


def main(argv):
    ctx = 2
    args = []
    tree = None
    blob_i = None
    it = iter(argv)
    for a in it:
        if a == '--ctx':
            ctx = int(next(it))
        elif a == '--tree':
            tree = int(next(it))
        elif a == '--blob':
            blob_i = int(next(it))
        else:
            args.append(a)
    e = oracle.entry(args[0])
    blobs = oracle.load_class(e)
    src = oracle.load_source(e).split('\n') if e.source else []
    if tree is not None:
        for bi, b in enumerate(blobs):
            if blob_i is not None and bi != blob_i:
                continue
            if tree >= len(b.trees):
                continue
            own = owners(b).get(tree)
            lm = None
            label = 'unowned'
            if own:
                label = disasm.owner_label(b, (own[0], own[1], 0, own[2]))
                if own[0] == 'method':
                    lm = disasm.local_names(b, own[1], own[2])
            nodes = b.trees[tree]
            if nodes and src:
                lo = min(n.slot for n in nodes)
                hi = max(n.slot for n in nodes)
                for ln in range(max(1, lo - 3), min(len(src), hi + 1) + 1):
                    print('%5d  %s' % (ln, src[ln - 1].rstrip('\r').expandtabs(4)))
            print('%s blob %d tree %d: %s (%d nodes)' % (e.cls, bi, tree, label, len(nodes)))
            for s in disasm.tree_lines(b, tree, locals_map=lm):
                print(s)
        return 0
    lo, hi = int(args[1]), int(args[2])
    for ln in range(lo, hi + 1):
        if 1 <= ln <= len(src):
            print('%5d  %s' % (ln, src[ln - 1].rstrip('\r').expandtabs(4)))
    for bi, b in enumerate(blobs):
        own = owners(b)
        for t, nodes in enumerate(b.trees):
            idx = [i for i, n in enumerate(nodes) if lo <= n.slot <= hi]
            if not idx:
                continue
            a, z = max(0, idx[0] - ctx), min(len(nodes), idx[-1] + 1 + ctx)
            o = own.get(t)
            lm = None
            label = 'unowned'
            if o:
                label = disasm.owner_label(b, (o[0], o[1], 0, o[2]))
                if o[0] == 'method':
                    lm = disasm.local_names(b, o[1], o[2])
            print('%s blob %d tree %d: %s (%d nodes), nodes %d-%d' % (
                e.cls, bi, t, label, len(nodes), a, z - 1))
            lines = disasm.tree_lines(b, t, locals_map=lm)
            for s in lines[a:z]:
                print(s)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
