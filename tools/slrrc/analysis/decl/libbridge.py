#!/usr/bin/env python3
"""libbridge.py - run the pool model on the AST of lib/parser.py.

poolmodel.py was developed on its own analysis parser (srcparse.py).  The
language work produced lib/parser.py, which is the parser the compiler will
be built on.  This module converts a lib/parser.py Unit into the small tree
poolmodel reads, so that the model runs unchanged on either parser:

    import parser as libparser          # compiler/lib/parser.py
    import libbridge, poolmodel, universe
    unit = libbridge.convert(libparser.parse(text))
    m = poolmodel.Model(unit, unit.classes[0], blob, universe.load()).run()

check_bridge.py proves that both parsers give the same pool and the same
rows for every class of every source.

Only what the model needs is carried over: declarations in full; of bodies
the statements that open scopes or declare locals, and the expressions that
carry a type (new, array creation, cast, instanceof); everything else as a
generic node with its children in source order.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))
sys.path.insert(0, HERE)

from srcparse import N  # noqa: E402


def _n(k, node, **kw):
    d = N(k=k, tok=node.tok0, line=node.line)
    d.update(kw)
    return d


def _type(t):
    return _n('type', t, base=t.name, dims=t.dims)


def convert(unit):
    """lib/parser.py Unit -> the unit node poolmodel reads."""
    u = N(k='unit', package=unit.package, imports=[], classes=[], stray=0)
    for i in unit.imports:
        u.imports.append(N(k='import', name=i.name, star=i.star, line=i.line,
                           odd=i.pack is not None))
    for c in unit.types:
        u.classes.append(_class(c))
    return u


def _class(c):
    out = _n('class', c, kind='interface' if c.interface else 'class',
             mods=list(c.modifiers), name=c.name, extends=c.extends,
             implements=list(c.implements), members=[], stray=0)
    for m in c.members:
        if m.kind == 'FieldDecl':
            ty = _type(m.type)
            for d in m.declarators:
                out.members.append(_n(
                    'field', d, mods=list(m.modifiers), type=ty, name=d.name,
                    dims=d.dims, init=_expr(d.init) if d.init is not None else None,
                    decl_tok=m.tok0))
        elif m.kind == 'MethodDecl':
            params = [_n('param', p, type=_type(p.type), name=p.name, dims=p.dims,
                         final='final' in p.modifiers) for p in m.params]
            out.members.append(_n(
                'ctor' if m.constructor else 'method', m, mods=list(m.modifiers),
                name='<init>' if m.constructor else m.name,
                ret=None if m.constructor else _type(m.type), params=params,
                body=_stmt(m.body) if m.body is not None else None))
        else:
            raise ValueError('member kind %s is not used by any source' % m.kind)
    return out


def _local(s):
    return _n('local', s, type=_type(s.type), final='final' in s.modifiers,
              decls=[_n('decl', d, name=d.name, dims=d.dims,
                        init=_expr(d.init) if d.init is not None else None)
                     for d in s.declarators])


def _stmt(s):
    k = s.kind
    if k == 'Block':
        return _n('block', s, stmts=[_stmt(x) for x in s.stmts])
    if k == 'LocalVar':
        return _local(s)
    if k == 'ExprStmt':
        return _n('expr', s, e=_expr(s.expr))
    if k == 'If':
        return _n('if', s, c=_expr(s.cond), a=_stmt(s.then),
                  b=_stmt(s.otherwise) if s.otherwise is not None else None)
    if k == 'While':
        return _n('while', s, c=_expr(s.cond), body=_stmt(s.body))
    if k == 'Do':
        return _n('do', s, body=_stmt(s.body), c=_expr(s.cond))
    if k == 'For':
        if isinstance(s.init, list):
            init = [_n('expr', x, e=_expr(x)) for x in s.init]
        else:
            init = [_local(s.init)]
        return _n('for', s, init=init,
                  cond=_expr(s.cond) if s.cond is not None else None,
                  upd=[_expr(x) for x in s.update], body=_stmt(s.body))
    if k == 'Switch':
        items = []
        for g in s.groups:
            for lab in g.labels:
                if lab.kind == 'CaseLabel':
                    items.append(_n('case', lab, e=_expr(lab.value)))
                else:
                    items.append(_n('default', lab))
            items.extend(_stmt(x) for x in g.stmts)
        d = _n('switch', s, e=_expr(s.expr))
        d['items'] = items
        return d
    if k == 'Return':
        return _n('return', s, e=_expr(s.value) if s.value is not None else None)
    if k in ('Break', 'Continue', 'Empty'):
        return _n(k.lower(), s)
    raise ValueError('statement kind %s is not used by any source' % k)


def _expr(e):
    k = e.kind
    if k == 'New':
        return _n('new', e, type=_type(e.type), args=[_expr(a) for a in e.args])
    if k == 'NewArray':
        ty = _n('type', e.type, base=e.type.name, dims=len(e.sizes) + e.dims)
        return _n('newarray', e, type=ty, size=_expr(e.sizes[0]))
    if k == 'Cast':
        return _n('cast', e, type=_type(e.type), e=_expr(e.operand))
    if k == 'InstanceOf':
        return _n('instanceof', e, e=_expr(e.operand), type=_type(e.type))
    # everything else: a generic node with its child expressions
    kids = [_expr(x) for x in e.children() if x.kind != 'Type']
    return _n('x' + k, e, kids=kids)
