#!/usr/bin/env python3
"""find_special.py - the smallest occurrences of special statement forms.

    python -I find_special.py [NAME ...]

NAME is a key of SPECS ("for(;;)", "switch default", "if nested dual", ...);
without arguments every form is listed.  Each line gives what examples.py
show needs: key, blob, tree, first node, end node, first line, last line.
Only the oracle pairs are searched.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, '..', '..', 'lib')))

import align        # noqa: E402
import check_stmt   # noqa: E402
import oracle       # noqa: E402
import sparse       # noqa: E402

N = sparse.N


def has(s, kind):
    if isinstance(s, list):
        return any(has(x, kind) for x in s)
    if not isinstance(s, N):
        return False
    if s.k == kind:
        return True
    for a, v in s.__dict__.items():
        if a in ('k', 't0', 't1'):
            continue
        if isinstance(v, (N, list)) and has(v, kind):
            return True
    return False


SPECS = {
    'for(;;)': ('for', lambda s, g, a, z, u: s.cond is None and s.init is None),
    'for expr init': ('for', lambda s, g, a, z, u: isinstance(s.init, list)),
    'for no update': ('for', lambda s, g, a, z, u: not s.update and s.cond is not None),
    'for with break': ('for', lambda s, g, a, z, u: has(s.body, 'break') and not has(s.body, 'switch')),
    'while with break': ('while', lambda s, g, a, z, u: has(s.body, 'break') and not has(s.body, 'switch')),
    'switch default': ('switch', lambda s, g, a, z, u: any(c.k == 'default' for gr in s.groups for c in gr.labels)),
    'switch group locals': ('switch', lambda s, g, a, z, u: any(x.k == 'local' for gr in s.groups for x in gr.stmts)),
    'switch no break last': ('switch', lambda s, g, a, z, u: s.groups and (not s.groups[-1].stmts or s.groups[-1].stmts[-1].k != 'break')),
    'return void': ('return', None),
    'local multi': ('local', lambda s, g, a, z, u: len(s.decls) > 1),
    'local noinit': ('local', lambda s, g, a, z, u: len(s.decls) == 1 and s.decls[0].init is None),
    'if nested dual': ('if', lambda s, g, a, z, u: s.then.k == 'if' and s.then.els is not None),
    'if then loop': ('if', lambda s, g, a, z, u: s.then.k in ('for', 'while')),
    'block in block': ('block', lambda s, g, a, z, u: any(x.k == 'block' for x in s.stmts)),
    'if-else empty then': ('if-else', lambda s, g, a, z, u: s.then.k == 'block' and not s.then.stmts),
    'if empty': ('if', lambda s, g, a, z, u: z - a <= 3 or (s.then.k == 'block' and not s.then.stmts)),
    'stmt after return': ('block', lambda s, g, a, z, u: any(x.k == 'return' and i + 1 < len(s.stmts) for i, x in enumerate(s.stmts))),
}


def main():
    want = sys.argv[1:] or sorted(SPECS)
    res = {w: [] for w in want}
    for e in oracle.iter_oracle_pairs():
        unit = sparse.parse(oracle.load_source(e))
        blobs = oracle.load_class(e)
        for bi, (cls, blob) in enumerate(zip(unit.classes, blobs)):
            for t, kind, node, row, st in align.align(cls, blob):
                if t is None:
                    continue
                r = check_stmt.run_tree(unit, blob, t, kind, node, row, st)
                for f, s, a, z in r.gen.spans:
                    for w in want:
                        form, pred = SPECS[w]
                        if f != form:
                            continue
                        if pred is not None and not pred(s, r.gen, a, z, unit):
                            continue
                        l0 = unit.toks[s.t0].line
                        l1 = unit.toks[min(s.t1, len(unit.toks) - 1)].line
                        res[w].append((z - a, e.key, bi, t, a, z, l0, l1))
    for w in want:
        res[w].sort()
        print('== %s (%d)' % (w, len(res[w])))
        for c in res[w][:5]:
            print('   %s %d %d %d %d %d %d   (%d nodes)' % (c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[0]))


main()
