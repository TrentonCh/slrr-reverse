#!/usr/bin/env python3
"""examples.py - find and render examples of statement forms.

    python -I examples.py list FORM [N] [--min NODES] [--max NODES]
        the N smallest occurrences of a statement form in the oracle pairs
        FORM: if  if-else  while  do  for  switch  break  continue  return
              "return expr"  local  expr  block
        prints: key blob tree first-node end-node first-line last-line
    python -I examples.py show KEY BLOB TREE A Z [L0 L1]
        source lines L0..L1 and nodes A..Z-1 of that tree

As a library: render(key, blob, tree, a, z, l0, l1) returns the two code
blocks as text; make_notes.py uses it.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import align       # noqa: E402
import check_stmt  # noqa: E402
import disasm      # noqa: E402
import oracle      # noqa: E402
import sparse      # noqa: E402
import tufa        # noqa: E402


def candidates(form, pred=None):
    out = []
    for e in oracle.iter_oracle_pairs():
        unit = sparse.parse(oracle.load_source(e))
        blobs = oracle.load_class(e)
        for bi, (cls, blob) in enumerate(zip(unit.classes, blobs)):
            for t, kind, node, row, st in align.align(cls, blob):
                if t is None:
                    continue
                r = check_stmt.run_tree(unit, blob, t, kind, node, row, st)
                if not r.ok_lines:
                    continue
                for f, s, a, z in r.gen.spans:
                    if f != form:
                        continue
                    if pred is not None and not pred(s, r.gen, a, z, unit):
                        continue
                    l0 = unit.toks[s.t0].line
                    l1 = unit.toks[min(s.t1, len(unit.toks) - 1)].line
                    out.append((z - a, l1 - l0, e.key, bi, t, a, z, l0, l1))
    out.sort()
    return out


def render(key, bi, t, a=None, z=None, l0=None, l1=None, max_width=118):
    """(source text, disassembly text) of nodes a..z-1 of a tree."""
    e = oracle.entry(key)
    blob = oracle.load_class(e)[bi]
    own = align.owners(blob).get(t)
    lm = None
    if own and own[0] == 'method':
        lm = disasm.local_names(blob, own[1], own[2])
    lines = disasm.tree_lines(blob, t, locals_map=lm)
    nodes = blob.trees[t]
    a = 0 if a is None else a
    z = len(nodes) if z is None else z
    if l0 is None:
        ls = [nodes[i].slot for i in range(a, z)] or [1]
        l0, l1 = min(ls), max(ls)
    src = oracle.load_source(e).split('\n')
    s_out = []
    for ln in range(l0, l1 + 1):
        s_out.append('%5d  %s' % (ln, src[ln - 1].rstrip('\r').expandtabs(4).rstrip()))
    d_out = []
    for s in lines[a:z]:
        if len(s) > max_width:
            s = s[:max_width - 3] + '...'
        d_out.append(s)
    return '\n'.join(s_out), '\n'.join(d_out)


def label(key, bi, t):
    e = oracle.entry(key)
    blob = oracle.load_class(e)[bi]
    own = align.owners(blob).get(t)
    what = 'unowned'
    if own:
        what = disasm.owner_label(blob, (own[0], own[1], 0, own[2]))
    return '`%s`, class `%s`, tree %d (%s)' % (e.cls, blob.name, t, what)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == 'list':
        form = argv[1]
        n = 10
        lo, hi = 0, 10 ** 9
        rest = argv[2:]
        i = 0
        while i < len(rest):
            if rest[i] == '--min':
                lo = int(rest[i + 1])
                i += 2
            elif rest[i] == '--max':
                hi = int(rest[i + 1])
                i += 2
            else:
                n = int(rest[i])
                i += 1
        shown = 0
        for c in candidates(form):
            if lo <= c[0] <= hi:
                print('%-60s blob %d tree %-3d nodes %d..%d  lines %d..%d  (%d nodes)'
                      % (c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[0]))
                shown += 1
                if shown >= n:
                    break
        return 0
    if argv[0] == 'show':
        key, bi, t = argv[1], int(argv[2]), int(argv[3])
        a = int(argv[4]) if len(argv) > 4 else None
        z = int(argv[5]) if len(argv) > 5 else None
        l0 = int(argv[6]) if len(argv) > 6 else None
        l1 = int(argv[7]) if len(argv) > 7 else None
        s, d = render(key, bi, t, a, z, l0, l1)
        print(label(key, bi, t))
        print(s)
        print()
        print(d)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
