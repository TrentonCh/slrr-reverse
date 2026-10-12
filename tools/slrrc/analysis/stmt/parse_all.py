#!/usr/bin/env python3
"""parse_all.py - run sparse.py over every source of the corpus.

Proof that the analysis parser accepts the whole dialect as it is used, and a
census of the statement and expression forms it finds (per category of the
corpus index).  Writes out/parse_all.txt.
"""
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, '..', '..', 'lib')))
sys.path.insert(0, _HERE)

import oracle   # noqa: E402
import sparse   # noqa: E402


def walk(n, c, depth=0):
    """Count node kinds below n (N, list or other)."""
    if isinstance(n, list):
        for x in n:
            walk(x, c, depth)
        return
    if not isinstance(n, sparse.N):
        return
    k = n.k
    if k == 'un' or k == 'post' or k == 'bin' or k == 'assign':
        c['%s %s' % (k, n.op)] += 1
    elif k == 'lit':
        c['lit %s' % n.lk] += 1
    elif k == 'if':
        c['if-else' if n.els is not None else 'if'] += 1
    elif k == 'for':
        c['for'] += 1
        init = 'none' if n.init is None else ('decl' if isinstance(n.init, sparse.N) else 'expr x%d' % len(n.init))
        c['for init=%s cond=%s update=%d' % (init, 'yes' if n.cond else 'no', len(n.update))] += 1
    elif k == 'local':
        c['local'] += 1
        for d in n.decls:
            c['declarator with init' if d.init is not None else 'declarator without init'] += 1
        if len(n.decls) > 1:
            c['local with %d declarators' % len(n.decls)] += 1
    elif k == 'return':
        c['return value' if n.e is not None else 'return'] += 1
    elif k == 'expr':
        c['expr stmt'] += 1
        e = n.e
        while e.k == 'paren':
            e = e.e
        c['expr stmt: %s' % (e.k if e.k not in ('un', 'post', 'assign') else '%s %s' % (e.k, e.op))] += 1
    elif k == 'group':
        c['switch group'] += 1
        c['switch group labels=%d' % len(n.labels)] += 1
    else:
        c[k] += 1
    for a, v in n.__dict__.items():
        if a in ('k', 't0', 't1'):
            continue
        if isinstance(v, (sparse.N, list)):
            walk(v, c, depth + 1)


def main():
    out = []
    w = out.append
    tot = Counter()
    fails = []
    files = Counter()
    members = Counter()
    for e in oracle.iter_entries():
        if not e.source:
            continue
        files[e.category] += 1
        try:
            u = sparse.parse(oracle.load_source(e))
        except (sparse.ParseError, sparse.srclex.LexError) as x:
            fails.append((e.source, str(x)))
            continue
        files[e.category + ' parsed'] += 1
        names = [((u.package + '.') if u.package else '') + c.name for c in u.classes]
        if e.declared_names != names:
            fails.append((e.source, 'declared types differ: %r / %r' % (names, e.declared_names)))
        for c in u.classes:
            for m in c.members:
                if m.k == 'method':
                    members['constructor' if m.ctor else 'method'] += 1
                    members['%s body %s%s' % ('constructor' if m.ctor else 'method', m.body_form,
                                              ' (empty)' if m.body is not None and not m.body.stmts else '')] += 1
                    for md in m.mods:
                        members['method modifier ' + md] += 1
                elif m.k == 'field':
                    members['field declaration'] += 1
                    for d in m.decls:
                        members['field declarator with init' if d.init is not None
                                else 'field declarator without init'] += 1
                else:
                    members[m.k] += 1
            walk(c.members, tot)
    w('sparse.py over the corpus')
    for k in sorted(files):
        w('  %-28s %d' % (k, files[k]))
    w('failures: %d' % len(fails))
    for s, x in fails:
        w('  %s: %s' % (s, x))
    w('')
    w('members')
    for k in sorted(members):
        w('  %-44s %d' % (k, members[k]))
    w('')
    w('syntax tree nodes (all sources)')
    for k in sorted(tot):
        w('  %-44s %d' % (k, tot[k]))
    text = '\n'.join(out) + '\n'
    os.makedirs(os.path.join(_HERE, 'out'), exist_ok=True)
    with open(os.path.join(_HERE, 'out', 'parse_all.txt'), 'w', newline='\n') as f:
        f.write(text)
    sys.stdout.write(text)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
