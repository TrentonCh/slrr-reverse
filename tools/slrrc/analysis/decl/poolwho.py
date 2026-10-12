#!/usr/bin/env python3
"""poolwho.py - list the constant pool of a class with, for every entry, who
refers to it: CLSS, a FILD / MTHD row, a tree node (tree, node, line) or
another pool entry.  Exploration tool for the pool-order analysis.

    python -I poolwho.py game/Marker [--blob N] [--src] [--model]

--src appends the numbered source, --model adds the rule of poolmodel.py that
interned each entry (pairs only).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import oracle  # noqa: E402
import tufa    # noqa: E402

# 16-bit opcodes whose immediate is a pool index
POOL_OPS = {0x1006: 'type', 0x4009: 'str', 0x4012: 'name', 0x401C: 'getfield',
            0x4025: 'call_on', 0x4026: 'call_super', 0x100E: 'rid',
            0x1019: 'class', 0x101A: 'method', 0x101B: 'field'}


def referrers(blob):
    """index -> list of labels, in the order CLSS, FILD, MTHD, trees."""
    who = {}

    def add(i, label):
        if i != tufa.NONE:
            who.setdefault(i, []).append(label)

    c = blob.clss
    add(c.name, 'CLSS.name')
    add(c.super, 'CLSS.super')
    for k, j in enumerate(c.interfaces):
        add(j, 'CLSS.iface%d' % k)
    for st, i, f in blob.fields():
        w = 'sf' if st else 'if'
        add(f.name, '%s%d.name' % (w, i))
        add(f.type, '%s%d.type' % (w, i))
    for st, i, m in blob.methods():
        w = 'sm' if st else 'im'
        add(m.name, '%s%d.name' % (w, i))
        add(m.desc, '%s%d.desc' % (w, i))
    for t, nodes in enumerate(blob.trees):
        for k, n in enumerate(nodes):
            code = tufa.OP_FLAGS[n.op]
            if code in POOL_OPS:
                add(n.imm, 't%d.n%d(L%d %s)' % (t, k, n.slot, POOL_OPS[code]))
    for i, e in enumerate(blob.cons):
        if e[0] == 'utf8':
            continue
        if e[0] == 'class':
            add(e[1], 'p%d(class)' % i)
        elif e[0] == 'rid':
            add(e[1], 'p%d(rid)' % i)
        elif e[0] == 'nat':
            add(e[1], 'p%d(nat.name)' % i)
            add(e[2], 'p%d(nat.desc)' % i)
        elif e[0] == 'ref':
            add(e[1], 'p%d(ref.class)' % i)
            add(e[2], 'p%d(ref.nat)' % i)
    return who


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    name = argv[0]
    only = None
    if '--blob' in argv:
        only = int(argv[argv.index('--blob') + 1])
    e = oracle.entry(name)
    blobs = oracle.load_class(e)
    for bi, b in enumerate(blobs):
        if only is not None and bi != only:
            continue
        print('== %s blob %d: %s extends %s  (%d entries, %d trees)' % (
            e.cls, bi, b.name, b.super_name, len(b.cons), len(b.trees)))
        own = b.tree_owners()
        for t in range(len(b.trees)):
            ln = [n.slot for n in b.trees[t]]
            print('   tree %d: %s  lines %s..%s' % (
                t, '; '.join(own.get(t, ['?'])), min(ln) if ln else '-',
                max(ln) if ln else '-'))
        who = referrers(b)
        rule = None
        if '--model' in argv and e.source:
            # the rule of poolmodel that interned each entry
            import srcparse
            import universe
            import poolmodel
            unit = srcparse.parse(oracle.load_source(e))
            m = poolmodel.Model(unit, unit.classes[bi], b, universe.load())
            m.run()
            if m.pool.entries == m.cons_content:
                rule = m.pool.phase
            else:
                print('   (the model does not reproduce this pool)')
        for i, ent in enumerate(b.cons):
            w = who.get(i, [])
            txt = ' '.join(w[:4]) + (' ...+%d' % (len(w) - 4) if len(w) > 4 else '')
            if rule is not None:
                print('  [%d] %-5s %-50s %-26s <- %s' % (
                    i, ent[0], b.describe(i), rule[i], txt or 'NOTHING'))
            else:
                print('  [%d] %-5s %-50s <- %s' % (i, ent[0], b.describe(i), txt or 'NOTHING'))
    if '--src' in argv and e.source:
        text = oracle.load_source(e, keep_eol=False)
        for k, line in enumerate(text.split('\n'), 1):
            print('%4d  %s' % (k, line))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
