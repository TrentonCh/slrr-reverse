#!/usr/bin/env python3
"""stats_decl.py - census of the declaration side of the corpus, for
notes/declarations.md.  Every number is taken from the sources of the pairs
and from the class files they compile to; the model (poolmodel) is used only
to label pool entries with the rule that interned them.

    python -I stats_decl.py
"""
import os
import re
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
NONE = tufa.NONE
POOL_OPS = (0x1006, 0x4009, 0x4012, 0x401C, 0x4025, 0x4026, 0x100E, 0x1019,
            0x101A, 0x101B)


def referenced(b):
    """Indices of the pool entries something refers to."""
    ref = set()
    c = b.clss
    for i in [c.name, c.super] + list(c.interfaces):
        if i != NONE:
            ref.add(i)
    for st, i, f in b.fields():
        ref.update((f.name, f.type))
    for st, i, m in b.methods():
        ref.update((m.name, m.desc))
    for t in b.trees:
        for n in t:
            if tufa.OP_FLAGS[n.op] in POOL_OPS:
                ref.add(n.imm)
    for e in b.cons:
        if e[0] in ('class',):
            ref.add(e[1])
        elif e[0] == 'rid':
            ref.add(e[1])
        elif e[0] in ('nat', 'ref'):
            ref.update((e[1], e[2]))
    return ref


def desc_form(d):
    """Shape of a descriptor with class names replaced by C."""
    return re.sub(r'L[^;]*;', 'LC;', d)


def body_form(m):
    if m.body is None:
        return ';'
    if not m.body.stmts:
        return '{}'
    return '{...}'


def main(argv):
    uni = universe.load()
    c = Counter()
    fflags = Counter()
    mflags = Counter()
    ftypes = Counter()
    mdesc = Counter()
    bodies = Counter()
    unref = Counter()
    unref_kind = Counter()
    roles = Counter()
    share_ex = []
    multi = []
    interleave = Counter()
    c['... of which the class file has an <init> row'] = 0
    # member names in angle brackets over every blob of the corpus: is there
    # any synthetic member besides the constructor name?
    angle = Counter()
    for rel, blobs in oracle.iter_class_files():
        for b in blobs:
            for st, i, r in b.methods():
                n = b.utf8(r.name)
                if n.startswith('<'):
                    angle[('method', 'static' if st else 'instance', n)] += 1
            for st, i, f in b.fields():
                n = b.utf8(f.name)
                if n.startswith('<'):
                    angle[('field', 'static' if st else 'instance', n)] += 1
    for e in oracle.iter_pairs(None):
        unit = srcparse.parse(oracle.load_source(e))
        blobs = oracle.load_class(e)
        c['pairs'] += 1
        if len(blobs) > 1:
            c['pairs with more than one class'] += 1
            multi.append((e.key, len(blobs)))
        if any(i.name == 'java.lang' for i in unit.imports):
            c['sources that import java.lang.*'] += 1
        for bi, (cd, b) in enumerate(zip(unit.classes, blobs)):
            c['classes'] += 1
            m = poolmodel.Model(unit, cd, b, uni)
            m.run()
            assert m.pool.entries == m.cons_content, e.key
            # ---- members
            kinds = [x.k for x in cd.members]
            fseen = False
            mseen = False
            mixed = False
            for k in kinds:
                if k == 'field':
                    if mseen:
                        mixed = True
                    fseen = True
                else:
                    mseen = True
            if mixed:
                interleave['classes with a field declared after a method'] += 1
            for group, label in ((lambda x: x.k == 'field', 'fields'),
                                 (lambda x: x.k != 'field', 'methods')):
                seq = ['static' in x.mods for x in cd.members if group(x)]
                # an instance member before a static one: the vectors are
                # not a prefix / suffix split of the source order
                if any(not seq[i] and any(seq[i + 1:]) for i in range(len(seq))):
                    interleave['classes with a static %s after an instance one' % label[:-1]] += 1
            last_decl = None
            for x in cd.members:
                if x.k == 'field':
                    c['field rows'] += 1
                    fl = m.flags(x.mods)
                    fflags[(' '.join(x.mods) or '(none)', fl)] += 1
                    ftypes[desc_form(m.desc(x.type, x.dims))] += 1
                    if x.init is not None:
                        c['field rows with an initialiser'] += 1
                    if x.decl_tok == last_decl:
                        c['field rows that share a declaration with the row before'] += 1
                    last_decl = x.decl_tok
                else:
                    last_decl = None
                    c['method rows'] += 1
                    if x.k == 'ctor':
                        c['constructor rows'] += 1
                    fl = m.flags(x.mods)
                    mflags[(x.k, ' '.join(x.mods) or '(none)', fl)] += 1
                    mdesc[desc_form(m.method_desc(x))[:0] + ('ctor ' if x.k == 'ctor' else 'method ')
                          + ('no return part' if x.k == 'ctor' else 'returns ' + desc_form(m.desc(x.ret)))] += 1
                    what = x.k
                    if 'native' in x.mods:
                        what += ' native'
                    if 'abstract' in x.mods:
                        what += ' abstract'
                    bodies[(what, body_form(x))] += 1
            if not any(x.k == 'ctor' for x in cd.members):
                c['classes without a constructor in the source'] += 1
                if any(b.utf8(r.name) == '<init>' for _, _, r in b.methods()):
                    c['... of which the class file has an <init> row'] += 1
            # ---- unreferenced entries, by the rule that interned them
            ref = referenced(b)
            for i, (ent, ph) in enumerate(zip(m.pool.entries, m.pool.phase)):
                if i not in ref:
                    unref[ph] += 1
                    unref_kind[ent[0]] += 1
                    c['unreferenced entries'] += 1
            c['pool entries'] += len(m.pool.entries)
            # ---- sharing: one utf8 in several roles
            role = {}
            for st, i, f in b.fields():
                role.setdefault(f.name, set()).add('field name')
                role.setdefault(f.type, set()).add('field type')
            for st, i, r in b.methods():
                role.setdefault(r.name, set()).add('method name')
                role.setdefault(r.desc, set()).add('method descriptor')
            for t in b.trees:
                for n in t:
                    code = tufa.OP_FLAGS[n.op]
                    if code == 0x4009:
                        role.setdefault(n.imm, set()).add('string literal')
                    elif code == 0x1006:
                        role.setdefault(n.imm, set()).add('type carrier')
                    elif code in (0x4012, 0x401C, 0x4025, 0x4026):
                        role.setdefault(n.imm, set()).add('unresolved name')
            for i, ent in enumerate(b.cons):
                if ent[0] == 'nat':
                    role.setdefault(ent[1], set()).add('nat name')
                    role.setdefault(ent[2], set()).add('nat descriptor')
                elif ent[0] == 'class':
                    role.setdefault(ent[1], set()).add('class name')
                elif ent[0] == 'rid':
                    role.setdefault(ent[1], set()).add('pack name')
            for i, rs in role.items():
                if len(rs) > 1:
                    roles[' + '.join(sorted(rs))] += 1
                    if 'string literal' in rs and len(share_ex) < 6 and \
                            ('field name' in rs or 'method name' in rs):
                        share_ex.append('%s#%d [%d] %r: %s' % (e.key, bi, i, b.utf8(i), ', '.join(sorted(rs))))
    lines = []
    out = lines.append
    out('Census of the %d pairs (%d classes)' % (c['pairs'], c['classes']))
    for k in sorted(c):
        out('   %-62s %7d' % (k, c[k]))
    out('')
    out('member names in angle brackets, all 2,993 blobs of the corpus')
    for k, n in sorted(angle.items()):
        out('   %-8s %-9s %-10s %6d' % (k + (n,)))
    out('')
    out('field rows by modifier keywords as written -> flags word')
    for (mods, fl), n in sorted(fflags.items(), key=lambda x: (x[0][1], x[0][0])):
        out('   0x%02X  %-34s %6d' % (fl, mods, n))
    out('')
    out('method rows by modifier keywords as written -> flags word')
    for (k, mods, fl), n in sorted(mflags.items(), key=lambda x: (x[0][2], x[0][0], x[0][1])):
        out('   0x%02X  %-7s %-34s %6d' % (fl, k, mods, n))
    out('')
    out('field type strings by shape (C = a dotted class name)')
    for k, n in ftypes.most_common():
        out('   %-12s %6d' % (k, n))
    out('')
    out('method rows by return part')
    for k, n in mdesc.most_common():
        out('   %-30s %6d' % (k, n))
    out('')
    out('bodies as written')
    for (what, form), n in sorted(bodies.items()):
        out('   %-24s %-6s %6d' % (what, form, n))
    out('')
    out('order of members')
    for k in sorted(interleave):
        out('   %-62s %7d' % (k, interleave[k]))
    out('')
    out('entries that nothing refers to, by the rule that interned them (%d of %d entries)' % (
        c['unreferenced entries'], c['pool entries']))
    for k, n in sorted(unref.items()):
        out('   %-46s %7d' % (k, n))
    out('   by kind: ' + ', '.join('%s %d' % kv for kv in sorted(unref_kind.items())))
    out('')
    out('utf8 entries used in more than one role (an entry is shared by value)')
    for k, n in roles.most_common(40):
        out('   %6d  %s' % (n, k))
    for x in share_ex:
        out('   e.g. ' + x)
    out('')
    out('pairs with more than one class: %d' % len(multi))
    out('   ' + ', '.join('%s (%d)' % (k.rsplit('/', 1)[1], n) for k, n in multi))
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'stats_decl.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
