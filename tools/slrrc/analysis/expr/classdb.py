#!/usr/bin/env python3
"""classdb.py - what every class of the corpus declares, for name resolution.

The compiler resolves names against the classes it can load.  This module
gives the analysis the same knowledge:

    db = classdb.load()                 # cached in analysis/expr/_classdb.pickle
    ci = db.get('java.game.Trigger')    # ClassInfo or None
    ci.name, ci.super, ci.interfaces, ci.is_interface, ci.origin
    ci.fields    {name: FieldInfo(name, desc, static, flags, owner)}
    ci.methods   [MethodInfo(name, desc, static, flags, owner, order)]
                 in SOURCE order (tree order; rows without a tree keep their
                 row position)
    db.find_field(cls, name)            # walks the superclass chain
    db.methods_named(cls, name)         # all methods of that name up the chain
    db.is_subclass(a, b)

    r = classdb.Resolver(db, package, imports)
    r.resolve('GameRef')                # 'java.util.resource.GameRef' or None
    r.descriptor(type_node)             # jparse 'type' N -> 'Ljava...;' / 'I' / '[F'

ClassInfo comes from the class file when one exists (2,993 blobs) and from the
parsed source for the 635 sources without a class.  A fully qualified name that
occurs more than once (12 names) keeps the first occurrence in corpus order,
except that a file named like the class wins over a blob inside another file.
"""
import os
import pickle
import sys
from collections import namedtuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import oracle  # noqa: E402
import tufa    # noqa: E402
import jparse  # noqa: E402

FieldInfo = namedtuple('FieldInfo', 'name desc static flags owner')
MethodInfo = namedtuple('MethodInfo', 'name desc static flags owner order')

CACHE = os.path.join(_HERE, '_classdb.pickle')
PRIMS = {'int': 'I', 'float': 'F', 'void': 'V'}


class ClassInfo(object):
    __slots__ = ('name', 'super', 'interfaces', 'is_interface', 'fields',
                 'methods', 'origin', 'dir', 'file')

    def __init__(self, name):
        self.name = name
        self.super = None
        self.interfaces = []
        self.is_interface = False
        self.fields = {}
        self.methods = []
        self.origin = None
        self.dir = None
        self.file = None

    def __getstate__(self):
        return {s: getattr(self, s) for s in self.__slots__}

    def __setstate__(self, d):
        for k, v in d.items():
            setattr(self, k, v)


def split_desc(desc):
    """'(ILx;[F)V' -> (['I','Lx;','[F'], 'V')"""
    i = 1
    out = []
    n = len(desc)
    while desc[i] != ')':
        j = i
        while desc[j] == '[':
            j += 1
        if desc[j] == 'L':
            j = desc.index(';', j) + 1
        else:
            j += 1
        out.append(desc[i:j])
        i = j
    return out, desc[i + 1:]


def info_from_blob(b):
    ci = ClassInfo(b.name)
    ci.super = b.super_name
    ci.interfaces = list(b.interface_names)
    ci.is_interface = b.is_interface
    ci.origin = 'class'
    for st, i, f in b.fields():
        nm = b.utf8(f.name)
        ci.fields[nm] = FieldInfo(nm, b.utf8(f.type), st, f.flags, b.name)
    rows = []
    for st, i, m in b.methods():
        key = m.tree if m.tree != tufa.NONE else 1 << 30
        rows.append((key, len(rows), MethodInfo(b.utf8(m.name), b.utf8(m.desc), st,
                                                 m.flags, b.name, 0)))
    rows.sort(key=lambda r: (r[0], r[1]))
    ci.methods = [r[2]._replace(order=k) for k, r in enumerate(rows)]
    return ci


class DB(object):
    def __init__(self):
        self.classes = {}
        self.packages = {}       # package -> set of simple names
        self.dups = {}

    def add(self, ci, prefer=False):
        old = self.classes.get(ci.name)
        if old is not None:
            self.dups.setdefault(ci.name, []).append(ci.file)
            if not prefer:
                return
        self.classes[ci.name] = ci
        pkg, _, simple = ci.name.rpartition('.')
        self.packages.setdefault(pkg, set()).add(simple)

    def get(self, name):
        return self.classes.get(name)

    def chain(self, name):
        """The class and its superclasses, nearest first."""
        seen = set()
        while name is not None and name not in seen:
            ci = self.classes.get(name)
            if ci is None:
                return
            seen.add(name)
            yield ci
            name = ci.super

    def find_field(self, cls, name):
        for ci in self.chain(cls):
            f = ci.fields.get(name)
            if f is not None:
                return f
        return None

    def methods_named(self, cls, name):
        out = []
        for ci in self.chain(cls):
            out.extend(m for m in ci.methods if m.name == name)
        return out

    def is_subclass(self, a, b):
        """True when class a is b or inherits from b (interfaces included)."""
        if a == b:
            return True
        seen = set()
        todo = [a]
        while todo:
            x = todo.pop()
            if x in seen:
                continue
            seen.add(x)
            if x == b:
                return True
            ci = self.classes.get(x)
            if ci is None:
                continue
            if ci.super:
                todo.append(ci.super)
            todo.extend(ci.interfaces)
        return False

    def chain_complete(self, cls):
        """True when every superclass up to the root is known."""
        name = cls
        seen = set()
        while name is not None:
            if name in seen:
                return False
            seen.add(name)
            ci = self.classes.get(name)
            if ci is None:
                return False
            name = ci.super
        return True


class Resolver(object):
    """Simple class name -> fully qualified name, for one source file."""

    def __init__(self, db, package, imports):
        self.db = db
        self.package = package
        self.single = {}
        self.star = []
        for name, star in imports:
            if ':' in name:
                continue
            if star:
                self.star.append(name)
            else:
                self.single[name.rpartition('.')[2]] = name
        self.cache = {}

    def candidates(self, simple):
        out = []
        if simple in self.single:
            out.append(self.single[simple])
        pk = [self.package] if self.package else []
        for p in pk + self.star + ['java.lang']:
            full = p + '.' + simple
            if full in self.db.classes and full not in out:
                out.append(full)
        return out

    def resolve(self, name):
        if name in self.cache:
            return self.cache[name]
        if '.' in name:
            r = name if name in self.db.classes else None
        else:
            c = self.candidates(name)
            r = c[0] if c else None
        self.cache[name] = r
        return r

    def descriptor(self, t, extra_dims=0):
        """Descriptor of a jparse type node (None when the class is unknown)."""
        dims = t.dims + extra_dims
        if t.name in PRIMS:
            base = PRIMS[t.name]
        else:
            full = self.resolve(t.name)
            if full is None:
                return None
            base = 'L%s;' % full
        return '[' * dims + base


def info_from_source(db, unit, td):
    """ClassInfo of a type declared in a parsed source (needs db for types)."""
    pkg = unit.package
    full = (pkg + '.' if pkg else '') + td.name
    r = Resolver(db, pkg, unit.imports)
    ci = ClassInfo(full)
    ci.origin = 'source'
    ci.is_interface = td.kind == 'interface'
    if td.extends:
        ci.super = r.resolve(td.extends) or td.extends
    elif full != 'java.lang.Object' and not ci.is_interface:
        ci.super = 'java.lang.Object'
    ci.interfaces = [r.resolve(x) or x for x in td.implements]
    order = 0
    for m in td.members:
        if m.k == 'fielddecl':
            d = r.descriptor(m.type, m.dims) or ('L?%s;' % m.type.name)
            ci.fields[m.name] = FieldInfo(m.name, d, 'static' in m.mods,
                                          mods_flags(m.mods), full)
        else:
            ps = []
            for p in m.params:
                ps.append(r.descriptor(p.type, p.dims) or ('L?%s;' % p.type.name))
            ret = ''
            if m.ret is not None:
                ret = r.descriptor(m.ret) or ('L?%s;' % m.ret.name)
            ci.methods.append(MethodInfo(m.name, '(%s)%s' % (''.join(ps), ret),
                                         'static' in m.mods, mods_flags(m.mods),
                                         full, order))
            order += 1
    return ci


_FLAG = {'public': 0x1, 'protected': 0x2, 'private': 0x4, 'static': 0x8,
         'abstract': 0x10, 'final': 0x20, 'native': 0x40, 'synchronized': 0x80}


def mods_flags(mods):
    f = 0
    for m in mods:
        f |= _FLAG[m]
    return f


def build():
    db = DB()
    # pass 1: class files
    for e in oracle.iter_entries():
        if e.cls is None:
            continue
        blobs = oracle.load_class(e)
        for b in blobs:
            ci = info_from_blob(b)
            ci.dir = e.dir
            ci.file = e.cls
            simple = ci.name.rpartition('.')[2]
            prefer = (e.name == simple)
            db.add(ci, prefer=prefer and ci.name in db.classes and
                   db.classes[ci.name].file != e.cls and
                   os.path.basename(db.classes[ci.name].file)[:-6] != simple)
    # pass 2: sources without a class (two rounds so that they see each other)
    units = []
    for e in oracle.iter_entries('source_only'):
        u = jparse.parse(oracle.load_source(e))
        units.append((e, u))
    for rnd in range(2):
        for e, u in units:
            for td in u.types:
                ci = info_from_source(db, u, td)
                ci.dir = e.dir
                ci.file = e.source
                if rnd == 0:
                    db.add(ci)
                else:
                    if db.classes.get(ci.name) is not None and \
                            db.classes[ci.name].file == e.source:
                        db.classes[ci.name] = ci
    return db


_DB = None


def load(rebuild=False):
    global _DB
    if _DB is not None and not rebuild:
        return _DB
    if not rebuild and os.path.exists(CACHE):
        with open(CACHE, 'rb') as f:
            _DB = pickle.load(f)
        return _DB
    _DB = build()
    with open(CACHE, 'wb') as f:
        pickle.dump(_DB, f, pickle.HIGHEST_PROTOCOL)
    return _DB


def main(argv):
    import classdb as _self          # so that the pickle names classdb.DB
    db = _self.load(rebuild=True)
    n_cls = sum(1 for c in db.classes.values() if c.origin == 'class')
    n_src = sum(1 for c in db.classes.values() if c.origin == 'source')
    print('classes %d (from class files %d, from sources %d), duplicate names %d'
          % (len(db.classes), n_cls, n_src, len(db.dups)))
    miss = 0
    for c in db.classes.values():
        if not db.chain_complete(c.name):
            miss += 1
    print('classes with an incomplete superclass chain: %d' % miss)
    nf = sum(len(c.fields) for c in db.classes.values())
    nm = sum(len(c.methods) for c in db.classes.values())
    print('fields %d, methods %d' % (nf, nm))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
