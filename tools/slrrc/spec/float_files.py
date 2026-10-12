#!/usr/bin/env python3
"""float_files.py - which class files need which float conversion, and where
those files sit.

    python -I float_files.py [--detail]

Reads out/float_res_oracle.pickle and out/float_samples_oracle.pickle written
by float_models.py.  Settings (all with the integer part modulo 2**32):

    A  div pc24    s = 1; per digit: s = s / 10; v = v + d * s      every result
                   rounded to single precision
    B  mul pc24    s = 0.1f; per digit: v = v + d * s; s = s * 0.1f  every result
                   rounded to single precision
    C  div pc53    as A, but d * s and the sum are formed in double precision
                   and only the stores to v and s round to single
    D  diva pc53   s = 0.1f; per digit: v = v + d * s; s = s / 10    all in double
                   precision, one rounding to single at the end

The write times of the class files cannot help to tell compile sessions apart:
every file of the corpus carries the day the corpus was copied.
"""
import os
import pickle
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))

SETS = (('A', 'div pc24 v24 s24 c24'), ('B', 'mul pc24 v24 s24 c24'),
        ('C', 'div pc53 v24 s24 c24'), ('D', 'diva pc53 v53 s53 c24'))


def main(argv):
    detail = '--detail' in argv
    with open(os.path.join(_HERE, 'out', 'float_samples_oracle.pickle'), 'rb') as f:
        per_file = pickle.load(f)
    with open(os.path.join(_HERE, 'out', 'float_res_oracle.pickle'), 'rb') as f:
        names, res = pickle.load(f)
    idx = {n: k for k, n in enumerate(names)}
    need = {}
    first = {}
    for f, v in per_file.items():
        key = ''
        for ch, nm in SETS:
            k = idx[nm]
            if all(res[tk][k] == imm for tk, imm, _ in v):
                key += ch
        need[f] = key or 'none'
        first[f] = key[0] if key else 'none'
    c = Counter(need.values())
    print('class files with a float node: %d' % len(per_file))
    print('by the settings that reproduce EVERY float node of the file: %s' % dict(sorted(c.items())))
    cf = Counter(first.values())
    print('by the first of A, B, C, D that does: %s' % dict(sorted(cf.items())))
    decisive = Counter()
    for f, key in need.items():
        if len(key) == 1:
            decisive[key] += 1
    print('files that exactly ONE setting reproduces: %s' % dict(sorted(decisive.items())))
    bydir = defaultdict(Counter)
    for f, key in first.items():
        bydir['/'.join(f.split('/')[:2])][key] += 1
    cols = [ch for ch, _ in SETS] + ['none']
    print()
    print('first setting that reproduces the file, by directory:')
    print('%-28s %s' % ('directory', '  '.join('%5s' % k for k in cols)))
    for d in sorted(bydir):
        print('%-28s %s' % (d, '  '.join('%5d' % bydir[d][k] for k in cols)))
    # how far apart the settings are
    spread = Counter()
    worst = None
    for f, v in per_file.items():
        for tk, imm, line in v:
            vals = [res[tk][idx[nm]] for _, nm in SETS]
            d = max(vals) - min(vals)
            spread[d] += 1
            if worst is None or d > worst[0]:
                worst = (d, tk, f, line, vals)
    print()
    print('largest difference between the four settings for one literal, in units of the last place: %s'
          % dict(sorted(spread.items())))
    if worst:
        print('  e.g. %s in %s line %d: %s' % (worst[1], worst[2], worst[3],
                                             ' '.join('%08X' % x for x in worst[4])))
    if detail:
        for f in sorted(per_file):
            if need[f] in ('B', 'C', 'D', 'none'):
                bad = [(tk, imm) for tk, imm, _ in per_file[f]
                       if res[tk][idx[SETS[0][1]]] != imm]
                print('%-4s %-90s %d of %d nodes differ under A, e.g. %s' % (
                    need[f], f, len(bad), len(per_file[f]),
                    ', '.join('%s->%08X' % b for b in bad[:3])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
