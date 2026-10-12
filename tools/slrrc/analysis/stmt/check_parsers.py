#!/usr/bin/env python3
"""check_parsers.py - sparse.py against lib/parser.py.

The statement study was built on its own parser (sparse.py) because
lib/parser.py did not exist yet.  Both keep the first and last token of every
syntax node, and the emitter of this study (stmtgen.py) depends on nothing
else, so if the two parsers agree, the rules of notes/statements.md carry over
to trees of lib/parser.py unchanged.

For every source of the corpus both trees are brought to one canonical form
(kind, line and text of the first token, line and text of the last token,
children) and compared.  Dotted names are the only structural difference:
sparse keeps `a.b.c` as one name, lib/parser nests Field(Field(Name a, b), c);
the canonical form is the nested one.

Writes out/check_parsers.txt and .json.
"""
import json
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import oracle   # noqa: E402
import sparse   # noqa: E402

try:
    import parser as libparser   # lib/parser.py (shadows nothing in Python 3.10+)
except ImportError:              # pragma: no cover
    libparser = None


# ---- canonical form of a sparse tree ------------------------------------

class S(object):
    def __init__(self, unit):
        self.t = unit.toks

    def span(self, a, b):
        ta, tb = self.t[a], self.t[b]
        return (ta.line, ta.text, tb.line, tb.text)

    def typ(self, ty):
        return ('.'.join(ty.parts), ty.dims)

    def name_chain(self, parts):
        node = ('Name', self.span(parts[0], parts[0]), self.t[parts[0]].text)
        for p in parts[1:]:
            node = ('Field', self.span(parts[0], p), node, self.t[p].text)
        return node

    def expr(self, e):
        if e is None:
            return None
        k = e.k
        sp = self.span(e.t0, e.t1)
        if k == 'lit':
            return ('Literal', sp, e.lk)
        if k == 'name':
            return self.name_chain(e.parts)
        if k == 'this':
            return ('This', sp)
        if k == 'super':
            return ('Super', sp)
        if k == 'field':
            return ('Field', sp, self.expr(e.obj), self.t[e.name_tok].text)
        if k == 'call':
            parts = e.name.parts
            target = self.name_chain(parts[:-1]) if len(parts) > 1 else None
            return ('Call', sp, target, self.t[parts[-1]].text, [self.expr(a) for a in e.args])
        if k == 'mcall':
            return ('Call', sp, self.expr(e.obj), self.t[e.name_tok].text,
                    [self.expr(a) for a in e.args])
        if k == 'scall':
            return ('Call', sp, ('Super', self.span(e.t0, e.t0)), self.t[e.name_tok].text,
                    [self.expr(a) for a in e.args])
        if k == 'ctor':
            return ('CtorCall', sp, e.which, [self.expr(a) for a in e.args])
        if k == 'index':
            return ('Index', sp, self.expr(e.arr), self.expr(e.idx))
        if k == 'new':
            return ('New', sp, self.typ(e.type), [self.expr(a) for a in e.args])
        if k == 'newarr':
            return ('NewArray', sp, '.'.join(e.type.parts), e.type.dims,
                    [self.expr(a) for a in e.sizes])
        if k == 'un':
            return ('Unary', sp, e.op, self.expr(e.e))
        if k == 'post':
            return ('Postfix', sp, e.op, self.expr(e.e))
        if k == 'cast':
            return ('Cast', sp, self.typ(e.type), self.expr(e.e))
        if k == 'bin':
            return ('Binary', sp, e.op, self.expr(e.l), self.expr(e.r))
        if k == 'instanceof':
            return ('InstanceOf', sp, self.expr(e.e), self.typ(e.type))
        if k == 'cond':
            return ('Cond', sp, self.expr(e.c), self.expr(e.a), self.expr(e.b))
        if k == 'assign':
            return ('Assign', sp, e.op, self.expr(e.l), self.expr(e.r))
        if k == 'paren':
            return ('Paren', sp, self.expr(e.e))
        raise ValueError('sparse expression kind %s' % k)

    def decls(self, ds):
        return [(d.name, d.dims, self.expr(d.init)) for d in ds]

    def stmt(self, s):
        if s is None:
            return None
        k = s.k
        sp = self.span(s.t0, s.t1)
        if k == 'block':
            return ('Block', sp, [self.stmt(x) for x in s.stmts])
        if k == 'empty':
            return ('Empty', sp)
        if k == 'local':
            return ('LocalVar', sp, self.typ(s.type), self.decls(s.decls))
        if k == 'expr':
            return ('ExprStmt', sp, self.expr(s.e))
        if k == 'if':
            return ('If', sp, self.expr(s.cond), self.stmt(s.then), self.stmt(s.els))
        if k == 'while':
            return ('While', sp, self.expr(s.cond), self.stmt(s.body))
        if k == 'do':
            return ('Do', sp, self.stmt(s.body), self.expr(s.cond))
        if k == 'for':
            if s.init is None:
                init = []
            elif isinstance(s.init, list):
                init = [self.expr(x) for x in s.init]
            else:
                init = self.stmt(s.init)
            return ('For', sp, init, self.expr(s.cond), [self.expr(x) for x in s.update],
                    self.stmt(s.body))
        if k == 'switch':
            groups = []
            for g in s.groups:
                labs = [('CaseLabel', self.expr(c.e)) if c.k == 'case' else ('DefaultLabel',)
                        for c in g.labels]
                groups.append(('SwitchGroup', self.span(g.t0, g.t1), labs,
                               [self.stmt(x) for x in g.stmts]))
            return ('Switch', sp, self.expr(s.subj), groups)
        if k == 'break':
            return ('Break', sp)
        if k == 'continue':
            return ('Continue', sp)
        if k == 'return':
            return ('Return', sp, self.expr(s.e))
        raise ValueError('sparse statement kind %s' % k)

    def unit(self, u):
        out = []
        for c in u.classes:
            mem = []
            for m in c.members:
                if m.k == 'field':
                    mem.append(('FieldDecl', tuple(m.mods), self.typ(m.type), self.decls(m.decls)))
                elif m.k == 'method':
                    mem.append(('MethodDecl', tuple(m.mods), self.typ(m.ret) if m.ret else None,
                                m.name, [(self.typ(p.type), p.name, p.dims) for p in m.params],
                                self.stmt(m.body)))
                else:
                    mem.append((m.k,))
            out.append((c.kind, c.name, mem))
        return out


# ---- canonical form of a lib/parser tree ---------------------------------

class P(object):
    def __init__(self, unit):
        self.t = unit.tokens

    def span(self, n):
        ta, tb = self.t[n.tok0], self.t[n.tok1]
        return (ta.line, ta.text, tb.line, tb.text)

    def typ(self, ty):
        return (ty.name, ty.dims)

    def expr(self, e):
        if e is None:
            return None
        k = e.kind
        sp = self.span(e)
        if k == 'Literal':
            return ('Literal', sp, e.lit)
        if k == 'Name':
            return ('Name', sp, e.name)
        if k in ('This', 'Super'):
            return (k, sp)
        if k == 'Field':
            return ('Field', sp, self.expr(e.target), e.name)
        if k == 'Call':
            return ('Call', sp, self.expr(e.target), e.name, [self.expr(a) for a in e.args])
        if k == 'CtorCall':
            return ('CtorCall', sp, e.which, [self.expr(a) for a in e.args])
        if k == 'Index':
            return ('Index', sp, self.expr(e.target), self.expr(e.index))
        if k == 'New':
            return ('New', sp, self.typ(e.type), [self.expr(a) for a in e.args])
        if k == 'NewArray':
            return ('NewArray', sp, e.type.name, e.type.dims + len(e.sizes) + e.dims,
                    [self.expr(a) for a in e.sizes])
        if k == 'Unary':
            return ('Unary', sp, e.op, self.expr(e.operand))
        if k == 'Postfix':
            return ('Postfix', sp, e.op, self.expr(e.operand))
        if k == 'Cast':
            return ('Cast', sp, self.typ(e.type), self.expr(e.operand))
        if k == 'Binary':
            return ('Binary', sp, e.op, self.expr(e.left), self.expr(e.right))
        if k == 'InstanceOf':
            return ('InstanceOf', sp, self.expr(e.operand), self.typ(e.type))
        if k == 'Cond':
            return ('Cond', sp, self.expr(e.cond), self.expr(e.then), self.expr(e.otherwise))
        if k == 'Assign':
            return ('Assign', sp, e.op, self.expr(e.target), self.expr(e.value))
        if k == 'Paren':
            return ('Paren', sp, self.expr(e.expr))
        raise ValueError('lib expression kind %s' % k)

    def decls(self, ds):
        return [(d.name, d.dims, self.expr(d.init)) for d in ds]

    def stmt(self, s):
        if s is None:
            return None
        k = s.kind
        sp = self.span(s)
        if k == 'Block':
            return ('Block', sp, [self.stmt(x) for x in s.stmts])
        if k == 'Empty':
            return ('Empty', sp)
        if k == 'LocalVar':
            return ('LocalVar', sp, self.typ(s.type), self.decls(s.declarators))
        if k == 'ExprStmt':
            return ('ExprStmt', sp, self.expr(s.expr))
        if k == 'If':
            return ('If', sp, self.expr(s.cond), self.stmt(s.then), self.stmt(s.otherwise))
        if k == 'While':
            return ('While', sp, self.expr(s.cond), self.stmt(s.body))
        if k == 'Do':
            return ('Do', sp, self.stmt(s.body), self.expr(s.cond))
        if k == 'For':
            if isinstance(s.init, list):
                init = [self.expr(x) for x in s.init]
            else:
                init = self.stmt(s.init)
            return ('For', sp, init, self.expr(s.cond), [self.expr(x) for x in s.update],
                    self.stmt(s.body))
        if k == 'Switch':
            groups = []
            for g in s.groups:
                labs = [('CaseLabel', self.expr(c.value)) if c.kind == 'CaseLabel'
                        else ('DefaultLabel',) for c in g.labels]
                groups.append(('SwitchGroup', self.span(g), labs,
                               [self.stmt(x) for x in g.stmts]))
            return ('Switch', sp, self.expr(s.expr), groups)
        if k == 'Break':
            return ('Break', sp)
        if k == 'Continue':
            return ('Continue', sp)
        if k == 'Return':
            return ('Return', sp, self.expr(s.value))
        raise ValueError('lib statement kind %s' % k)

    def unit(self, u):
        out = []
        for c in u.types:
            mem = []
            for m in c.members:
                if m.kind == 'FieldDecl':
                    mem.append(('FieldDecl', tuple(m.modifiers), self.typ(m.type),
                                self.decls(m.declarators)))
                elif m.kind == 'MethodDecl':
                    mem.append(('MethodDecl', tuple(m.modifiers),
                                self.typ(m.type) if m.type is not None else None, m.name,
                                [(self.typ(p.type), p.name, p.dims) for p in m.params],
                                self.stmt(m.body)))
                else:
                    mem.append((m.kind,))
            out.append(('interface' if c.interface else 'class', c.name, mem))
        return out


def first_diff(a, b, path=''):
    """Path and values of the first difference between two canonical trees."""
    if type(a) != type(b):
        return path, a, b
    if isinstance(a, (list, tuple)):
        if len(a) != len(b):
            return path + '/len', len(a), len(b)
        for i, (x, y) in enumerate(zip(a, b)):
            tag = x[0] if isinstance(x, tuple) and x and isinstance(x[0], str) else i
            d = first_diff(x, y, '%s/%s' % (path, tag))
            if d:
                return d
        return None
    if a != b:
        return path, a, b
    return None


def main():
    L = ['check_parsers.py: sparse.py against lib/parser.py', '']
    if libparser is None or not hasattr(libparser, 'parse'):
        L.append('lib/parser.py is not there; nothing compared')
        sys.stdout.write('\n'.join(L) + '\n')
        return 0
    c = Counter()
    diffs = []
    for e in oracle.iter_entries():
        if not e.source:
            continue
        text = oracle.load_source(e)
        c['sources'] += 1
        try:
            a = S(sparse.parse(text))
            ca = a.unit(sparse.parse(text))
        except Exception as x:   # noqa: BLE001
            c['sparse fails'] += 1
            diffs.append((e.source, 'sparse: %s' % x))
            continue
        try:
            pu = libparser.parse(text, name=e.source)
            cb = P(pu).unit(pu)
        except Exception as x:   # noqa: BLE001
            c['lib/parser fails'] += 1
            diffs.append((e.source, 'lib/parser: %s' % x))
            continue
        d = first_diff(ca, cb)
        if d is None:
            c['same tree, same token spans'] += 1
        else:
            c['different'] += 1
            diffs.append((e.source, '%s: sparse %r, lib/parser %r' % d))
    for k in sorted(c):
        L.append('  %-32s %d' % (k, c[k]))
    L.append('')
    L.append('differences: %d' % len(diffs))
    for s, d in diffs[:40]:
        L.append('  %s: %s' % (s, d[:300]))
    text = '\n'.join(L) + '\n'
    with open(os.path.join(_HERE, 'out', 'check_parsers.txt'), 'w', newline='\n') as f:
        f.write(text)
    with open(os.path.join(_HERE, 'out', 'check_parsers.json'), 'w', newline='\n') as f:
        json.dump({'counts': dict(c), 'differences': diffs}, f, indent=1, sort_keys=True)
        f.write('\n')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
