#!/usr/bin/env python3
"""t3_decl.py - milestone M3: declarations and the pool prefix, from the
source alone.

    python -I tests/t3_decl.py [--only TEXT] [-v] [--update]

slrrc.front() is given the source text and the CorpusProvider (the other
classes of the corpus; the classes of the source itself come from the source).
The class file of the source is opened only to compare.

Per pair and class:
  a  the class name, and the number of classes of the source against the
     number of blobs of the file;
  b  the predicted pool prefix (steps 1 to 3: seed, types, names) equals the
     head of the shipped pool, entry for entry and byte for byte;
  c  the CLSS, FILD and MTHD payloads are byte-identical, and FILD / MTHD are
     present exactly when the shipped file has them;
  d  the number of trees.
A pair passes when every class of it passes.  The oracle pairs (Build 940,
consistent) and the other pairs (stale, older builds; their declarations did
not change) are counted apart.

For every source of the corpus (the 635 without a class file included) and
every class of it:
  e  pool prefix and rows equal the ones of analysis/decl/poolmodel.py (the
     model the rules were proven with), run on the tree of lib/parser.py.

Headline:
  M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705,
  FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168;
  equal to poolmodel 3508/3508 classes
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import tufa                 # noqa: E402  lib
import parser as libparser  # noqa: E402  lib/parser.py
import universe             # noqa: E402  analysis/decl
import poolmodel            # noqa: E402  analysis/decl
import libbridge            # noqa: E402  analysis/decl

import slrrc                # noqa: E402
from slrrc import write     # noqa: E402


def as_tufa(rows):
    """Rows of slrrc.decl as the tuples of lib/tufa.py."""
    sf = [tufa.Field(r.flags, r.name, r.type, r.tree) for r in rows.static_fields]
    inf = [tufa.Field(r.flags, r.name, r.type, r.tree) for r in rows.instance_fields]
    sm = [tufa.Method(r.flags, r.name, r.desc, r.tree, r.locals) for r in rows.static_methods]
    im = [tufa.Method(r.flags, r.name, r.desc, r.tree, r.locals)
          for r in rows.instance_methods]
    clss = tufa.Clss(tufa.NONE, rows.interface, rows.name, rows.super, list(rows.interfaces))
    return sf, inf, sm, im, clss, len(rows.owners)


def content(cons):
    """Pool tuples in the index-free form of poolmodel."""
    b = tufa.Blob()
    b.cons = cons
    return poolmodel.content(b)


def against_class(cb, blob, count):
    """Compare one ClassBuild with the shipped blob.  count(key) is called
    for every part that agrees.  Returns the list of differences."""
    bad = []
    if cb.info.name != blob.name:
        bad.append('class name %s / %s' % (cb.info.name, blob.name))
    mine = cb.pool.to_cons()
    n = len(mine)
    theirs = blob.cons[:n]
    ok = mine == theirs
    if ok:
        # the same in bytes: entries only refer to lower indices, so the
        # prefix of the pool is a prefix of the CONS payload behind the count
        a = write.cons_payload(cb.pool)[4:]
        ok = tufa.chunk_payload(blob, 'CONS')[4:4 + len(a)] == a
        if not ok:
            bad.append('pool prefix equal as entries, different as bytes')
    else:
        k = 0
        while k < min(n, len(theirs)) and mine[k] == theirs[k]:
            k += 1
        ca, cc = content(mine), poolmodel.content(blob)
        bad.append('pool entry %d: %r / shipped %r' % (
            k, ca[k] if k < len(ca) else None, cc[k] if k < len(cc) else None))
    if ok:
        count('prefix')
    count('prefix entries', n)
    rows = cb.rows
    for tag, present, payload in (
            ('CLSS', True, write.clss_payload(rows)),
            ('FILD', write.has_fild(rows), write.fild_payload(rows)),
            ('MTHD', write.has_mthd(rows), write.mthd_payload(rows))):
        shipped = tag in blob.chunks
        if shipped:
            count(tag + ' present')
        if shipped != present:
            bad.append('%s chunk %s, shipped file %s' % (
                tag, 'written' if present else 'not written',
                'has one' if shipped else 'has none'))
        elif shipped:
            if tufa.chunk_payload(blob, tag) == payload:
                count(tag)
            else:
                bad.append('%s payload differs' % tag)
    if len(rows.owners) == len(blob.trees):
        count('tree count')
    else:
        bad.append('%d trees / %d' % (len(rows.owners), len(blob.trees)))
    return bad


def against_model(res, text, name, uni):
    """Compare every class of a front result with poolmodel on the tree of
    lib/parser.py.  Returns [(class name, None or difference)]."""
    unit = libbridge.convert(libparser.parse(text, name))
    out = []
    if len(unit.classes) != len(res.classes):
        return [('?', '%d classes / poolmodel %d' % (len(res.classes), len(unit.classes)))]
    for cdecl, cb in zip(unit.classes, res.classes):
        m = poolmodel.Model(unit, cdecl, None, uni).run()
        why = None
        mine = content(cb.pool.to_cons())
        if mine != m.pool.entries:
            k = poolmodel.first_diff(mine, m.pool.entries)
            why = 'pool entry %d: %r / poolmodel %r' % (
                k, mine[k] if k < len(mine) else None,
                m.pool.entries[k] if k < len(m.pool.entries) else None)
        elif repr(as_tufa(cb.rows)) != repr(m.member_rows()):
            why = 'rows differ from poolmodel'
        out.append((cb.info.name, why))
    return out


def run(opts):
    oc = harness.Outcome('M3 declarations')
    prov = harness.provider()
    uni = universe.load()
    options = slrrc.Options(strict=True)
    oracle_keys = set(e.key for e in harness.oracle_pairs())
    c = {}

    def counter(group):
        def count(key, n=1):
            c[group, key] = c.get((group, key), 0) + n
        return count

    passed = []
    model_passed = []
    model_classes = 0
    for e in harness.selected(harness.all_sources(), opts):
        text = harness.load_source(e)
        res = slrrc.front(text, e.source, prov, options)
        if not res.ok or res.diagnostics:
            d = res.diagnostics[0]
            oc.failures.append('%s: line %d column %d: %s %s' % (
                e.source, d.line, d.column, d.code, d.text))
            if e.is_pair:
                grp = 'oracle' if e.key in oracle_keys else 'other'
                counter(grp)('pairs')
                counter(grp)('classes', len(e.blobs))
            model_classes += len(e.declared)
            continue
        # e: poolmodel
        for cname, why in against_model(res, text, e.source, uni):
            model_classes += 1
            if why is None:
                model_passed.append('%s#%s' % (e.source, cname))
            else:
                oc.failures.append('%s %s: %s' % (e.source, cname, why))
        if not e.is_pair:
            continue
        # a to d: the shipped class
        grp = 'oracle' if e.key in oracle_keys else 'other'
        count = counter(grp)
        blobs = oracle.load_class(e)
        count('pairs')
        count('classes', len(blobs))
        file_ok = len(blobs) == len(res.classes)
        if not file_ok:
            oc.failures.append('%s: %d classes, the class file has %d' % (
                e.source, len(res.classes), len(blobs)))
        for cb, blob in zip(res.classes, blobs):
            bad = against_class(cb, blob, count)
            if bad:
                file_ok = False
                oc.failures.append('%s %s: %s' % (e.cls, blob.name, '; '.join(bad)))
            else:
                count('classes ok')
        if file_ok:
            count('pairs ok')
            passed.append(e.cls)
    for src, diags in prov.failed:
        d = diags[0]
        oc.failures.append('%s (declared for the provider): line %d: %s %s' % (
            src, d.line, d.code, d.text))

    def g(group, key):
        return c.get((group, key), 0)

    oc.text = ('%d/%d oracle pairs (%d/%d classes; CLSS %d/%d, FILD %d/%d, MTHD %d/%d; '
               '%d prefix entries); other pairs %d/%d; equal to poolmodel %d/%d classes' % (
                   g('oracle', 'pairs ok'), g('oracle', 'pairs'),
                   g('oracle', 'classes ok'), g('oracle', 'classes'),
                   g('oracle', 'CLSS'), g('oracle', 'CLSS present'),
                   g('oracle', 'FILD'), g('oracle', 'FILD present'),
                   g('oracle', 'MTHD'), g('oracle', 'MTHD present'),
                   g('oracle', 'prefix entries'),
                   g('other', 'pairs ok'), g('other', 'pairs'),
                   len(model_passed), model_classes))
    oc.ok = (g('oracle', 'pairs ok') == g('oracle', 'pairs') == 2651 and
             g('oracle', 'classes ok') == g('oracle', 'classes') == 2705 and
             g('oracle', 'CLSS') == g('oracle', 'CLSS present') == 2705 and
             g('oracle', 'FILD') == g('oracle', 'FILD present') == 238 and
             g('oracle', 'MTHD') == g('oracle', 'MTHD present') == 2701 and
             g('oracle', 'prefix') == 2705 and
             g('oracle', 'tree count') == 2705 and
             g('oracle', 'prefix entries') == 31141 and
             g('other', 'pairs ok') == g('other', 'pairs') == 168 and
             len(model_passed) == model_classes == 3508 and
             not prov.failed)
    oc.passed['m3.pass'] = passed
    oc.passed['m3_model.pass'] = model_passed
    oc.notes.append('other pairs: %d/%d classes; prefix exact %d + %d classes; '
                    'tree count %d + %d' % (
                        g('other', 'classes ok'), g('other', 'classes'),
                        g('oracle', 'prefix'), g('other', 'prefix'),
                        g('oracle', 'tree count'), g('other', 'tree count')))
    oc.notes.append('classes the provider declared from a source: %d' % sum(
        1 for e in harness.source_only() for n in e.declared_names
        if n in prov.classes and prov.files.get(n) is None))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
