#!/usr/bin/env python3
"""symbols.py - what a parsed source declares, in the terms of the class file.

Built on parser.py.  It turns the AST of one source into the rows the compiler
writes into CLSS, FILD and MTHD: class names, superclass, interfaces, and per
field and method the flags, the name, the descriptor, the tree it owns and the
locals word.  check_members.py proves every one of these against the class
files of all 2,819 pairs.

    import parser, symbols
    unit  = parser.parse(text)
    uni   = symbols.Universe.from_index()          # every class of the corpus
    res   = symbols.Resolver(unit, uni)
    for model in symbols.class_models(unit, res):
        model.name, model.super, model.interfaces, model.interface
        model.static_fields, model.instance_fields      # [FieldRow]
        model.static_methods, model.instance_methods    # [MethodRow]
        model.tree_count

How a type name becomes a class name (Resolver.resolve; notes/grammar.md
section 9): a dotted name is taken as written; a simple name is looked up in
the source's own package (which includes the other classes of the file), then
in the packages of its imports in source order, then in java.lang.  Simple
class names are unique among the packages any source can see, so the order of
these steps is not observable in the corpus (no use has two candidates).
"""
import os
import sys
from collections import namedtuple

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

NONE = 0xFFFFFFFF

FLAG_BITS = {'public': 0x1, 'protected': 0x2, 'private': 0x4, 'static': 0x8,
             'abstract': 0x10, 'final': 0x20, 'native': 0x40,
             'synchronized': 0x80}

FieldRow = namedtuple('FieldRow', 'static name flags desc init tree decl declarator')
MethodRow = namedtuple('MethodRow', 'static name flags desc nparams tree locals body decl')
# MethodRow.body: 'block' (statements), 'empty' ({}), 'none' (';')


def modifier_mask(mods):
    m = 0
    for x in mods:
        m |= FLAG_BITS[x]
    return m


class Universe(object):
    """Every class name of the corpus: the blobs of all class files plus the
    types declared by sources that have no class file."""

    def __init__(self):
        self.classes = {}        # full name -> list of relative paths
        self.packages = {}       # package -> set of simple names

    def add(self, full, where=None):
        self.classes.setdefault(full, []).append(where)
        pkg, _, simple = full.rpartition('.')
        self.packages.setdefault(pkg, set()).add(simple)

    def has(self, package, simple):
        s = self.packages.get(package)
        return s is not None and simple in s

    @classmethod
    def from_index(cls, entries=None):
        import oracle
        u = cls()
        for e in (entries if entries is not None else oracle.load_index()):
            if e.blobs:
                for b in e.blobs:
                    u.add(b['name'], e.cls)
            if e.declared and e.package:
                for d in e.declared:
                    full = e.package + '.' + d['name']
                    if not e.blobs or full not in [b['name'] for b in e.blobs]:
                        u.add(full, e.source)
        return u


class Resolver(object):
    """Type names of one compilation unit -> full class names."""

    def __init__(self, unit, universe):
        self.unit = unit
        self.universe = universe
        self.package = unit.package or ''
        self.own = set(c.name for c in unit.types)
        self.cache = {}

    def candidates(self, name):
        """[(step, full name)] for every place a simple name is visible."""
        out = []
        seen = set()

        def add(step, pkg):
            full = (pkg + '.' + name) if pkg else name
            if full not in seen:
                seen.add(full)
                out.append((step, full))
        if name in self.own:
            add('file', self.package)
        if self.universe.has(self.package, name):
            add('package', self.package)
        for imp in self.unit.imports:
            if imp.star and self.universe.has(imp.name, name):
                add('import', imp.name)
        if self.universe.has('java.lang', name):
            add('java.lang', 'java.lang')
        return out

    def resolve(self, name):
        """Full class name for a type name as written, or None."""
        r = self.cache.get(name)
        if r is None:
            r = self.resolve_how(name)
            self.cache[name] = r
        return r[1]

    def resolve_how(self, name):
        """(step, full name): step is 'dotted', 'file', 'package', 'import',
        'java.lang' or None when the name is unknown."""
        if '.' in name:
            if name in self.universe.classes:
                return ('dotted', name)
            return (None, None)
        c = self.candidates(name)
        if not c:
            return (None, None)
        return c[0]

    def descriptor(self, type_node, extra_dims=0):
        """Descriptor of a Type node: I, F, V, Lpkg.Name; with one '[' per
        dimension.  An unknown class keeps its written name."""
        n = type_node.name
        d = '[' * (type_node.dims + extra_dims)
        if n == 'int':
            return d + 'I'
        if n == 'float':
            return d + 'F'
        if n == 'void':
            return d + 'V'
        return d + 'L' + (self.resolve(n) or n) + ';'

    def method_descriptor(self, m):
        """'(params)ret'; a constructor has no return part."""
        d = '(' + ''.join(self.descriptor(p.type, p.dims) for p in m.params) + ')'
        if not m.constructor:
            d += self.descriptor(m.type, m.dims)
        return d


def split_descriptor(desc):
    """['I', '[F', 'Ljava.lang.String;'] for 'I[FLjava.lang.String;'."""
    out = []
    i = 0
    n = len(desc)
    while i < n:
        j = i
        while desc[j] == '[':
            j += 1
        if desc[j] == 'L':
            j = desc.index(';', j)
        out.append(desc[i:j + 1])
        i = j + 1
    return out


def descriptor_params(desc):
    """(parameter descriptors, return descriptor or '') of '(...)R'."""
    ps, _, ret = desc[1:].partition(')')
    return split_descriptor(ps), ret


# ---------------------------------------------------------------------------
# locals
# ---------------------------------------------------------------------------

SCOPE_MODELS = ('real', 'switch is one scope', 'for variable outlives the loop',
                'no scopes')


def _depth(node, model='real'):
    """Highest number of declared locals alive at once inside a statement.
    A local lives from its declaration to the end of the block that declares
    it; the header of a for statement and each group of a switch (the
    statements from one case label to the next) are scopes too.

    model selects a deliberately different scoping, for check_members.py:
    'switch is one scope' (the whole switch body is one block), 'for
    variable outlives the loop' (it belongs to the enclosing block), 'no
    scopes' (a local lives to the end of the method)."""
    return _walk(node, model)[0]


def _walk(node, model):
    """(peak, leaked): the highest number of locals alive inside the
    statement, and how many of them stay alive after it."""
    if node is None:
        return 0, 0
    k = node.kind
    if k == 'Block':
        peak, live = _walk_list(node.stmts, model)
        return peak, (live if model == 'no scopes' else 0)
    if k == 'LocalVar':
        n = len(node.declarators)
        return n, n
    if k == 'If':
        p1, l1 = _walk(node.then, model)
        if model == 'no scopes':
            p2, l2 = _walk(node.otherwise, model)
            return max(p1, l1 + p2), l1 + l2
        p2, _ = _walk(node.otherwise, model)
        return max(p1, p2), 0
    if k in ('While', 'Do', 'Labeled'):
        p, l = _walk(node.body, model)
        return p, (l if model == 'no scopes' else 0)
    if k == 'For':
        n = len(node.init.declarators) if not isinstance(node.init, list) else 0
        p, l = _walk(node.body, model)
        if model == 'no scopes':
            return n + p, n + l
        if model == 'for variable outlives the loop':
            return n + p, n
        return n + p, 0
    if k == 'Switch':
        if model == 'no scopes':
            peak, live = _walk_list([s for g in node.groups for s in g.stmts], model)
            return peak, live
        if model == 'switch is one scope':
            peak, _ = _walk_list([s for g in node.groups for s in g.stmts], model)
            return peak, 0
        return max([_walk_list(g.stmts, model)[0] for g in node.groups] or [0]), 0
    return 0, 0


def _walk_list(stmts, model):
    live = 0
    best = 0
    for s in stmts:
        p, l = _walk(s, model)
        best = max(best, live + p)
        live += l
    return best, live


def locals_word(m, model='real'):
    """The fifth word of the method row: this + parameters + the deepest
    nesting of declared locals; 0 when the method owns no code."""
    if method_body_form(m) != 'block' and not (m.constructor and m.body is not None):
        return 0
    base = (0 if 'static' in m.modifiers else 1) + len(m.params)
    return base + _depth(m.body, model)


def method_body_form(m):
    if m.body is None:
        return 'none'
    return 'block' if m.body.stmts else 'empty'


def has_code(m):
    """True when the compiler writes nodes for the method: a body with
    statements, or any body at all for a constructor (the implicit super
    call).  ';' and, for a method, '{}' give an empty tree."""
    if m.body is None:
        return False
    return bool(m.body.stmts) or m.constructor


# ---------------------------------------------------------------------------
# the class model
# ---------------------------------------------------------------------------

class ClassModel(object):
    def __init__(self):
        self.name = None
        self.simple_name = None
        self.super = None
        self.interfaces = []
        self.interface = False
        self.static_fields = []
        self.instance_fields = []
        self.static_methods = []
        self.instance_methods = []
        self.tree_count = 0
        self.node = None

    def fields(self):
        return self.static_fields + self.instance_fields

    def methods(self):
        return self.static_methods + self.instance_methods


def class_model(unit, cls, resolver):
    """ClassModel of one Class node."""
    m = ClassModel()
    m.node = cls
    m.simple_name = cls.name
    m.name = (unit.package + '.' if unit.package else '') + cls.name
    m.interface = bool(cls.interface)
    if cls.extends:
        m.super = resolver.resolve(cls.extends) or cls.extends
    elif cls.interface or m.name == 'java.lang.Object':
        m.super = None
    else:
        m.super = 'java.lang.Object'
    m.interfaces = [resolver.resolve(x) or x for x in cls.implements]
    tree = 0
    for mem in cls.members:
        if mem.kind == 'FieldDecl':
            static = 'static' in mem.modifiers
            flags = modifier_mask(mem.modifiers)
            for d in mem.declarators:
                t = NONE
                if d.init is not None:
                    t = tree
                    tree += 1
                row = FieldRow(static, d.name, flags,
                               resolver.descriptor(mem.type, d.dims), d.init, t, mem, d)
                (m.static_fields if static else m.instance_fields).append(row)
        elif mem.kind == 'MethodDecl':
            static = 'static' in mem.modifiers
            if 'abstract' in mem.modifiers:
                t = NONE
            else:
                t = tree
                tree += 1
            row = MethodRow(static, '<init>' if mem.constructor else mem.name,
                            modifier_mask(mem.modifiers),
                            resolver.method_descriptor(mem), len(mem.params), t,
                            locals_word(mem), method_body_form(mem), mem)
            (m.static_methods if static else m.instance_methods).append(row)
    m.tree_count = tree
    return m


def class_models(unit, resolver):
    return [class_model(unit, c, resolver) for c in unit.types]
