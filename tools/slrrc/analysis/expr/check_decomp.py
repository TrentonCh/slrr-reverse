#!/usr/bin/env python3
"""check_decomp.py - the expression rules checked on EVERY blob, source or not.

check_expr.py proves the rules where a source exists (2,804 consistent pairs).
This script covers the rest of the corpus from the class side alone: all
2,993 blobs, among them the 102 class files without a source (the runtime
library) and the 15 stale pairs.

1. Shape.  decomp.decompile() accepts exactly the node shapes of
   notes/expressions.md.  Every tree that it rejects is listed: these are the
   node patterns in expression position that the rules do not explain.

2. Resolution, read backwards.  For every decompiled expression the choices
   the compiler made are re-derived from the class database:
   R1  a lone field ref names a field of the class chain; its class is the
       declaring class for a static field, the class itself for an instance one
   R2  path segments: each field ref is found in the class of the segment
       before it, with the same rule for the class named
   R3  no field path holds a by-name segment
   R4  the receiver of getfield / call_on is never a bare local (other than
       this), a lone field ref or a path: those forms would be paths
   R5  a call that ends in a method ref: every argument is a literal, a
       local, a lone field ref or this; the ref names the receiver class; the
       model's method lookup finds the same descriptor
   R6  a call that ends in a name: some argument is of another form, or the
       lookup finds nothing
   R7  the argument count box equals the number of arguments (by construction
       of the decompiler) and, for a method ref, the descriptor's parameter count

    python -I check_decomp.py [--show N] [--json FILE]
"""
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle     # noqa: E402
import tufa       # noqa: E402
import classdb    # noqa: E402
import decomp     # noqa: E402
import gen        # noqa: E402
import symbolic   # noqa: E402

STRING = 'Ljava.lang.String;'
LIT_TYPE = {'int': 'I', 'float': 'F', 'str': STRING, 'rid': 'I', 'null': 'null', 'bool': 'I'}


class TreeCtx(object):
    """Types of the locals of one tree, by node position."""

    def __init__(self, blob, owner, sym):
        self.cls = blob.name
        self.static = owner[3]
        self.base = {}
        if owner[0] == 'method':
            params = classdb.split_desc(owner[2])[0]
            k = 0
            if not self.static:
                self.base[0] = 'L%s;' % self.cls
                k = 1
            for j, p in enumerate(reversed(params)):
                self.base[k + j] = p
        elif not self.static:
            self.base[0] = 'L%s;' % self.cls
        self.events = defaultdict(list)       # local index -> [(node, desc)]
        for i, (mn, op) in enumerate(sym):
            if mn == 'declare' and i + 1 < len(sym) and sym[i + 1][0] == 'type':
                self.events[op].append((i, sym[i + 1][1]))

    def local_type(self, idx, at):
        t = None
        for pos, desc in self.events.get(idx, ()):
            if pos <= at:
                t = desc
        if t is None:
            t = self.base.get(idx)
        return t

    def is_this(self, idx):
        return idx == 0 and not self.static


def main(argv):
    show = 0
    out_json = None
    if '--show' in argv:
        show = int(argv[argv.index('--show') + 1])
    if '--json' in argv:
        out_json = argv[argv.index('--json') + 1]
    db = classdb.load()
    model = gen.Model(db)
    cat_of = {}
    for e in oracle.iter_entries():
        if e.cls:
            cat_of[e.cls] = (e.category, e.consistent)
    st = Counter()
    unattr = []
    rule = defaultdict(Counter)          # rule -> Counter(ok / fail)
    fails = defaultdict(list)
    shapes = Counter()
    stmt_kinds = Counter()
    recv_forms = Counter()

    def note(r, ok, where, detail):
        rule[r]['ok' if ok else 'fail'] += 1
        if not ok and len(fails[r]) < 40:
            fails[r].append('%s: %s' % (where, detail))

    for rel, blobs in oracle.iter_class_files():
        cat, cons = cat_of.get(rel, ('?', None))
        grp = cat if cons is not False else 'stale pair'
        for bi, b in enumerate(blobs):
            # the blob itself is the truth about its own class (nine dialog
            # classes exist in two versions, see study_duplicates.py)
            saved = db.classes.get(b.name)
            mine = classdb.info_from_blob(b)
            mine.file = rel
            db.classes[b.name] = mine
            model._rows.pop(b.name, None)
            own = {}
            for stt, i, f in b.fields():
                if f.init != tufa.NONE:
                    own[f.init] = ('field', b.utf8(f.name), b.utf8(f.type), stt)
            for stt, i, m in b.methods():
                if m.tree != tufa.NONE:
                    own[m.tree] = ('method', b.utf8(m.name), b.utf8(m.desc), stt)
            st['blobs'] += 1
            st['blobs ' + grp] += 1
            for ti, nodes in enumerate(b.trees):
                st['trees'] += 1
                st['nodes'] += len(nodes)
                sym = symbolic.sym_nodes(b, nodes)
                where = '%s#%d tree %d' % (rel, bi, ti)
                try:
                    stmts = decomp.decompile(b, nodes, sym)
                except decomp.Unattributed as x:
                    st['trees rejected'] += 1
                    unattr.append((rel, bi, ti, own.get(ti), x.at,
                                   nodes[min(x.at, len(nodes) - 1)].line if nodes else 0,
                                   x.msg, grp))
                    continue
                st['trees accepted'] += 1
                st['trees accepted ' + grp] += 1
                ctx = TreeCtx(b, own[ti], sym)
                for s in stmts:
                    stmt_kinds[s.kind] += 1
                    if s.e is None:
                        continue
                    if s.kind == 'expr':
                        shapes['statement: %s %s' % (s.e.k, s.e.a if s.e.k == 'op' else '')] += 1
                    for e in decomp.walk(s.e):
                        st['expressions'] += 1
                        check(db, model, ctx, e, where, note, recv_forms, shapes)
            if saved is not None:
                db.classes[b.name] = saved
            model._rows.pop(b.name, None)
    # ---- report --------------------------------------------------------
    print('blobs %d, trees %d, nodes %d' % (st['blobs'], st['trees'], st['nodes']))
    for k in sorted(st):
        if k.startswith('blobs ') or k.startswith('trees accepted '):
            print('  %-40s %d' % (k, st[k]))
    print('trees accepted by the shape grammar: %d of %d' % (st['trees accepted'], st['trees']))
    print('expressions (sub-expressions included): %d' % st['expressions'])
    print('rejected trees (unattributed patterns): %d' % len(unattr))
    for u in unattr:
        print('  %s#%d tree %d %s node %d line %d: %s  [%s]' % (
            u[0], u[1], u[2], '%s %s' % (u[3][0], u[3][1]) if u[3] else '?', u[4], u[5], u[6], u[7]))
    print('rules read backwards (holds / fails):')
    for r in sorted(rule):
        print('  %-4s %8d %6d' % (r, rule[r]['ok'], rule[r]['fail']))
        for x in fails[r][:show]:
            print('         ' + x[:260])
    print('receiver forms of getfield / call_on:')
    for k, v in recv_forms.most_common():
        print('  %7d  %s' % (v, k))
    print('statement kinds:', dict(stmt_kinds))
    print('expression statements by root:')
    for k, v in sorted(shapes.items(), key=lambda kv: -kv[1]):
        if k.startswith('statement'):
            print('  %7d  %s' % (v, k))
    if out_json:
        with open(out_json, 'w', encoding='utf-8') as f:
            json.dump({'counts': dict(st),
                       'unattributed': [list(u[:3]) + [list(u[3]) if u[3] else None] + list(u[4:])
                                        for u in unattr],
                       'rules': {r: dict(c) for r, c in rule.items()},
                       'fails': dict(fails),
                       'receiver_forms': dict(recv_forms),
                       'statement_kinds': dict(stmt_kinds),
                       'shapes': dict(shapes)}, f, indent=1)
    return 0


def type_of(db, ctx, e):
    """(type desc or None, typed-form?) of a decompiled expression as the
    compiler's argument typing sees it."""
    if e.k == 'lit':
        return LIT_TYPE[e.a[0]], True
    if e.k == 'local':
        return ctx.local_type(e.a, e.lo), True
    if e.k == 'field':
        return e.a[2], True
    return None, False


def field_ref_ok(db, cls, ref):
    """Is `ref` what the rules give for member ref[1] looked up on class cls?
    Returns (ok, detail, type)."""
    c, n, d = ref
    if cls is None:
        return False, 'receiver class unknown', d
    if cls.startswith('['):
        ok = n == 'length' and d == 'I' and c == cls
        return ok, 'array member %s.%s:%s on %s' % (c, n, d, cls), d
    f = db.find_field(cls, n)
    if f is None:
        return False, 'no field %s in %s' % (n, cls), d
    want = f.owner if f.static else cls
    if f.desc != d:
        return False, 'type %s, database says %s' % (d, f.desc), d
    if c != want:
        return False, 'names %s, rule says %s (%s field)' % (
            c, want, 'static' if f.static else 'instance'), d
    return True, '', d


def check(db, model, ctx, e, where, note, recv_forms, shapes):
    k = e.k
    at = '%s node %d' % (where, e.hi)
    if k == 'field':
        ok, detail, _ = field_ref_ok(db, ctx.cls, e.a)
        note('R1', ok, at, detail)
    elif k == 'path':
        cls, t = path_walk(db, ctx, e.a, e.lo, at, note)
    elif k in ('getfield', 'call_on'):
        recv = e.x[-1] if k == 'call_on' else e.x[0]
        form = recv.k
        if recv.k == 'local':
            form = 'this' if ctx.is_this(recv.a) else 'local (not this)'
        elif recv.k == 'op':
            form = 'op ' + recv.a
        recv_forms['%s <- %s' % (k, form)] += 1
        bad = recv.k in ('field', 'path') or (recv.k == 'local' and not ctx.is_this(recv.a))
        note('R4', not bad, at, '%s on %s' % (k, form))
    elif k == 'call':
        segs, cal = e.a
        if segs:
            cls, t = path_walk(db, ctx, segs, e.lo, at, note)
            if t is not None:
                cls = gen.Emitter.class_of_desc(t)
        else:
            cls = ctx.cls
        types = []
        typed = True
        for a in e.x:
            t, ty = type_of(db, ctx, a)
            types.append(t)
            typed = typed and ty
        if cal[0] == 'method':
            c, n, d = cal[1]
            note('R7', len(classdb.split_desc(d)[0]) == len(e.x), at, 'argument count')
            if not typed:
                note('R5', False, at, 'method ref %s.%s%s with an argument of another form'
                     % (c, n, d))
            elif c != cls:
                note('R5', False, at, 'ref names %s, receiver class is %s' % (c, cls))
            else:
                m = model.find_method(cls, n, types) if cls and not cls.startswith('[') else None
                if m is None:
                    note('R5', False, at, 'lookup finds nothing for %s.%s%r (class has %s)' % (
                        cls, n, tuple(types), d))
                elif m.desc != d:
                    note('R5', False, at, 'lookup gives %s, class has %s for %s.%s%r' % (
                        m.desc, d, cls, n, tuple(types)))
                else:
                    note('R5', True, at, '')
        else:
            n = cal[1]
            if not typed:
                note('R6', True, at, '')
            else:
                m = model.find_method(cls, n, types) if cls and not cls.startswith('[') else None
                note('R6', m is None, at, 'by name although the lookup finds %s.%s%s for %r' % (
                    cls, n, m.desc if m else '', tuple(types)))


def path_walk(db, ctx, segs, lo, at, note):
    """Check the segments of a field path or of the receiver part of a call
    path.  Returns (class context, type of the last segment)."""
    mn, op = segs[0]
    cls = None
    t = None
    if mn == 'local':
        t = ctx.local_type(op, lo)
    elif mn == 'field':
        ok, detail, t = field_ref_ok(db, ctx.cls, op)
        note('R2', ok, at, 'head: ' + detail)
    elif mn == 'class':
        cls = op
    for mn, op in segs[1:]:
        cur = cls if cls is not None else gen.Emitter.class_of_desc(t)
        if mn == 'name':
            note('R3', False, at, 'by-name segment %s in a path' % op)
            t, cls = None, None
            continue
        ok, detail, t = field_ref_ok(db, cur, op)
        note('R2', ok, at, detail)
        note('R3', True, at, '')
        cls = None
    return cls, t


if __name__ == '__main__':
    sys.exit(main(sys.argv))
