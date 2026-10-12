#!/usr/bin/env python3
"""decomp.py - turn a TREE node stream back into statements and expressions.

The node stream is postfix, so one left-to-right pass with a stack of partial
expressions rebuilds every expression.  The pass accepts exactly the shapes
that notes/expressions.md lists; anything else raises Unattributed with the
node index, so running it over the whole corpus (check_decomp.py) is the proof
that no other shape occurs in expression position.

    import decomp
    stmts = decomp.decompile(blob, nodes)      # [Stmt]
    Stmt.kind     'expr' (expression statement), 'decl', 'cond' (operand of
                  jump_false / jump_true), 'case', 'return', 'init-end'
                  (field initialiser), 'ctor'
    Stmt.e        the expression (an X) or None
    Stmt.at       node index of the node that ended the statement

    X.k           kind:
       lit        a = (carrier mnemonic, operand)
       local      a = index
       field      a = (class, name, desc)            a lone field ref
       path       a = [segment, ...]  segment = ('local', i) | ('field', ref)
                                                 | ('class', name) | ('name', utf8)
       getfield   a = name, x = [receiver]
       index      x = [index, array]
       op         a = tag name, x = operands in stack order (first pushed first)
       land/lor   x = [left, right]
       cast       a = ('class', name) | ('type', desc), x = [value]
       instanceof a = desc, x = [value]
       new        a = desc, x = arguments
       newarray   a = desc, x = [size]
       call       a = (segments or None, ('method', ref) | ('name', utf8)), x = arguments
       call_on    a = name, x = arguments + [receiver]
       call_super a = name, x = arguments
       void       the dummy value of push_void
    X.lo, X.hi    first and last node index of the expression
    X.used        'expr' or 'stmt' for kinds that carry the expr/stmt marker
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import symbolic   # noqa: E402

UNARY = frozenset(('bnot', 'lnot', 'neg', 'postinc', 'postdec', 'preinc', 'predec'))
BINARY = frozenset(('bor', 'bxor', 'band', 'ne', 'eq', 'ge', 'le', 'gt', 'lt',
                    'shr', 'shl', 'sub', 'add', 'mod', 'div', 'mul'))
ASSIGN = frozenset(('assign', 'mul_assign', 'div_assign', 'mod_assign',
                    'add_assign', 'sub_assign', 'and_assign', 'xor_assign',
                    'or_assign'))
CARRIERS = frozenset(('str', 'float', 'int', 'null', 'bool', 'rid'))


class Unattributed(ValueError):
    def __init__(self, at, msg):
        ValueError.__init__(self, 'node %d: %s' % (at, msg))
        self.at = at
        self.msg = msg


class X(object):
    __slots__ = ('k', 'a', 'x', 'lo', 'hi', 'used')

    def __init__(self, k, a=None, x=(), lo=0, hi=0, used=None):
        self.k = k
        self.a = a
        self.x = list(x)
        self.lo = lo
        self.hi = hi
        self.used = used

    def __repr__(self):
        return 'X(%s %r %r)' % (self.k, self.a, self.x)


class Stmt(object):
    __slots__ = ('kind', 'e', 'at', 'a')

    def __init__(self, kind, e, at, a=None):
        self.kind = kind
        self.e = e
        self.at = at
        self.a = a


class _Mark(object):
    """Stack markers that are not values: the argument count box, the object
    of a `new` under its arguments, the variable of a declaration."""
    __slots__ = ('k', 'a', 'at')

    def __init__(self, k, a, at):
        self.k = k
        self.a = a
        self.at = at


def decompile(blob, nodes, sym=None):
    if sym is None:
        sym = symbolic.sym_nodes(blob, nodes)
    n = len(sym)
    stack = []
    out = []
    pending = []          # (kind, index of the jump, target) of open && / ||
    i = 0

    def pop_x(at, what):
        if not stack:
            raise Unattributed(at, 'empty stack, expected %s' % what)
        v = stack.pop()
        if not isinstance(v, X):
            raise Unattributed(at, 'expected %s, found marker %s' % (what, v.k))
        return v

    def pop_args(at):
        if not stack or not isinstance(stack[-1], _Mark) or stack[-1].k != 'argc':
            raise Unattributed(at, 'call form without an argument count box')
        cnt = stack.pop()
        args = []
        for _ in range(cnt.a):
            args.append(pop_x(at, 'argument'))
        args.reverse()
        return args, cnt.at

    def segments(first, count, at, call):
        segs = []
        for j in range(first, first + count):
            if j >= n:
                raise Unattributed(at, 'path runs off the tree')
            mn, op = sym[j]
            last = j == first + count - 1
            if j == first:
                ok = mn in ('local', 'field', 'class') or (call and mn == 'name' and count == 1)
            elif call and last:
                ok = mn in ('method', 'name')
            else:
                ok = mn in ('field', 'name')
            if not ok:
                raise Unattributed(j, 'unexpected path segment %s' % mn)
            if mn == 'class' and count < 2:
                raise Unattributed(j, 'class segment alone')
            segs.append((mn, op))
        return segs

    while i < n:
        mn, op = sym[i]
        if mn in ('expr', 'stmt'):
            tag = op
            if tag == 'literal':
                if i + 1 >= n or sym[i + 1][0] not in CARRIERS or mn != 'expr':
                    raise Unattributed(i, 'literal without carrier')
                stack.append(X('lit', sym[i + 1], lo=i, hi=i + 1))
                i += 2
                continue
            if tag in UNARY:
                x = pop_x(i, 'operand')
                e = X('op', tag, [x], x.lo, i, mn)
            elif tag in BINARY or tag in ASSIGN:
                b = pop_x(i, 'operand')
                a = pop_x(i, 'operand')
                e = X('op', tag, [a, b], a.lo, i, mn)
            elif tag == 'cond':
                c = pop_x(i, 'operand')
                b = pop_x(i, 'operand')
                a = pop_x(i, 'operand')
                e = X('op', tag, [a, b, c], a.lo, i, mn)
            elif tag in ('land', 'lor'):
                b = pop_x(i, 'operand')
                a = pop_x(i, 'operand')
                if not pending or pending[-1][0] != tag:
                    raise Unattributed(i, '%s without its test' % tag)
                kind, jat, target = pending.pop()
                if target != i + 1:
                    raise Unattributed(i, 'skip of %s lands at %d, not behind it' % (tag, target))
                if not (a.hi == jat and b.lo == jat + 1):
                    raise Unattributed(i, 'operands of %s do not frame the test' % tag)
                e = X(tag, None, [a, b], a.lo, i, mn)
            elif tag == 'call':
                x = pop_x(i, 'call')
                if x.k not in ('call', 'call_on', 'call_super') or x.hi != i - 1 or x.used:
                    raise Unattributed(i, '%s call not directly behind a call form' % mn)
                x.used = mn
                x.hi = i
                e = x
            elif tag == 'new':
                if i + 1 >= n or sym[i + 1][0] != 'type':
                    raise Unattributed(i, 'new without type carrier')
                stack.append(_Mark('new', (sym[i + 1][1], mn), i))
                i += 2
                continue
            elif tag == 'newarray':
                if i + 1 >= n or sym[i + 1][0] != 'type':
                    raise Unattributed(i, 'newarray without type carrier')
                x = pop_x(i, 'size')
                e = X('newarray', sym[i + 1][1], [x], x.lo, i + 1, mn)
                i += 1
            elif tag == 'instanceof':
                if i + 1 >= n or sym[i + 1][0] != 'type':
                    raise Unattributed(i, 'instanceof without type carrier')
                x = pop_x(i, 'operand')
                e = X('instanceof', sym[i + 1][1], [x], x.lo, i + 1, mn)
                i += 1
            elif tag == 'cast':
                if i + 1 >= n or sym[i + 1][0] not in ('type', 'class'):
                    raise Unattributed(i, 'cast without carrier')
                x = pop_x(i, 'operand')
                e = X('cast', sym[i + 1], [x], x.lo, i + 1, mn)
                i += 1
            elif tag == 'ctor':
                if mn != 'stmt' or not out or out[-1].kind != 'ctor' or out[-1].at != i - 1:
                    raise Unattributed(i, 'ctor marker not behind this_init / super_init')
                out[-1].at = i
                i += 1
                continue
            else:
                raise Unattributed(i, 'tag %s' % tag)
            if mn == 'stmt':
                out.append(Stmt('expr', e, i))
            else:
                stack.append(e)
            i += 1
            continue
        if mn == 'local':
            stack.append(X('local', op, lo=i, hi=i))
        elif mn == 'field':
            stack.append(X('field', op, lo=i, hi=i))
        elif mn == 'path':
            if op < 2:
                raise Unattributed(i, 'field path of %d segments' % op)
            segs = segments(i + 1, op, i, False)
            stack.append(X('path', segs, lo=i, hi=i + op))
            i += op
        elif mn == 'getfield':
            x = pop_x(i, 'receiver')
            stack.append(X('getfield', op, [x], x.lo, i))
        elif mn == 'index':
            arr = pop_x(i, 'array')
            idx = pop_x(i, 'index')
            stack.append(X('index', None, [idx, arr], idx.lo, i))
        elif mn in ('and_test', 'or_test'):
            if not stack or not isinstance(stack[-1], X) or stack[-1].hi != i - 1:
                raise Unattributed(i, '%s without a left operand' % mn)
            if i + 1 >= n or sym[i + 1][0] != 'jump':
                raise Unattributed(i, '%s without its jump' % mn)
            pending.append(('land' if mn == 'and_test' else 'lor', i + 1, i + 1 + sym[i + 1][1]))
            stack[-1].hi = i + 1      # the test and its jump belong to the left operand
            i += 1
        elif mn == 'push_int':
            stack.append(_Mark('argc', op, i))
        elif mn == 'call':
            args, cat = pop_args(i)
            if i + 1 >= n:
                raise Unattributed(i, 'call at the end of the tree')
            m2, o2 = sym[i + 1]
            lo = args[0].lo if args else cat
            if m2 == 'method':
                stack.append(X('call', (None, (m2, o2)), args, lo, i + 1))
                i += 1
            elif m2 == 'path':
                segs = segments(i + 2, o2, i, True)
                if segs[-1][0] not in ('method', 'name'):
                    raise Unattributed(i, 'call path does not end in a method')
                stack.append(X('call', (segs[:-1], segs[-1]), args, lo, i + 1 + o2))
                i += 1 + o2
            else:
                raise Unattributed(i, 'call followed by %s' % m2)
        elif mn == 'call_on':
            recv = pop_x(i, 'receiver')
            args, cat = pop_args(i)
            lo = args[0].lo if args else cat
            stack.append(X('call_on', op, args + [recv], lo, i))
        elif mn == 'call_super':
            args, cat = pop_args(i)
            lo = args[0].lo if args else cat
            stack.append(X('call_super', op, args, lo, i))
        elif mn == 'new_init':
            args, cat = pop_args(i)
            if not stack or not isinstance(stack[-1], _Mark) or stack[-1].k != 'new':
                raise Unattributed(i, 'new_init without expr new')
            mk = stack.pop()
            e = X('new', mk.a[0], args, mk.at, i, mk.a[1])
            if mk.a[1] == 'stmt':
                out.append(Stmt('expr', e, i))
            else:
                stack.append(e)
        elif mn in ('this_init', 'super_init'):
            args, cat = pop_args(i)
            out.append(Stmt('ctor', X('ctor', mn, args, args[0].lo if args else cat, i), i))
        elif mn == 'declare':
            if i + 1 >= n or sym[i + 1][0] != 'type':
                raise Unattributed(i, 'declare without type')
            if i + 2 < n and sym[i + 2][0] == 'init':
                stack.append(_Mark('decl', (op, sym[i + 1][1]), i))
                i += 2
            else:
                out.append(Stmt('decl', None, i + 1, (op, sym[i + 1][1])))
                i += 1
        elif mn == 'store':
            v = pop_x(i, 'initialiser')
            if not stack or not isinstance(stack[-1], _Mark) or stack[-1].k != 'decl':
                raise Unattributed(i, 'store without declaration')
            mk = stack.pop()
            out.append(Stmt('decl', v, i, mk.a))
        elif mn in ('jump_false', 'jump_true'):
            out.append(Stmt('cond', pop_x(i, 'condition'), i, mn))
        elif mn == 'case':
            v = pop_x(i, 'case value')
            if not stack or not isinstance(stack[-1], X):
                raise Unattributed(i, 'case without subject')
            out.append(Stmt('case', v, i))
        elif mn == 'pop':
            out.append(Stmt('switch', pop_x(i, 'switch subject'), i))
        elif mn == 'push_void':
            stack.append(X('void', None, lo=i, hi=i))
        elif mn == 'value':
            if not stack or not isinstance(stack[-1], X):
                raise Unattributed(i, 'value without operand')
        elif mn == 'ret_value':
            out.append(Stmt('return', pop_x(i, 'return value'), i))
        elif mn == 'end':
            out.append(Stmt('init-end', pop_x(i, 'initialiser value'), i))
        elif mn in ('release', 'jump', 'ret'):
            pass
        else:
            raise Unattributed(i, 'node %s outside any known shape' % mn)
        i += 1
    if pending:
        raise Unattributed(n, 'unfinished && / ||')
    # after a switch whose case matched the VM has dropped the subject; the
    # linear pass has dropped it at `pop`.  Nothing may be left.
    if stack:
        raise Unattributed(n, '%d items left on the stack' % len(stack))
    return out


def walk(e):
    """Every X below (and including) e, operands first."""
    for c in e.x:
        for y in walk(c):
            yield y
    yield e


def render(e):
    """Compact text of an expression (for reports)."""
    k = e.k
    if k == 'lit':
        mn, op = e.a
        if mn == 'str':
            return '"%s"' % op
        if mn == 'null':
            return 'null'
        if mn == 'float':
            return 'float:%08X' % op
        if mn == 'rid':
            return '%s:0x%X' % op
        return str(op)
    if k == 'local':
        return 'L%d' % e.a
    if k == 'field':
        return e.a[1]
    if k == 'path':
        return '.'.join(str(s[1][1]) if s[0] == 'field' else
                        ('L%d' % s[1] if s[0] == 'local' else str(s[1])) for s in e.a)
    if k == 'getfield':
        return '%s.%s' % (render(e.x[0]), e.a)
    if k == 'index':
        return '%s[%s]' % (render(e.x[1]), render(e.x[0]))
    if k == 'op':
        return '%s(%s)' % (e.a, ', '.join(render(c) for c in e.x))
    if k in ('land', 'lor'):
        return '%s(%s, %s)' % (k, render(e.x[0]), render(e.x[1]))
    if k == 'cast':
        return '(%s)%s' % (e.a[1], render(e.x[0]))
    if k == 'instanceof':
        return '%s instanceof %s' % (render(e.x[0]), e.a)
    if k == 'new':
        return 'new %s(%s)' % (e.a, ', '.join(render(c) for c in e.x))
    if k == 'newarray':
        return 'new %s{%s}' % (e.a, render(e.x[0]))
    if k == 'call':
        segs, cal = e.a
        pre = ''
        if segs:
            pre = '.'.join(str(s[1][1]) if s[0] == 'field' else
                           ('L%d' % s[1] if s[0] == 'local' else str(s[1])) for s in segs) + '.'
        nm = cal[1][1] if cal[0] == 'method' else cal[1]
        return '%s%s(%s)' % (pre, nm, ', '.join(render(c) for c in e.x))
    if k == 'call_on':
        return '%s.%s(%s)' % (render(e.x[-1]), e.a, ', '.join(render(c) for c in e.x[:-1]))
    if k == 'call_super':
        return 'super.%s(%s)' % (e.a, ', '.join(render(c) for c in e.x))
    if k == 'ctor':
        return '%s(%s)' % (e.a, ', '.join(render(c) for c in e.x))
    if k == 'void':
        return 'void'
    return k
