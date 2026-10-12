#!/usr/bin/env python3
"""srcparse.py - a recursive-descent parser for the SLRR script dialect, built
for the declaration / constant-pool analysis (analysis/decl).

It is an ANALYSIS parser: it accepts every source of the corpus (3,454 of
3,454, see check_parse.py) and yields a plain tree that keeps the token index
of every construct, so that a walker can replay "what the yacc parser had seen
by then" in any order it wants to test.  It is not the compiler's grammar.

    import srcparse
    unit = srcparse.parse(text)            # text from oracle.load_source
    unit.package, unit.imports, unit.classes
    cls.kind, cls.mods, cls.name, cls.extends, cls.implements, cls.members
    member.kind  'field' | 'method' | 'ctor'

Nodes are instances of N (a dict with attribute access); every node has
'k' (kind), 'tok' (index of its first token) and 'line'.

Expression kinds
    lit(kind,text)  name(id)  this  super  null  field(obj,id)  call(target,args)
    new(type,args)  newarray(type,size)  index(arr,idx)  unary(op,e)
    postfix(op,e)  binary(op,a,b)  assign(op,lhs,rhs)  cond(c,a,b)
    cast(type,e)  instanceof(e,type)  paren(e)
Statement kinds
    block(stmts)  local(type,decls)  expr(e)  if(c,a,b)  while(c,body)
    do(body,c)  for(init,cond,upd,body)  switch(e,items)  break  continue
    return(e)  empty  thiscall(args)  supercall(args)
Type: N(k='type', base='int'|'float'|'void'|dotted name, dims=n)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'lib')))

import srclex  # noqa: E402

MODIFIERS = ('public', 'private', 'protected', 'static', 'final', 'abstract',
             'native', 'synchronized')
ASSIGN_OPS = ('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=',
              '>>=', '>>>=')
BIN_LEVELS = (
    ('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='),
    ('<', '>', '<=', '>=', 'instanceof'), ('<<', '>>', '>>>'), ('+', '-'),
    ('*', '/', '%'),
)


class ParseError(ValueError):
    pass


class N(dict):
    """A tree node: dict with attribute access."""
    __slots__ = ()

    def __getattr__(self, k):
        try:
            return self[k]
        except KeyError:
            raise AttributeError(k)

    def __setattr__(self, k, v):
        self[k] = v


class Parser(object):
    def __init__(self, text):
        self.toks = srclex.tokens(text)
        self.n = len(self.toks)
        self.p = 0

    # ---- token helpers ---------------------------------------------------
    def peek(self, k=0):
        i = self.p + k
        return self.toks[i] if i < self.n else None

    def text(self, k=0):
        t = self.peek(k)
        return t.text if t is not None else None

    def kind(self, k=0):
        t = self.peek(k)
        return t.kind if t is not None else None

    def at(self, s, k=0):
        t = self.peek(k)
        return t is not None and t.text == s and t.kind in ('op', 'kw')

    def line(self, k=0):
        t = self.peek(k)
        return t.line if t is not None else (self.toks[-1].line if self.toks else 0)

    def fail(self, what):
        t = self.peek()
        raise ParseError('line %d: %s (at %r)' % (
            self.line(), what, t.text if t else 'EOF'))

    def eat(self, s):
        if not self.at(s):
            self.fail('expected %r' % s)
        self.p += 1

    def opt(self, s):
        if self.at(s):
            self.p += 1
            return True
        return False

    def ident(self):
        t = self.peek()
        if t is None or t.kind != 'id':
            self.fail('expected an identifier')
        self.p += 1
        return t.text

    def node(self, k, tok=None, **kw):
        i = self.p if tok is None else tok
        ln = self.toks[i].line if i < self.n else self.line()
        d = N(k=k, tok=i, line=ln)
        d.update(kw)
        return d

    # ---- compilation unit ------------------------------------------------
    def unit(self):
        u = N(k='unit', package=None, imports=[], classes=[], stray=0)
        while self.p < self.n:
            if self.at('package'):
                self.p += 1
                u.package = self.qname()
                self.eat(';')
            elif self.at('import'):
                ln = self.line()
                self.p += 1
                # an import is "a.b.c.*;" in all but two sources, which hold
                # "import parts:part.root.BodyPart.*;" (City.java and
                # Baiern_FR_quarterpanel.java); the raw text is kept
                parts = []
                while not self.at(';'):
                    if self.p >= self.n:
                        self.fail('import not closed')
                    parts.append(self.text())
                    self.p += 1
                self.eat(';')
                raw = ''.join(parts)
                star = raw.endswith('.*')
                name = raw[:-2] if star else raw
                u.imports.append(N(k='import', name=name, star=star, line=ln,
                                   odd=(':' in raw)))
            elif self.at(';'):
                self.p += 1
                u.stray += 1
            else:
                u.classes.append(self.classdecl())
        return u

    def qname(self):
        name = self.ident()
        while self.at('.') and self.kind(1) == 'id':
            self.p += 1
            name += '.' + self.ident()
        return name

    def mods(self):
        out = []
        while self.kind() == 'kw' and self.text() in MODIFIERS:
            out.append(self.text())
            self.p += 1
        return out

    def classdecl(self):
        tok = self.p
        mods = self.mods()
        if not (self.at('class') or self.at('interface')):
            self.fail('expected class or interface')
        kind = self.text()
        self.p += 1
        c = self.node('class', tok, kind=kind, mods=mods, name=self.ident(),
                      extends=None, implements=[], members=[], stray=0)
        c.name_tok = self.p - 1
        if self.opt('extends'):
            c.extends = self.qname()
            # an interface may extend several
            while self.opt(','):
                c.implements.append(self.qname())
        if self.opt('implements'):
            c.implements.append(self.qname())
            while self.opt(','):
                c.implements.append(self.qname())
        self.eat('{')
        while not self.at('}'):
            if self.p >= self.n:
                self.fail('class body not closed')
            if self.at(';'):
                self.p += 1
                c.stray += 1
                continue
            c.members.extend(self.member(c))
        self.eat('}')
        c.end_tok = self.p - 1
        return c

    def type_(self):
        tok = self.p
        t = self.peek()
        if t is None:
            self.fail('expected a type')
        if t.kind == 'kw' and t.text in ('int', 'float', 'void'):
            self.p += 1
            base = t.text
        elif t.kind == 'id':
            base = self.qname()
        else:
            self.fail('expected a type')
        dims = 0
        while self.at('[') and self.at(']', 1):
            self.p += 2
            dims += 1
        return self.node('type', tok, base=base, dims=dims, end=self.p)

    def member(self, c):
        tok = self.p
        mods = self.mods()
        # constructor: Name (
        if self.kind() == 'id' and self.at('(', 1) and self.text() == c.name:
            name_tok = self.p
            self.p += 1
            params = self.params()
            m = self.node('ctor', tok, mods=mods, name='<init>', ret=None,
                          params=params, name_tok=name_tok)
            m.hdr_end = self.p
            m.body = self.body()
            m.end_tok = self.p - 1
            return [m]
        ty = self.type_()
        if self.kind() == 'id' and self.at('(', 1):
            name_tok = self.p
            name = self.ident()
            params = self.params()
            m = self.node('method', tok, mods=mods, name=name, ret=ty,
                          params=params, name_tok=name_tok)
            # "int f()[]" does not occur
            m.hdr_end = self.p
            m.body = self.body()
            m.end_tok = self.p - 1
            return [m]
        if self.at('('):
            # a method without a return type whose name is not the class name
            self.fail('method without return type')
        out = []
        while True:
            ftok = self.p
            name = self.ident()
            dims = 0
            while self.at('[') and self.at(']', 1):
                self.p += 2
                dims += 1
            init = None
            if self.opt('='):
                init = self.expr_noassign_top()
            out.append(self.node('field', ftok, mods=mods, type=ty, name=name,
                                 dims=dims, init=init, decl_tok=tok,
                                 end=self.p))
            if not self.opt(','):
                break
        self.eat(';')
        return out

    def params(self):
        self.eat('(')
        out = []
        if not self.at(')'):
            while True:
                tok = self.p
                final = self.opt('final')
                ty = self.type_()
                name = self.ident()
                dims = 0
                while self.at('[') and self.at(']', 1):
                    self.p += 2
                    dims += 1
                out.append(self.node('param', tok, type=ty, name=name,
                                     dims=dims, final=final))
                if not self.opt(','):
                    break
        self.eat(')')
        return out

    def body(self):
        if self.opt(';'):
            return None
        return self.block()

    # ---- statements ------------------------------------------------------
    def block(self):
        tok = self.p
        self.eat('{')
        stmts = []
        while not self.at('}'):
            if self.p >= self.n:
                self.fail('block not closed')
            stmts.append(self.stmt())
        self.eat('}')
        return self.node('block', tok, stmts=stmts, end=self.p - 1)

    def is_local_decl(self):
        """Does a local variable declaration start here?"""
        t = self.peek()
        if t is None:
            return False
        if t.kind == 'kw' and t.text == 'final':
            return True
        if t.kind == 'kw' and t.text in ('int', 'float'):
            return True
        if t.kind != 'id':
            return False
        j = 1
        while self.at('.', j) and self.kind(j + 1) == 'id':
            j += 2
        while self.at('[', j) and self.at(']', j + 1):
            j += 2
        return self.kind(j) == 'id'

    def local_decl(self):
        tok = self.p
        final = self.opt('final')
        ty = self.type_()
        decls = []
        while True:
            dtok = self.p
            name = self.ident()
            dims = 0
            while self.at('[') and self.at(']', 1):
                self.p += 2
                dims += 1
            init = None
            if self.opt('='):
                init = self.expr_noassign_top()
            decls.append(self.node('decl', dtok, name=name, dims=dims, init=init))
            if not self.opt(','):
                break
        return self.node('local', tok, type=ty, decls=decls, final=final)

    def stmt(self):
        tok = self.p
        t = self.peek()
        if t is None:
            self.fail('statement expected')
        x = t.text
        if t.kind == 'op':
            if x == '{':
                return self.block()
            if x == ';':
                self.p += 1
                return self.node('empty', tok)
        if t.kind == 'kw':
            if x == 'if':
                self.p += 1
                self.eat('(')
                c = self.expr()
                self.eat(')')
                a = self.stmt()
                b = None
                if self.opt('else'):
                    b = self.stmt()
                return self.node('if', tok, c=c, a=a, b=b)
            if x == 'while':
                self.p += 1
                self.eat('(')
                c = self.expr()
                self.eat(')')
                return self.node('while', tok, c=c, body=self.stmt())
            if x == 'do':
                self.p += 1
                body = self.stmt()
                self.eat('while')
                self.eat('(')
                c = self.expr()
                self.eat(')')
                self.opt(';')
                return self.node('do', tok, body=body, c=c)
            if x == 'for':
                self.p += 1
                self.eat('(')
                init = []
                if not self.at(';'):
                    if self.is_local_decl():
                        init.append(self.local_decl())
                    else:
                        init.append(self.node('expr', None, e=self.expr()))
                        while self.opt(','):
                            init.append(self.node('expr', None, e=self.expr()))
                self.eat(';')
                cond = None if self.at(';') else self.expr()
                self.eat(';')
                upd = []
                if not self.at(')'):
                    upd.append(self.expr())
                    while self.opt(','):
                        upd.append(self.expr())
                self.eat(')')
                return self.node('for', tok, init=init, cond=cond, upd=upd,
                                 body=self.stmt())
            if x == 'switch':
                self.p += 1
                self.eat('(')
                e = self.expr()
                self.eat(')')
                self.eat('{')
                items = []
                while not self.at('}'):
                    if self.at('case'):
                        ctok = self.p
                        self.p += 1
                        v = self.expr()
                        self.eat(':')
                        items.append(self.node('case', ctok, e=v))
                    elif self.at('default'):
                        ctok = self.p
                        self.p += 1
                        self.eat(':')
                        items.append(self.node('default', ctok))
                    else:
                        items.append(self.stmt())
                self.eat('}')
                return self.node('switch', tok, e=e, items=items)
            if x == 'break':
                self.p += 1
                self.eat(';')
                return self.node('break', tok)
            if x == 'continue':
                self.p += 1
                self.eat(';')
                return self.node('continue', tok)
            if x == 'return':
                self.p += 1
                e = None if self.at(';') else self.expr()
                self.eat(';')
                return self.node('return', tok, e=e)
            if x in ('this', 'super') and self.at('(', 1):
                self.p += 1
                args = self.args()
                self.eat(';')
                return self.node('thiscall' if x == 'this' else 'supercall',
                                 tok, args=args)
        if self.is_local_decl():
            d = self.local_decl()
            self.eat(';')
            return d
        e = self.expr()
        self.eat(';')
        return self.node('expr', tok, e=e)

    # ---- expressions -----------------------------------------------------
    def args(self):
        self.eat('(')
        out = []
        if not self.at(')'):
            out.append(self.expr())
            while self.opt(','):
                out.append(self.expr())
        self.eat(')')
        return out

    def expr_noassign_top(self):
        # initialisers: a full expression (assignment allowed, no comma)
        return self.expr()

    def expr(self):
        tok = self.p
        lhs = self.cond()
        t = self.peek()
        if t is not None and t.kind == 'op' and t.text in ASSIGN_OPS:
            optok = self.p
            self.p += 1
            rhs = self.expr()
            return self.node('assign', tok, op=t.text, lhs=lhs, rhs=rhs,
                             optok=optok)
        return lhs

    def cond(self):
        tok = self.p
        c = self.binary(0)
        if self.at('?'):
            optok = self.p
            self.p += 1
            a = self.expr()
            self.eat(':')
            b = self.cond()
            return self.node('cond', tok, c=c, a=a, b=b, optok=optok)
        return c

    def binary(self, level):
        if level == len(BIN_LEVELS):
            return self.unary()
        tok = self.p
        left = self.binary(level + 1)
        ops = BIN_LEVELS[level]
        while True:
            t = self.peek()
            if t is None or t.text not in ops or t.kind not in ('op', 'kw'):
                return left
            optok = self.p
            self.p += 1
            if t.text == 'instanceof':
                ty = self.type_()
                left = self.node('instanceof', tok, e=left, type=ty, optok=optok)
            else:
                right = self.binary(level + 1)
                left = self.node('binary', tok, op=t.text, a=left, b=right,
                                 optok=optok)

    def is_cast(self):
        """At '(' : is this "(Type) operand"?"""
        j = 1
        t = self.peek(j)
        if t is None:
            return False
        if t.kind == 'kw' and t.text in ('int', 'float'):
            j += 1
            prim = True
        elif t.kind == 'id':
            j += 1
            prim = False
            while self.at('.', j) and self.kind(j + 1) == 'id':
                j += 2
        else:
            return False
        while self.at('[', j) and self.at(']', j + 1):
            j += 2
        if not self.at(')', j):
            return False
        nx = self.peek(j + 1)
        if nx is None:
            return False
        if prim:
            return True
        if nx.kind in ('id', 'int', 'float', 'str', 'rid', 'chr'):
            return True
        if nx.kind == 'kw' and nx.text in ('this', 'super', 'null', 'true',
                                           'false', 'new'):
            return True
        if nx.kind == 'op' and nx.text in ('(', '!', '~'):
            return True
        return False

    def unary(self):
        tok = self.p
        t = self.peek()
        if t is None:
            self.fail('expression expected')
        if t.kind == 'op':
            if t.text in ('-', '+', '!', '~', '++', '--'):
                self.p += 1
                e = self.unary()
                return self.node('unary', tok, op=t.text, e=e)
            if t.text == '(' and self.is_cast():
                self.p += 1
                ty = self.type_()
                self.eat(')')
                e = self.unary()
                return self.node('cast', tok, type=ty, e=e)
        return self.postfix()

    def postfix(self):
        tok = self.p
        e = self.primary()
        while True:
            t = self.peek()
            if t is None:
                return e
            if t.kind == 'op':
                if t.text == '.':
                    self.p += 1
                    ntok = self.p
                    name = self.ident()
                    if self.at('('):
                        target = self.node('field', tok, obj=e, name=name,
                                           name_tok=ntok)
                        args = self.args()
                        e = self.node('call', tok, target=target, args=args,
                                      name_tok=ntok, end=self.p)
                    else:
                        e = self.node('field', tok, obj=e, name=name,
                                      name_tok=ntok)
                    continue
                if t.text == '[':
                    self.p += 1
                    i = self.expr()
                    self.eat(']')
                    e = self.node('index', tok, arr=e, idx=i)
                    continue
                if t.text in ('++', '--'):
                    self.p += 1
                    e = self.node('postfix', tok, op=t.text, e=e)
                    continue
            return e

    def primary(self):
        tok = self.p
        t = self.peek()
        if t is None:
            self.fail('expression expected')
        k, x = t.kind, t.text
        if k in ('int', 'float', 'str', 'rid', 'chr'):
            self.p += 1
            return self.node('lit', tok, lit=k, text=x)
        if k == 'id':
            self.p += 1
            if self.at('('):
                target = self.node('name', tok, name=x)
                args = self.args()
                return self.node('call', tok, target=target, args=args,
                                 name_tok=tok, end=self.p)
            return self.node('name', tok, name=x)
        if k == 'kw':
            if x in ('true', 'false'):
                self.p += 1
                return self.node('lit', tok, lit='bool', text=x)
            if x == 'null':
                self.p += 1
                return self.node('null', tok)
            if x == 'this':
                self.p += 1
                return self.node('this', tok)
            if x == 'super':
                self.p += 1
                return self.node('super', tok)
            if x == 'new':
                self.p += 1
                ty = self.type_base_only()
                if self.at('['):
                    self.p += 1
                    size = self.expr()
                    self.eat(']')
                    dims = 1
                    while self.at('[') and self.at(']', 1):
                        self.p += 2
                        dims += 1
                    ty.dims = dims
                    return self.node('newarray', tok, type=ty, size=size)
                args = self.args()
                return self.node('new', tok, type=ty, args=args, end=self.p)
        if k == 'op' and x == '(':
            self.p += 1
            e = self.expr()
            self.eat(')')
            return self.node('paren', tok, e=e)
        self.fail('expression expected')

    def type_base_only(self):
        tok = self.p
        t = self.peek()
        if t is not None and t.kind == 'kw' and t.text in ('int', 'float'):
            self.p += 1
            base = t.text
        else:
            base = self.qname()
        return self.node('type', tok, base=base, dims=0, end=self.p)


def parse(text):
    """Parse one source text; returns the unit node."""
    p = Parser(text)
    u = p.unit()
    u.ntokens = p.n
    u.toks = p.toks
    return u


# ---------------------------------------------------------------------------
# walking
# ---------------------------------------------------------------------------

def children(node):
    """Child nodes of a node in source order."""
    for v in node.values():
        if isinstance(v, N):
            yield v
        elif isinstance(v, list):
            for x in v:
                if isinstance(x, N):
                    yield x


def walk(node):
    """Every node below (and including) node, pre-order, source order."""
    yield node
    kids = sorted(children(node), key=lambda x: x.tok)
    for c in kids:
        for x in walk(c):
            yield x


if __name__ == '__main__':
    import oracle
    for name in sys.argv[1:]:
        e = oracle.entry(name)
        u = parse(oracle.load_source(e))
        print(e.source, 'package', u.package, 'imports',
              [i.name + ('.*' if i.star else '') for i in u.imports])
        for c in u.classes:
            print(' ', c.kind, c.name, 'extends', c.extends, 'implements',
                  c.implements, 'mods', c.mods)
            for m in c.members:
                if m.k == 'field':
                    print('    field', m.mods, m.type.base, m.type.dims, m.name,
                          m.dims, 'init' if m.init else '')
                else:
                    print('   ', m.k, m.mods, m.ret and (m.ret.base, m.ret.dims),
                          m.name, [(p.type.base, p.type.dims, p.name) for p in m.params],
                          'body' if m.body else ';')
