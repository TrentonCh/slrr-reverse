#!/usr/bin/env python3
"""jparse.py - recursive-descent parser for the SLRR script dialect.

ANALYSIS parser (not the compiler's yacc grammar): it turns a source file into
an AST that the expression study walks.  Its grammar is Java's, cut down to
what the 3,454 corpus sources use; `python -I jparse.py` parses all of them
and reports failures (none).

    import jparse
    unit = jparse.parse(text)            # text from oracle.load_source
    unit.package, unit.imports, unit.types
    t = unit.types[0]                    # TypeDecl: name, extends, implements,
                                         #   members (FieldDecl / MethodDecl)

Every AST object is an `N` with a `.k` (kind) and named attributes, plus
    .line     line of its first token
    .t0, .t1  index of its first token and of the token after its last one

Expression kinds (k):
    lit      kind in int/float/str/rid/null/bool, text
    name     id                      simple identifier
    this
    field    obj, name               obj.name          (obj: expr or 'super' N)
    index    arr, idx                arr[idx]
    call     obj, name, args         obj.name(args); obj None for name(args);
                                     obj.k == 'super' for super.name(args)
    new      type, args              new T(args)
    newarray type, size              new T[size]   (type = element type)
    unary    op, x                   op in + - ! ~ ++ --
    postfix  op, x                   op in ++ --
    binary   op, l, r
    assign   op, l, r                op in = += -= ...
    cond     c, a, b
    cast     type, x
    instanceof x, type
    paren    x

Statement kinds:
    block stmts | local type decls[(name, dims, init, N)] | expr e | if c a b
    for init[] cond update[] body | while c body | do body c
    switch e items[]  (items: N('case', e) / N('default') / statements)
    break | continue | return e | empty | ctorcall which ('this'/'super') args

Types are N('type', name=dotted or 'int'/'float'/'void', dims=n).
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import srclex  # noqa: E402


class ParseError(ValueError):
    pass


class N(object):
    """AST node: kind + attributes."""

    def __init__(self, k, **kw):
        self.k = k
        self.line = 0
        self.t0 = 0
        self.t1 = 0
        self.__dict__.update(kw)

    def __repr__(self):
        d = dict(self.__dict__)
        k = d.pop('k')
        for x in ('line', 't0', 't1'):
            d.pop(x, None)
        return '%s(%s)' % (k, ', '.join('%s=%r' % kv for kv in d.items()))


MODIFIERS = frozenset(('public', 'private', 'protected', 'static', 'final',
                       'abstract', 'native', 'synchronized'))
ASSIGN_OPS = frozenset(('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
                        '<<=', '>>=', '>>>='))
BINARY_LEVELS = [
    ('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='),
    ('<', '>', '<=', '>=', 'instanceof'), ('<<', '>>', '>>>'), ('+', '-'),
    ('*', '/', '%'),
]


class Parser(object):
    def __init__(self, text):
        self.toks = srclex.tokens(text)
        self.n = len(self.toks)
        self.i = 0

    # ---- token helpers --------------------------------------------------
    def peek(self, k=0):
        j = self.i + k
        return self.toks[j] if j < self.n else None

    def text(self, k=0):
        t = self.peek(k)
        return t.text if t is not None else None

    def kind(self, k=0):
        t = self.peek(k)
        return t.kind if t is not None else None

    def at(self, s, k=0):
        t = self.peek(k)
        return t is not None and t.text == s and t.kind in ('op', 'kw')

    def err(self, msg):
        t = self.peek()
        where = 'line %d near %r' % (t.line, t.text) if t else 'end of file'
        raise ParseError('%s (%s)' % (msg, where))

    def expect(self, s):
        if not self.at(s):
            self.err('expected %r' % s)
        self.i += 1
        return self.toks[self.i - 1]

    def accept(self, s):
        if self.at(s):
            self.i += 1
            return True
        return False

    def ident(self):
        t = self.peek()
        if t is None or t.kind != 'id':
            self.err('expected identifier')
        self.i += 1
        return t.text

    def mk(self, k, t0, **kw):
        n = N(k, **kw)
        n.t0 = t0
        n.t1 = self.i
        n.line = self.toks[t0].line if t0 < self.n else 0
        return n

    # ---- compilation unit -----------------------------------------------
    def unit(self):
        t0 = self.i
        package = None
        imports = []
        types = []
        while self.i < self.n:
            if self.at('package'):
                self.i += 1
                package = self.qname()
                self.expect(';')
            elif self.at('import'):
                self.i += 1
                name = self.ident()
                star = False
                if self.accept(':'):         # import parts:part.root.BodyPart.*;
                    name += ':' + self.ident()
                while self.accept('.'):
                    if self.accept('*'):
                        star = True
                        break
                    name += '.' + self.ident()
                self.expect(';')
                imports.append((name, star))
            elif self.at(';'):
                self.i += 1
            else:
                types.append(self.type_decl())
        return self.mk('unit', t0, package=package, imports=imports, types=types)

    def qname(self):
        name = self.ident()
        while self.at('.') and self.kind(1) == 'id':
            self.i += 1
            name += '.' + self.ident()
        return name

    def modifiers(self):
        mods = []
        while self.kind() == 'kw' and self.text() in MODIFIERS:
            mods.append(self.text())
            self.i += 1
        return mods

    def type_decl(self):
        t0 = self.i
        mods = self.modifiers()
        if self.at('class'):
            kind = 'class'
        elif self.at('interface'):
            kind = 'interface'
        else:
            self.err('expected class or interface')
        self.i += 1
        name = self.ident()
        ext = None
        impl = []
        if self.accept('extends'):
            ext = self.qname()
            while self.accept(','):          # interface extends A, B
                impl.append(self.qname())
        if self.accept('implements'):
            impl.append(self.qname())
            while self.accept(','):
                impl.append(self.qname())
        self.expect('{')
        members = []
        while not self.at('}'):
            if self.i >= self.n:
                self.err('unterminated class body')
            if self.accept(';'):
                continue
            members.extend(self.member(name))
        self.expect('}')
        return self.mk('typedecl', t0, kind=kind, name=name, mods=mods,
                       extends=ext, implements=impl, members=members)

    def is_type_start(self):
        t = self.peek()
        return t is not None and (t.kind == 'id' or
                                  (t.kind == 'kw' and t.text in ('int', 'float', 'void')))

    def type(self):
        t0 = self.i
        t = self.peek()
        if t is None:
            self.err('expected type')
        if t.kind == 'kw' and t.text in ('int', 'float', 'void'):
            self.i += 1
            name = t.text
        elif t.kind == 'id':
            name = self.qname()
        else:
            self.err('expected type')
        dims = 0
        while self.at('[') and self.at(']', 1):
            self.i += 2
            dims += 1
        return self.mk('type', t0, name=name, dims=dims)

    def member(self, class_name):
        t0 = self.i
        mods = self.modifiers()
        # constructor: Name (
        if self.kind() == 'id' and self.at('(', 1):
            name = self.ident()
            params = self.params()
            body = self.method_body()
            return [self.mk('method', t0, mods=mods, name='<init>', ctor_name=name,
                            ret=None, params=params, body=body)]
        typ = self.type()
        name_t0 = self.i
        name = self.ident()
        if self.at('('):
            params = self.params()
            while self.at('[') and self.at(']', 1):   # int f()[]
                self.i += 2
                typ.dims += 1
            body = self.method_body()
            return [self.mk('method', t0, mods=mods, name=name, ctor_name=None,
                            ret=typ, params=params, body=body)]
        out = []
        while True:
            dims = 0
            while self.at('[') and self.at(']', 1):
                self.i += 2
                dims += 1
            init = None
            if self.accept('='):
                init = self.expr()
            out.append(self.mk('fielddecl', name_t0, mods=mods, type=typ, name=name,
                               dims=dims, init=init))
            if self.accept(','):
                name_t0 = self.i
                name = self.ident()
                continue
            break
        self.expect(';')
        return out

    def params(self):
        self.expect('(')
        out = []
        if not self.at(')'):
            while True:
                t0 = self.i
                self.modifiers()
                typ = self.type()
                name = self.ident()
                dims = 0
                while self.at('[') and self.at(']', 1):
                    self.i += 2
                    dims += 1
                out.append(self.mk('param', t0, type=typ, name=name, dims=dims))
                if not self.accept(','):
                    break
        self.expect(')')
        return out

    def method_body(self):
        if self.accept(';'):
            return None
        return self.block()

    # ---- statements -----------------------------------------------------
    def block(self):
        t0 = self.i
        self.expect('{')
        stmts = []
        while not self.at('}'):
            if self.i >= self.n:
                self.err('unterminated block')
            stmts.append(self.statement())
        self.expect('}')
        return self.mk('block', t0, stmts=stmts)

    def looks_like_local_decl(self):
        """Type name ...  at the current position?"""
        j = self.i
        t = self.peek()
        if t is None:
            return False
        if t.kind == 'kw' and t.text == 'final':
            return True
        if t.kind == 'kw' and t.text in ('int', 'float'):
            return True
        if t.kind != 'id':
            return False
        j += 1
        while j + 1 < self.n and self.toks[j].text == '.' and self.toks[j + 1].kind == 'id':
            j += 2
        while j + 1 < self.n and self.toks[j].text == '[' and self.toks[j + 1].text == ']':
            j += 2
        return j < self.n and self.toks[j].kind == 'id'

    def local_decl(self):
        t0 = self.i
        self.modifiers()
        typ = self.type()
        decls = []
        while True:
            d0 = self.i
            name = self.ident()
            dims = 0
            while self.at('[') and self.at(']', 1):
                self.i += 2
                dims += 1
            init = None
            if self.accept('='):
                init = self.expr()
            decls.append(self.mk('decl', d0, name=name, dims=dims, init=init))
            if not self.accept(','):
                break
        return self.mk('local', t0, type=typ, decls=decls)

    def statement(self):
        t0 = self.i
        t = self.peek()
        if t is None:
            self.err('expected statement')
        x = t.text
        if t.kind == 'op':
            if x == '{':
                return self.block()
            if x == ';':
                self.i += 1
                return self.mk('empty', t0)
        if t.kind == 'kw':
            if x == 'if':
                self.i += 1
                self.expect('(')
                c = self.expr()
                self.expect(')')
                a = self.statement()
                b = None
                if self.accept('else'):
                    b = self.statement()
                return self.mk('if', t0, c=c, a=a, b=b)
            if x == 'while':
                self.i += 1
                self.expect('(')
                c = self.expr()
                self.expect(')')
                body = self.statement()
                return self.mk('while', t0, c=c, body=body)
            if x == 'do':
                self.i += 1
                body = self.statement()
                self.expect('while')
                self.expect('(')
                c = self.expr()
                self.expect(')')
                self.accept(';')
                return self.mk('do', t0, c=c, body=body)
            if x == 'for':
                self.i += 1
                self.expect('(')
                init = []
                if not self.at(';'):
                    if self.looks_like_local_decl():
                        init.append(self.local_decl())
                    else:
                        while True:
                            e0 = self.i
                            init.append(self.mk('expr', e0, e=self.expr()))
                            if not self.accept(','):
                                break
                self.expect(';')
                cond = None if self.at(';') else self.expr()
                self.expect(';')
                update = []
                if not self.at(')'):
                    while True:
                        e0 = self.i
                        update.append(self.mk('expr', e0, e=self.expr()))
                        if not self.accept(','):
                            break
                self.expect(')')
                body = self.statement()
                return self.mk('for', t0, init=init, cond=cond, update=update, body=body)
            if x == 'switch':
                self.i += 1
                self.expect('(')
                e = self.expr()
                self.expect(')')
                self.expect('{')
                items = []
                while not self.at('}'):
                    c0 = self.i
                    if self.accept('case'):
                        ce = self.expr()
                        self.expect(':')
                        items.append(self.mk('case', c0, e=ce))
                    elif self.accept('default'):
                        self.expect(':')
                        items.append(self.mk('default', c0))
                    else:
                        items.append(self.statement())
                self.expect('}')
                return self.mk('switch', t0, e=e, items=items)
            if x == 'break':
                self.i += 1
                self.expect(';')
                return self.mk('break', t0)
            if x == 'continue':
                self.i += 1
                self.expect(';')
                return self.mk('continue', t0)
            if x == 'return':
                self.i += 1
                e = None if self.at(';') else self.expr()
                self.expect(';')
                return self.mk('return', t0, e=e)
            if x in ('this', 'super') and self.at('(', 1):
                self.i += 1
                args = self.args()
                self.expect(';')
                return self.mk('ctorcall', t0, which=x, args=args)
        if self.looks_like_local_decl():
            d = self.local_decl()
            self.expect(';')
            return d
        e = self.expr()
        self.expect(';')
        return self.mk('expr', t0, e=e)

    # ---- expressions ----------------------------------------------------
    def args(self):
        self.expect('(')
        out = []
        if not self.at(')'):
            while True:
                out.append(self.expr())
                if not self.accept(','):
                    break
        self.expect(')')
        return out

    def expr(self):
        return self.assignment()

    def assignment(self):
        t0 = self.i
        left = self.conditional()
        t = self.peek()
        if t is not None and t.kind == 'op' and t.text in ASSIGN_OPS:
            self.i += 1
            optok = self.i - 1
            right = self.assignment()
            return self.mk('assign', t0, op=t.text, l=left, r=right, optok=optok)
        return left

    def conditional(self):
        t0 = self.i
        c = self.binary(0)
        if self.at('?'):
            self.i += 1
            a = self.assignment()
            self.expect(':')
            b = self.conditional()
            return self.mk('cond', t0, c=c, a=a, b=b)
        return c

    def binary(self, level):
        if level == len(BINARY_LEVELS):
            return self.unary()
        t0 = self.i
        left = self.binary(level + 1)
        ops = BINARY_LEVELS[level]
        while True:
            t = self.peek()
            if t is None or t.kind not in ('op', 'kw') or t.text not in ops:
                return left
            self.i += 1
            optok = self.i - 1
            if t.text == 'instanceof':
                typ = self.type()
                left = self.mk('instanceof', t0, x=left, type=typ, optok=optok)
            else:
                right = self.binary(level + 1)
                left = self.mk('binary', t0, op=t.text, l=left, r=right, optok=optok)

    def is_cast(self):
        """'(' Type ')' followed by the start of an operand."""
        if not self.at('('):
            return False
        j = self.i + 1
        if j >= self.n:
            return False
        t = self.toks[j]
        prim = t.kind == 'kw' and t.text in ('int', 'float')
        if not (prim or t.kind == 'id'):
            return False
        j += 1
        if not prim:
            while j + 1 < self.n and self.toks[j].text == '.' and self.toks[j + 1].kind == 'id':
                j += 2
        while j + 1 < self.n and self.toks[j].text == '[' and self.toks[j + 1].text == ']':
            j += 2
        if j + 1 >= self.n or self.toks[j].text != ')' or self.toks[j].kind != 'op':
            return False
        nx = self.toks[j + 1]
        if prim:
            return True
        if nx.kind in ('id', 'int', 'float', 'str', 'rid', 'chr'):
            return True
        if nx.kind == 'kw' and nx.text in ('this', 'super', 'null', 'true', 'false', 'new'):
            return True
        if nx.kind == 'op' and nx.text in ('(', '!', '~'):
            return True
        return False

    def unary(self):
        t0 = self.i
        t = self.peek()
        if t is None:
            self.err('expected expression')
        if t.kind == 'op' and t.text in ('+', '-', '!', '~', '++', '--'):
            self.i += 1
            x = self.unary()
            return self.mk('unary', t0, op=t.text, x=x)
        if self.is_cast():
            self.i += 1
            typ = self.type()
            self.expect(')')
            x = self.unary()
            return self.mk('cast', t0, type=typ, x=x)
        return self.postfix()

    def postfix(self):
        t0 = self.i
        e = self.primary()
        while True:
            t = self.peek()
            if t is None:
                return e
            if t.kind == 'op' and t.text == '.':
                self.i += 1
                name = self.ident()
                if self.at('('):
                    args = self.args()
                    e = self.mk('call', t0, obj=e, name=name, args=args)
                else:
                    e = self.mk('field', t0, obj=e, name=name)
            elif t.kind == 'op' and t.text == '[':
                self.i += 1
                idx = self.expr()
                self.expect(']')
                e = self.mk('index', t0, arr=e, idx=idx)
            elif t.kind == 'op' and t.text in ('++', '--'):
                self.i += 1
                e = self.mk('postfix', t0, op=t.text, x=e)
            else:
                return e

    def primary(self):
        t0 = self.i
        t = self.peek()
        if t is None:
            self.err('expected expression')
        k, x = t.kind, t.text
        if k in ('int', 'float', 'str', 'rid', 'chr'):
            self.i += 1
            return self.mk('lit', t0, kind=k, text=x)
        if k == 'kw':
            if x == 'null':
                self.i += 1
                return self.mk('lit', t0, kind='null', text=x)
            if x in ('true', 'false'):
                self.i += 1
                return self.mk('lit', t0, kind='bool', text=x)
            if x == 'this':
                self.i += 1
                return self.mk('this', t0)
            if x == 'super':
                self.i += 1
                return self.mk('super', t0)
            if x == 'new':
                self.i += 1
                ty0 = self.i
                tt = self.peek()
                if tt is not None and tt.kind == 'kw' and tt.text in ('int', 'float'):
                    self.i += 1
                    name = tt.text
                else:
                    name = self.qname()
                typ = self.mk('type', ty0, name=name, dims=0)
                if self.at('['):
                    self.i += 1
                    size = self.expr()
                    self.expect(']')
                    extra = 0
                    while self.at('[') and self.at(']', 1):
                        self.i += 2
                        extra += 1
                    return self.mk('newarray', t0, type=typ, size=size, extra=extra)
                args = self.args()
                return self.mk('new', t0, type=typ, args=args)
        if k == 'op' and x == '(':
            self.i += 1
            e = self.expr()
            self.expect(')')
            return self.mk('paren', t0, x=e)
        if k == 'id':
            self.i += 1
            if self.at('('):
                args = self.args()
                return self.mk('call', t0, obj=None, name=x, args=args)
            return self.mk('name', t0, id=x)
        self.err('expected expression')


def parse(text):
    p = Parser(text)
    u = p.unit()
    u.toks = p.toks
    return u


def walk(node):
    """Yield every N below (and including) node, pre-order."""
    stack = [node]
    while stack:
        x = stack.pop()
        if isinstance(x, N):
            yield x
            for v in reversed(list(x.__dict__.values())):
                if isinstance(v, (N, list, tuple)):
                    stack.append(v)
        elif isinstance(x, (list, tuple)):
            for v in reversed(x):
                if isinstance(v, (N, list, tuple)):
                    stack.append(v)


def main(argv):
    import oracle
    from collections import Counter
    ok = bad = 0
    kinds = Counter()
    for e in oracle.iter_entries():
        if e.source is None:
            continue
        text = oracle.load_source(e)
        try:
            u = parse(text)
        except (ParseError, srclex.LexError) as x:
            bad += 1
            print('FAIL %s: %s' % (e.source, x))
            continue
        ok += 1
        for n in walk(u):
            kinds[n.k] += 1
    print('parsed %d sources, %d failures' % (ok, bad))
    for k, v in sorted(kinds.items()):
        print('  %-12s %d' % (k, v))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
