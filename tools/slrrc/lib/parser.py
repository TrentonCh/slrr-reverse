#!/usr/bin/env python3
"""parser.py - recursive descent parser for the SLRR script dialect.

    import parser
    unit = parser.parse(text, name='Marker.java')     # -> Node 'Unit'
    unit.package, unit.imports, unit.types
    for cls in unit.types:
        for m in cls.members: ...
    parser.dump(unit)                                  # indented text
    unit.to_dict()                                     # plain dict / list / str

The grammar is the one written down in notes/grammar.md; this file is its
executable form.  Every one of the 3,454 shipped sources parses
(check_parse.py), and the classes, fields and methods found agree with the
class files of all pairs (check_members.py).

The AST is plain: every node is a Node with
    kind            node kind (see below)
    line, col       position of its first token
    end_line        line of its last token
    tok0, tok1      index of its first and last token in unit.tokens
    op_tok          index of the operator token (Binary, InstanceOf, Assign,
                    Cond, Unary, Postfix)
    name_tok        index of the member or method name token (Field, Call)
plus the fields of its kind.  The token positions are kept because the
compiler stamps each emitted node with the line the parser had reached, which
is a function of token positions, not of the tree.

Declarations
    Unit        package (dotted str | None), imports [Import], types [Class];
                also tokens (the lexer's list), comments, notes
    Import      name (dotted str, without the ".*"), star (bool),
                pack (str | None: the word before ":" in "import parts:a.b.*;")
    Class       interface (bool), modifiers [str], name, extends (str | None),
                extends_list [str], implements [str], members [...]
    FieldDecl   modifiers, type (Type), declarators [Declarator]
    Declarator  name, dims (int: brackets after the name), init (expr | None)
    MethodDecl  modifiers, type (Type | None for a constructor), name,
                params [Param], dims (brackets after the parameter list),
                body (Block | None when the declaration ends with ';'),
                constructor (bool)
    Param       modifiers, type (Type), name, dims
    Initializer static (bool), body (Block)       (the corpus has none)
    Type        name ('int' | 'float' | 'void' | dotted class name),
                dims (int), primitive (bool)

Statements
    Block       stmts
    LocalVar    modifiers, type, declarators
    ExprStmt    expr
    If          cond, then, otherwise (stmt | None)
    While       cond, body
    Do          body, cond
    For         init (LocalVar | [expr]), cond (expr | None), update [expr], body
    Switch      expr, groups [SwitchGroup]
    SwitchGroup labels [CaseLabel | DefaultLabel] (one or more), stmts
                (the statements up to the next label; they form one scope)
    CaseLabel   value (expr)
    DefaultLabel
    Break, Continue   label (str | None; always None in the corpus)
    Return      value (expr | None)
    Empty
    Labeled     label, body                        (the corpus has none)

Expressions
    Literal     lit ('int' | 'float' | 'str' | 'rid' | 'null' | 'bool'), text
    Name        name                    a single identifier
    This, Super
    Field       target (expr), name     target.name
    Index       target, index           target[index]
    Call        target (expr | None), name, args      name(args), target.name(args)
    CtorCall    which ('this' | 'super'), args         this(args), super(args)
    New         type (Type), args
    NewArray    type (element Type), sizes [expr], dims (trailing empty brackets),
                init (ArrayInit | None)
    ArrayInit   elements                            (the corpus has none)
    Unary       op ('+' '-' '!' '~' '++' '--'), operand
    Postfix     op ('++' '--'), operand
    Binary      op, left, right
    Assign      op ('=' '+=' ...), target, value
    Cond        cond, then, otherwise
    Cast        type (Type), operand
    InstanceOf  operand, type (Type)
    Paren       expr

unit.notes is a list of (code, line, text): constructs the parser accepts
although a Java compiler would reject them, and a few it wants counted
(see NOTE_CODES).  A source that does not fit the grammar raises ParseError.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer  # noqa: E402

Token = lexer.Token

NOTE_CODES = {
    'rid-literal': 'resource id literal pack:0xHEXr',
    'body-semicolon': 'method or constructor without body (";") that is neither abstract nor native nor in an interface',
    'native-field': 'field declared native',
    'abstract-field': 'field declared abstract',
    'native-body': 'native method with a body',
    'stray-semicolon-member': '";" between class members',
    'stray-semicolon-top': '";" between top-level declarations',
    'ctor-name': 'constructor whose name is not the class name (a method without return type)',
    'modifier-repeated': 'the same modifier twice',
    'import-after-type': 'import after a type declaration',
    'no-package': 'no package declaration',
    'import-colon': 'import whose name starts with "word:"',
    'import-single': 'import without ".*"',
    'assign-target': 'assignment to something that is not a name, field or array element',
    'incdec-target': '++ / -- applied to something that is not a name, field or array element',
    'expr-statement': 'expression statement that is not an assignment, call, ++/-- or new',
    'extends-list': 'class extends more than one name',
    'java-reserved-id': 'Java reserved word used as an identifier',
    'local-modifier': 'modifier other than final on a local variable or parameter',
    'case-after-default': 'case label after the default label',
    'default-repeated': 'second default label',
    'label': 'labelled statement, or break / continue with a label',
    'array-init': 'array initialiser list',
    'multi-dim': 'array type or creation with more than one dimension',
    'method-dims': 'brackets after a parameter list',
    'float-suffix-d': 'float literal with suffix d',
    'empty-body-class': 'class with an empty body',
    'decl-as-substatement': 'declaration as the direct body of if / else / for / while / do',
}


class ParseError(ValueError):
    def __init__(self, message, tok, name=None, construct=None):
        ValueError.__init__(self, '%s:%d:%d: %s (at %s)' % (
            name or '<source>', tok.line, tok.col, message,
            repr(tok.text) if tok.kind != 'eof' else 'end of file'))
        self.message = message
        self.tok = tok
        self.line = tok.line
        self.col = tok.col
        self.name = name
        self.construct = construct


class Node(object):
    """One AST node.  Fields are plain attributes; fields() lists them in
    declaration order."""
    META = ('kind', 'line', 'col', 'end_line', 'tok0', 'tok1', 'op_tok', 'name_tok')

    def __init__(self, kind, tok, **fields):
        self.kind = kind
        self.line = tok.line
        self.col = tok.col
        self.end_line = tok.line
        self.tok0 = tok.index
        self.tok1 = tok.index
        self.__dict__.update(fields)

    def fields(self):
        return [(k, v) for k, v in self.__dict__.items()
                if k not in Node.META and k not in ('tokens', 'notes', 'comments')]

    def children(self):
        """Direct child nodes in field order."""
        for _, v in self.fields():
            if isinstance(v, Node):
                yield v
            elif isinstance(v, list):
                for x in v:
                    if isinstance(x, Node):
                        yield x

    def walk(self):
        """This node and every node below it, parents first, source order."""
        stack = [self]
        while stack:
            n = stack.pop()
            yield n
            stack.extend(reversed(list(n.children())))

    def to_dict(self):
        d = {'kind': self.kind, 'line': self.line}
        for k, v in self.fields():
            d[k] = _plain(v)
        return d

    def __repr__(self):
        return '<%s line %d>' % (self.kind, self.line)


def _plain(v):
    if isinstance(v, Node):
        return v.to_dict()
    if isinstance(v, list):
        return [_plain(x) for x in v]
    return v


def dump(node, indent=0, out=None):
    """Indented text form of a tree (returns the text when out is None)."""
    lines = [] if out is None else out
    pad = '  ' * indent
    scalars = []
    subs = []
    for k, v in node.fields():
        if isinstance(v, Node) or (isinstance(v, list) and v and
                                   any(isinstance(x, Node) for x in v)):
            subs.append((k, v))
        elif v not in (None, [], False) or k in ('name', 'op', 'lit'):
            scalars.append('%s=%s' % (k, v if not isinstance(v, str) else repr(v)))
    lines.append('%s%s @%d %s' % (pad, node.kind, node.line, ' '.join(scalars)))
    for k, v in subs:
        if isinstance(v, Node):
            lines.append('%s  .%s' % (pad, k))
            dump(v, indent + 2, lines)
        else:
            lines.append('%s  .%s [%d]' % (pad, k, len(v)))
            for x in v:
                if isinstance(x, Node):
                    dump(x, indent + 2, lines)
                else:
                    lines.append('%s    %r' % (pad, x))
    if out is None:
        return '\n'.join(lines)
    return None


# operator tables ------------------------------------------------------------

# binary operators: text -> precedence (higher binds tighter); all are left
# associative.  'instanceof' sits with the relational operators.
BINARY_PREC = {
    '||': 1,
    '&&': 2,
    '|': 3,
    '^': 4,
    '&': 5,
    '==': 6, '!=': 6,
    '<': 7, '>': 7, '<=': 7, '>=': 7, 'instanceof': 7,
    '<<': 8, '>>': 8, '>>>': 8,
    '+': 9, '-': 9,
    '*': 10, '/': 10, '%': 10,
}
ASSIGN_OPS = frozenset(('=', '*=', '/=', '%=', '+=', '-=', '<<=', '>>=', '>>>=',
                        '&=', '^=', '|='))
PREFIX_OPS = frozenset(('+', '-', '!', '~', '++', '--'))
MODIFIERS = frozenset(lexer.MODIFIERS)
LVALUE_KINDS = ('Name', 'Field', 'Index')
STATEMENT_EXPR_KINDS = ('Assign', 'Call', 'CtorCall', 'New', 'Postfix')


class Parser(object):
    # Tables as class attributes so that check_sensitivity.py can parse with
    # a deliberately wrong grammar and count the trees that notice.
    PREC = BINARY_PREC
    RIGHT_ASSOC = frozenset()

    def __init__(self, text, name=None):
        self.name = name
        self.comments = []
        self.toks = lexer.tokenize(text, self.comments, name)
        self.i = 0
        self.notes = []
        for t in self.toks:
            if t.kind == 'id' and t.text in lexer.JAVA_RESERVED_UNUSED:
                self.note('java-reserved-id', t, t.text)

    # -- token helpers -------------------------------------------------------

    def note(self, code, tok, text=''):
        assert code in NOTE_CODES, code
        self.notes.append((code, tok.line, text))

    def error(self, message, construct=None, tok=None):
        raise ParseError(message, tok or self.toks[self.i], self.name, construct)

    @property
    def tok(self):
        return self.toks[self.i]

    def peek(self, k=1):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else self.toks[-1]

    def at(self, text):
        t = self.toks[self.i]
        return t.text == text and t.kind in ('op', 'kw')

    def at_kind(self, kind):
        return self.toks[self.i].kind == kind

    def accept(self, text):
        t = self.toks[self.i]
        if t.text == text and t.kind in ('op', 'kw'):
            self.i += 1
            return t
        return None

    def expect(self, text, construct=None):
        t = self.toks[self.i]
        if t.text == text and t.kind in ('op', 'kw'):
            self.i += 1
            return t
        self.error('expected %r' % text, construct)

    def ident(self, construct=None):
        t = self.toks[self.i]
        if t.kind != 'id':
            self.error('expected an identifier', construct)
        self.i += 1
        return t

    def node(self, kind, tok, **fields):
        return Node(kind, tok, **fields)

    def done(self, node):
        """Close a node: its last token is the one consumed last."""
        last = self.toks[self.i - 1] if self.i else self.toks[0]
        node.tok1 = last.index
        node.end_line = last.line
        return node

    # -- names and types -----------------------------------------------------

    def qualified_name(self, construct=None):
        parts = [self.ident(construct).text]
        while self.at('.') and self.peek().kind == 'id':
            self.i += 1
            parts.append(self.ident(construct).text)
        return '.'.join(parts)

    def dims(self):
        """Count '[' ']' pairs."""
        n = 0
        while self.at('[') and self.peek().text == ']':
            self.i += 2
            n += 1
        return n

    def at_type_start(self):
        t = self.tok
        return t.kind == 'id' or (t.kind == 'kw' and t.text in lexer.PRIMITIVES)

    def type(self, construct=None):
        t = self.tok
        if t.kind == 'kw' and t.text in lexer.PRIMITIVES:
            self.i += 1
            n = self.node('Type', t, name=t.text, dims=0, primitive=True)
        elif t.kind == 'id':
            n = self.node('Type', t, name=self.qualified_name(construct), dims=0,
                          primitive=False)
        else:
            self.error('expected a type', construct)
        n.dims = self.dims()
        if n.dims > 1:
            self.note('multi-dim', t, n.name)
        return self.done(n)

    def modifiers(self):
        mods = []
        while self.tok.kind == 'kw' and self.tok.text in MODIFIERS:
            if self.tok.text in mods:
                self.note('modifier-repeated', self.tok, self.tok.text)
            mods.append(self.tok.text)
            self.i += 1
        return mods

    # -- compilation unit ----------------------------------------------------

    def unit(self):
        first = self.tok
        u = self.node('Unit', first, package=None, imports=[], types=[])
        if self.at('package'):
            self.i += 1
            u.package = self.qualified_name('package declaration')
            self.expect(';', 'package declaration')
        else:
            self.note('no-package', first)
        while not self.at_kind('eof'):
            t = self.tok
            if self.at('import'):
                if u.types:
                    self.note('import-after-type', t)
                u.imports.append(self.import_decl())
            elif self.at(';'):
                self.note('stray-semicolon-top', t)
                self.i += 1
            elif self.at('package'):
                self.error('second package declaration', 'package declaration')
            else:
                u.types.append(self.type_decl())
        self.done(u)
        u.tokens = self.toks
        u.notes = self.notes
        u.comments = self.comments
        return u

    def import_decl(self):
        t = self.expect('import')
        n = self.node('Import', t, name=None, star=False, pack=None)
        first = self.ident('import')
        if self.at(':') and self.peek().kind == 'id':
            # import parts:part.root.BodyPart.*;  (2 sources)
            self.note('import-colon', first, first.text)
            self.i += 1
            n.pack = first.text
            first = self.ident('import')
        parts = [first.text]
        while self.accept('.'):
            if self.accept('*'):
                n.star = True
                break
            parts.append(self.ident('import').text)
        n.name = '.'.join(parts)
        if not n.star:
            self.note('import-single', t, n.name)
        self.expect(';', 'import')
        return self.done(n)

    def type_decl(self):
        start = self.tok
        mods = self.modifiers()
        if self.at('class'):
            interface = False
        elif self.at('interface'):
            interface = True
        else:
            self.error('expected class or interface', 'type declaration')
        self.i += 1
        n = self.node('Class', start, interface=interface, modifiers=mods,
                      name=self.ident('type declaration').text, extends=None,
                      extends_list=[], implements=[], members=[])
        if self.accept('extends'):
            n.extends_list.append(self.qualified_name('extends'))
            while self.accept(','):
                n.extends_list.append(self.qualified_name('extends'))
            n.extends = n.extends_list[0]
            if len(n.extends_list) > 1 and not interface:
                self.note('extends-list', start, n.name)
        if self.accept('implements'):
            n.implements.append(self.qualified_name('implements'))
            while self.accept(','):
                n.implements.append(self.qualified_name('implements'))
        self.expect('{', 'class body')
        while not self.at('}'):
            if self.at_kind('eof'):
                self.error('class body is not closed', 'class body')
            if self.at(';'):
                self.note('stray-semicolon-member', self.tok, n.name)
                self.i += 1
                continue
            n.members.append(self.member(n))
        self.expect('}', 'class body')
        if not n.members:
            self.note('empty-body-class', start, n.name)
        return self.done(n)

    def member(self, cls):
        start = self.tok
        mods = self.modifiers()
        if self.at('{'):
            n = self.node('Initializer', start, static='static' in mods, body=None)
            n.body = self.block()
            return self.done(n)
        # constructor: Name '('
        if self.tok.kind == 'id' and self.peek().text == '(':
            name = self.ident().text
            if name != cls.name:
                self.note('ctor-name', start, name)
            n = self.node('MethodDecl', start, modifiers=mods, type=None, name=name,
                          params=None, dims=0, body=None, constructor=True)
            self.method_rest(n, cls)
            return self.done(n)
        if not self.at_type_start():
            self.error('expected a member declaration', 'class member')
        ty = self.type('member declaration')
        name_tok = self.ident('member declaration')
        if self.at('('):
            n = self.node('MethodDecl', start, modifiers=mods, type=ty,
                          name=name_tok.text, params=None, dims=0, body=None,
                          constructor=False)
            self.method_rest(n, cls)
            return self.done(n)
        n = self.node('FieldDecl', start, modifiers=mods, type=ty, declarators=[])
        if 'native' in mods:
            self.note('native-field', start, name_tok.text)
        if 'abstract' in mods:
            self.note('abstract-field', start, name_tok.text)
        self.i -= 1
        n.declarators = self.declarators('field declaration')
        self.expect(';', 'field declaration')
        return self.done(n)

    def method_rest(self, n, cls):
        self.expect('(', 'parameter list')
        n.params = []
        if not self.at(')'):
            while True:
                n.params.append(self.param())
                if not self.accept(','):
                    break
        self.expect(')', 'parameter list')
        n.dims = self.dims()
        if n.dims:
            self.note('method-dims', self.toks[self.i - 1], n.name)
        if self.at('{'):
            if 'native' in n.modifiers:
                self.note('native-body', self.tok, n.name)
            n.body = self.block()
        else:
            t = self.expect(';', 'method body')
            if not ('native' in n.modifiers or 'abstract' in n.modifiers or
                    cls.interface):
                self.note('body-semicolon', t, n.name)
        return n

    def param(self):
        start = self.tok
        mods = self.modifiers()
        if mods and mods != ['final']:
            self.note('local-modifier', start, ' '.join(mods))
        ty = self.type('parameter')
        name = self.ident('parameter').text
        n = self.node('Param', start, modifiers=mods, type=ty, name=name, dims=0)
        n.dims = self.dims()
        return self.done(n)

    def declarators(self, construct):
        out = []
        while True:
            t = self.ident(construct)
            d = self.node('Declarator', t, name=t.text, dims=0, init=None)
            d.dims = self.dims()
            if self.accept('='):
                if self.at('{'):
                    d.init = self.array_init()
                else:
                    d.init = self.expression()
            out.append(self.done(d))
            if not self.accept(','):
                break
        return out

    def array_init(self):
        t = self.expect('{')
        self.note('array-init', t)
        n = self.node('ArrayInit', t, elements=[])
        while not self.at('}'):
            if self.at('{'):
                n.elements.append(self.array_init())
            else:
                n.elements.append(self.expression())
            if not self.accept(','):
                break
        self.expect('}', 'array initialiser')
        return self.done(n)

    # -- statements ----------------------------------------------------------

    def block(self):
        t = self.expect('{', 'block')
        n = self.node('Block', t, stmts=[])
        while not self.at('}'):
            if self.at_kind('eof'):
                self.error('block is not closed', 'block')
            n.stmts.append(self.statement())
        self.i += 1
        return self.done(n)

    def at_local_decl(self):
        """True when the tokens ahead read  Type Identifier  (a local
        variable declaration) rather than an expression."""
        t = self.tok
        if t.kind == 'kw':
            if t.text in ('int', 'float'):
                return True
            if t.text in MODIFIERS:
                return True
            return False
        if t.kind != 'id':
            return False
        j = self.i + 1
        toks = self.toks
        while toks[j].text == '.' and toks[j].kind == 'op' and toks[j + 1].kind == 'id':
            j += 2
        while toks[j].text == '[' and toks[j + 1].text == ']':
            j += 2
        return toks[j].kind == 'id'

    def local_decl(self):
        start = self.tok
        mods = self.modifiers()
        if mods and mods != ['final']:
            self.note('local-modifier', start, ' '.join(mods))
        ty = self.type('local variable declaration')
        n = self.node('LocalVar', start, modifiers=mods, type=ty, declarators=[])
        n.declarators = self.declarators('local variable declaration')
        return self.done(n)

    def substatement(self, construct):
        """The statement that is the body of if / else / loops."""
        if self.at_local_decl():
            self.note('decl-as-substatement', self.tok, construct)
        return self.statement()

    def statement(self):
        t = self.tok
        if t.kind == 'op':
            if t.text == '{':
                return self.block()
            if t.text == ';':
                self.i += 1
                return self.done(self.node('Empty', t))
        elif t.kind == 'kw':
            x = t.text
            if x == 'if':
                return self.if_statement()
            if x == 'while':
                self.i += 1
                n = self.node('While', t, cond=None, body=None)
                self.expect('(', 'while')
                n.cond = self.expression()
                self.expect(')', 'while')
                n.body = self.substatement('while')
                return self.done(n)
            if x == 'do':
                self.i += 1
                n = self.node('Do', t, body=None, cond=None)
                n.body = self.substatement('do')
                self.expect('while', 'do')
                self.expect('(', 'do')
                n.cond = self.expression()
                self.expect(')', 'do')
                self.expect(';', 'do')
                return self.done(n)
            if x == 'for':
                return self.for_statement()
            if x == 'switch':
                return self.switch_statement()
            if x == 'return':
                self.i += 1
                n = self.node('Return', t, value=None)
                if not self.at(';'):
                    n.value = self.expression()
                self.expect(';', 'return')
                return self.done(n)
            if x in ('break', 'continue'):
                self.i += 1
                n = self.node('Break' if x == 'break' else 'Continue', t, label=None)
                if self.at_kind('id'):
                    n.label = self.ident().text
                    self.note('label', t, n.label)
                self.expect(';', x)
                return self.done(n)
            if x in ('case', 'default'):
                self.error('%s label outside a switch body' % x, 'switch')
            if x == 'else':
                self.error('else without if', 'if')
        if t.kind == 'id' and self.peek().text == ':' and self.peek().kind == 'op':
            self.i += 2
            self.note('label', t, t.text)
            n = self.node('Labeled', t, label=t.text, body=None)
            n.body = self.statement()
            return self.done(n)
        if self.at_local_decl():
            n = self.local_decl()
            self.expect(';', 'local variable declaration')
            return self.done(n)
        n = self.node('ExprStmt', t, expr=None)
        n.expr = self.expression()
        e = n.expr
        if e.kind not in STATEMENT_EXPR_KINDS and not (
                e.kind == 'Unary' and e.op in ('++', '--')):
            self.note('expr-statement', t, e.kind)
        self.expect(';', 'expression statement')
        return self.done(n)

    def if_statement(self):
        """An else belongs to the nearest if that has none yet."""
        t = self.expect('if')
        n = self.node('If', t, cond=None, then=None, otherwise=None)
        self.expect('(', 'if')
        n.cond = self.expression()
        self.expect(')', 'if')
        n.then = self.substatement('if')
        if self.accept('else'):
            n.otherwise = self.substatement('else')
        return self.done(n)

    def for_statement(self):
        t = self.expect('for')
        n = self.node('For', t, init=[], cond=None, update=[], body=None)
        self.expect('(', 'for')
        if not self.at(';'):
            if self.at_local_decl():
                n.init = self.local_decl()
            else:
                n.init = self.expression_list()
        self.expect(';', 'for')
        if not self.at(';'):
            n.cond = self.expression()
        self.expect(';', 'for')
        if not self.at(')'):
            n.update = self.expression_list()
        self.expect(')', 'for')
        n.body = self.substatement('for')
        return self.done(n)

    def expression_list(self):
        out = [self.expression()]
        while self.accept(','):
            out.append(self.expression())
        return out

    def switch_statement(self):
        t = self.expect('switch')
        n = self.node('Switch', t, expr=None, groups=[])
        self.expect('(', 'switch')
        n.expr = self.expression()
        self.expect(')', 'switch')
        self.expect('{', 'switch')
        seen_default = False
        group = None
        while not self.at('}'):
            c = self.tok
            if self.at('case') or self.at('default'):
                if group is None or group.stmts:
                    if group is not None:
                        self.done(group)
                    group = self.node('SwitchGroup', c, labels=[], stmts=[])
                    n.groups.append(group)
                self.i += 1
                if c.text == 'case':
                    lab = self.node('CaseLabel', c, value=None)
                    lab.value = self.expression()
                    self.expect(':', 'case label')
                    if seen_default:
                        self.note('case-after-default', c)
                else:
                    lab = self.node('DefaultLabel', c)
                    self.expect(':', 'default label')
                    if seen_default:
                        self.note('default-repeated', c)
                    seen_default = True
                group.labels.append(self.done(lab))
            elif self.at_kind('eof'):
                self.error('switch body is not closed', 'switch')
            else:
                if group is None:
                    self.error('statement before the first case label', 'switch')
                group.stmts.append(self.statement())
        if group is not None:
            self.done(group)
        self.i += 1
        return self.done(n)

    # -- expressions ---------------------------------------------------------

    def expression(self):
        """Assignment level (right associative)."""
        start = self.tok
        left = self.conditional()
        t = self.tok
        if t.kind == 'op' and t.text in ASSIGN_OPS:
            self.i += 1
            if left.kind not in LVALUE_KINDS:
                self.note('assign-target', t, left.kind)
            n = self.node('Assign', start, op=t.text, target=left, value=None)
            n.op_tok = t.index
            n.value = self.expression()
            return self.done(n)
        return left

    def conditional(self):
        start = self.tok
        c = self.binary(1)
        if self.at('?'):
            q = self.tok
            self.i += 1
            n = self.node('Cond', start, cond=c, then=None, otherwise=None)
            n.op_tok = q.index
            n.then = self.expression()
            self.expect(':', 'conditional expression')
            n.otherwise = self.conditional()
            return self.done(n)
        return c

    def binary(self, min_prec):
        start = self.tok
        left = self.unary()
        while True:
            t = self.tok
            if t.kind not in ('op', 'kw'):
                break
            prec = self.PREC.get(t.text)
            if prec is None or prec < min_prec:
                break
            self.i += 1
            if t.text == 'instanceof':
                n = self.node('InstanceOf', start, operand=left, type=None)
                n.op_tok = t.index
                n.type = self.type('instanceof')
            else:
                n = self.node('Binary', start, op=t.text, left=left, right=None)
                n.op_tok = t.index
                n.right = self.binary(prec if t.text in self.RIGHT_ASSOC else prec + 1)
            left = self.done(n)
        return left

    def at_cast(self):
        """At '(': decide whether a cast follows.  Returns True for
        '(' primitive dims ')'  and for  '(' Name dims ')' followed by a
        token that can start an operand other than + and -."""
        toks = self.toks
        j = self.i + 1
        t = toks[j]
        if t.kind == 'kw' and t.text in ('int', 'float'):
            j += 1
            while toks[j].text == '[' and toks[j + 1].text == ']':
                j += 2
            return toks[j].text == ')' and toks[j].kind == 'op'
        if t.kind != 'id':
            return False
        j += 1
        while toks[j].text == '.' and toks[j].kind == 'op' and toks[j + 1].kind == 'id':
            j += 2
        while toks[j].text == '[' and toks[j + 1].text == ']':
            j += 2
        if not (toks[j].text == ')' and toks[j].kind == 'op'):
            return False
        nx = toks[j + 1]
        if nx.kind in ('id', 'int', 'float', 'str', 'chr', 'rid'):
            return True
        if nx.kind == 'kw':
            return nx.text in ('this', 'super', 'null', 'true', 'false', 'new')
        if nx.kind == 'op':
            return nx.text in ('(', '!', '~')
        return False

    def unary(self):
        t = self.tok
        if t.kind == 'op':
            if t.text in PREFIX_OPS:
                self.i += 1
                n = self.node('Unary', t, op=t.text, operand=None)
                n.op_tok = t.index
                n.operand = self.unary()
                if t.text in ('++', '--') and n.operand.kind not in LVALUE_KINDS:
                    self.note('incdec-target', t, n.operand.kind)
                return self.done(n)
            if t.text == '(' and self.at_cast():
                self.i += 1
                n = self.node('Cast', t, type=None, operand=None)
                n.type = self.type('cast')
                self.expect(')', 'cast')
                n.operand = self.unary()
                return self.done(n)
        return self.postfix()

    def postfix(self):
        start = self.tok
        e = self.primary()
        while True:
            t = self.tok
            if t.kind != 'op':
                break
            if t.text == '.':
                self.i += 1
                name = self.ident('member access')
                if self.at('('):
                    n = self.node('Call', start, target=e, name=name.text, args=None)
                    n.name_tok = name.index
                    n.args = self.arguments()
                else:
                    n = self.node('Field', start, target=e, name=name.text)
                    n.name_tok = name.index
                e = self.done(n)
            elif t.text == '[':
                if e.kind == 'NewArray':
                    break
                self.i += 1
                n = self.node('Index', start, target=e, index=None)
                n.index = self.expression()
                self.expect(']', 'array access')
                e = self.done(n)
            elif t.text in ('++', '--'):
                self.i += 1
                if e.kind not in LVALUE_KINDS:
                    self.note('incdec-target', t, e.kind)
                n = self.node('Postfix', start, op=t.text, operand=e)
                n.op_tok = t.index
                e = self.done(n)
            else:
                break
        return e

    def arguments(self):
        self.expect('(', 'argument list')
        args = []
        if not self.at(')'):
            while True:
                args.append(self.expression())
                if not self.accept(','):
                    break
        self.expect(')', 'argument list')
        return args

    def primary(self):
        t = self.tok
        k = t.kind
        if k == 'id':
            self.i += 1
            if self.at('('):
                n = self.node('Call', t, target=None, name=t.text, args=None)
                n.name_tok = t.index
                n.args = self.arguments()
                return self.done(n)
            return self.done(self.node('Name', t, name=t.text))
        if k in ('int', 'float', 'str', 'chr', 'rid'):
            self.i += 1
            if k == 'rid':
                self.note('rid-literal', t, t.text)
            elif k == 'float' and t.text[-1] in 'dD':
                self.note('float-suffix-d', t, t.text)
            return self.done(self.node('Literal', t, lit=k, text=t.text))
        if k == 'kw':
            x = t.text
            if x == 'null':
                self.i += 1
                return self.done(self.node('Literal', t, lit='null', text=x))
            if x in ('true', 'false'):
                self.i += 1
                return self.done(self.node('Literal', t, lit='bool', text=x))
            if x in ('this', 'super'):
                self.i += 1
                if self.at('('):
                    n = self.node('CtorCall', t, which=x, args=None)
                    n.args = self.arguments()
                    return self.done(n)
                if x == 'super' and not self.at('.'):
                    self.error('super must be followed by "." or "("', 'super')
                return self.done(self.node('This' if x == 'this' else 'Super', t))
            if x == 'new':
                return self.new_expression()
        if k == 'op' and t.text == '(':
            self.i += 1
            n = self.node('Paren', t, expr=None)
            n.expr = self.expression()
            self.expect(')', 'parenthesised expression')
            return self.done(n)
        self.error('expected an expression', 'expression')

    def new_expression(self):
        t = self.expect('new')
        tt = self.tok
        if tt.kind == 'kw' and tt.text in ('int', 'float'):
            self.i += 1
            ty = self.node('Type', tt, name=tt.text, dims=0, primitive=True)
        elif tt.kind == 'id':
            ty = self.node('Type', tt, name=self.qualified_name('new'), dims=0,
                           primitive=False)
        else:
            self.error('expected a type after new', 'new')
        self.done(ty)
        if self.at('('):
            n = self.node('New', t, type=ty, args=None)
            n.args = self.arguments()
            return self.done(n)
        if self.at('['):
            n = self.node('NewArray', t, type=ty, sizes=[], dims=0, init=None)
            while self.at('[') and self.peek().text != ']':
                self.i += 1
                n.sizes.append(self.expression())
                self.expect(']', 'array creation')
            n.dims = self.dims()
            if len(n.sizes) + n.dims > 1:
                self.note('multi-dim', t, ty.name)
            if self.at('{'):
                n.init = self.array_init()
            elif not n.sizes:
                self.error('array creation without a size', 'new')
            return self.done(n)
        self.error('expected "(" or "[" after the type of new', 'new')


def parse(text, name=None, parser_class=None):
    """Parse the text of one source file and return its Unit node."""
    return (parser_class or Parser)(text, name).unit()


def parse_file(path):
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1')
    return parse(text, path)


def main(argv):
    import json
    args = [a for a in argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        print('usage: python -I parser.py [--json] [--notes] FILE.java', file=sys.stderr)
        return 2
    try:
        u = parse_file(args[0])
    except (ParseError, lexer.LexError) as e:
        print(e, file=sys.stderr)
        return 1
    if '--json' in argv:
        json.dump(u.to_dict(), sys.stdout, indent=1)
        print()
    else:
        print(dump(u))
    if '--notes' in argv:
        for code, line, text in u.notes:
            print('note %s line %d %s' % (code, line, text))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
