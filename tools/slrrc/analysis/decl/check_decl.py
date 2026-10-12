#!/usr/bin/env python3
"""check_decl.py - the proof run of the declaration / constant-pool model.

For every source/class pair the model (poolmodel.py) predicts

    CONS   from the source (seed, type and name phases) and, for the entries
           that depend on symbol resolution and on emission, from the TREE
           chunk of the shipped class used as an oracle of first-use order
    CLSS, FILD, MTHD   from the source alone

and the prediction is compared with the shipped chunk BYTE FOR BYTE.

    python -I check_decl.py [--verbose] [--json out.json]

Output: counts per selection of pairs (the 2,651 oracle pairs, all Build 940
pairs, the pairs of older builds, all pairs), per chunk, plus the list of
every class that fails.
"""
import json
import os
import sys
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle      # noqa: E402
import srcparse    # noqa: E402
import tufa        # noqa: E402
import universe    # noqa: E402
import poolmodel   # noqa: E402

OUT = os.path.join(HERE, 'out')


def predicted_blob(m, actual):
    """A tufa.Blob holding the model's CONS, FILD, MTHD and CLSS (TREE and
    build are copied from the shipped blob; they are not this model's job)."""
    b = tufa.Blob()
    b.build = actual.build
    b.cons = poolmodel.to_indexed(m.pool.entries)
    sf, inf, sm, im, clss, ntrees = m.member_rows()
    b.static_fields, b.instance_fields = sf, inf
    b.static_methods, b.instance_methods = sm, im
    b.clss = clss
    b.trees = actual.trees
    b.chunks = tufa.canonical_chunks(b)
    return b, ntrees


def check_class(unit, c, actual, uni, opts=None):
    """Returns dict chunk -> True/False, plus details."""
    m = poolmodel.Model(unit, c, actual, uni, opts)
    m.run()
    res = OrderedDict()
    det = []
    try:
        pb, ntrees = predicted_blob(m, actual)
    except KeyError as x:
        # a row names an entry the predicted pool does not hold
        res['CONS'] = False
        res['CLSS'] = res['FILD'] = res['MTHD'] = res['LAYOUT'] = False
        res['NTREES'] = False
        det.append('row names an entry missing from the predicted pool: %r' % (x,))
        m.predicted = None
        return res, det, m
    m.predicted = pb
    res['LAYOUT'] = pb.chunks == list(actual.chunks)
    if not res['LAYOUT']:
        det.append('chunks %s / %s' % (pb.chunks, actual.chunks))
    for tag in ('CONS', 'CLSS', 'FILD', 'MTHD'):
        pa = tag in actual.chunks
        pp = tag in pb.chunks
        if not pa and not pp:
            res[tag] = True
            continue
        if pa != pp:
            res[tag] = False
            continue
        x = tufa.chunk_payload(pb, tag)
        y = tufa.chunk_payload(actual, tag)
        res[tag] = x == y
        if x != y:
            off = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]),
                       min(len(x), len(y)))
            det.append('%s differs at +0x%X: %s' % (tag, off, oracle.locate(
                pb if off < len(x) else actual, tag, off)))
    res['NTREES'] = ntrees == len(actual.trees)
    if not res['NTREES']:
        det.append('tree count %d / %d' % (ntrees, len(actual.trees)))
    if m.unresolved:
        det.append('unresolved names %r' % (sorted(set(m.unresolved)),))
    return res, det, m


SELECTIONS = (
    ('oracle pairs (Build 940, consistent)',
     lambda e: e.builds == [oracle.BUILD_940] and e.consistent),
    ('Build 940 pairs, stale ones included',
     lambda e: e.builds == [oracle.BUILD_940]),
    ('pairs of older builds (0x24F5C, 0x11AE1)',
     lambda e: e.builds != [oracle.BUILD_940]),
    ('all pairs', lambda e: True),
)
TAGS = ('CONS', 'CLSS', 'FILD', 'MTHD', 'LAYOUT', 'NTREES')
BYTES = Counter()
FILES = []


def run(opts=None, verbose=False):
    uni = universe.load()
    rows = []
    for e in oracle.iter_pairs(None):
        unit = srcparse.parse(oracle.load_source(e))
        blobs = oracle.load_class(e)
        if len(unit.classes) != len(blobs):
            rows.append((e, None, None, {t: False for t in TAGS},
                         ['class count %d / %d' % (len(unit.classes), len(blobs))]))
            continue
        pred = []
        for bi, (c, b) in enumerate(zip(unit.classes, blobs)):
            res, det, m = check_class(unit, c, b, uni, opts)
            rows.append((e, bi, b, res, det))
            pred.append(m.predicted)
            for tag in ('CONS', 'CLSS', 'FILD', 'MTHD', 'TREE'):
                if tag in b.chunks:
                    BYTES[tag] += b.chunk_spans[tag][1] + 8
            BYTES['TUFA'] += 12
        # the whole file: predicted CONS / FILD / MTHD / CLSS around the
        # shipped TREE, serialised and compared with the bytes on disk
        data = oracle.load_class_bytes(e)
        ok = None not in pred and tufa.serialise(pred) == data
        FILES.append((e, ok, len(data)))
    return rows


def summarise(rows, out=sys.stdout, verbose=False):
    doc = OrderedDict()
    for label, sel in SELECTIONS:
        sub = [r for r in rows if sel(r[0])]
        pairs = OrderedDict()
        for r in sub:
            pairs.setdefault(r[0].key, []).append(r)
        d = OrderedDict()
        d['pairs'] = len(pairs)
        d['classes'] = len(sub)
        print('%s: %d pairs, %d classes' % (label, len(pairs), len(sub)), file=out)
        for tag in TAGS:
            okc = sum(1 for r in sub if r[3][tag])
            okp = sum(1 for v in pairs.values() if all(r[3][tag] for r in v))
            d[tag] = {'classes_ok': okc, 'pairs_ok': okp}
            extra = ''
            if tag in ('CONS', 'CLSS', 'FILD', 'MTHD'):
                # how many classes really hold the chunk (FILD and MTHD are
                # absent from a class without fields / methods; predicting
                # the absence counts as right, but is reported apart)
                have = [r for r in sub if r[2] is not None and tag in r[2].chunks]
                okh = sum(1 for r in have if r[3][tag])
                d[tag]['chunks_present'] = len(have)
                d[tag]['chunks_present_ok'] = okh
                extra = '   chunks present %5d, identical %5d' % (len(have), okh)
            print('   %-6s  classes %5d of %5d   pairs %5d of %5d%s' % (
                tag, okc, len(sub), okp, len(pairs), extra), file=out)
        allc = sum(1 for r in sub if all(r[3][t] for t in TAGS))
        allp = sum(1 for v in pairs.values()
                   if all(all(r[3][t] for t in TAGS) for r in v))
        d['all'] = {'classes_ok': allc, 'pairs_ok': allp}
        print('   %-6s  classes %5d of %5d   pairs %5d of %5d' % (
            'all', allc, len(sub), allp, len(pairs)), file=out)
        doc[label] = d
    okf = sum(1 for e, ok, n in FILES if ok)
    print('whole class files rebuilt from the predicted CONS, FILD, MTHD, CLSS and '
          'the shipped TREE: %d of %d identical to the file on disk (%d bytes)' % (
              okf, len(FILES), sum(n for e, ok, n in FILES)), file=out)
    okf2 = sum(1 for e, ok, n in FILES if ok and e.builds == [oracle.BUILD_940] and e.consistent)
    print('   of the oracle pairs: %d of %d' % (okf2, sum(
        1 for e, ok, n in FILES if e.builds == [oracle.BUILD_940] and e.consistent)), file=out)
    tot = sum(BYTES.values())
    print('bytes by chunk, headers included: ' + ', '.join(
        '%s %d (%.1f%%)' % (t, BYTES[t], 100.0 * BYTES[t] / tot)
        for t in ('TUFA', 'CONS', 'FILD', 'MTHD', 'CLSS', 'TREE')), file=out)
    doc['files'] = {'identical': okf, 'of': len(FILES), 'bytes_by_chunk': dict(BYTES)}
    fails = [r for r in rows if not all(r[3][t] for t in TAGS)]
    print('classes that fail anything: %d' % len(fails), file=out)
    doc['failures'] = []
    for e, bi, b, res, det in fails:
        bad = [t for t in TAGS if not res[t]]
        line = '%s#%s build %s consistent=%s: %s' % (
            e.key, bi, ','.join('0x%X' % x for x in e.builds), e.consistent,
            ' '.join(bad))
        doc['failures'].append({'class': '%s#%s' % (e.key, bi), 'chunks': bad,
                                'details': det})
        if verbose or len(fails) <= 60:
            print('   ' + line, file=out)
            for x in det[:4]:
                print('      ' + x, file=out)
    return doc


def main(argv):
    verbose = '--verbose' in argv
    rows = run(verbose=verbose)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'check_decl.txt'), 'w', encoding='utf-8') as f:
        doc = summarise(rows, f, verbose=True)
    summarise(rows, sys.stdout, verbose)
    with open(os.path.join(OUT, 'check_decl.json'), 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
