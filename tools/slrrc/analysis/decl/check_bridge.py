#!/usr/bin/env python3
"""check_bridge.py - the pool model on lib/parser.py.

Two parsers were written independently: analysis/decl/srcparse.py (the one
the pool model was developed on) and lib/parser.py (the language work; the
parser a compiler will use).  This check runs the model on both:

  B1  every source: both parsers give the same classes, and for each class
      the same pool of steps 1 to 3 and the same FILD / MTHD rows and CLSS
  B2  every pair: the model on lib/parser.py reproduces CONS, CLSS, FILD and
      MTHD of the shipped class byte for byte

    python -I check_bridge.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle       # noqa: E402
import srcparse     # noqa: E402
import universe     # noqa: E402
import poolmodel    # noqa: E402
import libbridge    # noqa: E402
import check_decl   # noqa: E402

OUT = os.path.join(HERE, 'out')


def main(argv):
    try:
        import parser as libparser
        libparser.parse
    except (ImportError, AttributeError) as x:
        print('lib/parser.py is not available: %s' % x)
        return 1
    uni = universe.load()
    lines = []
    out = lines.append
    nsrc = ncls = same = 0
    diff = []
    pairs = pair_cls = 0
    ok = {t: 0 for t in ('CONS', 'CLSS', 'FILD', 'MTHD')}
    allok = 0
    for e in oracle.iter_entries():
        if e.source is None:
            continue
        nsrc += 1
        text = oracle.load_source(e)
        a = srcparse.parse(text)
        b = libbridge.convert(libparser.parse(text, e.source))
        if [c.name for c in a.classes] != [c.name for c in b.classes]:
            diff.append('%s: classes %r / %r' % (e.key, [c.name for c in a.classes],
                                                 [c.name for c in b.classes]))
            continue
        for ca, cb in zip(a.classes, b.classes):
            ncls += 1
            ma = poolmodel.Model(a, ca, None, uni).run()
            mb = poolmodel.Model(b, cb, None, uni).run()
            if ma.pool.entries == mb.pool.entries and \
                    repr(ma.member_rows()) == repr(mb.member_rows()):
                same += 1
            else:
                d = poolmodel.first_diff(ma.pool.entries, mb.pool.entries)
                diff.append('%s %s: pools differ at %s' % (e.key, ca.name, d))
        if e.is_pair:
            pairs += 1
            blobs = oracle.load_class(e)
            for cb, blob in zip(b.classes, blobs):
                pair_cls += 1
                res, det, m = check_decl.check_class(b, cb, blob, uni)
                for t in ok:
                    ok[t] += bool(res[t])
                allok += all(res[t] for t in check_decl.TAGS)
    out('B1 sources: %d, classes: %d; same pool (steps 1 to 3) and same rows from '
        'both parsers: %d of %d' % (nsrc, ncls, same, ncls))
    for x in diff[:20]:
        out('      ' + x)
    out('B2 pairs: %d, classes: %d; the model on lib/parser.py, byte for byte:' % (
        pairs, pair_cls))
    for t in ('CONS', 'CLSS', 'FILD', 'MTHD'):
        out('      %s  %d of %d classes' % (t, ok[t], pair_cls))
    out('      all four chunks, chunk list and tree count: %d of %d classes' % (
        allok, pair_cls))
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'check_bridge.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
