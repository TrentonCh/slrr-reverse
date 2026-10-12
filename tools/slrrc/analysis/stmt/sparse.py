#!/usr/bin/env python3
"""sparse.py - a recursive-descent parser for the SLRR script dialect.

ANALYSIS parser (statement study): it builds a syntax tree in which every
node knows the index of its first and last token, so that the emitter in
stmtgen.py can ask "which line is the last token of this construct on" and
"which line is the token after it on".  It is not the compiler's yacc
grammar; it accepts every source of the corpus (parse_all.py proves that).

    import sparse
    unit = sparse.parse(text)            # text as oracle.load_source gives it
    unit.toks                            # [srclex.Token]
    unit.package, unit.imports
    for c in unit.classes:               # top-level types in source order
        c.name, c.kind, c.extends, c.implements, c.members
        for m in c.members:
            m.k == 'field'   m.mods, m.type, m.decls = [Decl]
            m.k == 'method'  m.mods, m.ret (None for a constructor), m.name,
                             m.params = [Param], m.body (Block or None),
                             m.body_form in ('block', 'semi')

Every node is an N with .k (kind) and .t0 / .t1 (first / last token index,
inclusive).  Kinds:

  types        'type'      parts=[str], dims
  statements   'block'     stmts
               'local'     type, decls=[N('decl' name, name_tok, dims, init)]
               'expr'      e
               'if'        cond, then, els (or None)
               'while'     cond, body
               'do'        body, cond
               'for'       init (None | N('local') | [expr]), cond (or None),
                           update ([expr]), body
               'switch'    subj, groups=[N('group' labels=[N('case' e) |
                           N('default')], stmts)]
               'break' 'continue' 'empty'
               'return'    e (or None)
  expressions  'lit'       lk in int float str rid null bool chr, tok
               'name'      parts=[token index of each identifier]
               'this'
               'field'     obj, name_tok              obj.name, obj not a name
               'call'      name=N('name'), args       f(..), a.b.f(..)
               'mcall'     obj, name_tok, args        (expr).f(..)
               'scall'     name_tok, args             super.f(..)
               'ctor'      which in this super, args  this(..) / super(..)
               'index'     arr, idx
               'new'       type, args
               'newarr'    type (element type + all dims), sizes=[expr]
               'un'        op, e      op in + - ! ~ ++ --
               'post'      op, e      op in ++ --
               'cast'      type, e
               'bin'       op, l, r
               'instanceof' e, type
               'cond'      c, a, b
               'assign'    op, l, r
               'paren'     e
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import srclex  # noqa: E402

MODIFIERS = frozenset(('public', 'private', 'protected', 'static', 'final',
                       'abstract', 'native', 'synchronized'))
ASSIGN_OPS = frozenset(('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
                        '<<=', '>>=', '>>>='))
BIN_PREC = [
    ('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='),
    ('<', '>', '<=', '>=', 'instanceof'), ('<<', '>>', '>>>'), ('+', '-'),
    ('*', '/', '%'),
]
_PREC = {}
for _lvl, _ops in enumerate(BIN_PREC):
    for _o in _ops:
        _PREC[_o] = _lvl


class ParseError(ValueError):
    pass


class N(object):
    """Syntax tree node: kind, first/last token index, free attributes."""

    def __init__(self, k, t0, t1, **kw):
        self.k = k
        self.t0 = t0
        self.t1 = t1
        self.__dict__.update(kw)

    def __repr__(self):
        d = {a: b for a, b in self.__dict__.items() if a not in ('k', 't0', 't1')}
        return 'N(%s %d..%d %r)' % (self.k, self.t0, self.t1, d)


class Unit(object):
    def __init__(self):
        self.toks = []
        self.package = None
        self.imports = []
        self.classes = []


class Parser(object):
    def __init__(self, text):
        self.toks = srclex.tokens(text)
        self.n = len(self.toks)
        self.p = 0

    # ---- token helpers --------------------------------------------------
    def tx(self, i=None):
        i = self.p if i is None else i
        return self.toks[i].text if i < self.n else None

    def kind(self, i=None):
        i = self.p if i is None else i
        return self.toks[i].kind if i < self.n else None

    def err(self, msg):
        if self.p < self.n:
            t = self.toks[self.p]
            raise ParseError('line %d: %s (at %r)' % (t.line, msg, t.text))
        raise ParseError('end of file: %s' % msg)

    def is_op(self, x, i=None):
        i = self.p if i is None else i
        return i < self.n and self.toks[i].kind == 'op' and self.toks[i].text == x

    def is_kw(self, x, i=None):
        i = self.p if i is None else i
        return i < self.n and self.toks[i].kind == 'kw' and self.toks[i].text == x

    def expect_op(self, x):
        if not self.is_op(x):
            self.err('expected %r' % x)
        self.p += 1
        return self.p - 1

    def expect_kw(self, x):
        if not self.is_kw(x):
            self.err('expected %r' % x)
        self.p += 1
        return self.p - 1

    def expect_id(self):
        if self.kind() != 'id':
            self.err('expected an identifier')
        self.p += 1
        return self.p - 1

    # ---- compilation unit -----------------------------------------------
    def unit(self):
        u = Unit()
        u.toks = self.toks
        while self.p < self.n:
            if self.is_kw('package'):
                self.p += 1
                u.package = self.dotted()
                self.expect_op(';')
            elif self.is_kw('import'):
                self.p += 1
                # "import a.b.*;" and the odd "import parts:part.root.X.*;"
                # (2 sources): everything up to the ';' is the import text
                parts = []
                while not self.is_op(';'):
                    if self.p >= self.n:
                        self.err('import not closed')
                    parts.append(self.tx())
                    self.p += 1
                u.imports.append(''.join(parts))
                self.expect_op(';')
            elif self.is_op(';'):
                self.p += 1
            else:
                u.classes.append(self.type_decl())
        return u

    def dotted(self):
        parts = [self.tx(self.expect_id())]
        while self.is_op('.') and self.kind(self.p + 1) == 'id':
            self.p += 2
            parts.append(self.tx(self.p - 1))
        return '.'.join(parts)

    def modifiers(self):
        mods = []
        while self.kind() == 'kw' and self.tx() in MODIFIERS:
            mods.append(self.tx())
            self.p += 1
        return mods

    def type_decl(self):
        t0 = self.p
        mods = self.modifiers()
        if self.is_kw('class'):
            kind = 'class'
        elif self.is_kw('interface'):
            kind = 'interface'
        else:
            self.err('expected class or interface')
        self.p += 1
        name = self.tx(self.expect_id())
        ext = None
        impl = []
        if self.is_kw('extends'):
            self.p += 1
            ext = self.dotted()
            while self.is_op(','):          # interface extends a, b
                self.p += 1
                impl.append(self.dotted())
        if self.is_kw('implements'):
            self.p += 1
            impl.append(self.dotted())
            while self.is_op(','):
                self.p += 1
                impl.append(self.dotted())
        lb = self.expect_op('{')
        members = []
        while not self.is_op('}'):
            if self.p >= self.n:
                self.err('class body not closed')
            if self.is_op(';'):
                self.p += 1
                continue
            members.append(self.member(name))
        rb = self.expect_op('}')
        return N('class', t0, rb, mods=mods, kind=kind, name=name, extends=ext,
                 implements=impl, members=members, lbrace=lb)

    def member(self, cname):
        t0 = self.p
        mods = self.modifiers()
        # static initialiser block?
        if self.is_op('{'):
            body = self.block()
            return N('initblock', t0, body.t1, mods=mods, body=body)
        # constructor: Name '('
        if self.kind() == 'id' and self.is_op('(', self.p + 1):
            name_tok = self.p
            self.p += 1
            params = self.params()
            body, form = self.method_body()
            return N('method', t0, self.p - 1, mods=mods, ret=None,
                     name=self.toks[name_tok].text, name_tok=name_tok,
                     params=params, body=body, body_form=form, ctor=True)
        typ = self.type()
        name_tok = self.expect_id()
        if self.is_op('('):
            params = self.params()
            # "int f()[]" does not occur
            body, form = self.method_body()
            return N('method', t0, self.p - 1, mods=mods, ret=typ,
                     name=self.toks[name_tok].text, name_tok=name_tok,
                     params=params, body=body, body_form=form, ctor=False)
        decls = self.declarators(name_tok)
        semi = self.expect_op(';')
        return N('field', t0, semi, mods=mods, type=typ, decls=decls)

    def method_body(self):
        if self.is_op(';'):
            self.p += 1
            return None, 'semi'
        return self.block(), 'block'

    def params(self):
        self.expect_op('(')
        out = []
        if not self.is_op(')'):
            while True:
                t0 = self.p
                while self.is_kw('final'):
                    self.p += 1
                typ = self.type()
                nt = self.expect_id()
                dims = 0
                while self.is_op('[') and self.is_op(']', self.p + 1):
                    self.p += 2
                    dims += 1
                out.append(N('param', t0, self.p - 1, type=typ,
                             name=self.toks[nt].text, name_tok=nt, dims=dims))
                if self.is_op(','):
                    self.p += 1
                    continue
                break
        self.expect_op(')')
        return out

    def type(self):
        t0 = self.p
        if self.kind() == 'kw' and self.tx() in ('int', 'float', 'void'):
            parts = [self.tx()]
            self.p += 1
        elif self.kind() == 'id':
            parts = [self.tx()]
            self.p += 1
            while self.is_op('.') and self.kind(self.p + 1) == 'id':
                self.p += 2
                parts.append(self.tx(self.p - 1))
        else:
            self.err('expected a type')
        dims = 0
        while self.is_op('[') and self.is_op(']', self.p + 1):
            self.p += 2
            dims += 1
        return N('type', t0, self.p - 1, parts=parts, dims=dims)

    def declarators(self, first_name_tok):
        """After the type and the first name token have been read."""
        out = []
        nt = first_name_tok
        while True:
            dims = 0
            while self.is_op('[') and self.is_op(']', self.p + 1):
                self.p += 2
                dims += 1
            init = None
            eq = None
            if self.is_op('='):
                eq = self.p
                self.p += 1
                init = self.expr()
            out.append(N('decl', nt, self.p - 1, name=self.toks[nt].text,
                         name_tok=nt, dims=dims, init=init, eq_tok=eq))
            if self.is_op(','):
                self.p += 1
                nt = self.expect_id()
                continue
            break
        return out

    # ---- statements -----------------------------------------------------
    def block(self):
        lb = self.expect_op('{')
        stmts = []
        while not self.is_op('}'):
            if self.p >= self.n:
                self.err('block not closed')
            stmts.append(self.stmt())
        rb = self.expect_op('}')
        return N('block', lb, rb, stmts=stmts)

    def looks_like_local_decl(self):
        """Type Identifier at the cursor (optionally after 'final')."""
        i = self.p
        if self.is_kw('final', i):
            return True
        if self.kind(i) == 'kw' and self.tx(i) in ('int', 'float'):
            i += 1
        elif self.kind(i) == 'id':
            i += 1
            while self.is_op('.', i) and self.kind(i + 1) == 'id':
                i += 2
        else:
            return False
        while self.is_op('[', i) and self.is_op(']', i + 1):
            i += 2
        return self.kind(i) == 'id'

    def local_decl(self):
        t0 = self.p
        final = False
        while self.is_kw('final'):
            self.p += 1
            final = True
        typ = self.type()
        nt = self.expect_id()
        decls = self.declarators(nt)
        return N('local', t0, self.p - 1, type=typ, decls=decls, final=final)

    def stmt(self):
        t0 = self.p
        if self.is_op('{'):
            return self.block()
        if self.is_op(';'):
            self.p += 1
            return N('empty', t0, t0)
        k = self.kind()
        x = self.tx()
        if k == 'kw':
            if x == 'if':
                self.p += 1
                self.expect_op('(')
                cond = self.expr()
                rp = self.expect_op(')')
                then = self.stmt()
                els = None
                else_tok = None
                if self.is_kw('else'):
                    else_tok = self.p
                    self.p += 1
                    els = self.stmt()
                return N('if', t0, self.p - 1, cond=cond, then=then, els=els,
                         rparen=rp, else_tok=else_tok)
            if x == 'while':
                self.p += 1
                self.expect_op('(')
                cond = self.expr()
                rp = self.expect_op(')')
                body = self.stmt()
                return N('while', t0, self.p - 1, cond=cond, body=body, rparen=rp)
            if x == 'do':
                self.p += 1
                body = self.stmt()
                wt = self.expect_kw('while')
                self.expect_op('(')
                cond = self.expr()
                rp = self.expect_op(')')
                semi = None
                if self.is_op(';'):
                    semi = self.p
                    self.p += 1
                return N('do', t0, self.p - 1, body=body, cond=cond,
                         while_tok=wt, rparen=rp, semi=semi)
            if x == 'for':
                self.p += 1
                self.expect_op('(')
                init = None
                if not self.is_op(';'):
                    if self.looks_like_local_decl():
                        init = self.local_decl()
                    else:
                        init = [self.expr()]
                        while self.is_op(','):
                            self.p += 1
                            init.append(self.expr())
                semi1 = self.expect_op(';')
                cond = None
                if not self.is_op(';'):
                    cond = self.expr()
                semi2 = self.expect_op(';')
                update = []
                if not self.is_op(')'):
                    update.append(self.expr())
                    while self.is_op(','):
                        self.p += 1
                        update.append(self.expr())
                rp = self.expect_op(')')
                body = self.stmt()
                return N('for', t0, self.p - 1, init=init, cond=cond,
                         update=update, body=body, semi1=semi1, semi2=semi2,
                         rparen=rp)
            if x == 'switch':
                self.p += 1
                self.expect_op('(')
                subj = self.expr()
                rp = self.expect_op(')')
                lb = self.expect_op('{')
                groups = []
                while not self.is_op('}'):
                    g0 = self.p
                    labels = []
                    while self.is_kw('case') or self.is_kw('default'):
                        l0 = self.p
                        if self.is_kw('case'):
                            self.p += 1
                            e = self.expr()
                            c = self.expect_op(':')
                            labels.append(N('case', l0, c, e=e))
                        else:
                            self.p += 1
                            c = self.expect_op(':')
                            labels.append(N('default', l0, c))
                    if not labels:
                        self.err('statement in a switch before any case')
                    stmts = []
                    while not (self.is_op('}') or self.is_kw('case') or
                               self.is_kw('default')):
                        if self.p >= self.n:
                            self.err('switch not closed')
                        stmts.append(self.stmt())
                    groups.append(N('group', g0, self.p - 1, labels=labels,
                                    stmts=stmts))
                rb = self.expect_op('}')
                return N('switch', t0, rb, subj=subj, groups=groups, rparen=rp,
                         lbrace=lb)
            if x == 'break':
                self.p += 1
                semi = self.expect_op(';')
                return N('break', t0, semi)
            if x == 'continue':
                self.p += 1
                semi = self.expect_op(';')
                return N('continue', t0, semi)
            if x == 'return':
                self.p += 1
                e = None
                if not self.is_op(';'):
                    e = self.expr()
                semi = self.expect_op(';')
                return N('return', t0, semi, e=e)
        if self.looks_like_local_decl():
            d = self.local_decl()
            semi = self.expect_op(';')
            d.t1 = semi
            return d
        e = self.expr()
        semi = self.expect_op(';')
        return N('expr', t0, semi, e=e)

    # ---- expressions ----------------------------------------------------
    def expr(self):
        return self.assignment()

    def assignment(self):
        left = self.conditional()
        if self.kind() == 'op' and self.tx() in ASSIGN_OPS:
            op = self.tx()
            op_tok = self.p
            self.p += 1
            right = self.assignment()
            return N('assign', left.t0, right.t1, op=op, l=left, r=right,
                     op_tok=op_tok)
        return left

    def conditional(self):
        c = self.binary(0)
        if self.is_op('?'):
            q = self.p
            self.p += 1
            a = self.expr()
            self.expect_op(':')
            b = self.conditional()
            return N('cond', c.t0, b.t1, c=c, a=a, b=b, q_tok=q)
        return c

    def binary(self, lvl):
        if lvl >= len(BIN_PREC):
            return self.unary()
        left = self.binary(lvl + 1)
        while True:
            x = self.tx()
            k = self.kind()
            if x is None or _PREC.get(x) != lvl or (k != 'op' and x != 'instanceof'):
                return left
            op_tok = self.p
            self.p += 1
            if x == 'instanceof':
                typ = self.type()
                left = N('instanceof', left.t0, typ.t1, e=left, type=typ,
                         op_tok=op_tok)
            else:
                right = self.binary(lvl + 1)
                left = N('bin', left.t0, right.t1, op=x, l=left, r=right,
                         op_tok=op_tok)

    def is_cast_ahead(self):
        """'(' Type ')' followed by the start of an operand."""
        i = self.p + 1
        prim = False
        if self.kind(i) == 'kw' and self.tx(i) in ('int', 'float'):
            prim = True
            i += 1
        elif self.kind(i) == 'id':
            i += 1
            while self.is_op('.', i) and self.kind(i + 1) == 'id':
                i += 2
        else:
            return False
        while self.is_op('[', i) and self.is_op(']', i + 1):
            i += 2
        if not self.is_op(')', i):
            return False
        j = i + 1
        if j >= self.n:
            return False
        nk, nx = self.kind(j), self.tx(j)
        if prim:
            return True
        if nk in ('id', 'int', 'float', 'str', 'rid', 'chr'):
            return True
        if nk == 'kw' and nx in ('this', 'super', 'null', 'true', 'false', 'new'):
            return True
        if nk == 'op' and nx in ('(', '!', '~'):
            return True
        return False

    def unary(self):
        t0 = self.p
        if self.kind() == 'op':
            x = self.tx()
            if x in ('+', '-', '!', '~', '++', '--'):
                self.p += 1
                e = self.unary()
                return N('un', t0, e.t1, op=x, e=e)
            if x == '(' and self.is_cast_ahead():
                self.p += 1
                typ = self.type()
                rp = self.expect_op(')')
                e = self.unary()
                return N('cast', t0, e.t1, type=typ, e=e, rparen=rp)
        return self.postfix()

    def args(self):
        self.expect_op('(')
        out = []
        if not self.is_op(')'):
            out.append(self.expr())
            while self.is_op(','):
                self.p += 1
                out.append(self.expr())
        rp = self.expect_op(')')
        return out, rp

    def postfix(self):
        e = self.primary()
        while True:
            if self.is_op('.'):
                if self.kind(self.p + 1) != 'id':
                    self.err('expected a member name after "."')
                nt = self.p + 1
                self.p += 2
                if self.is_op('('):
                    a, rp = self.args()
                    if e.k == 'name':
                        nm = N('name', e.t0, nt, parts=e.parts + [nt])
                        e = N('call', e.t0, rp, name=nm, args=a)
                    elif e.k == 'super':
                        e = N('scall', e.t0, rp, name_tok=nt, args=a)
                    else:
                        e = N('mcall', e.t0, rp, obj=e, name_tok=nt, args=a)
                elif e.k == 'name':
                    e = N('name', e.t0, nt, parts=e.parts + [nt])
                else:
                    e = N('field', e.t0, nt, obj=e, name_tok=nt)
            elif self.is_op('['):
                self.p += 1
                idx = self.expr()
                rb = self.expect_op(']')
                e = N('index', e.t0, rb, arr=e, idx=idx)
            elif self.is_op('++') or self.is_op('--'):
                op = self.tx()
                self.p += 1
                e = N('post', e.t0, self.p - 1, op=op, e=e)
            else:
                return e

    def primary(self):
        t0 = self.p
        k = self.kind()
        x = self.tx()
        if k in ('int', 'float', 'str', 'rid', 'chr'):
            self.p += 1
            return N('lit', t0, t0, lk=k, tok=t0)
        if k == 'id':
            self.p += 1
            if self.is_op('('):
                a, rp = self.args()
                nm = N('name', t0, t0, parts=[t0])
                return N('call', t0, rp, name=nm, args=a)
            return N('name', t0, t0, parts=[t0])
        if k == 'kw':
            if x == 'null':
                self.p += 1
                return N('lit', t0, t0, lk='null', tok=t0)
            if x in ('true', 'false'):
                self.p += 1
                return N('lit', t0, t0, lk='bool', tok=t0)
            if x == 'this':
                self.p += 1
                if self.is_op('('):
                    a, rp = self.args()
                    return N('ctor', t0, rp, which='this', args=a)
                return N('this', t0, t0)
            if x == 'super':
                self.p += 1
                if self.is_op('('):
                    a, rp = self.args()
                    return N('ctor', t0, rp, which='super', args=a)
                return N('super', t0, t0)
            if x == 'new':
                self.p += 1
                ty0 = self.p
                if self.kind() == 'kw' and self.tx() in ('int', 'float'):
                    parts = [self.tx()]
                    self.p += 1
                elif self.kind() == 'id':
                    parts = [self.tx()]
                    self.p += 1
                    while self.is_op('.') and self.kind(self.p + 1) == 'id':
                        self.p += 2
                        parts.append(self.tx(self.p - 1))
                else:
                    self.err('expected a type after new')
                if self.is_op('('):
                    typ = N('type', ty0, self.p - 1, parts=parts, dims=0)
                    a, rp = self.args()
                    return N('new', t0, rp, type=typ, args=a)
                sizes = []
                dims = 0
                while self.is_op('['):
                    if self.is_op(']', self.p + 1):
                        self.p += 2
                        dims += 1
                        continue
                    self.p += 1
                    sizes.append(self.expr())
                    self.expect_op(']')
                    dims += 1
                if not dims:
                    self.err('expected "(" or "[" after new Type')
                typ = N('type', ty0, self.p - 1, parts=parts, dims=dims)
                return N('newarr', t0, self.p - 1, type=typ, sizes=sizes)
        if k == 'op' and x == '(':
            self.p += 1
            e = self.expr()
            rp = self.expect_op(')')
            return N('paren', t0, rp, e=e)
        self.err('expected an expression')


def parse(text):
    p = Parser(text)
    u = p.unit()
    return u


if __name__ == '__main__':
    import oracle
    e = oracle.entry(sys.argv[1])
    u = parse(oracle.load_source(e))
    for c in u.classes:
        print(c.kind, c.name, 'extends', c.extends, 'members', len(c.members))
        for m in c.members:
            if m.k == 'method':
                print('   method', m.name, len(m.params), m.body_form,
                      'line', u.toks[m.t0].line)
            elif m.k == 'field':
                print('   field', [d.name for d in m.decls])
            else:
                print('  ', m.k)
