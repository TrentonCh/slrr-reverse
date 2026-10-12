#!/usr/bin/env python3
"""study_lines.py - which source line does an expression node record?

Every node carries a line number (the u16 behind the op byte).  The model of
gen.py knows, for each node it predicts, the construct it belongs to and
(for most) the token it stands for.  Candidates for the line:

    own     the line of the node's own token (identifier, literal, operator,
            closing bracket; see the `tok=` arguments in gen.py)
    after   the line of the token behind the own token
    last    the line of the last token of the construct
    next    the line of the token that follows the construct (the parser's
            lookahead when it reduces the construct)

The script aligns prediction and class file (check_expr.iter_trees) and
counts, per node role, how often each candidate is the recorded line: over all
nodes, and over the "informative" ones (those whose candidates do not all
name the same line).  RULE below is the candidate chosen per role; the last
table is the proof of that choice: nodes for which the chosen candidate gives
the recorded line.

    python -I study_lines.py [--set 940] [--examples N]
"""
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import classdb      # noqa: E402
import check_expr   # noqa: E402
import gen          # noqa: E402

CANDS = ('own', 'after', 'last', 'next')

RULE = gen.LINE_RULE


def candidates(unit, e, tok):
    toks = unit.toks
    n = len(toks)
    out = {}
    if tok is not None:
        out['own'] = toks[tok].line
        out['after'] = toks[tok + 1].line if tok + 1 < n else toks[tok].line
    out['last'] = toks[e.t1 - 1].line if e.t1 > e.t0 else toks[e.t0].line
    out['next'] = toks[e.t1].line if e.t1 < n else out['last']
    return out


def node_key(pn, role):
    mn, op = pn
    if mn in ('expr', 'stmt'):
        if role in ('binary operator', 'assignment', 'prefix operator', 'postfix operator'):
            return '%s: %s <op>' % (role, mn)
        return '%s: %s %s' % (role, mn, op)
    return '%s: %s' % (role, mn)


def main(argv):
    which = '940'
    nex = 2
    if '--set' in argv:
        which = argv[argv.index('--set') + 1]
    if '--examples' in argv:
        nex = int(argv[argv.index('--examples') + 1])
    db = classdb.load()
    model = gen.Model(db)
    allc = defaultdict(Counter)
    inf = defaultdict(Counter)
    ex = defaultdict(list)
    total = 0
    for r in check_expr.iter_trees(check_expr.entries_of(which), model):
        if not r.ok:
            continue
        lines = None
        # the callee nodes that differ from the class file (section 9.2 of the
        # notes) are not predicted nodes
        excl = set()
        for xpk, pf, af in r.exc:
            excl.update(range(xpk, xpk + pf[2]))
        for k, pn in enumerate(r.pt.nodes):
            e = r.pt.src[k]
            if isinstance(e, tuple):
                e = e[0]
            if e is None or k in excl:
                continue
            tok, role = r.pt.anchor[k]
            total += 1
            act = r.nodes[r.keep[r.align[k]]].line
            c = candidates(r.unit, e, tok)
            key = node_key(pn, role)
            allc[key]['n'] += 1
            informative = len(set(c.values())) > 1
            if informative:
                inf[key]['n'] += 1
            hit = False
            for name, ln in c.items():
                if ln == act:
                    allc[key][name] += 1
                    hit = True
                    if informative:
                        inf[key][name] += 1
            if not hit:
                allc[key]['none'] += 1
                if informative:
                    inf[key]['none'] += 1
                if len(ex[key]) < nex:
                    if lines is None:
                        lines = r.text.split('\n')
                    ex[key].append('%s tree %d node line %d, candidates %s: %s' % (
                        r.entry.key, r.ti, act, c, lines[act - 1].strip()[:70]))
    print('nodes with a source construct: %d' % total)
    good = bad = 0
    for key in sorted(allc):
        role = key.split(':')[0]
        mn = key.split(': ')[1].split(' ')[0]
        cand = RULE.get('%s: %s' % (role, mn)) or RULE.get(role)
        good += allc[key][cand]
        bad += allc[key]['n'] - allc[key][cand]
        if allc[key]['n'] != allc[key][cand]:
            print('  rule %s for %s: %d of %d' % (cand, key, allc[key][cand], allc[key]['n']))
    print('line predicted by the rule table: %d of %d nodes (%d exceptions)' % (
        good, good + bad, bad))
    print('%-44s %8s | %7s %7s %7s %7s %6s | informative: %5s %5s %5s %5s %5s %5s' % (
        'role: node', 'nodes', 'own', 'after', 'last', 'next', 'none',
        'n', 'own', 'after', 'last', 'next', 'none'))
    if '--json' in argv:
        import json
        doc = {'total': total, 'predicted': good, 'exceptions': bad, 'roles': {}}
        for key in sorted(allc):
            role = key.split(':')[0]
            mn = key.split(': ')[1].split(' ')[0]
            doc['roles'][key] = {'rule': RULE.get('%s: %s' % (role, mn)) or RULE.get(role),
                                 'all': dict(allc[key]), 'informative': dict(inf[key])}
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump(doc, f, indent=1)
    for key in sorted(allc):
        a, i = allc[key], inf[key]
        print('%-44s %8d | %7d %7d %7d %7d %6d | %18d %5d %5d %5d %5d %5d' % (
            key, a['n'], a['own'], a['after'], a['last'], a['next'], a['none'],
            i['n'], i['own'], i['after'], i['last'], i['next'], i['none']))
        for x in ex.get(key, []):
            print('        ' + x)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
