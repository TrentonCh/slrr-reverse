#!/usr/bin/env python3
"""study_refclass.py - which class does a member ref of the pool name?

A ref entry is (class, name, descriptor).  For every field / method node of
every tree the script determines

    R  the class the lookup starts from: the own class for a bare member, the
       class named by a preceding class node, the static type of the
       preceding path segment
    D  the class that declares the member (first hit walking up from R)
    X  the class the ref names

and tabulates X against R and D, split by static / instance member.  The
class table is built from every blob of the corpus plus, for the sources that
have no class file, the declarations predicted by poolmodel.

    python -I study_refclass.py
"""
import os
import sys
from collections import Counter

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


class Cls(object):
    __slots__ = ('name', 'super', 'ifaces', 'fields', 'methods')

    def __init__(self, name, sup, ifaces):
        self.name = name
        self.super = sup
        self.ifaces = ifaces
        self.fields = {}     # name -> (desc, static)
        self.methods = {}    # (name, desc) -> static


def build_table():
    table = {}
    for rel, blobs in oracle.iter_class_files():
        for b in blobs:
            c = Cls(b.name, b.super_name, b.interface_names)
            for st, i, f in b.fields():
                c.fields[b.utf8(f.name)] = (b.utf8(f.type), st)
            for st, i, m in b.methods():
                c.methods[(b.utf8(m.name), b.utf8(m.desc))] = st
            # ten class names are defined by more than one blob (nine dialog
            # classes ship both inside Dialog.class and as stand-alone class
            # files, java.game.track_data once per map): keep them all
            table.setdefault(b.name, []).append(c)
    uni = universe.load()
    nsrc = 0
    for e in oracle.iter_entries('source_only'):
        unit = srcparse.parse(oracle.load_source(e))
        for cd in unit.classes:
            m = poolmodel.Model(unit, cd, None, uni)
            if m.fq in table:
                continue
            c = Cls(m.fq, m.super_name(), m.interface_names())
            for mem in cd.members:
                st = 'static' in mem.mods
                if mem.k == 'field':
                    c.fields[mem.name] = (m.desc(mem.type, mem.dims), st)
                else:
                    c.methods[(mem.name, m.method_desc(mem))] = st
            table[m.fq] = [c]
            nsrc += 1
    return table, nsrc


def find(table, start, kind, name, desc):
    """(declaring class, static?) walking up from start, or (None, None).
    Where a class name has several definitions, each is tried."""
    todo = list(table.get(start, ()))
    seen = 0
    while todo and seen < 256:
        c = todo.pop(0)
        seen += 1
        if kind == 'field':
            hit = c.fields.get(name)
            if hit is not None and hit[0] == desc:
                return c.name, hit[1]
        else:
            hit = c.methods.get((name, desc))
            if hit is not None:
                return c.name, hit
        if c.super:
            todo.extend(table.get(c.super, ()))
    return None, None


def depth(table, start, target):
    """Number of superclass steps from start up to target (None: not above)."""
    level = [start]
    d = 0
    while level and d < 64:
        if target in level:
            return d
        nxt = []
        for n in level:
            for c in table.get(n, ()):
                if c.super:
                    nxt.append(c.super)
        level = nxt
        d += 1
    return None


def type_class(T):
    if T is None:
        return None
    if T.startswith('L') and T.endswith(';'):
        return T[1:-1]
    if T.startswith('['):
        e = T[1:]
        if e.startswith('L') and e.endswith(';'):
            e = e[1:-1]
        return '[' + e
    return None


def main(argv):
    table, nsrc = build_table()
    tab = Counter()
    ex = {}
    arr = Counter()
    deep = Counter()

    def note(kind, ctx, own, R, X, name, desc, where):
        if R is not None and R.startswith('['):
            arr[(kind, name, desc, X == R)] += 1
            return
        D, st = find(table, R, kind, name, desc) if R else (None, None)
        if D is None:
            key = (kind, ctx, 'member not found from R', X == R)
        else:
            rel = []
            if X == R:
                rel.append('X=R')
            if X == D:
                rel.append('X=D')
            if X == own:
                rel.append('X=own')
            key = (kind, ctx, 'static' if st else 'instance',
                   'R=D' if R == D else 'R!=D', ' '.join(rel) or 'X is another class')
            if R != D:
                deep[(kind, 'static' if st else 'instance', ctx, depth(table, R, D))] += 1
        tab[key] += 1
        ex.setdefault(key, where + ' %s.%s:%s (R=%s D=%s)' % (X, name, desc, R, D))

    for rel, blobs in oracle.iter_class_files():
        for bi, b in enumerate(blobs):
            cc = poolmodel.content(b)
            m = poolmodel.Model.from_blob(b)
            own_rows = m.tree_owner_rows()
            for t, nodes in enumerate(b.trees):
                if t not in own_rows:
                    continue
                st, mrow = own_rows[t]
                loc = m.initial_locals(st, mrow)
                code = [tufa.OP_FLAGS[x.op] for x in nodes]
                n = len(nodes)
                i = 0
                while i < n:
                    c = code[i]
                    nd = nodes[i]
                    where = '%s#%d t%d n%d' % (rel, bi, t, i)
                    if c == 0x1002 and i + 1 < n and code[i + 1] == 0x1006:
                        loc[nd.imm] = cc[nodes[i + 1].imm][1]
                        i += 1
                        continue
                    if c in (0x1011, 0x0024):
                        j = i + 1
                        if c == 0x0024:
                            if j < n and code[j] == 0x101A:
                                e = cc[nodes[j].imm]
                                note('method', 'bare', b.name, b.name, e[1], e[2], e[3], where)
                                i = j + 1
                                continue
                            if j < n and code[j] == 0x1011:
                                cnt = nodes[j].imm
                                j += 1
                            else:
                                i += 1
                                continue
                        else:
                            cnt = nd.imm
                        R = b.name
                        ctx = 'first segment'
                        for k in range(j, min(n, j + cnt)):
                            ck = code[k]
                            nk = nodes[k]
                            if ck == 0x1001:
                                T = loc.get(nk.imm)
                                R = b.name if T == 'this' else type_class(T)
                                ctx = 'after a local'
                            elif ck == 0x1019:
                                R = cc[nk.imm][1]
                                ctx = 'after a class name'
                            elif ck in (0x101B, 0x101A):
                                e = cc[nk.imm]
                                note('field' if ck == 0x101B else 'method', ctx,
                                     b.name, R, e[1], e[2], e[3], where)
                                R = type_class(e[3]) if ck == 0x101B else None
                                ctx = 'after a field'
                            elif ck == 0x4012:
                                R = None
                                ctx = 'after an unresolved name'
                        i = j + cnt
                        continue
                    if c in (0x101B, 0x101A):
                        e = cc[nd.imm]
                        note('field' if c == 0x101B else 'method', 'bare',
                             b.name, b.name, e[1], e[2], e[3], where)
                    i += 1
    lines = []
    out = lines.append
    out('class table: %d class names (%d of them from sources without a class file, %d defined by more than one blob)' % (len(table), nsrc, sum(1 for v in table.values() if len(v) > 1)))
    out('')
    out('member ref nodes: X = class named by the ref, R = class the lookup starts from, D = declaring class')
    out('   %-7s %-22s %-9s %-6s %-26s %8s' % ('kind', 'position', 'member', 'R / D', 'X', 'nodes'))
    for key in sorted(tab):
        if len(key) == 4:
            out('   %-7s %-22s %-43s X=R:%-5s %8d   e.g. %s' % (key[0], key[1], key[2], key[3], tab[key], ex[key][:150]))
        else:
            out('   %-7s %-22s %-9s %-6s %-26s %8d   e.g. %s' % (key + (tab[key], ex[key][:150])))
    out('')
    out('members declared above R: number of superclass steps from R to D')
    for key in sorted(deep, key=lambda k: (k[0], k[1], k[2], k[3] or 0)):
        out('   %-7s %-9s %-22s %s step%s  %d' % (key[0], key[1], key[2], key[3], '' if key[3] == 1 else 's', deep[key]))
    out('')
    out('members of array receivers:')
    for key in sorted(arr):
        out('   %-7s %s:%s  X=R:%s  %d' % (key[0], key[1], key[2], key[3], arr[key]))
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'study_refclass.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
