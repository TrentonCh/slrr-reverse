#!/usr/bin/env python3
"""decl_expected.py - the numbers milestone M3 has to print, computed today from
the existing models (analysis/decl/poolmodel.py on the AST of lib/parser.py).

    python -I decl_expected.py

From the SOURCE ALONE (the class file is opened only to compare):

    prefix   the first entries of CONS: seed, types, names (steps 1 to 3 of
             notes/declarations.md); compared with the same number of entries
             at the head of the shipped pool
    CLSS, FILD, MTHD   the chunk payloads, byte for byte (their pool indices
             all lie in the prefix)

This is what the first implementation step of the compiler must reproduce
without the analysis code: the same counts, printed by its own test script.
"""
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', 'lib'))
_DECL = os.path.normpath(os.path.join(_HERE, '..', 'analysis', 'decl'))
for p in (_DECL, _LIB):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle             # noqa: E402
import tufa               # noqa: E402
import parser as libparser  # noqa: E402
import universe           # noqa: E402
import poolmodel          # noqa: E402
import libbridge          # noqa: E402


def main(argv):
    uni = universe.load()
    c = Counter()
    fails = []
    for e in oracle.iter_entries():
        if e.category not in ('pair_940', 'pair_other'):
            continue
        grp = 'oracle' if (e.category == 'pair_940' and e.consistent) else 'rest'
        unit = libbridge.convert(libparser.parse(oracle.load_source(e), e.source))
        blobs = oracle.load_class(e)
        file_ok = len(unit.classes) == len(blobs)
        for cdecl, blob in zip(unit.classes, blobs):
            c[grp, 'classes'] += 1
            m = poolmodel.Model(unit, cdecl, None, uni).run()
            pre = m.pool.entries
            actual = poolmodel.content(blob)
            ok_pre = actual[:len(pre)] == pre
            c[grp, 'prefix'] += ok_pre
            c[grp, 'prefix entries'] += len(pre)
            c[grp, 'pool entries'] += len(actual)
            pb = tufa.Blob()
            pb.build = blob.build
            pb.cons = poolmodel.to_indexed(pre)
            sf, inf, sm, im, clss, ntrees = m.member_rows()
            pb.static_fields, pb.instance_fields = sf, inf
            pb.static_methods, pb.instance_methods = sm, im
            pb.clss = clss
            pb.trees = [[] for _ in range(ntrees)]
            pb.chunks = tufa.canonical_chunks(pb)
            ok_all = ok_pre
            for tag in ('CLSS', 'FILD', 'MTHD'):
                pa, pp = tag in blob.chunks, tag in pb.chunks
                if pa:
                    c[grp, tag + ' present'] += 1
                same = (pa == pp) and (not pa or tufa.chunk_payload(pb, tag)
                                       == tufa.chunk_payload(blob, tag))
                if pa and same:
                    c[grp, tag] += 1
                ok_all = ok_all and same
            ok_trees = ntrees == len(blob.trees)
            c[grp, 'tree count'] += ok_trees
            ok_all = ok_all and ok_trees
            c[grp, 'all'] += ok_all
            file_ok = file_ok and ok_all
            if not ok_all:
                fails.append('%s %s' % (e.cls, blob.name))
        c[grp, 'pairs'] += 1
        c[grp, 'pairs ok'] += file_ok
    for grp, label in (('oracle', 'oracle pairs (Build 940, consistent)'),
                       ('rest', 'the other pairs (stale, older builds)')):
        print(label)
        print('  pairs %d, classes %d' % (c[grp, 'pairs'], c[grp, 'classes']))
        print('  pool prefix (seed, types, names) exact   %d of %d classes  (%d of %d pool entries are prefix)'
              % (c[grp, 'prefix'], c[grp, 'classes'], c[grp, 'prefix entries'], c[grp, 'pool entries']))
        for tag in ('CLSS', 'FILD', 'MTHD'):
            print('  %s payload byte-identical             %d of %d chunks' % (
                tag, c[grp, tag], c[grp, tag + ' present']))
        print('  tree count                               %d of %d classes' % (
            c[grp, 'tree count'], c[grp, 'classes']))
        print('  everything above                         %d of %d classes, %d of %d pairs' % (
            c[grp, 'all'], c[grp, 'classes'], c[grp, 'pairs ok'], c[grp, 'pairs']))
    print('M3 declarations %d/%d oracle pairs' % (c['oracle', 'pairs ok'], c['oracle', 'pairs']))
    for f in fails[:20]:
        print('  FAIL %s' % f)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
