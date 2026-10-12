#!/usr/bin/env python3
"""spike_compiler.py - a composition spike: source text -> class file bytes.

NOT the compiler.  It answers one question for notes/compiler_spec.md: do the
three models that the analyses proved separately compose into whole class
files without anything in between that nobody modelled?

    statements, locals, jumps, lines   analysis/stmt/stmtgen.py   (run in its
                                       free mode, no class to look at)
    names, calls, method lookup        the rules of analysis/expr/gen.py,
                                       re-stated here on the statement
                                       parser's tree (methods name, call_path)
    pool order, CLSS / FILD / MTHD     analysis/decl/poolmodel.py, fed with
                                       the trees emitted here instead of the
                                       shipped ones
    bytes                              lib/tufa.py

Nothing is read from the class file of the source being compiled: its own
classes enter the class database as the source declares them (own_view).
Other classes are known through analysis/expr/classdb.py (class files, and
sources for classes without one), which is what the exe's compiler has in its
VM.  Two tables stand in for state of the exe that is not in any source: the
spelling of pack names (pack_rule 'table': the one spelling each pack has in
the shipped pools; 'disk': the file names of the install) and the float
conversion (float_mode A to D, see float_models.py).

    import spike_compiler as sc
    env = sc.Env()                         # class database, pack names
    data, info = sc.compile_source(text, env, float_mode='A')
"""
import os
import pickle
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, '..'))
for _p in (os.path.join(_ROOT, 'analysis', 'expr'), os.path.join(_ROOT, 'analysis', 'decl'),
           os.path.join(_ROOT, 'analysis', 'stmt'), os.path.join(_ROOT, 'lib')):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import oracle               # noqa: E402
import srclex               # noqa: E402
import tufa                 # noqa: E402
import parser as libparser  # noqa: E402
import sparse               # noqa: E402
import stmtgen              # noqa: E402
import classdb              # noqa: E402
import jparse               # noqa: E402
import gen                  # noqa: E402
import universe             # noqa: E402
import poolmodel            # noqa: E402
import libbridge            # noqa: E402
import libast               # noqa: E402
import symbols              # noqa: E402

STRING = 'Ljava.lang.String;'
FLOAT_SETTINGS = {'A': 'div pc24 v24 s24 c24', 'B': 'mul pc24 v24 s24 c24',
                  'C': 'div pc53 v24 s24 c24', 'D': 'diva pc53 v53 s53 c24'}


class CompileError(Exception):
    pass


class Env(object):
    """What the compiler knows besides the source: the other classes, the
    spelling of pack names, the float conversion."""

    def __init__(self, root=None):
        self.root = root or oracle.corpus_root()
        self.db = classdb.load()
        self.model = gen.Model(self.db)
        self.uni = universe.load()
        self._float = {}
        self._cands = None
        self.disk = {}
        for dp, dn, fn in os.walk(self.root):
            dn[:] = [d for d in dn if not d.lower().startswith('tree_rpk_scan_boot')]
            for f in fn:
                if f.lower().endswith('.rpk'):
                    rel = os.path.relpath(os.path.join(dp, f), self.root).replace('/', '\\')
                    self.disk[rel.lower()] = rel
        # one spelling per pack over all shipped pools (spec/rid_packs.py)
        self.table = {}
        for e in oracle.iter_entries():
            if not e.cls:
                continue
            for b in oracle.load_class(e):
                for ent in b.cons:
                    if ent[0] == 'rid':
                        p = b.utf8(ent[1])
                        self.table.setdefault(p.lower(), p)
        for which in ('oracle', 'other'):
            p = os.path.join(_HERE, 'out', 'float_res_%s.pickle' % which)
            if os.path.exists(p):
                with open(p, 'rb') as f:
                    names, res = pickle.load(f)
                idx = {n: k for k, n in enumerate(names)}
                for tk, v in res.items():
                    for ch, nm in FLOAT_SETTINGS.items():
                        self._float[(ch, tk)] = v[idx[nm]]

    def sym_universe(self):
        """lib/symbols.Universe holding every class name of the database."""
        u = getattr(self, '_sym_uni', None)
        if u is None:
            u = symbols.Universe()
            for full in self.db.classes:
                u.add(full)
            self._sym_uni = u
        return u

    def float_bits(self, text, mode):
        key = (mode, text)
        v = self._float.get(key)
        if v is None:
            if self._cands is None:
                import float_models
                self._cands = {ch: [c for c in float_models.candidates() if c.name == nm][0]
                               for ch, nm in FLOAT_SETTINGS.items()}
            v = self._cands[mode].run(text)
            self._float[key] = v
        return v

    def pack(self, written, local_id, rule='table'):
        """(pool text, local id) of a resource id literal."""
        w = written.replace('.', '\\') + '.rpk'
        if rule == 'written':
            return w, local_id
        d = self.disk.get(w.lower())
        if rule == 'disk':
            return (d or w), local_id
        if d is None:
            return 'system.rpk', 0
        return self.table.get(w.lower(), d), local_id


class FullGen(stmtgen.Gen):
    """stmtgen.Gen with the oracle window replaced by name resolution."""

    def __init__(self, unit, cls, resolver, env, float_mode, pack_rule, db=None, model=None):
        stmtgen.Gen.__init__(self, unit, None, None)
        self.cls = cls
        self.r = resolver
        self.env = env
        self.db = db or env.db
        self.model = model or env.model
        self.float_mode = float_mode
        self.pack_rule = pack_rule
        self.ltypes = []
        self.syms = {}          # node index -> pool entry in content form
        self._estack = []
        self._hdr = []
        self._decl_type = None

    # ---- bookkeeping ------------------------------------------------------
    def sym(self, g, content):
        self.syms[len(self.out) - 1] = content
        return g

    def desc(self, typ, extra_dims=0):
        dims = typ.dims + extra_dims
        base = typ.parts
        if base == ['int']:
            d = 'I'
        elif base == ['float']:
            d = 'F'
        elif base == ['void']:
            d = 'V'
        else:
            full = self.r.resolve('.'.join(base))
            if full is None:
                raise CompileError('unknown type %s' % '.'.join(base))
            d = 'L%s;' % full
        return '[' * dims + d

    def declare_local(self, name):
        idx = stmtgen.Gen.declare_local(self, name)
        if self._hdr:
            t = self._hdr.pop(0)
        else:
            t = self._decl_type
        self.ltypes.append(t)
        return idx

    def releases(self, down_to, cat, lt, pop, alts=None):
        stmtgen.Gen.releases(self, down_to, cat, lt, pop, alts)
        del self.ltypes[len(self.locals):]

    def method(self, m):
        hdr = [] if 'static' in m.mods else ['L%s;' % self.cls]
        for p in reversed(m.params):
            hdr.append(self.desc(p.type, p.dims))
        self._hdr = hdr
        stmtgen.Gen.method(self, m)
        self._hdr = []
        self.ltypes = []

    # ---- operands the statement model copied from the class ---------------
    def type_carrier(self, typ, cat, lt, extra_dims=0, alts=None):
        d = self.desc(typ, extra_dims)
        g = self.emit(stmtgen.TYPE, None, cat, lt, alts=alts)
        self.sym(g, ('utf8', d))
        if cat == 'decl.type':
            self._decl_type = d
        return g

    def expr(self, e, stmt=False):
        self._estack.append(e)
        try:
            return stmtgen.Gen.expr(self, e, stmt)
        finally:
            self._estack.pop()

    def oracle(self, code, cat, lt, alts=None):
        e = self._estack[-1]
        if cat == 'lit.float':
            return self.emit(code, self.env.float_bits(self.text(e.tok), self.float_mode),
                             cat, lt, alts=alts)
        if cat == 'lit.str':
            g = self.emit(code, None, cat, lt, alts=alts)
            return self.sym(g, ('utf8', srclex.string_value(self.text(e.tok))))
        if cat == 'lit.rid':
            pack, lid = srclex.rid_parts(self.text(e.tok))
            g = self.emit(code, None, cat, lt, alts=alts)
            return self.sym(g, ('rid',) + self.env.pack(pack, lid, self.pack_rule))
        if cat in ('getfield', 'mcall.call_on', 'scall.call_super'):
            g = self.emit(code, None, cat, lt, alts=alts)
            return self.sym(g, ('utf8', self.text(e.name_tok)))
        if cat == 'cast.type':
            typ = e.type
            if typ.parts in (['int'], ['float']) or typ.dims:
                g = self.emit(stmtgen.TYPE, None, cat, lt, alts=alts)
                return self.sym(g, ('utf8', self.desc(typ)))
            full = self.r.resolve('.'.join(typ.parts))
            if full is None:
                raise CompileError('unknown type %s' % '.'.join(typ.parts))
            g = self.emit(stmtgen.CLASS, None, 'cast.class', lt, alts=alts)
            return self.sym(g, ('class', full))
        raise CompileError('no rule for the operand of %s' % cat)

    # ---- names (notes/expressions.md section 5) ---------------------------
    @staticmethod
    def class_of_desc(desc):
        if desc is None:
            return None
        if desc.startswith('L') and desc.endswith(';'):
            return desc[1:-1]
        if desc.startswith('['):
            el = desc[1:]
            if el.startswith('L') and el.endswith(';'):
                return '[' + el[1:-1]
            return '[' + el
        return None

    def head(self, name):
        """(segment, type, class context) of the first identifier: a local,
        then a field of the class chain, then a class."""
        idx = self.lookup_local(name)
        if idx is not None:
            return ('local', idx), self.ltypes[idx], None
        f = self.db.find_field(self.cls, name)
        if f is not None:
            rc = f.owner if f.static else self.cls
            return ('field', (rc, f.name, f.desc)), f.desc, None
        full = self.r.resolve(name)
        if full is not None:
            return ('class', full), None, full
        return None, None, None

    def walk(self, names):
        seg, t, cctx = self.head(names[0])
        if seg is None:
            raise CompileError('unknown name %s' % names[0])
        segs = [seg]
        for nm in names[1:]:
            cls = cctx if cctx is not None else self.class_of_desc(t)
            if cls is not None and cls.startswith('['):
                if nm != 'length':
                    raise CompileError('member %s of an array' % nm)
                segs.append(('field', (cls, 'length', 'I')))
                t, cctx = 'I', None
                continue
            f = self.db.find_field(cls, nm) if cls is not None else None
            if f is None:
                raise CompileError('unknown member %s in %s' % (nm, '.'.join(names)))
            rc = f.owner if f.static else cls
            segs.append(('field', (rc, f.name, f.desc)))
            t, cctx = f.desc, None
        return segs, t, cctx

    def put_seg(self, seg, cat, lt):
        k, v = seg
        if k == 'local':
            self.emit(stmtgen.LOCAL, v, cat + '.seg.local', lt)
        elif k == 'field':
            self.sym(self.emit(stmtgen.FIELD, None, cat + '.seg.field', lt), ('ref',) + v)
        elif k == 'class':
            self.sym(self.emit(stmtgen.CLASS, None, cat + '.seg.class', lt), ('class', v))
        else:
            raise CompileError('segment kind %s' % k)

    def name(self, e):
        names = [self.text(t) for t in e.parts]
        segs, t, cctx = self.walk(names)
        if cctx is not None:
            raise CompileError('class name %s used as a value' % '.'.join(names))
        if len(names) == 1:
            k, v = segs[0]
            if k == 'local':
                self.emit(stmtgen.LOCAL, v, 'name.local', e.t1)
            else:
                self.sym(self.emit(stmtgen.FIELD, None, 'name.field', e.t1), ('ref',) + v)
            return
        n = len(segs)
        self.emit(stmtgen.PATH, n, 'name.path', e.t1)
        for j, seg in enumerate(segs):
            self.put_seg(seg, 'name.path', e.parts[len(e.parts) - n + j])

    # ---- calls (notes/expressions.md section 6) ----------------------------
    def arg_type(self, a):
        """(typed form?, type) of an argument: the compiler knows a type only
        for a literal, a local, a field written as a simple name, and this."""
        if a.k == 'lit':
            lk = a.lk
            t = {'int': 'I', 'rid': 'I', 'bool': 'I', 'float': 'F', 'str': STRING,
                 'null': 'null'}.get(lk)
            return True, t
        if a.k == 'this':
            return True, 'L%s;' % self.cls
        if a.k == 'name' and len(a.parts) == 1:
            nm = self.text(a.parts[0])
            idx = self.lookup_local(nm)
            if idx is not None:
                return True, self.ltypes[idx]
            f = self.db.find_field(self.cls, nm)
            return True, (f.desc if f is not None else None)
        return False, None

    def callee(self, cls, name, e, lone, lt_path, lt_name):
        typed = True
        argt = []
        for a in e.args:
            ok, t = self.arg_type(a)
            typed = typed and ok
            argt.append(t)
        m = None
        if cls is not None and not cls.startswith('[') and typed:
            m, score, tie = self.model.find_method_ex(cls, name, argt)
        if m is not None:
            self.sym(self.emit(stmtgen.METHOD, None, 'call.method', lt_name),
                     ('ref', cls, name, m.desc))
            return
        if lone:
            self.emit(stmtgen.PATH, 1, 'call.path', lt_path)
        self.sym(self.emit(stmtgen.NAME, None, 'call.path.seg.name', lt_name), ('utf8', name))

    def call_path(self, e):
        nm = e.name
        names = [self.text(t) for t in nm.parts]
        if len(names) == 1:
            self.callee(self.cls, names[0], e, True, nm.t1, nm.parts[0])
            return
        segs, t, cctx = self.walk(names[:-1])
        n = len(segs) + 1
        self.emit(stmtgen.PATH, n, 'call.path', nm.t1)
        for j, seg in enumerate(segs):
            self.put_seg(seg, 'call.path', nm.parts[len(nm.parts) - n + j])
        cls = cctx if cctx is not None else self.class_of_desc(t)
        self.callee(cls, names[-1], e, False, nm.t1, nm.parts[-1])


def own_view(env, text):
    """The class database as the compiler of THIS source sees it: every other
    class as the corpus has it, the classes of the source itself as the source
    declares them (so that nothing comes from the class file under test)."""
    junit = jparse.parse(text)
    db = classdb.DB()
    db.classes = dict(env.db.classes)
    db.packages = {k: set(v) for k, v in env.db.packages.items()}
    pkg = junit.package
    names = []
    for td in junit.types:
        full = (pkg + '.' if pkg else '') + td.name
        names.append(full)
        db.packages.setdefault(pkg or '', set()).add(td.name)
        if full not in db.classes:
            db.classes[full] = classdb.ClassInfo(full)
    for rnd in range(2):
        for td, full in zip(junit.types, names):
            db.classes[full] = classdb.info_from_source(db, junit, td)
    return db


def own_view_lib(env, pu):
    """own_view on the tree of lib/parser.py: the classes of the source as
    lib/symbols.py reads their rows (the declarations proven in grammar.md)."""
    db = classdb.DB()
    db.classes = dict(env.db.classes)
    db.packages = {k: set(v) for k, v in env.db.packages.items()}
    uni = env.sym_universe()
    pkg = pu.package or ''
    for c in pu.types:
        full = (pkg + '.' if pkg else '') + c.name
        db.packages.setdefault(pkg, set()).add(c.name)
        if not uni.has(pkg, c.name):
            uni.add(full)
    res = symbols.Resolver(pu, uni)
    for m in symbols.class_models(pu, res):
        ci = classdb.ClassInfo(m.name)
        ci.super = m.super
        ci.interfaces = list(m.interfaces)
        ci.is_interface = m.interface
        ci.origin = 'source'
        for row in m.fields():
            ci.fields[row.name] = classdb.FieldInfo(row.name, row.desc, row.static,
                                                    row.flags, m.name)
        rows = sorted(m.methods(), key=lambda r: r.decl.tok0)
        ci.methods = [classdb.MethodInfo(r.name, r.desc, r.static, r.flags, m.name, k)
                      for k, r in enumerate(rows)]
        db.classes[m.name] = ci
    return db


def _imports(unit):
    out = []
    for s in unit.imports:
        if s.endswith('.*'):
            out.append((s[:-2], True))
        else:
            out.append((s, False))
    return out


def compile_source(text, env, float_mode='A', pack_rule='table', build=tufa.BUILD_940,
                   name='<source>', own_from_source=True, parsers='lib'):
    """Returns (bytes of the class file, info).  Raises CompileError,
    sparse.ParseError, libparser.ParseError, srclex/lexer errors.

    parsers='lib' (default): one scanner and one parser, lib/lexer.py and
    lib/parser.py; the models read its tree through their adapters (libast,
    libbridge).  parsers='analysis': the three analysis parsers the models
    were developed on (sparse, srcparse through libbridge, jparse)."""
    pu = libparser.parse(text, name)
    punit = libbridge.convert(pu)
    if parsers == 'lib':
        unit = libast.convert(pu)
        imports = [(i.name, bool(i.star)) for i in pu.imports if i.pack is None]
        db = own_view_lib(env, pu) if own_from_source else env.db
    else:
        unit = sparse.parse(text)
        imports = _imports(unit)
        db = own_view(env, text) if own_from_source else env.db
    if len(unit.classes) != len(punit.classes):
        raise CompileError('the two parsers disagree on the classes')
    model = gen.Model(db)
    r = classdb.Resolver(db, unit.package, imports)
    blobs = []
    info = {'trees': 0, 'nodes': 0, 'classes': []}
    for cdecl, pdecl in zip(unit.classes, punit.classes):
        full = (unit.package + '.' if unit.package else '') + cdecl.name
        # ---- trees, in source order
        trees = []
        for mem in cdecl.members:
            if mem.k == 'field':
                for d in mem.decls:
                    if d.init is None:
                        continue
                    g = FullGen(unit, full, r, env, float_mode, pack_rule, db, model)
                    g.field_init(d)
                    g.finish()
                    trees.append(g)
            elif mem.k == 'method':
                if 'abstract' in mem.mods:
                    continue
                g = FullGen(unit, full, r, env, float_mode, pack_rule, db, model)
                g.method(mem)
                g.finish()
                trees.append(g)
            else:
                raise CompileError('member kind %s' % mem.k)
        # ---- rows and the head of the pool, from the source alone
        m0 = poolmodel.Model(punit, pdecl, None, env.uni).run()
        sf, inf, sm, im, clss, ntrees = m0.member_rows()
        if ntrees != len(trees):
            raise CompileError('tree count %d / %d' % (ntrees, len(trees)))
        # ---- a scratch blob: the trees with their operands in ANY pool order
        scratch = poolmodel.Pool()
        scratch.preload(m0.pool.entries)

        def intern(pool, c):
            k = c[0]
            if k == 'utf8':
                return pool.utf8(c[1])
            if k == 'class':
                return pool.cls(c[1])
            if k == 'ref':
                return pool.ref(c[1], c[2], c[3])
            if k == 'rid':
                return pool.rid(c[1], c[2])
            raise CompileError('pool entry kind %s' % k)
        sb = tufa.Blob()
        sb.build = build
        stree = []
        for g in trees:
            nodes = []
            for i, x in enumerate(g.out):
                imm = x.imm
                if i in g.syms:
                    imm = intern(scratch, g.syms[i])
                nodes.append(tufa.Node(stmtgen.RAW_OF[x.code], x.ln, imm))
            stree.append(nodes)
        sb.cons = poolmodel.to_indexed(scratch.entries)
        sb.static_fields, sb.instance_fields = sf, inf
        sb.static_methods, sb.instance_methods = sm, im
        sb.clss = clss
        sb.trees = stree
        # ---- the pool in the compiler's order (steps 1 to 5), then final indices
        m = poolmodel.Model(punit, pdecl, sb, env.uni).run()
        ix = m.pool.index
        b = tufa.Blob()
        b.build = build
        b.cons = poolmodel.to_indexed(m.pool.entries)
        b.static_fields, b.instance_fields, b.static_methods, b.instance_methods, \
            b.clss, _ = m.member_rows()
        b.trees = []
        for g in trees:
            nodes = []
            for i, x in enumerate(g.out):
                imm = x.imm
                if i in g.syms:
                    imm = ix[g.syms[i]]
                nodes.append(tufa.Node(stmtgen.RAW_OF[x.code], x.ln, imm))
            b.trees.append(nodes)
            info['nodes'] += len(nodes)
        info['trees'] += len(trees)
        # the locals word from the emitter must equal the one from the source
        rows = {}
        for row in b.static_methods + b.instance_methods:
            if row.tree != tufa.NONE:
                rows[row.tree] = row
        ti = 0
        for mem in cdecl.members:
            if mem.k == 'field':
                ti += sum(1 for d in mem.decls if d.init is not None)
            elif 'abstract' not in mem.mods:
                g = trees[ti]
                if g.out and rows[ti].locals != g.max_locals:
                    raise CompileError('locals word %d / high-water mark %d'
                                       % (rows[ti].locals, g.max_locals))
                ti += 1
        b.chunks = tufa.canonical_chunks(b)
        blobs.append(b)
        info['classes'].append(full)
    return tufa.serialise(blobs), info


def float_token_count(text):
    return sum(1 for t in srclex.tokenize(text) if t.kind == 'float')


if __name__ == '__main__':
    e = oracle.entry(sys.argv[1] if len(sys.argv) > 1 else 'game/Trigger')
    env = Env()
    data, info = compile_source(oracle.load_source(e), env)
    theirs = oracle.load_class_bytes(e)
    print(info['classes'], info['trees'], 'trees', info['nodes'], 'nodes',
          len(data), 'bytes;', 'IDENTICAL' if data == theirs else 'different')
    if data != theirs:
        for i, d in oracle.compare_files(data, theirs):
            if not d.equal:
                print(i, d)
