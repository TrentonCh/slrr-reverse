"""decl.py - the declarations of a class: the first three steps of its pool
and the rows of FILD, MTHD and CLSS.

Everything here follows from the source alone (notes/declarations.md 3, 4, 6.2
to 6.4 and 12; byte-identical for all 2,873 classes of the pairs):

    1 seed    the class, its superclass, its interfaces: utf8 + class entry each
    2 types   the type descriptors, in the order the game's parser met them
              (type_walk)
    3 names   per member in source order: field -> its name; method -> its
              descriptor, then its name ("<init>" for a constructor)

The rows only name entries of these steps.  Steps 4 and 5 (member refs,
strings) need the finished trees and belong to the writer.

C++ (sc_decl.cpp): the same functions.
"""
from . import classes
from . import parser as ps
from .pool import Pool
from .parser import (
    ACC_MASK, ACC_ABSTRACT, ACC_STATIC, F_CTOR, F_INTERFACE,
    FIELD_DECL, BLOCK, LOCAL_VAR, EXPR_STMT, IF, WHILE, DO, FOR, SWITCH,
    CASE_LABEL, RETURN, FIELD, INDEX, CALL, CTOR_CALL, NEW, NEW_ARRAY, UNARY,
    POSTFIX, BINARY, ASSIGN, COND, CAST, INSTANCEOF, PAREN)

NONE = 0xFFFFFFFF


class FieldRow(object):
    """One row of FILD (16 bytes)."""
    __slots__ = ('flags', 'name', 'type', 'tree')

    def __init__(self, flags, name, type, tree):
        self.flags = flags          # OR of the modifier bits
        self.name = name            # utf8
        self.type = type            # utf8 of the descriptor
        self.tree = tree            # initialiser tree, NONE without one


class MethodRow(object):
    """One row of MTHD (20 bytes)."""
    __slots__ = ('flags', 'name', 'desc', 'tree', 'locals')

    def __init__(self, flags, name, desc, tree, locals):
        self.flags = flags
        self.name = name            # utf8
        self.desc = desc            # utf8 of the descriptor
        self.tree = tree            # NONE for an abstract method
        self.locals = locals        # number of local slots, 0 for an empty tree


class TreeOwner(object):
    """The member a tree belongs to.  The trees of a class are numbered in
    source order over the fields with an initialiser and the methods that are
    not abstract; owners[t] is the owner of tree t."""
    __slots__ = ('node', 'decl', 'is_field', 'is_static')

    def __init__(self, node, decl, is_field, is_static):
        self.node = node            # DECLARATOR of the field, or the METHOD_DECL
        self.decl = decl            # the FIELD_DECL of a declarator, or the METHOD_DECL
        self.is_field = is_field
        self.is_static = is_static


class Rows(object):
    """FILD, MTHD and CLSS of a class, with pool indices."""
    __slots__ = ('static_fields', 'instance_fields', 'static_methods',
                 'instance_methods', 'interface', 'name', 'super', 'interfaces',
                 'owners')

    def __init__(self):
        self.static_fields = []     # FieldRow, source order
        self.instance_fields = []
        self.static_methods = []    # MethodRow, source order
        self.instance_methods = []
        self.interface = 0          # CLSS: 1 for an interface
        self.name = NONE            # CLSS: class entry of the class
        self.super = NONE           # CLSS: class entry of the superclass, or NONE
        self.interfaces = []        # CLSS: class entries
        self.owners = []            # TreeOwner per tree


class ClassBuild(object):
    """One class of a source after the declaration steps."""
    __slots__ = ('node', 'info', 'first_in_file', 'pool', 'rows', 'trees', 'pool_steps')

    def __init__(self, node, info, first_in_file, pool, rows):
        self.node = node            # the CLASS node
        self.info = info            # ClassInfo
        self.first_in_file = first_in_file
        self.pool = pool            # Pool after steps 1 to 3 (the writer adds steps 4 and 5)
        self.rows = rows            # Rows
        self.trees = []             # emit.Tree per tree owner, once the emitter has run
        self.pool_steps = []        # length of the pool after each step taken so far


# ---------------------------------------------------------------------------
# step 1: seed
# ---------------------------------------------------------------------------

def seed(pool, info):
    """The class, then its superclass, then its interfaces in implements
    order.  java.lang.Object and an interface have no superclass pair."""
    pool.cls(info.name)
    if info.super is not None:
        pool.cls(info.super)
    for i in info.interfaces:
        pool.cls(i)


# ---------------------------------------------------------------------------
# step 2: types
# ---------------------------------------------------------------------------

def type_walk(pool, scope, class_node, info, first_in_file):
    """Intern the type descriptors of a class body in the order the game's
    parser reduces the constructs that carry them (notes/declarations.md 6.3):

        field             its type, at the declaration
        parameter         its type, left to right (also of a bodiless method)
        method            its return type, after the parameters, before the body
        constructor       nothing for a return type
        new T(args)       "LT;" before the arguments
        new T[n]          the array type "[LT;" / "[I" / "[F", before n
        (int)x (float)x   "I" / "F", before x; a cast to a class: nothing
        x instanceof T    "LT;" after x
        T a = e, b = f;   the type of a local after its initialisers
        end of the body   "LI;" per interface, then "LC;" of the class itself
                          when it is not the first class of its source

    A descriptor that is in the pool adds nothing, so only first uses show.

    Five of these places are a choice no shipped class can tell from its
    alternative (notes/declarations.md 11); the choice made here is the one
    of the model that reproduced the 2,873 pools: a field type before its
    initialiser; a local type after the last declarator (not after each);
    and "new T[n]" before n, "(int)x" before x, "x instanceof T" after x."""
    for m in class_node.items:
        if m.kind == FIELD_DECL:
            for d in m.items:
                pool.utf8(classes.descriptor(scope, m.a, d.dims()))
                if d.a is not None:
                    expr_types(pool, scope, d.a)
        else:
            for p in m.items:
                pool.utf8(classes.descriptor(scope, p.a, p.dims()))
            if m.a is not None:
                pool.utf8(classes.descriptor(scope, m.a, 0))
            if m.b is not None:
                stmt_types(pool, scope, m.b)
    for i in info.interfaces:
        pool.utf8('L' + i + ';')
    if not first_in_file:
        pool.utf8('L' + info.name + ';')


def local_types(pool, scope, s):
    """A local declaration: the types inside its initialisers first, then
    its own type."""
    for d in s.items:
        if d.a is not None:
            expr_types(pool, scope, d.a)
    for d in s.items:
        pool.utf8(classes.descriptor(scope, s.a, d.dims()))


def stmt_types(pool, scope, s):
    k = s.kind
    if k == BLOCK:
        for x in s.items:
            stmt_types(pool, scope, x)
    elif k == LOCAL_VAR:
        local_types(pool, scope, s)
    elif k == EXPR_STMT:
        expr_types(pool, scope, s.a)
    elif k == IF:
        expr_types(pool, scope, s.a)
        stmt_types(pool, scope, s.b)
        if s.c is not None:
            stmt_types(pool, scope, s.c)
    elif k == WHILE:
        expr_types(pool, scope, s.a)
        stmt_types(pool, scope, s.b)
    elif k == DO:
        stmt_types(pool, scope, s.a)
        expr_types(pool, scope, s.b)
    elif k == FOR:
        if s.c is not None:
            if s.c.kind == LOCAL_VAR:
                local_types(pool, scope, s.c)
            else:
                for e in s.c.items:
                    expr_types(pool, scope, e)
        if s.a is not None:
            expr_types(pool, scope, s.a)
        for e in s.items:
            expr_types(pool, scope, e)
        stmt_types(pool, scope, s.b)
    elif k == SWITCH:
        expr_types(pool, scope, s.a)
        for g in s.items:
            for lab in g.a.items:
                if lab.kind == CASE_LABEL:
                    expr_types(pool, scope, lab.a)
            for x in g.items:
                stmt_types(pool, scope, x)
    elif k == RETURN:
        if s.a is not None:
            expr_types(pool, scope, s.a)
    # BREAK, CONTINUE, EMPTY carry no type


def expr_types(pool, scope, e):
    k = e.kind
    if k == NEW:
        pool.utf8(classes.descriptor(scope, e.a, 0))
        for x in e.items:
            expr_types(pool, scope, x)
    elif k == NEW_ARRAY:
        pool.utf8(classes.descriptor(scope, e.a, 1))
        expr_types(pool, scope, e.b)
    elif k == CAST:
        if ps.is_primitive(scope.ast, e.a):
            pool.utf8(classes.descriptor(scope, e.a, 0))
        expr_types(pool, scope, e.b)
    elif k == INSTANCEOF:
        expr_types(pool, scope, e.a)
        pool.utf8(classes.descriptor(scope, e.b, 0))
    elif k == FIELD or k == UNARY or k == POSTFIX or k == PAREN:
        expr_types(pool, scope, e.a)
    elif k == INDEX or k == BINARY or k == ASSIGN:
        expr_types(pool, scope, e.a)
        expr_types(pool, scope, e.b)
    elif k == COND:
        expr_types(pool, scope, e.a)
        expr_types(pool, scope, e.b)
        expr_types(pool, scope, e.c)
    elif k == CALL:
        if e.a is not None:
            expr_types(pool, scope, e.a)
        for x in e.items:
            expr_types(pool, scope, x)
    elif k == CTOR_CALL:
        for x in e.items:
            expr_types(pool, scope, x)
    # LITERAL, NAME, THIS, SUPER carry no type


# ---------------------------------------------------------------------------
# step 3: names, and the rows
# ---------------------------------------------------------------------------

def has_code(m):
    """Does a method own a tree with nodes?  A body written ";" gives an
    empty tree, and so does "{}" on a method; a constructor written "{}"
    still holds the implicit super call (notes/declarations.md 4.5)."""
    if m.b is None:
        return False
    if m.flags & F_CTOR:
        return True
    return len(m.b.items) > 0


def max_live(s):
    """Largest number of declared locals alive at one time inside a
    statement.  A declaration lives to the end of the enclosing block, one in
    a for header to the end of the for statement, one in a switch group to
    the next label."""
    k = s.kind
    if k == BLOCK:
        return max_live_list(s.items)
    if k == SWITCH:
        best = 0
        for g in s.items:
            n = max_live_list(g.items)
            if n > best:
                best = n
        return best
    if k == LOCAL_VAR:
        return len(s.items)
    if k == IF:
        n = max_live(s.b)
        if s.c is not None:
            m = max_live(s.c)
            if m > n:
                n = m
        return n
    if k == WHILE:
        return max_live(s.b)
    if k == DO:
        return max_live(s.a)
    if k == FOR:
        live = 0
        if s.c is not None and s.c.kind == LOCAL_VAR:
            live = len(s.c.items)
        return live + max_live(s.b)
    return 0


def max_live_list(stmts):
    live = 0
    best = 0
    for s in stmts:
        if s.kind == LOCAL_VAR:
            live += len(s.items)
            if live > best:
                best = live
        else:
            n = live + max_live(s)
            if n > best:
                best = n
    return best


def locals_word(m):
    """The fifth word of a method row: this (unless static) + parameters +
    the largest number of declared locals alive at one time; 0 when the tree
    is empty."""
    if not has_code(m):
        return 0
    base = 0 if m.flags & ACC_STATIC else 1
    return base + len(m.items) + max_live(m.b)


def names_and_rows(pool, class_node, info):
    """Step 3 and the rows.  The members are walked once in source order,
    fields and methods mixed: the names are interned, the rows go to the
    static or the instance vector (a stable split), and one counter numbers
    the trees: a field with an initialiser and every method that is not
    abstract takes the next number, also when its tree is empty."""
    rows = Rows()
    rows.interface = 1 if class_node.flags & F_INTERFACE else 0
    rows.name = pool.cls(info.name)
    if info.super is not None:
        rows.super = pool.cls(info.super)
    for i in info.interfaces:
        rows.interfaces.append(pool.cls(i))
    fi = 0
    mi = 0
    for m in class_node.items:
        flags = m.flags & ACC_MASK
        is_static = (flags & ACC_STATIC) != 0
        if m.kind == FIELD_DECL:
            for d in m.items:
                f = info.fields[fi]
                fi += 1
                tree = NONE
                if d.a is not None:
                    tree = len(rows.owners)
                    rows.owners.append(TreeOwner(d, m, True, is_static))
                name = pool.utf8(f.name)
                desc = pool.utf8(f.desc)        # in the pool since step 2
                row = FieldRow(flags, name, desc, tree)
                if is_static:
                    rows.static_fields.append(row)
                else:
                    rows.instance_fields.append(row)
        else:
            f = info.methods[mi]
            mi += 1
            desc = pool.utf8(f.desc)
            name = pool.utf8(f.name)
            tree = NONE
            if not (flags & ACC_ABSTRACT):
                tree = len(rows.owners)
                rows.owners.append(TreeOwner(m, m, False, is_static))
            row = MethodRow(flags, name, desc, tree, locals_word(m))
            if is_static:
                rows.static_methods.append(row)
            else:
                rows.instance_methods.append(row)
    return rows


def declare_class(scope, class_node, info, first_in_file):
    """Steps 1 to 3 for one class of a source: its pool so far and its rows."""
    pool = Pool()
    seed(pool, info)
    n_seed = len(pool.entries)
    type_walk(pool, scope, class_node, info, first_in_file)
    n_types = len(pool.entries)
    rows = names_and_rows(pool, class_node, info)
    build = ClassBuild(class_node, info, first_in_file, pool, rows)
    build.pool_steps = [n_seed, n_types, len(pool.entries)]
    return build
