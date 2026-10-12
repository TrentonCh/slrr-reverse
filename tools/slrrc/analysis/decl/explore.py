#!/usr/bin/env python3
"""explore.py - run poolmodel over the oracle pairs and report where the
predicted pool first departs from the shipped one.

    python -I explore.py [--all] [--show N] [--only SUBSTR] [--opt k=v ...]
                         [--phase LABEL] [--list]

Prints the number of classes (blobs) and of pairs whose pool is predicted
exactly, and a census of the first mismatch by the phase of the predicted
entry.  --show N prints N examples per bucket, --phase restricts the
examples to one bucket.
"""
import os
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle      # noqa: E402
import srcparse    # noqa: E402
import universe    # noqa: E402
import poolmodel   # noqa: E402


def fmt(e):
    if e is None:
        return '<end>'
    if e[0] == 'utf8':
        return 'utf8 %r' % (e[1][:70],)
    return ' '.join(str(x) for x in e)


def run(pairs, opts, model_cls=None):
    model_cls = model_cls or poolmodel.Model
    uni = universe.load()
    res = []
    for e in pairs:
        text = oracle.load_source(e)
        unit = srcparse.parse(text)
        blobs = oracle.load_class(e)
        for bi, (c, b) in enumerate(zip(unit.classes, blobs)):
            m = model_cls(unit, c, b, uni, opts)
            m.run()
            actual = m.cons_content
            pred = m.pool.entries
            d = poolmodel.first_diff(pred, actual)
            res.append((e, bi, m, d))
    return res


def bucket(m, d):
    pred = m.pool.entries
    actual = m.cons_content
    ph = m.pool.phase[d] if d < len(pred) else 'end'
    ph = ph.split(':')[0]
    a = actual[d] if d < len(actual) else None
    p = pred[d] if d < len(pred) else None
    return '%s: pred %s / actual %s' % (ph, p[0] if p else 'END', a[0] if a else 'END')


def main(argv):
    opts = {}
    show = 0
    only = None
    phase = None
    allpairs = '--all' in argv
    i = 0
    while i < len(argv):
        if argv[i] == '--opt':
            k, v = argv[i + 1].split('=', 1)
            if v in ('True', 'False'):
                v = v == 'True'
            opts[k] = v
            i += 1
        elif argv[i] == '--show':
            show = int(argv[i + 1])
            i += 1
        elif argv[i] == '--only':
            only = argv[i + 1].lower()
            i += 1
        elif argv[i] == '--phase':
            phase = argv[i + 1]
            i += 1
        i += 1
    pairs = list(oracle.iter_pairs(None) if allpairs else oracle.iter_oracle_pairs())
    if only:
        pairs = [e for e in pairs if only in e.key.lower()]
    res = run(pairs, opts)
    nb = len(res)
    okb = sum(1 for r in res if r[3] is None)
    by_pair = defaultdict(list)
    for e, bi, m, d in res:
        by_pair[e.key].append(d is None)
    okp = sum(1 for v in by_pair.values() if all(v))
    print('options: %r' % (opts,))
    print('blobs predicted exactly: %d of %d' % (okb, nb))
    print('pairs predicted exactly: %d of %d' % (okp, len(by_pair)))
    unres = Counter()
    for e, bi, m, d in res:
        for u in m.unresolved:
            unres[u] += 1
    if unres:
        print('unresolved simple names: %d distinct, e.g. %r' % (
            len(unres), unres.most_common(12)))
    cnt = Counter()
    ex = defaultdict(list)
    for e, bi, m, d in res:
        if d is None:
            continue
        bk = bucket(m, d)
        cnt[bk] += 1
        ex[bk].append((e, bi, m, d))
    for bk, n in cnt.most_common():
        print('%6d  %s' % (n, bk))
        if show and (phase is None or bk.startswith(phase)):
            for e, bi, m, d in sorted(ex[bk], key=lambda r: len(r[2].cons_content))[:show]:
                pred = m.pool.entries
                actual = m.cons_content
                print('          %s#%d  at %d of %d/%d' % (e.key, bi, d, len(pred), len(actual)))
                for j in range(max(0, d - 2), d + 4):
                    a = actual[j] if j < len(actual) else None
                    p = pred[j] if j < len(pred) else None
                    ph = m.pool.phase[j] if j < len(pred) else ''
                    print('            [%d] %s actual %-44s pred %-44s (%s)' % (
                        j, '  ' if a == p else '!=', fmt(a), fmt(p), ph))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
