#!/usr/bin/env python3
"""probe_stmt_roots.py - which expression kinds stand in statement position?

Counts, over all 3,454 sources, the root kind of every expression statement
(including the init and update parts of `for`), separately for sources with a
class (pairs) and without.  Shows the first examples of the unusual ones.
"""
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle   # noqa: E402
import jparse   # noqa: E402


def root_kind(e):
    paren = ''
    while e.k == 'paren':
        e = e.x
        paren = '(paren) '
    if e.k in ('assign', 'unary', 'postfix', 'binary'):
        return paren + e.k + ' ' + e.op
    return paren + e.k


def main(argv):
    c = {'pair': Counter(), 'source_only': Counter()}
    ex = defaultdict(list)
    for e in oracle.iter_entries():
        if e.source is None:
            continue
        grp = 'pair' if e.cls else 'source_only'
        text = oracle.load_source(e)
        u = jparse.parse(text)
        lines = text.split('\n')
        for n in jparse.walk(u):
            if n.k == 'expr':
                k = root_kind(n.e)
                c[grp][k] += 1
                if len(ex[k]) < 3:
                    ex[k].append('%s:%d  %s' % (e.source, n.line, lines[n.line - 1].strip()[:90]))
    keys = sorted(set(c['pair']) | set(c['source_only']),
                  key=lambda k: -(c['pair'][k] + c['source_only'][k]))
    print('%-24s %8s %12s' % ('root of an expression statement', 'pairs', 'source only'))
    for k in keys:
        print('%-24s %8d %12d' % (k, c['pair'][k], c['source_only'][k]))
        if c['pair'][k] + c['source_only'][k] < 200:
            for x in ex[k]:
                print('        ' + x)
    if '--json' in argv:
        import json
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump({'pair': dict(c['pair']), 'source_only': dict(c['source_only'])}, f,
                      indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
