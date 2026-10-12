#!/usr/bin/env python3
"""poolmodel.py - the model of how the compiler builds CONS, CLSS, FILD and
MTHD of a class from its source.

This is the executable form of notes/declarations.md.  Input: the parsed
source (srcparse), the class universe (universe) and, as an oracle for the
two things this model does not derive itself (symbol resolution and code
emission), the TREE chunk of the shipped class: which class / member / string
/ resource id every node names, in node order.  The pool indices of the
shipped class are never used; they are what the model predicts.

    m = poolmodel.Model(unit, class_decl, blob, universe)
    m.run()
    m.pool.entries          predicted pool, content form, in order
    m.pool.phase            per entry: the phase and rule that interned it
    m.member_rows()         predicted FILD / MTHD rows and CLSS record
    poolmodel.content(blob) the pool of a shipped blob in content form
    poolmodel.to_indexed(entries)   content form -> tufa.Blob.cons

Content form of a pool entry (index free, so that pools compare by value):
    ('utf8', text)  ('class', name)  ('nat', name, desc)
    ('ref', class name, member name, desc)  ('rid', pack file name, local id)

THE MODEL (each step interns into one pool; an entry that exists is reused)

  1 seed     class name utf8 + class entry; superclass utf8 + class entry;
             per interface utf8 + class entry
  2 types    while the class body is parsed, type descriptors as utf8, in the
             order the parser reduces the constructs that carry them:
               field           its type
               parameter       its type, left to right
               method          its return type, AFTER the parameters
               new T(args)     "LT;" BEFORE the arguments
               new T[n]        "[LT;" / "[I" / "[F"
               (int)x (float)x "I" / "F"       (a cast to a class: nothing)
               x instanceof T  "LT;" after x
               T a = e, b = f; the type AFTER the initialisers
             at the end of the class body: "LI;" per implemented interface,
             then "LC;" of the class itself when it is not the first class
             of its source file
  3 names    members in source order: field -> name; method -> descriptor,
             then name ("<init>" for a constructor)
  4 resolve  the trees of the static methods, the instance methods, the
             static field initialisers, the instance field initialisers
             (the four row vectors, each in source order), node by node:
               class node       utf8 + class entry
               member ref       class (utf8, class), nat (name utf8,
                                descriptor utf8, nat), ref
               path segment after a receiver of static class type T
                                the class entry of T first, also when the
                                segment stays an unresolved name
               this             utf8 "LC;" of the own class
  5 emit     all trees in tree order (= source order), node by node: the
             utf8 of string literals, of unresolved names (name, getfield,
             call_on, call_super) and, per resource id literal, the utf8 of
             the pack file name + the rid entry

Options (DEFAULT_OPTS) switch single rules to an alternative; they exist so
that check_alternatives.py can show how many classes each rule is pinned by.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

import tufa       # noqa: E402
import srcparse   # noqa: E402

NONE = tufa.NONE
FLAG = {'public': 0x1, 'protected': 0x2, 'private': 0x4, 'static': 0x8,
        'abstract': 0x10, 'final': 0x20, 'native': 0x40, 'synchronized': 0x80}

OP_LOCAL, OP_DECLARE, OP_TYPE, OP_STR = 0x1001, 0x1002, 0x1006, 0x4009
OP_PATH, OP_NAME, OP_RID, OP_CLASS = 0x1011, 0x4012, 0x100E, 0x1019
OP_METHOD, OP_FIELD, OP_GETFIELD = 0x101A, 0x101B, 0x401C
OP_CALL, OP_CALL_ON, OP_CALL_SUPER = 0x0024, 0x4025, 0x4026

DEFAULT_OPTS = {
    # ---- step 1
    'seed': 'name,super,ifaces',
    # ---- step 2
    'ret': 'after',            # return type: before / after the parameters, end (after the body)
    'field': 'pre',            # field type before / after its initialiser
    'local': 'post',           # local type: pre, each (after each declarator), post
    'new': 'pre',              # type of "new T(args)" before / after the arguments
    'newarray': 'pre',         # type of "new T[n]" before / after n
    'cast': 'pre',             # type of "(int)x" before / after x
    'cast_class_type': False,  # does "(T)x" intern "LT;"
    'cast_class_entry': 'resolve',  # class entry of "(T)x": in step 4, or while parsing
    'instanceof': 'post',      # type of "x instanceof T" before / after x
    'param_types': True,       # are parameter types interned while parsing
    'iface_types': True,       # "LI;" per interface at the end of the body
    'own_type': 'nonfirst',    # "LC;" at the end of the body: nonfirst, never, always
    'class_end': 'ifaces,own',  # order of the two at the end of the body
    # ---- step 3
    'names': 'source',         # source: members in source order; vectors: sf,if,sm,im
    'method_name': 'desc,name',
    # ---- step 4
    'ref_order': 'sm,im,sf,if',
    'this_type': True,         # "this" interns "L<own class>;"
    'touch': 'all',            # receiver class before: all segments, name only, member only, none
    'touch_array': False,      # ... also for a receiver of array type
    'ref_inner': 'class,nat',  # order inside a member ref
    'nat_inner': 'name,desc',
    # ---- step 5
    'emit_order': 'tree',      # tree: tree index order; vectors: as step 4
    'emit_types': False,       # (control) let step 5 intern type carriers too
    'merge_45': False,         # (control) one pass doing steps 4 and 5 per tree
}


def content(blob):
    """The pool of a blob in content form."""
    out = []
    c = blob.cons
    for e in c:
        k = e[0]
        if k == 'utf8':
            out.append(('utf8', e[1]))
        elif k == 'class':
            out.append(('class', c[e[1]][1]))
        elif k == 'nat':
            out.append(('nat', c[e[1]][1], c[e[2]][1]))
        elif k == 'ref':
            n = c[e[2]]
            out.append(('ref', c[c[e[1]][1]][1], c[n[1]][1], c[n[2]][1]))
        elif k == 'rid':
            out.append(('rid', c[e[1]][1], e[2]))
    return out


def to_indexed(entries):
    """Content-form entries -> tufa.Blob.cons tuples (indices resolved)."""
    idx = {e: i for i, e in enumerate(entries)}
    out = []
    for e in entries:
        k = e[0]
        if k == 'utf8':
            out.append(('utf8', e[1]))
        elif k == 'class':
            out.append(('class', idx[('utf8', e[1])]))
        elif k == 'nat':
            out.append(('nat', idx[('utf8', e[1])], idx[('utf8', e[2])]))
        elif k == 'ref':
            out.append(('ref', idx[('class', e[1])], idx[('nat', e[2], e[3])]))
        elif k == 'rid':
            out.append(('rid', idx[('utf8', e[1])], e[2]))
    return out


def split_params(desc):
    """'(ILjava.lang.String;[F)V' -> ['I', 'Ljava.lang.String;', '[F']."""
    out = []
    i = 1
    n = len(desc)
    while i < n and desc[i] != ')':
        j = i
        while desc[j] == '[':
            j += 1
        if desc[j] == 'L':
            j = desc.index(';', j)
        out.append(desc[i:j + 1])
        i = j + 1
    return out


class Pool(object):
    """An interning pool that records which phase and rule added each entry."""

    def __init__(self, opts=None):
        self.entries = []
        self.index = {}
        self.phase = []
        self.label = 'seed'
        o = opts or DEFAULT_OPTS
        self.class_first = o['ref_inner'] == 'class,nat'
        self.name_first = o['nat_inner'] == 'name,desc'

    def preload(self, entries, label='given'):
        for e in entries:
            self.index[e] = len(self.entries)
            self.entries.append(e)
            self.phase.append(label)

    def _add(self, e):
        i = self.index.get(e)
        if i is None:
            i = len(self.entries)
            self.index[e] = i
            self.entries.append(e)
            self.phase.append(self.label)
        return i

    def utf8(self, s):
        return self._add(('utf8', s))

    def cls(self, name):
        self.utf8(name)
        return self._add(('class', name))

    def nat(self, name, desc):
        if self.name_first:
            self.utf8(name)
            self.utf8(desc)
        else:
            self.utf8(desc)
            self.utf8(name)
        return self._add(('nat', name, desc))

    def ref(self, cname, name, desc):
        if self.class_first:
            self.cls(cname)
            self.nat(name, desc)
        else:
            self.nat(name, desc)
            self.cls(cname)
        return self._add(('ref', cname, name, desc))

    def rid(self, pack, local):
        self.utf8(pack)
        return self._add(('rid', pack, local))


class Model(object):
    """The model for one class.  blob may be None (steps 1 to 3 only: what
    can be said about a source that has no class file)."""

    def __init__(self, unit, cdecl, blob, uni, opts=None):
        self.unit = unit
        self.c = cdecl
        self.blob = blob
        self.uni = uni
        self.opts = dict(DEFAULT_OPTS)
        if opts:
            self.opts.update(opts)
        self.pool = Pool(self.opts)
        self.unresolved = []
        self.ambiguous = []        # (simple name, packages that hold it)
        self.scope_used = []       # (simple name, scope that resolved it)
        self.stats = {}
        if unit is not None:
            self.package = unit.package
            self.imports = [i.name for i in unit.imports
                            if i.star and not i.get('odd')]
            self.local_classes = [c.name for c in unit.classes]
            self.fq = (self.package + '.' if self.package else '') + cdecl.name
            self.first_in_file = cdecl is unit.classes[0]
        else:
            self.fq = blob.name
        self.cons_content = content(blob) if blob is not None else None

    @classmethod
    def from_blob(cls, blob, opts=None):
        """A model without a source: only steps 4 and 5 can run (run_tail)."""
        return cls(None, None, blob, None, opts)

    def count(self, key, n=1):
        self.stats[key] = self.stats.get(key, 0) + n

    # ---- names and descriptors --------------------------------------------
    def resolve(self, simple):
        """Fully qualified name of a class named in the source: a dotted
        name stands for itself; a simple name is looked for among the
        classes of the same source, in the own package, in the on-demand
        imports in source order, in java.lang."""
        if '.' in simple:
            return simple
        pre = self.package + '.' if self.package else ''
        if simple in self.local_classes:
            self.scope_used.append((simple, 'same source'))
            return pre + simple
        scopes = [('own package', self.package)] + \
                 [('import', i) for i in self.imports] + \
                 [('java.lang (implicit)', 'java.lang')]
        hits = []
        for label, pkg in scopes:
            if pkg is not None and simple in self.uni.packages.get(pkg, ()):
                if not any(h[1] == pkg for h in hits):
                    hits.append((label, pkg))
        if hits:
            self.scope_used.append((simple, hits[0][0]))
            if len(hits) > 1:
                # the search order is decisive only here
                self.ambiguous.append((simple, [h[1] for h in hits]))
            return hits[0][1] + '.' + simple
        self.unresolved.append(simple)
        return '?' + simple

    def class_name_census(self):
        """Every class name the source of this class writes, once per
        occurrence: (simple name, scope that resolves it, number of visible
        packages that hold it).  Occurrences are the names behind extends and
        implements and every type that names a class (fields, parameters,
        return types, locals, new, array creation, casts, instanceof)."""
        names = []
        if self.c.extends is not None:
            names.append(self.c.extends)
        names.extend(self.c.implements)
        for m in self.c.members:
            for x in srcparse.walk(m):
                if x.k == 'type' and x.base not in ('int', 'float', 'void'):
                    names.append(x.base)
        out = []
        for n in names:
            mark = (len(self.scope_used), len(self.ambiguous), len(self.unresolved))
            self.resolve(n)
            scope = self.scope_used[mark[0]][1] if len(self.scope_used) > mark[0] \
                else 'unresolved'
            amb = len(self.ambiguous[mark[1]][1]) if len(self.ambiguous) > mark[1] else 1
            if len(self.unresolved) > mark[2]:
                amb = 0
            out.append((n, scope, amb))
        # the census must not leave traces in the counters of a model run
        del self.scope_used[:]
        del self.ambiguous[:]
        return out

    def desc(self, ty, extra_dims=0):
        """Descriptor of a type node: I, F, V, Lpkg.Name; with one "[" per
        array dimension in front."""
        b = ty.base
        if b == 'int':
            s = 'I'
        elif b == 'float':
            s = 'F'
        elif b == 'void':
            s = 'V'
        else:
            s = 'L' + self.resolve(b) + ';'
        return '[' * (ty.dims + extra_dims) + s

    def method_desc(self, m):
        """"(" parameter descriptors ")" return descriptor; a constructor
        has no return part at all."""
        s = '(' + ''.join(self.desc(p.type, p.dims) for p in m.params) + ')'
        if m.k != 'ctor':
            s += self.desc(m.ret)
        return s

    def super_name(self):
        c = self.c
        if c.kind == 'interface':
            return None
        if c.extends is None:
            return None if self.fq == 'java.lang.Object' else 'java.lang.Object'
        return self.resolve(c.extends)

    def interface_names(self):
        return [self.resolve(i) for i in self.c.implements]

    # ---- the run -------------------------------------------------------------
    def run(self):
        self.phase_seed()
        self.phase_types()
        self.phase_names()
        if self.blob is not None:
            if self.opts['merge_45']:
                self.phase_merged()
            else:
                self.phase_refs()
                self.phase_emit()
        return self

    def run_tail(self, prefix_len):
        """Source-free run: take the first prefix_len entries of the shipped
        pool as given and predict the rest (steps 4 and 5)."""
        self.pool.preload(self.cons_content[:prefix_len])
        self.phase_refs()
        self.phase_emit()
        return self

    # ---- step 1 ----------------------------------------------------------------
    def phase_seed(self):
        p = self.pool
        for part in self.opts['seed'].split(','):
            if part == 'name':
                p.label = 'seed:name'
                p.cls(self.fq)
            elif part == 'super':
                p.label = 'seed:super'
                sup = self.super_name()
                if sup is not None:
                    p.cls(sup)
            elif part == 'ifaces':
                p.label = 'seed:interface'
                for i in self.interface_names():
                    p.cls(i)

    # ---- step 2 ----------------------------------------------------------------
    def type_events(self):
        """(rule, type string) in the order the parser interns them."""
        o = self.opts
        for m in self.c.members:
            if m.k == 'field':
                if o['field'] == 'pre':
                    yield 'field', self.desc(m.type, m.dims)
                if m.init is not None:
                    for ev in self.body_types(m.init):
                        yield ev
                if o['field'] == 'post':
                    yield 'field', self.desc(m.type, m.dims)
                continue
            if m.k == 'method' and o['ret'] == 'before':
                yield 'return', self.desc(m.ret)
            if o['param_types']:
                for prm in m.params:
                    yield 'param', self.desc(prm.type, prm.dims)
            if m.k == 'method' and o['ret'] == 'after':
                yield 'return', self.desc(m.ret)
            if m.body is not None:
                for ev in self.body_types(m.body):
                    yield ev
            if m.k == 'method' and o['ret'] == 'end':
                yield 'return', self.desc(m.ret)
        # end of the class body
        for part in o['class_end'].split(','):
            if part == 'ifaces' and o['iface_types']:
                for i in self.interface_names():
                    yield 'interface', 'L' + i + ';'
            elif part == 'own':
                if o['own_type'] == 'always' or (
                        o['own_type'] == 'nonfirst' and not self.first_in_file):
                    yield 'own', 'L' + self.fq + ';'

    def body_types(self, x):
        """(rule, type string) interned while the parser reduces x."""
        o = self.opts
        k = x.k
        if k == 'local':
            if o['local'] == 'pre':
                yield 'local', self.desc(x.type)
            for d in x.decls:
                if d.init is not None:
                    for ev in self.body_types(d.init):
                        yield ev
                if o['local'] == 'each':
                    yield 'local', self.desc(x.type, d.dims)
            if o['local'] == 'post':
                yield 'local', self.desc(x.type)
            return
        if k == 'new':
            if o['new'] == 'pre':
                yield 'new', self.desc(x.type)
            for a in x.args:
                for ev in self.body_types(a):
                    yield ev
            if o['new'] == 'post':
                yield 'new', self.desc(x.type)
            return
        if k == 'newarray':
            if o['newarray'] == 'pre':
                yield 'newarray', self.desc(x.type)
            for ev in self.body_types(x.size):
                yield ev
            if o['newarray'] == 'post':
                yield 'newarray', self.desc(x.type)
            return
        if k == 'cast':
            prim = x.type.base in ('int', 'float')
            want = prim or o['cast_class_type']
            if want and o['cast'] == 'pre':
                yield 'cast', self.desc(x.type)
            if not prim and o['cast_class_entry'] == 'parse' and not x.type.dims:
                # (control) the class entry of a cast interned while parsing
                yield 'cast class entry', self.resolve(x.type.base)
            for ev in self.body_types(x.e):
                yield ev
            if want and o['cast'] == 'post':
                yield 'cast', self.desc(x.type)
            return
        if k == 'instanceof':
            if o['instanceof'] == 'pre':
                yield 'instanceof', self.desc(x.type)
            for ev in self.body_types(x.e):
                yield ev
            if o['instanceof'] == 'post':
                yield 'instanceof', self.desc(x.type)
            return
        for c in sorted(srcparse.children(x), key=lambda n: n.tok):
            for ev in self.body_types(c):
                yield ev

    def phase_types(self):
        p = self.pool
        for rule, s in self.type_events():
            p.label = 'types:' + rule
            if rule == 'cast class entry':
                p.cls(s)
            else:
                p.utf8(s)

    # ---- step 3 ----------------------------------------------------------------
    def phase_names(self):
        p = self.pool
        members = list(self.c.members)
        if self.opts['names'] == 'vectors':
            st = lambda m: 'static' in m.mods
            members = [m for m in members if m.k == 'field' and st(m)] + \
                      [m for m in members if m.k == 'field' and not st(m)] + \
                      [m for m in members if m.k != 'field' and st(m)] + \
                      [m for m in members if m.k != 'field' and not st(m)]
        for m in members:
            if m.k == 'field':
                p.label = 'names:field'
                p.utf8(m.name)
            elif self.opts['method_name'] == 'desc,name':
                p.label = 'names:descriptor'
                p.utf8(self.method_desc(m))
                p.label = 'names:method'
                p.utf8(m.name)
            else:
                p.label = 'names:method'
                p.utf8(m.name)
                p.label = 'names:descriptor'
                p.utf8(self.method_desc(m))

    # ---- step 4 ----------------------------------------------------------------
    def tree_groups(self):
        b = self.blob
        sm = [m.tree for m in b.static_methods if m.tree != NONE]
        im = [m.tree for m in b.instance_methods if m.tree != NONE]
        sf = [f.init for f in b.static_fields if f.init != NONE]
        if_ = [f.init for f in b.instance_fields if f.init != NONE]
        return {'sm': sm, 'im': im, 'sf': sf, 'if': if_,
                'm': sorted(sm + im), 'f': sorted(sf + if_),
                'all': sorted(sm + im + sf + if_)}

    def tree_order(self, spec):
        g = self.tree_groups()
        out = []
        for name in spec.split(','):
            out += g[name]
        return out

    def tree_owner_rows(self):
        """tree index -> (is_static, method row or None)."""
        b = self.blob
        own = {}
        for st, i, f in b.fields():
            if f.init != NONE:
                own[f.init] = (st, None)
        for st, i, m in b.methods():
            if m.tree != NONE:
                own[m.tree] = (st, m)
        return own

    def initial_locals(self, is_static, mrow):
        """local index -> type descriptor at entry of a tree: this (instance
        code only), then the parameters in reverse order."""
        loc = {}
        base = 0
        if not is_static:
            loc[0] = 'this'
            base = 1
        if mrow is not None:
            params = split_params(self.blob.utf8(mrow.desc))
            n = len(params)
            for k in range(n):
                loc[base + k] = params[n - 1 - k]
        return loc

    def phase_refs(self):
        own = self.tree_owner_rows()
        for t in self.tree_order(self.opts['ref_order']):
            st, mrow = own[t]
            self.resolve_tree(t, self.blob.trees[t], self.initial_locals(st, mrow))

    def touch_class_of(self, T, why):
        """The resolver is about to look a member up on a receiver of static
        type T: it interns the class entry of T first."""
        if T is None:
            return
        o = self.opts['touch']
        if o == 'none' or (o == 'name' and why != 'name') or \
                (o == 'member' and why == 'name'):
            return
        p = self.pool
        before = len(p.entries)
        p.label = 'resolve:receiver class (%s)' % why
        if T.startswith('L') and T.endswith(';'):
            p.cls(T[1:-1])
        elif T.startswith('[') and self.opts['touch_array']:
            e = T[1:]
            if e.startswith('L') and e.endswith(';'):
                e = e[1:-1]
            p.cls('[' + e)
        self.count('touch %s' % why)
        if len(p.entries) != before:
            self.count('touch %s interned a class' % why)

    def this_type(self):
        if self.opts['this_type']:
            self.pool.label = 'resolve:this'
            self.pool.utf8('L' + self.fq + ';')
            self.count('this')

    def resolve_tree(self, t, nodes, loc, emit=False):
        """Walk one tree the way the resolution pass does and intern what it
        interns.  With emit=True the strings of step 5 are interned in the
        same walk (control experiment 'merge_45')."""
        p = self.pool
        cc = self.cons_content
        n = len(nodes)
        code = [tufa.OP_FLAGS[x.op] for x in nodes]
        i = 0
        while i < n:
            c = code[i]
            nd = nodes[i]
            if emit:
                self.emit_node(c, nd)
            if c == OP_DECLARE:                    # declare k ; type T
                if i + 1 < n and code[i + 1] == OP_TYPE:
                    loc[nd.imm] = cc[nodes[i + 1].imm][1]
                i += 1
                continue
            if c == OP_PATH or c == OP_CALL:
                # a field path, or a call followed by its path
                j = i + 1
                if c == OP_CALL:
                    if j < n and code[j] == OP_METHOD:
                        e = cc[nodes[j].imm]
                        p.label = 'resolve:member ref'
                        p.ref(e[1], e[2], e[3])
                        self.count('lone method ref')
                        i = j + 1
                        continue
                    if j < n and code[j] == OP_PATH:
                        cnt = nodes[j].imm
                        j += 1
                    else:
                        # irregular: call followed by something else
                        self.count('irregular call')
                        i += 1
                        continue
                else:
                    cnt = nd.imm
                T = None           # static type of the walk so far
                unknown = False    # a segment stayed an unresolved name
                for k in range(j, min(n, j + cnt)):
                    ck = code[k]
                    nk = nodes[k]
                    first = k == j
                    if emit:
                        self.emit_node(ck, nk)
                    if ck == OP_LOCAL:
                        T = loc.get(nk.imm)
                        if T == 'this':
                            T = 'L' + self.fq + ';'
                            self.this_type()
                            self.count('this as first segment')
                    elif ck == OP_CLASS:
                        p.label = 'resolve:class'
                        p.cls(cc[nk.imm][1])
                        T = None
                    elif ck == OP_FIELD:
                        if unknown:
                            self.count('member ref after an unresolved name')
                        if not first:
                            self.touch_class_of(T, 'field')
                        e = cc[nk.imm]
                        p.label = 'resolve:member ref'
                        p.ref(e[1], e[2], e[3])
                        T = e[3]
                    elif ck == OP_METHOD:
                        if unknown:
                            self.count('member ref after an unresolved name')
                        if not first:
                            self.touch_class_of(T, 'method')
                        e = cc[nk.imm]
                        p.label = 'resolve:member ref'
                        p.ref(e[1], e[2], e[3])
                        T = None
                    elif ck == OP_NAME:
                        self.count('name segments')
                        if not (c == OP_CALL and k == j + cnt - 1):
                            self.count('name segment that is not the last of a call path')
                        if not first:
                            self.touch_class_of(T, 'name')
                        T = None
                        unknown = True
                    else:
                        self.count('irregular segment 0x%04X' % ck)
                i = j + cnt
                continue
            if c == OP_CLASS:
                p.label = 'resolve:class'
                p.cls(cc[nd.imm][1])
            elif c in (OP_METHOD, OP_FIELD):
                e = cc[nd.imm]
                p.label = 'resolve:member ref'
                p.ref(e[1], e[2], e[3])
            elif c == OP_LOCAL and loc.get(nd.imm) == 'this':
                self.this_type()
            i += 1

    # ---- step 5 ----------------------------------------------------------------
    def emit_node(self, code, nd):
        p = self.pool
        cc = self.cons_content
        if code == OP_STR:
            p.label = 'emit:string literal'
            p.utf8(cc[nd.imm][1])
        elif code in (OP_NAME, OP_GETFIELD, OP_CALL_ON, OP_CALL_SUPER):
            p.label = 'emit:unresolved name'
            p.utf8(cc[nd.imm][1])
        elif code == OP_RID:
            e = cc[nd.imm]
            p.label = 'emit:resource id'
            p.rid(e[1], e[2])
        elif code == OP_TYPE:
            if self.opts['emit_types']:
                p.label = 'emit:type carrier'
                p.utf8(cc[nd.imm][1])
            elif ('utf8', cc[nd.imm][1]) not in p.index:
                # a type carrier whose string step 2 did not predict
                self.count('type carrier not interned by step 2')
                p.label = 'emit:type carrier (MISSED BY STEP 2)'
                p.utf8(cc[nd.imm][1])

    def phase_emit(self):
        b = self.blob
        if self.opts['emit_order'] == 'tree':
            order = range(len(b.trees))
        else:
            order = self.tree_order(self.opts['ref_order'])
        for t in order:
            for nd in b.trees[t]:
                self.emit_node(tufa.OP_FLAGS[nd.op], nd)

    def phase_merged(self):
        own = self.tree_owner_rows()
        for t in self.tree_order(self.opts['ref_order']):
            st, mrow = own[t]
            self.resolve_tree(t, self.blob.trees[t],
                              self.initial_locals(st, mrow), emit=True)

    # ---- CLSS, FILD, MTHD ----------------------------------------------------
    def flags(self, mods):
        """The flags word: the OR of the modifier bits, nothing else."""
        f = 0
        for m in mods:
            f |= FLAG[m]
        return f

    def member_rows(self):
        """Predict the FILD and MTHD rows and the CLSS record from the source
        alone (they only name entries of steps 1 to 3).

        Returns (static_fields, instance_fields, static_methods,
        instance_methods, clss, tree count) as tufa tuples."""
        ix = self.pool.index
        sf, inf, sm, im = [], [], [], []
        tree = 0
        for m in self.c.members:
            fl = self.flags(m.mods)
            static = bool(fl & FLAG['static'])
            if m.k == 'field':
                init = NONE
                if m.init is not None:
                    init = tree
                    tree += 1
                row = tufa.Field(fl, ix[('utf8', m.name)],
                                 ix[('utf8', self.desc(m.type, m.dims))], init)
                (sf if static else inf).append(row)
            else:
                if fl & FLAG['abstract']:
                    t = NONE
                else:
                    t = tree
                    tree += 1
                row = tufa.Method(fl, ix[('utf8', m.name)],
                                  ix[('utf8', self.method_desc(m))], t,
                                  self.locals_word(m, static))
                (sm if static else im).append(row)
        sup = self.super_name()
        clss = tufa.Clss(NONE, 1 if self.c.kind == 'interface' else 0,
                         ix[('class', self.fq)],
                         NONE if sup is None else ix[('class', sup)],
                         [ix[('class', i)] for i in self.interface_names()])
        return sf, inf, sm, im, clss, tree

    def has_code(self, m):
        """Does the member own a tree with nodes?  A body written ";" gives
        an empty tree, and so does "{}" (no statement at all) on a method;
        a constructor with "{}" still holds the implicit super call."""
        if m.body is None:
            return False
        if m.k == 'ctor':
            return True
        return len(m.body.stmts) > 0

    def locals_word(self, m, static):
        """Fifth word of a method row: this + parameters + the largest number
        of declared locals alive at one time; 0 when the tree is empty."""
        if not self.has_code(m):
            return 0
        base = (0 if static else 1) + len(m.params)
        return base + max_live(m.body)


def max_live(node):
    """Largest number of local variables alive at one time inside a
    statement: a declaration lives to the end of the enclosing block, a
    declaration in a for header to the end of the for statement, and a
    declaration in a switch body to the next case / default label."""
    k = node.k
    if k == 'block':
        live = 0
        best = 0
        for s in node.stmts:
            if s.k == 'local':
                live += len(s.decls)
                best = max(best, live)
            else:
                best = max(best, live + max_live(s))
        return best
    if k == 'switch':
        live = 0
        best = 0
        for s in node['items']:
            if s.k == 'local':
                live += len(s.decls)
                best = max(best, live)
            elif s.k in ('case', 'default'):
                # a case label closes the scope of the group before it: the
                # locals declared since the previous label are released
                live = 0
            else:
                best = max(best, live + max_live(s))
        return best
    if k == 'local':
        return len(node.decls)
    if k == 'if':
        return max(max_live(node.a), max_live(node.b) if node.b is not None else 0)
    if k in ('while', 'do'):
        return max_live(node.body)
    if k == 'for':
        live = 0
        for s in node.init:
            if s.k == 'local':
                live += len(s.decls)
        return live + max_live(node.body)
    return 0


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n
