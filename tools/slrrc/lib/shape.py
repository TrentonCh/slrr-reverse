#!/usr/bin/env python3
"""shape.py - the postfix "shape" of a method, from the AST and from the class.

The compiled code is postfix: operands first, operator after.  The sequence of
operators, literals, types, calls and jumps of a tree therefore fixes the
expression and statement structure the compiler's parser saw.  This module
produces that sequence twice:

    tree_events(blob, tree)            from the nodes of a class file
    method_events(method, resolver)    from a MethodDecl of parser.py
    initialiser_events(expr, resolver) from the initialiser of a field

check_shape.py compares the two for every tree of every pair.  Equal sequences
prove that parser.py builds the tree the game's compiler built: precedence,
associativity, what is a cast, what is a declaration, which else belongs to
which if, where a switch group ends.

An event is a tuple.  Nodes that only manage the locals (0x1003 release) are
not events.  Jumps carry the index of the event they lead to (the first event
at or after the target node), so no node count has to be predicted.

    ('op', name, form)        expr / stmt node; form 'e' value used, 's' statement
    ('lit', kind, value)      literal: kind int / float / str / rid / null / bool
    ('var', name)             a name path, a field or a local pushed as operand;
                              name of the last segment, None for a local
    ('getfield', name)        field of a computed receiver
    ('index',)
    ('argc', n)               the argument count box before every call form
    ('call', name)            call through a name path (or a lone method ref)
    ('call_on', name)         call on a computed receiver
    ('call_super', name)
    ('new_init',) ('this_init',) ('super_init',)
    ('new', form, descriptor) ('newarray', descriptor)
    ('instanceof', descriptor) ('cast', descriptor)
    ('declare', descriptor, has_init)   ('store',)
    ('and_test',) ('or_test',)
    ('jump', target) ('jump_false', target) ('jump_true', target)
    ('case', target) ('pop',)
    ('value',) ('push_void',) ('ret',) ('ret_value',) ('end',)

This is not a code generator: it does not resolve names (a path stays one
event whatever its segments are) and it does not emit the release nodes.
"""
import bisect
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer    # noqa: E402
import tufa     # noqa: E402

TAG_NAMES = {
    0x00: 'cond', 0x01: 'lor', 0x02: 'land', 0x03: 'bor', 0x04: 'bxor',
    0x05: 'band', 0x06: 'ne', 0x07: 'eq', 0x08: 'ge', 0x09: 'le', 0x0A: 'gt',
    0x0B: 'lt', 0x0C: 'ushr', 0x0D: 'shr', 0x0E: 'shl', 0x0F: 'sub',
    0x10: 'add', 0x11: 'mod', 0x12: 'div', 0x13: 'mul', 0x14: 'bnot',
    0x15: 'lnot', 0x16: 'neg', 0x17: 'postinc', 0x18: 'postdec',
    0x19: 'preinc', 0x1A: 'predec', 0x1B: 'call', 0x1C: 'literal',
    0x1E: 'new', 0x1F: 'newarray', 0x20: 'instanceof', 0x21: 'cast',
    0x22: 'ctor', 0x23: 'assign', 0x24: 'mul_assign', 0x25: 'div_assign',
    0x26: 'mod_assign', 0x27: 'add_assign', 0x28: 'sub_assign',
    0x29: 'shl_assign', 0x2A: 'shr_assign', 0x2B: 'ushr_assign',
    0x2C: 'and_assign', 0x2D: 'xor_assign', 0x2E: 'or_assign',
}

BINARY_NAMES = {
    '|': 'bor', '^': 'bxor', '&': 'band', '!=': 'ne', '==': 'eq', '>=': 'ge',
    '<=': 'le', '>': 'gt', '<': 'lt', '>>>': 'ushr', '>>': 'shr', '<<': 'shl',
    '-': 'sub', '+': 'add', '%': 'mod', '/': 'div', '*': 'mul',
    '||': 'lor', '&&': 'land',
}
ASSIGN_NAMES = {
    '=': 'assign', '*=': 'mul_assign', '/=': 'div_assign', '%=': 'mod_assign',
    '+=': 'add_assign', '-=': 'sub_assign', '<<=': 'shl_assign',
    '>>=': 'shr_assign', '>>>=': 'ushr_assign', '&=': 'and_assign',
    '^=': 'xor_assign', '|=': 'or_assign',
}
UNARY_NAMES = {'-': 'neg', '!': 'lnot', '~': 'bnot', '++': 'preinc', '--': 'predec'}
POSTFIX_NAMES = {'++': 'postinc', '--': 'postdec'}

JUMPS = {0x1014: 'jump', 0x1015: 'jump_false', 0x1016: 'jump_true', 0x1017: 'case'}
SIMPLE = {
    0x0004: 'and_test', 0x0005: 'or_test', 0x0018: 'pop', 0x0020: 'index',
    0x0021: 'new_init', 0x0022: 'this_init', 0x0023: 'super_init',
    0x0028: 'value', 0x0029: 'push_void', 0x002A: 'ret', 0x002B: 'ret_value',
    0x002D: 'store', 0x0010: 'end',
}


# ---------------------------------------------------------------------------
# class side
# ---------------------------------------------------------------------------

def _segment_name(blob, nd):
    code = nd.code
    if code in (0x101A, 0x101B):
        return blob.ref(nd.imm)[1]
    if code == 0x4012:
        return blob.utf8(nd.imm)
    return None                      # local, class


def _head(blob, nd):
    """What a name path starts with: ('class', name), ('local', None),
    ('field', name) or ('name', name)."""
    code = nd.code
    if code == 0x1019:
        return ('class', blob.class_name(nd.imm))
    if code == 0x1001:
        return ('local', None)
    if code == 0x101B:
        return ('field', blob.ref(nd.imm)[1])
    if code == 0x4012:
        return ('name', blob.utf8(nd.imm))
    if code == 0x101A:
        return ('method', blob.ref(nd.imm)[1])
    return ('?', None)


def tree_events(blob, tree, heads=None):
    """Events of one tree (a list of tufa.Node).  Raises ValueError on a node
    sequence the tables do not describe.  heads, when given, receives per
    event what its name path starts with (_head) or None."""
    ev = []
    starts = []
    if heads is None:
        heads = []
    jumps = []                       # (event index, target node index)
    n = len(tree)
    i = 0
    while i < n:
        nd = tree[i]
        code = nd.code
        at = i
        head = None
        if code in (0x1007, 0x1008):
            form = 'e' if code == 0x1007 else 's'
            tag = nd.imm
            name = TAG_NAMES.get(tag)
            if name is None:
                raise ValueError('node %d: unknown expression kind 0x%X' % (i, tag))
            if name == 'literal':
                c = tree[i + 1]
                cc = c.code
                if cc == 0x4009:
                    e = ('lit', 'str', blob.utf8(c.imm))
                elif cc == 0x200A:
                    e = ('lit', 'float', c.imm)
                elif cc == 0x100B:
                    e = ('lit', 'int', c.imm)
                elif cc == 0x000C:
                    e = ('lit', 'null', None)
                elif cc == 0x100D:
                    e = ('lit', 'bool', c.imm)
                elif cc == 0x100E:
                    e = ('lit', 'rid', blob.rid(c.imm)[1])
                else:
                    raise ValueError('node %d: literal without carrier' % i)
                if form != 'e':
                    e = e + ('s',)
                i += 2
            elif name in ('new', 'newarray', 'instanceof', 'cast'):
                c = tree[i + 1]
                if c.code == 0x1006:
                    desc = blob.utf8(c.imm)
                elif c.code == 0x1019:
                    desc = 'L' + blob.class_name(c.imm) + ';'
                else:
                    raise ValueError('node %d: %s without type carrier' % (i, name))
                if name == 'new':
                    e = ('new', form, desc)
                else:
                    e = (name, desc) if form == 'e' else (name, desc, 's')
                i += 2
            else:
                e = ('op', name, form)
                i += 1
        elif code == 0x1001:
            e = ('var', None)
            head = ('local', None)
            i += 1
        elif code == 0x101B:
            e = ('var', blob.ref(nd.imm)[1])
            head = ('field', e[1])
            i += 1
        elif code == 0x1011:
            k = nd.imm
            e = ('var', _segment_name(blob, tree[i + k]))
            head = _head(blob, tree[i + 1]) + (k,)
            i += 1 + k
        elif code == 0x1002:
            desc = blob.utf8(tree[i + 1].imm)
            has_init = i + 2 < n and tree[i + 2].code == 0x002C
            e = ('declare', desc, has_init)
            i += 3 if has_init else 2
        elif code == 0x1003:
            i += 1
            continue
        elif code == 0x1027:
            e = ('argc', nd.imm)
            i += 1
        elif code == 0x0024:
            nx = tree[i + 1]
            if nx.code == 0x101A:
                e = ('call', blob.ref(nx.imm)[1])
                head = ('method', e[1], 1)
                i += 2
            elif nx.code == 0x1011:
                k = nx.imm
                e = ('call', _segment_name(blob, tree[i + 1 + k]))
                head = _head(blob, tree[i + 2]) + (k,)
                i += 2 + k
            else:
                raise ValueError('node %d: call without path' % i)
        elif code == 0x4025:
            e = ('call_on', blob.utf8(nd.imm))
            i += 1
        elif code == 0x4026:
            e = ('call_super', blob.utf8(nd.imm))
            i += 1
        elif code == 0x401C:
            e = ('getfield', blob.utf8(nd.imm))
            i += 1
        elif code in JUMPS:
            imm = nd.imm
            if imm >= 0x80000000:
                imm -= 0x100000000
            jumps.append((len(ev), i + imm))
            e = (JUMPS[code], None)
            i += 1
        elif code in SIMPLE:
            e = (SIMPLE[code],)
            i += 1
        else:
            raise ValueError('node %d: opcode 0x%04X outside the tables' % (i, code))
        ev.append(e)
        starts.append(at)
        heads.append(head)
    for k, target in jumps:
        ev[k] = (ev[k][0], bisect.bisect_left(starts, target))
    return ev


# ---------------------------------------------------------------------------
# source side
# ---------------------------------------------------------------------------

class _Label(object):
    __slots__ = ('pos',)

    def __init__(self):
        self.pos = None


def is_name_chain(e):
    """True for  a  and  a.b.c  (identifiers and dots only)."""
    while e.kind == 'Field':
        e = e.target
    return e.kind == 'Name'


def chain_head(e):
    """(first identifier, number of identifiers) of a name chain."""
    n = 1
    while e.kind == 'Field':
        e = e.target
        n += 1
    return e.name, n


class Emitter(object):
    def __init__(self, resolver):
        self.res = resolver
        self.ev = []
        self.heads = {}          # event index -> (first identifier, segments)
        self.breaks = []         # labels a break leads to
        self.continues = []      # labels a continue leads to

    # -- helpers -------------------------------------------------------------
    def add(self, *e):
        self.ev.append(e)

    def place(self, label):
        label.pos = len(self.ev)

    def finish(self):
        out = []
        for e in self.ev:
            if len(e) == 2 and isinstance(e[1], _Label):
                e = (e[0], e[1].pos)
            out.append(e)
        return out

    # -- expressions -----------------------------------------------------------
    def expr(self, e, form='e'):
        k = e.kind
        if k == 'Paren':
            self.expr(e.expr, form)
        elif k == 'Literal':
            lit = e.lit
            if lit == 'int':
                v = lexer.int_value(e.text)
            elif lit == 'float':
                v = e.text
            elif lit == 'str':
                v = lexer.string_value(e.text)
            elif lit == 'rid':
                v = lexer.rid_parts(e.text)[1] & 0xFFFFFFFF
            elif lit == 'bool':
                v = 1 if e.text == 'true' else 0
            else:
                v = None
            if form == 'e':
                self.add('lit', lit, v)
            else:
                self.add('lit', lit, v, 's')
        elif k == 'Name':
            self.heads[len(self.ev)] = (e.name, 1)
            self.add('var', e.name)
        elif k == 'This':
            self.add('var', '#this')
        elif k == 'Field':
            if is_name_chain(e):
                self.heads[len(self.ev)] = chain_head(e)
                self.add('var', e.name)
            else:
                self.expr(e.target)
                self.add('getfield', e.name)
        elif k == 'Index':
            self.expr(e.index)
            self.expr(e.target)
            self.add('index')
        elif k == 'Call':
            for a in e.args:
                self.expr(a)
            self.add('argc', len(e.args))
            t = e.target
            if t is None or is_name_chain(t):
                if t is not None:
                    first, n = chain_head(t)
                    self.heads[len(self.ev)] = (first, n + 1)
                else:
                    self.heads[len(self.ev)] = (None, 1)
                self.add('call', e.name)
            elif t.kind == 'Super':
                self.add('call_super', e.name)
            else:
                self.expr(t)
                self.add('call_on', e.name)
            self.add('op', 'call', form)
        elif k == 'CtorCall':
            for a in e.args:
                self.expr(a)
            self.add('argc', len(e.args))
            self.add('this_init' if e.which == 'this' else 'super_init')
            self.add('op', 'ctor', form)
        elif k == 'New':
            self.add('new', form, self.res.descriptor(e.type))
            for a in e.args:
                self.expr(a)
            self.add('argc', len(e.args))
            self.add('new_init')
        elif k == 'NewArray':
            for s in e.sizes:
                self.expr(s)
            desc = '[' * (len(e.sizes) + e.dims) + self.res.descriptor(e.type)
            if form == 'e':
                self.add('newarray', desc)
            else:
                self.add('newarray', desc, 's')
        elif k == 'Unary':
            self.expr(e.operand)
            if e.op != '+':
                self.add('op', UNARY_NAMES[e.op], form)
        elif k == 'Postfix':
            self.expr(e.operand)
            self.add('op', POSTFIX_NAMES[e.op], form)
        elif k == 'Binary':
            if e.op in ('&&', '||'):
                skip = _Label()
                self.expr(e.left)
                self.add('and_test' if e.op == '&&' else 'or_test')
                self.add('jump', skip)
                self.expr(e.right)
                self.add('op', BINARY_NAMES[e.op], form)
                self.place(skip)
            else:
                self.expr(e.left)
                self.expr(e.right)
                self.add('op', BINARY_NAMES[e.op], form)
        elif k == 'Assign':
            self.expr(e.value)
            self.expr(e.target)
            self.add('op', ASSIGN_NAMES[e.op], form)
        elif k == 'Cond':
            self.expr(e.cond)
            self.expr(e.then)
            self.expr(e.otherwise)
            self.add('op', 'cond', form)
        elif k == 'Cast':
            self.expr(e.operand)
            if form == 'e':
                self.add('cast', self.res.descriptor(e.type))
            else:
                self.add('cast', self.res.descriptor(e.type), 's')
        elif k == 'InstanceOf':
            self.expr(e.operand)
            if form == 'e':
                self.add('instanceof', self.res.descriptor(e.type))
            else:
                self.add('instanceof', self.res.descriptor(e.type), 's')
        else:
            raise ValueError('no shape for expression %s' % k)

    # -- statements ------------------------------------------------------------
    def local_var(self, s):
        for d in s.declarators:
            self.add('declare', self.res.descriptor(s.type, d.dims), d.init is not None)
            if d.init is not None:
                self.expr(d.init)
                self.add('store')

    def stmts(self, stmts):
        for s in stmts:
            self.stmt(s)

    def stmt(self, s):
        k = s.kind
        if k == 'Block':
            self.stmts(s.stmts)
        elif k == 'Empty':
            pass
        elif k == 'ExprStmt':
            self.expr(s.expr, 's')
        elif k == 'LocalVar':
            self.local_var(s)
        elif k == 'If':
            l_else = _Label()
            self.expr(s.cond)
            self.add('jump_false', l_else)
            self.stmt(s.then)
            if s.otherwise is not None:
                l_end = _Label()
                self.add('jump', l_end)
                self.place(l_else)
                self.stmt(s.otherwise)
                self.place(l_end)
            else:
                self.place(l_else)
        elif k == 'While':
            l_top, l_end = _Label(), _Label()
            self.place(l_top)
            self.expr(s.cond)
            self.add('jump_false', l_end)
            self.loop_body(s.body, l_end, l_top)
            self.add('jump', l_top)
            self.place(l_end)
        elif k == 'Do':
            l_top, l_cond, l_end = _Label(), _Label(), _Label()
            self.place(l_top)
            self.loop_body(s.body, l_end, l_cond)
            self.place(l_cond)
            self.expr(s.cond)
            self.add('jump_true', l_top)
            self.place(l_end)
        elif k == 'For':
            l_top, l_update, l_end = _Label(), _Label(), _Label()
            if isinstance(s.init, list):
                for e in s.init:
                    self.expr(e, 's')
            else:
                self.local_var(s.init)
            self.place(l_top)
            if s.cond is not None:
                self.expr(s.cond)
                self.add('jump_false', l_end)
            self.loop_body(s.body, l_end, l_update)
            self.place(l_update)
            for e in s.update:
                self.expr(e, 's')
            self.add('jump', l_top)
            self.place(l_end)
        elif k == 'Switch':
            l_end = _Label()
            l_default = None
            self.expr(s.expr)
            labels = []
            for g in s.groups:
                lg = _Label()
                labels.append(lg)
                for lab in g.labels:
                    if lab.kind == 'CaseLabel':
                        self.expr(lab.value)
                        self.add('case', lg)
                    else:
                        l_default = lg
            self.add('pop')
            self.add('jump', l_default if l_default is not None else l_end)
            self.breaks.append(l_end)
            for g, lg in zip(s.groups, labels):
                self.place(lg)
                self.stmts(g.stmts)
            self.breaks.pop()
            self.place(l_end)
        elif k == 'Break':
            self.add('jump', self.breaks[-1])
        elif k == 'Continue':
            self.add('jump', self.continues[-1])
        elif k == 'Return':
            if s.value is not None:
                self.expr(s.value)
                self.add('value')
                self.add('ret_value')
            elif self.constructor:
                self.add('ret')
            else:
                self.add('push_void')
                self.add('ret_value')
        elif k == 'Labeled':
            self.stmt(s.body)
        else:
            raise ValueError('no shape for statement %s' % k)

    def loop_body(self, body, l_break, l_continue):
        self.breaks.append(l_break)
        self.continues.append(l_continue)
        self.stmt(body)
        self.breaks.pop()
        self.continues.pop()

    constructor = False


def method_events(m, resolver, heads=None):
    """Events of a MethodDecl that has code (symbols.has_code).  heads, a
    dict, receives per event index of a name path (first identifier, number
    of identifiers)."""
    em = Emitter(resolver)
    if heads is not None:
        em.heads = heads
    em.constructor = m.constructor
    body = m.body.stmts
    if m.constructor:
        first = body[0] if body else None
        if not (first is not None and first.kind == 'ExprStmt' and
                first.expr.kind == 'CtorCall'):
            em.add('argc', 0)
            em.add('super_init')
            em.add('op', 'ctor', 's')
    em.stmts(body)
    if not (body and body[-1].kind == 'Return'):
        if m.constructor:
            em.add('ret')
        else:
            em.add('push_void')
            em.add('ret_value')
    return em.finish()


def initialiser_events(expr, resolver, heads=None):
    em = Emitter(resolver)
    if heads is not None:
        em.heads = heads
    em.expr(expr)
    em.add('end')
    return em.finish()


# ---------------------------------------------------------------------------
# comparison
# ---------------------------------------------------------------------------

def same_event(src, cls):
    """Does a source event describe a class event?  A local carries no name
    in the class and a float literal is compared by kind only (the bits
    depend on the compiler's own conversion, notes/foundation.md section 7)."""
    if src == cls:
        return True
    if src[0] != cls[0] or len(src) != len(cls):
        return False
    if src[0] == 'var':
        return cls[1] is None
    if src[0] == 'lit' and src[1] == 'float' and cls[1] == 'float':
        return True
    return False


def first_difference(src, cls):
    """Index of the first event that differs, or None when the sequences
    describe the same tree."""
    n = min(len(src), len(cls))
    for i in range(n):
        if not same_event(src[i], cls[i]):
            return i
    if len(src) != len(cls):
        return n
    return None
