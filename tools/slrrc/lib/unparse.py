#!/usr/bin/env python3
"""unparse.py - the token sequence of an AST.

    import parser, unparse
    unit = parser.parse(text)
    unparse.tokens(unit)        # ['package', 'java', '.', 'game', ';', ...]
    unparse.text(unit)          # the same, joined into compilable text

The AST of parser.py keeps everything of the source except comments and white
space, so the tokens regenerated from it must equal the tokens the lexer read.
check_parse.py proves that for every source of the corpus (852,211 tokens):
no token is dropped, reordered or invented by the parser, and no two different
token sequences share an AST.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)


class Unparser(object):
    def __init__(self):
        self.out = []

    def w(self, *toks):
        self.out.extend(toks)

    def name(self, dotted):
        parts = dotted.split('.')
        self.w(parts[0])
        for p in parts[1:]:
            self.w('.', p)

    def dims(self, n):
        for _ in range(n):
            self.w('[', ']')

    def type(self, t):
        if t.primitive:
            self.w(t.name)
        else:
            self.name(t.name)
        self.dims(t.dims)

    def commas(self, items, fn):
        for i, x in enumerate(items):
            if i:
                self.w(',')
            fn(x)

    # -- declarations ----------------------------------------------------------
    def unit(self, u):
        if u.package:
            self.w('package')
            self.name(u.package)
            self.w(';')
        # imports and types in source order (an import may follow a type)
        items = sorted(u.imports + u.types, key=lambda n: n.tok0)
        for n in items:
            if n.kind == 'Import':
                self.w('import')
                if n.pack:
                    self.w(n.pack, ':')
                self.name(n.name)
                if n.star:
                    self.w('.', '*')
                self.w(';')
            else:
                self.cls(n)

    def cls(self, c):
        self.w(*c.modifiers)
        self.w('interface' if c.interface else 'class', c.name)
        if c.extends_list:
            self.w('extends')
            self.commas(c.extends_list, self.name)
        if c.implements:
            self.w('implements')
            self.commas(c.implements, self.name)
        self.w('{')
        for m in c.members:
            k = m.kind
            if k == 'FieldDecl':
                self.w(*m.modifiers)
                self.type(m.type)
                self.commas(m.declarators, self.declarator)
                self.w(';')
            elif k == 'MethodDecl':
                self.w(*m.modifiers)
                if m.type is not None:
                    self.type(m.type)
                self.w(m.name, '(')
                self.commas(m.params, self.param)
                self.w(')')
                self.dims(m.dims)
                if m.body is None:
                    self.w(';')
                else:
                    self.stmt(m.body)
            else:
                self.w(*(['static'] if m.static else []))
                self.stmt(m.body)
        self.w('}')

    def param(self, p):
        self.w(*p.modifiers)
        self.type(p.type)
        self.w(p.name)
        self.dims(p.dims)

    def declarator(self, d):
        self.w(d.name)
        self.dims(d.dims)
        if d.init is not None:
            self.w('=')
            self.expr(d.init)

    def local(self, s):
        self.w(*s.modifiers)
        self.type(s.type)
        self.commas(s.declarators, self.declarator)

    # -- statements --------------------------------------------------------------
    def stmt(self, s):
        k = s.kind
        w = self.w
        if k == 'Block':
            w('{')
            for x in s.stmts:
                self.stmt(x)
            w('}')
        elif k == 'Empty':
            w(';')
        elif k == 'ExprStmt':
            self.expr(s.expr)
            w(';')
        elif k == 'LocalVar':
            self.local(s)
            w(';')
        elif k == 'If':
            w('if', '(')
            self.expr(s.cond)
            w(')')
            self.stmt(s.then)
            if s.otherwise is not None:
                w('else')
                self.stmt(s.otherwise)
        elif k == 'While':
            w('while', '(')
            self.expr(s.cond)
            w(')')
            self.stmt(s.body)
        elif k == 'Do':
            w('do')
            self.stmt(s.body)
            w('while', '(')
            self.expr(s.cond)
            w(')', ';')
        elif k == 'For':
            w('for', '(')
            if isinstance(s.init, list):
                self.commas(s.init, self.expr)
            else:
                self.local(s.init)
            w(';')
            if s.cond is not None:
                self.expr(s.cond)
            w(';')
            self.commas(s.update, self.expr)
            w(')')
            self.stmt(s.body)
        elif k == 'Switch':
            w('switch', '(')
            self.expr(s.expr)
            w(')', '{')
            for g in s.groups:
                for lab in g.labels:
                    if lab.kind == 'CaseLabel':
                        w('case')
                        self.expr(lab.value)
                        w(':')
                    else:
                        w('default', ':')
                for x in g.stmts:
                    self.stmt(x)
            w('}')
        elif k in ('Break', 'Continue'):
            w(k.lower())
            if s.label:
                w(s.label)
            w(';')
        elif k == 'Return':
            w('return')
            if s.value is not None:
                self.expr(s.value)
            w(';')
        elif k == 'Labeled':
            w(s.label, ':')
            self.stmt(s.body)
        else:
            raise ValueError('cannot unparse statement %s' % k)

    # -- expressions -------------------------------------------------------------
    def args(self, args):
        self.w('(')
        self.commas(args, self.expr)
        self.w(')')

    def expr(self, e):
        k = e.kind
        w = self.w
        if k == 'Literal':
            w(e.text)
        elif k == 'Name':
            w(e.name)
        elif k == 'This':
            w('this')
        elif k == 'Super':
            w('super')
        elif k == 'Paren':
            w('(')
            self.expr(e.expr)
            w(')')
        elif k == 'Field':
            self.expr(e.target)
            w('.', e.name)
        elif k == 'Index':
            self.expr(e.target)
            w('[')
            self.expr(e.index)
            w(']')
        elif k == 'Call':
            if e.target is not None:
                self.expr(e.target)
                w('.')
            w(e.name)
            self.args(e.args)
        elif k == 'CtorCall':
            w(e.which)
            self.args(e.args)
        elif k == 'New':
            w('new')
            self.type(e.type)
            self.args(e.args)
        elif k == 'NewArray':
            w('new')
            self.type(e.type)
            for s in e.sizes:
                w('[')
                self.expr(s)
                w(']')
            self.dims(e.dims)
            if e.init is not None:
                self.expr(e.init)
        elif k == 'ArrayInit':
            w('{')
            self.commas(e.elements, self.expr)
            w('}')
        elif k == 'Unary':
            w(e.op)
            self.expr(e.operand)
        elif k == 'Postfix':
            self.expr(e.operand)
            w(e.op)
        elif k == 'Binary':
            self.expr(e.left)
            w(e.op)
            self.expr(e.right)
        elif k == 'Assign':
            self.expr(e.target)
            w(e.op)
            self.expr(e.value)
        elif k == 'Cond':
            self.expr(e.cond)
            w('?')
            self.expr(e.then)
            w(':')
            self.expr(e.otherwise)
        elif k == 'Cast':
            w('(')
            self.type(e.type)
            w(')')
            self.expr(e.operand)
        elif k == 'InstanceOf':
            self.expr(e.operand)
            w('instanceof')
            self.type(e.type)
        else:
            raise ValueError('cannot unparse expression %s' % k)


def tokens(unit):
    """The token texts of a Unit, in order."""
    u = Unparser()
    u.unit(unit)
    return u.out


def text(unit):
    """Compilable text: the tokens separated by a blank, one line per ';',
    '{' and '}'.  (Adjacent '+' '+' and '-' '-' need the blank.)"""
    out = []
    line = []
    for t in tokens(unit):
        line.append(t)
        if t in (';', '{', '}'):
            out.append(' '.join(line))
            line = []
    if line:
        out.append(' '.join(line))
    return '\n'.join(out) + '\n'
