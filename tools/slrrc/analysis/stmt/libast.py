#!/usr/bin/env python3
"""libast.py - trees of lib/parser.py in the shape stmtgen.py reads.

stmtgen.py was written against sparse.py.  lib/parser.py (the parser of the
compiler prototype) came later; check_parsers.py shows that the two produce
the same tree with the same token spans for every source.  This adapter turns
a lib/parser Unit into the node shape of sparse.py, so that the statement
model runs on the prototype's own trees:

    import libast
    unit = libast.parse(text)          # same attributes as sparse.parse(text)
    g = stmtgen.Gen(unit, blob, blob.trees[t])

`python -I check_stmt.py --lib` runs the whole proof through this adapter.

The only structural change is the dotted name: lib/parser nests
Field(Field(Name a, b), c), stmtgen wants the identifiers of `a.b.c` as one
name (the compiler emits one path for it), and it wants the three kinds of
call apart: through a name (`f(..)`, `a.b.f(..)`), on a computed value
(`x().f(..)`), and `super.f(..)`.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import sparse  # noqa: E402

N = sparse.N


def _type(ty, t1=None, dims=None):
    return N('type', ty.tok0, ty.tok1 if t1 is None else t1, parts=ty.name.split('.'),
             dims=ty.dims if dims is None else dims)


def expr(e):
    if e is None:
        return None
    k = e.kind
    a, z = e.tok0, e.tok1
    if k == 'Literal':
        return N('lit', a, z, lk=e.lit, tok=a)
    if k == 'Name':
        return N('name', a, z, parts=[a])
    if k == 'This':
        return N('this', a, z)
    if k == 'Super':
        return N('super', a, z)
    if k == 'Field':
        t = expr(e.target)
        if t.k == 'name':
            return N('name', a, z, parts=t.parts + [e.name_tok])
        return N('field', a, z, obj=t, name_tok=e.name_tok)
    if k == 'Call':
        args = [expr(x) for x in e.args]
        if e.target is None:
            return N('call', a, z, name=N('name', e.name_tok, e.name_tok, parts=[e.name_tok]),
                     args=args)
        if e.target.kind == 'Super':
            return N('scall', a, z, name_tok=e.name_tok, args=args)
        t = expr(e.target)
        if t.k == 'name':
            return N('call', a, z, name=N('name', t.t0, e.name_tok, parts=t.parts + [e.name_tok]),
                     args=args)
        return N('mcall', a, z, obj=t, name_tok=e.name_tok, args=args)
    if k == 'CtorCall':
        return N('ctor', a, z, which=e.which, args=[expr(x) for x in e.args])
    if k == 'Index':
        return N('index', a, z, arr=expr(e.target), idx=expr(e.index))
    if k == 'New':
        return N('new', a, z, type=_type(e.type), args=[expr(x) for x in e.args])
    if k == 'NewArray':
        return N('newarr', a, z,
                 type=_type(e.type, t1=z, dims=e.type.dims + len(e.sizes) + e.dims),
                 sizes=[expr(x) for x in e.sizes])
    if k == 'Unary':
        return N('un', a, z, op=e.op, e=expr(e.operand))
    if k == 'Postfix':
        return N('post', a, z, op=e.op, e=expr(e.operand))
    if k == 'Cast':
        return N('cast', a, z, type=_type(e.type), e=expr(e.operand), rparen=e.type.tok1 + 1)
    if k == 'Binary':
        return N('bin', a, z, op=e.op, l=expr(e.left), r=expr(e.right), op_tok=e.op_tok)
    if k == 'InstanceOf':
        return N('instanceof', a, z, e=expr(e.operand), type=_type(e.type), op_tok=e.op_tok)
    if k == 'Cond':
        return N('cond', a, z, c=expr(e.cond), a=expr(e.then), b=expr(e.otherwise),
                 q_tok=e.op_tok)
    if k == 'Assign':
        return N('assign', a, z, op=e.op, l=expr(e.target), r=expr(e.value), op_tok=e.op_tok)
    if k == 'Paren':
        return N('paren', a, z, e=expr(e.expr))
    raise ValueError('lib/parser expression kind %s has no counterpart' % k)


def _decls(ds):
    return [N('decl', d.tok0, d.tok1, name=d.name, name_tok=d.tok0, dims=d.dims,
              init=expr(d.init), eq_tok=None) for d in ds]


def stmt(s):
    if s is None:
        return None
    k = s.kind
    a, z = s.tok0, s.tok1
    if k == 'Block':
        return N('block', a, z, stmts=[stmt(x) for x in s.stmts])
    if k == 'Empty':
        return N('empty', a, z)
    if k == 'LocalVar':
        return N('local', a, z, type=_type(s.type), decls=_decls(s.declarators),
                 final='final' in s.modifiers)
    if k == 'ExprStmt':
        return N('expr', a, z, e=expr(s.expr))
    if k == 'If':
        return N('if', a, z, cond=expr(s.cond), then=stmt(s.then), els=stmt(s.otherwise),
                 rparen=None, else_tok=None)
    if k == 'While':
        return N('while', a, z, cond=expr(s.cond), body=stmt(s.body), rparen=None)
    if k == 'Do':
        return N('do', a, z, body=stmt(s.body), cond=expr(s.cond), while_tok=None,
                 rparen=z - 1, semi=z)
    if k == 'For':
        if isinstance(s.init, list):
            init = [expr(x) for x in s.init] or None
        else:
            init = stmt(s.init)
        return N('for', a, z, init=init, cond=expr(s.cond), update=[expr(x) for x in s.update],
                 body=stmt(s.body), semi1=None, semi2=None, rparen=None)
    if k == 'Switch':
        groups = []
        for g in s.groups:
            labels = []
            for c in g.labels:
                if c.kind == 'CaseLabel':
                    labels.append(N('case', c.tok0, c.tok1, e=expr(c.value)))
                else:
                    labels.append(N('default', c.tok0, c.tok1))
            groups.append(N('group', g.tok0, g.tok1, labels=labels,
                            stmts=[stmt(x) for x in g.stmts]))
        return N('switch', a, z, subj=expr(s.expr), groups=groups, rparen=s.expr.tok1 + 1,
                 lbrace=s.expr.tok1 + 2)
    if k == 'Break':
        return N('break', a, z)
    if k == 'Continue':
        return N('continue', a, z)
    if k == 'Return':
        return N('return', a, z, e=expr(s.value))
    raise ValueError('lib/parser statement kind %s has no counterpart' % k)


def convert(pu):
    """lib/parser Unit -> an object with the attributes of sparse.Unit."""
    u = sparse.Unit()
    u.toks = pu.tokens
    u.package = pu.package
    u.imports = [i.name for i in pu.imports]
    for c in pu.types:
        members = []
        for m in c.members:
            if m.kind == 'FieldDecl':
                members.append(N('field', m.tok0, m.tok1, mods=list(m.modifiers),
                                 type=_type(m.type), decls=_decls(m.declarators)))
            elif m.kind == 'MethodDecl':
                params = [N('param', p.tok0, p.tok1, type=_type(p.type), name=p.name,
                            name_tok=None, dims=p.dims) for p in m.params]
                members.append(N('method', m.tok0, m.tok1, mods=list(m.modifiers),
                                 ret=_type(m.type) if m.type is not None else None,
                                 name=m.name, name_tok=None, params=params,
                                 body=stmt(m.body),
                                 body_form='block' if m.body is not None else 'semi',
                                 ctor=bool(m.constructor)))
            else:
                members.append(N('initblock', m.tok0, m.tok1, mods=[], body=stmt(m.body)))
        u.classes.append(N('class', c.tok0, c.tok1, mods=list(c.modifiers),
                           kind='interface' if c.interface else 'class', name=c.name,
                           extends=c.extends, implements=list(c.implements),
                           members=members, lbrace=None))
    return u


def parse(text, name=None):
    import parser as libparser      # lib/parser.py
    return convert(libparser.parse(text, name=name))
