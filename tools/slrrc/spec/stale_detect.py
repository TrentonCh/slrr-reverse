#!/usr/bin/env python3
"""stale_detect.py - which cheap test finds the pairs whose class is not the
compilation of the source that ships beside it?

    python -I stale_detect.py [--json OUT]

Known stale pairs: the 15 that lib/paircheck.py finds (token census, line
check) and the 2 that only a full compile finds (compiled against another
version of a class they call; notes/expressions.md 9.2).

Tests compared, per pair:
    time      the write times of source and class, at the 2 s resolution of a
              DOS time stamp (the exe's own staleness rule is "the two packed
              times differ", docs/ENGINE.md 4.2)
    newer     the source is newer than the class by more than 2 s
    members   the rows of CLSS / FILD / MTHD predicted from the source differ
              from the class file (lib/symbols.py; names, flags, descriptors,
              tree numbers, locals word)
    census    lib/paircheck.py (what the corpus index stores)

The corpus is a copy; its time stamps are those the copy kept.  The script
prints how many pairs have equal stamps so that the reader can judge whether
the stamps mean anything in this copy.
"""
import json
import os
import sys
import time
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import oracle    # noqa: E402
import parser    # noqa: E402
import symbols   # noqa: E402

DEP_STALE = ('parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart/'
             'differential/Center_Differential', 'sl/Scripts/game/Garage')


def members_differ(e, uni):
    """True when the rows predicted from the source differ from the class."""
    unit = parser.parse(oracle.load_source(e), e.source)
    res = symbols.Resolver(unit, uni)
    blobs = oracle.load_class(e)
    models = list(symbols.class_models(unit, res))
    if len(models) != len(blobs):
        return True
    for m, b in zip(models, blobs):
        if m.name != b.name or m.super != b.super_name:
            return True
        if list(m.interfaces) != list(b.interface_names):
            return True
        for rows, brows, isf in ((m.static_fields, b.static_fields, True),
                                 (m.instance_fields, b.instance_fields, True),
                                 (m.static_methods, b.static_methods, False),
                                 (m.instance_methods, b.instance_methods, False)):
            if len(rows) != len(brows):
                return True
            for r, br in zip(rows, brows):
                if isf:
                    if (r.name, r.flags, r.desc, r.tree) != (
                            b.utf8(br.name), br.flags, b.utf8(br.type), br.init):
                        return True
                else:
                    if (r.name, r.flags, r.desc, r.tree, r.locals) != (
                            b.utf8(br.name), br.flags, b.utf8(br.desc), br.tree, br.locals):
                        return True
    return False


def main(argv):
    out_json = argv[argv.index('--json') + 1] if '--json' in argv else None
    root = oracle.corpus_root()
    uni = symbols.Universe.from_index()
    c = Counter()
    rows = []
    for e in oracle.iter_entries():
        if e.category not in ('pair_940', 'pair_other'):
            continue
        ts = os.path.getmtime(os.path.join(root, e.source))
        tc = os.path.getmtime(os.path.join(root, e.cls))
        dos_s, dos_c = int(ts) // 2, int(tc) // 2
        t_time = dos_s != dos_c
        t_newer = ts - tc > 2
        t_mem = members_differ(e, uni)
        t_census = e.consistent is False
        key = e.dir + '/' + e.name
        known = 'stale' if t_census else 'dep' if key in DEP_STALE else 'ok'
        c['pairs'] += 1
        c['known ' + known] += 1
        for name, t in (('time', t_time), ('newer', t_newer), ('members', t_mem),
                        ('census', t_census)):
            if t:
                c['%s flags' % name] += 1
                c['%s flags, known %s' % (name, known)] += 1
        if known != 'ok':
            rows.append((key, e.category, known, t_time, t_newer, t_mem, t_census,
                         round(ts - tc)))
    print('pairs: %d; known stale by census: %d; known stale by dependency: %d' % (
        c['pairs'], c['known stale'], c['known dep']))
    days = Counter()
    for e in oracle.iter_entries():
        for rel in (e.source, e.cls):
            if rel:
                days[time.strftime('%Y-%m-%d', time.gmtime(
                    os.path.getmtime(os.path.join(root, rel))))] += 1
    print('write times of all sources and class files of this corpus, by day: %s' % dict(days))
    if len(days) == 1:
        print('  every file carries the day the corpus was copied: the two time tests below '
              'measure the copy, not the game.  They are listed to show that a copied or '
              'unpacked install defeats the exe\'s own staleness rule.')
    print()
    print('%-10s %14s %22s %20s %14s' % ('test', 'pairs flagged', 'of the 15 census-stale',
                                         'of the 2 dep-stale', 'of the others'))
    for name in ('time', 'newer', 'members', 'census'):
        print('%-10s %14d %22d %20d %14d' % (
            name, c['%s flags' % name], c['%s flags, known stale' % name],
            c['%s flags, known dep' % name], c['%s flags, known ok' % name]))
    print()
    print('the known stale pairs (source time - class time in seconds):')
    for r in rows:
        print('  %-108s %-10s %-5s time %-5s newer %-5s members %-5s census %-5s  %+d s' % r)
    if out_json:
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump({'counts': dict(c), 'known': rows}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
