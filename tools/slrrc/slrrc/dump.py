"""dump.py - text forms of tokens and trees, for tests and for comparing two
implementations (the C++ compiler prints the same lines).

    dump_tokens(source, toks)   one line per token: line, kind, text
    dump_ast(ast)               one line per node, in creation order
    ast_tokens(ast)             the token texts regenerated from the AST
    dump_tree(tree)             one line per node of an emitted tree
    dump_trees(builds)          the trees of every class of a source

ast_tokens walks the tree and writes every token the grammar puts between the
parts of a construct.  The tree keeps everything of a source except comments
and white space, so the result must equal the token texts the scanner read
(milestone M2: equal for all 3,454 sources).

An emitted tree is written with its operands by content, the form in which
the emitter holds them (milestone M4):

    tree 2 locals 3 nodes 9
    0 L40 push_int 0
    1 L40 call
    2 L40 method java.game.Marker.show:()V
    3 L40 stmt call
    4 L41 field java.game.Marker.symbol:Ljava.util.resource.RenderRef;
    5 L43 jump_false +4
    6 L42 expr literal
    7 L42 str "a\\tb"
    8 L42 float 0x3F800000

A class is written with the indices of its pool (dump_class, milestone M5):
the pool entry by entry, then the CLSS record and the rows.

    class java.game.Trigger entries 59 steps 4 12 24 54 59
    0 utf8 "java.game.Trigger"
    1 class 0
    26 nat 12 4
    27 ref 1 26
    55 rid 54 0x00000034
    clss interface 0 name 1 super 3 interfaces []
    field s flags 19 name 12 type 4 tree 0
    method i flags 1 name 19 desc 18 tree 1 locals 3

steps: the length of the pool after each of the five steps (seed, types,
names, resolve, emit).  Flags are hexadecimal; "-" stands for "none".
"""
from . import emit
from . import pool as pl
from . import scanner as sc
from . import parser as ps
from .parser import NONE


def dump_tokens(source, toks):
    out = []
    for t in toks:
        out.append('%d %s %s' % (t.line, sc.KIND_NAMES[t.kind],
                                 source[t.pos:t.pos + t.length]))
    return '\n'.join(out) + '\n'


def _tok(i):
    return '-' if i == NONE else '%d' % i


def _ref(n):
    return '-' if n is None else '%d' % n.index


def dump_ast(ast):
    """index kind tok0 tok1 op_tok name_tok a b c flags [items]"""
    out = []
    for n in ast.nodes:
        out.append('%d %s %d %d %s %s %s %s %s %X [%s]' % (
            n.index, ps.KIND_NAMES[n.kind], n.tok0, n.tok1, _tok(n.op_tok),
            _tok(n.name_tok), _ref(n.a), _ref(n.b), _ref(n.c), n.flags,
            ' '.join('%d' % x.index for x in n.items)))
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------------------
# tokens regenerated from the tree
# ---------------------------------------------------------------------------

class _Unparser(object):
    def __init__(self, ast):
        self.ast = ast
        self.out = []

    def w(self, text):
        self.out.append(text)

    def tok(self, i):
        self.out.append(self.ast.text(i))

    def modifiers(self, n):
        """The modifier keywords of a declaration are its first tokens."""
        toks = self.ast.toks
        i = n.tok0
        while toks[i].kind in ps.MODIFIER_BITS:
            self.tok(i)
            i += 1

    def dotted(self, tok0, tok1):
        i = tok0
        while i <= tok1:
            if i > tok0:
                self.w('.')
            self.tok(i)
            i += 2

    def dims(self, d):
        while d > 0:
            self.w('[')
            self.w(']')
            d -= 1

    def type(self, t):
        self.dotted(t.tok0, t.name_tok)
        self.dims(t.dims())

    def commas(self, items, fn):
        first = True
        for x in items:
            if not first:
                self.w(',')
            first = False
            fn(x)

    # -- declarations --------------------------------------------------------

    def unit(self, u):
        if u.a is not None:
            self.w('package')
            self.dotted(u.a.tok0, u.a.tok1)
            self.w(';')
        for n in u.items:
            if n.kind == ps.IMPORT:
                self.w('import')
                if n.flags & ps.F_COLON:
                    self.tok(n.tok0 + 1)
                    self.w(':')
                self.dotted(n.a.tok0, n.a.tok1)
                if n.flags & ps.F_STAR:
                    self.w('.')
                    self.w('*')
                self.w(';')
            else:
                self.cls(n)

    def cls(self, c):
        self.modifiers(c)
        self.w('interface' if c.flags & ps.F_INTERFACE else 'class')
        self.tok(c.name_tok)
        if c.a is not None:
            self.w('extends')
            self.type(c.a)
        if c.b is not None:
            self.w('implements')
            self.commas(c.b.items, self.type)
        self.w('{')
        for m in c.items:
            self.modifiers(m)
            if m.kind == ps.FIELD_DECL:
                self.type(m.a)
                self.commas(m.items, self.declarator)
                self.w(';')
            else:
                if m.a is not None:
                    self.type(m.a)
                self.tok(m.name_tok)
                self.w('(')
                self.commas(m.items, self.param)
                self.w(')')
                if m.b is None:
                    self.w(';')
                else:
                    self.stmt(m.b)
        self.w('}')

    def param(self, p):
        self.modifiers(p)
        self.type(p.a)
        self.tok(p.name_tok)
        self.dims(p.dims())

    def declarator(self, d):
        self.tok(d.name_tok)
        self.dims(d.dims())
        if d.a is not None:
            self.w('=')
            self.expr(d.a)

    def local(self, s):
        self.modifiers(s)
        self.type(s.a)
        self.commas(s.items, self.declarator)

    # -- statements ----------------------------------------------------------

    def stmt(self, s):
        k = s.kind
        w = self.w
        if k == ps.BLOCK:
            w('{')
            for x in s.items:
                self.stmt(x)
            w('}')
        elif k == ps.EMPTY:
            w(';')
        elif k == ps.EXPR_STMT:
            self.expr(s.a)
            w(';')
        elif k == ps.LOCAL_VAR:
            self.local(s)
            w(';')
        elif k == ps.IF:
            w('if')
            w('(')
            self.expr(s.a)
            w(')')
            self.stmt(s.b)
            if s.c is not None:
                w('else')
                self.stmt(s.c)
        elif k == ps.WHILE:
            w('while')
            w('(')
            self.expr(s.a)
            w(')')
            self.stmt(s.b)
        elif k == ps.DO:
            w('do')
            self.stmt(s.a)
            w('while')
            w('(')
            self.expr(s.b)
            w(')')
            w(';')
        elif k == ps.FOR:
            w('for')
            w('(')
            if s.c is not None:
                if s.c.kind == ps.LOCAL_VAR:
                    self.local(s.c)
                else:
                    self.commas(s.c.items, self.expr)
            w(';')
            if s.a is not None:
                self.expr(s.a)
            w(';')
            self.commas(s.items, self.expr)
            w(')')
            self.stmt(s.b)
        elif k == ps.SWITCH:
            w('switch')
            w('(')
            self.expr(s.a)
            w(')')
            w('{')
            for g in s.items:
                for lab in g.a.items:
                    if lab.kind == ps.CASE_LABEL:
                        w('case')
                        self.expr(lab.a)
                        w(':')
                    else:
                        w('default')
                        w(':')
                for x in g.items:
                    self.stmt(x)
            w('}')
        elif k == ps.BREAK:
            w('break')
            w(';')
        elif k == ps.CONTINUE:
            w('continue')
            w(';')
        elif k == ps.RETURN:
            w('return')
            if s.a is not None:
                self.expr(s.a)
            w(';')
        else:
            raise ValueError('not a statement: %r' % s)

    # -- expressions ---------------------------------------------------------

    def args(self, items):
        self.w('(')
        self.commas(items, self.expr)
        self.w(')')

    def expr(self, e):
        k = e.kind
        w = self.w
        if k == ps.LITERAL or k == ps.NAME or k == ps.THIS or k == ps.SUPER:
            self.tok(e.tok0)
        elif k == ps.PAREN:
            w('(')
            self.expr(e.a)
            w(')')
        elif k == ps.FIELD:
            self.expr(e.a)
            w('.')
            self.tok(e.name_tok)
        elif k == ps.INDEX:
            self.expr(e.a)
            w('[')
            self.expr(e.b)
            w(']')
        elif k == ps.CALL:
            if e.a is not None:
                self.expr(e.a)
                w('.')
            self.tok(e.name_tok)
            self.args(e.items)
        elif k == ps.CTOR_CALL:
            self.tok(e.tok0)
            self.args(e.items)
        elif k == ps.NEW:
            w('new')
            self.type(e.a)
            self.args(e.items)
        elif k == ps.NEW_ARRAY:
            w('new')
            self.type(e.a)
            w('[')
            self.expr(e.b)
            w(']')
        elif k == ps.UNARY:
            self.tok(e.op_tok)
            self.expr(e.a)
        elif k == ps.POSTFIX:
            self.expr(e.a)
            self.tok(e.op_tok)
        elif k == ps.BINARY or k == ps.ASSIGN:
            self.expr(e.a)
            self.tok(e.op_tok)
            self.expr(e.b)
        elif k == ps.COND:
            self.expr(e.a)
            w('?')
            self.expr(e.b)
            w(':')
            self.expr(e.c)
        elif k == ps.CAST:
            w('(')
            self.type(e.a)
            w(')')
            self.expr(e.b)
        elif k == ps.INSTANCEOF:
            self.expr(e.a)
            w('instanceof')
            self.type(e.b)
        else:
            raise ValueError('not an expression: %r' % e)


def ast_tokens(ast):
    """The token texts of the source, regenerated from the tree."""
    u = _Unparser(ast)
    u.unit(ast.root)
    return u.out


# ---------------------------------------------------------------------------
# emitted trees
# ---------------------------------------------------------------------------

def quoted(text):
    """A text in double quotes; backslash, quote and every character outside
    printable ASCII are escaped (\\\\ \\" \\n \\t \\r \\xHH)."""
    out = ['"']
    for ch in text:
        o = ord(ch)
        if ch == '\\' or ch == '"':
            out.append('\\' + ch)
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ch == '\r':
            out.append('\\r')
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append('\\x%02X' % o)
    out.append('"')
    return ''.join(out)


def operand_text(tree, n):
    """The operand of node n of an emitted tree:
        nothing         -
        expr, stmt      the name of the kind of expression
        a jump          the offset in nodes, with its sign
        float           the bits, 0xHHHHHHHH
        other numbers   unsigned, decimal
        a text          "text"
        a class         the class name
        a member        class.name:descriptor
        a resource      pack file name:0xHHHHHHHH"""
    k = n.kind
    if k == emit.K_NONE:
        return '-'
    if k == emit.K_IMM:
        op = n.op
        if op == emit.OP_EXPR or op == emit.OP_STMT:
            return emit.X_NAMES.get(n.value, '%d' % n.value)
        if (op == emit.OP_JUMP or op == emit.OP_JUMP_FALSE or op == emit.OP_JUMP_TRUE or
                op == emit.OP_CASE):
            if n.value >= 0x80000000:
                return '-%d' % (0x100000000 - n.value)
            return '+%d' % n.value
        if op == emit.OP_FLOAT:
            return '0x%08X' % n.value
        return '%d' % n.value
    if k == emit.K_LABEL:
        return 'label %d' % n.value
    o = tree.operands[n.value]
    if k == emit.K_UTF8:
        return quoted(o.text)
    if k == emit.K_CLASS:
        return o.text
    if k == emit.K_REF:
        return '%s.%s:%s' % (o.text, o.name, o.desc)
    return '%s:0x%08X' % (o.text, o.number)


def dump_tree(tree):
    """index L<line> mnemonic [operand], one line per node."""
    out = []
    i = 0
    for n in tree.nodes:
        name = emit.OP_NAMES.get(n.op, 'op%d' % n.op)
        if n.kind == emit.K_NONE:
            out.append('%d L%d %s' % (i, n.line, name))
        else:
            out.append('%d L%d %s %s' % (i, n.line, name, operand_text(tree, n)))
        i += 1
    return '\n'.join(out) + '\n' if out else ''


def dump_trees(builds):
    """The trees of every class of a source (the ClassBuilds of
    slrrc.emit_trees), in order."""
    out = []
    for cb in builds:
        out.append('class %s trees %d\n' % (cb.info.name, len(cb.trees)))
        t = 0
        for tree in cb.trees:
            out.append('tree %d locals %d nodes %d\n' % (t, tree.max_locals, len(tree.nodes)))
            out.append(dump_tree(tree))
            t += 1
    return ''.join(out)


# ---------------------------------------------------------------------------
# classes: pool and rows
# ---------------------------------------------------------------------------

def _index(i):
    return '-' if i == NONE else '%d' % i


def dump_class(cb):
    """The pool, the CLSS record and the rows of a ClassBuild, with pool
    indices (the form is in the module text)."""
    out = ['class %s entries %d steps %s' % (
        cb.info.name, len(cb.pool.entries), ' '.join('%d' % n for n in cb.pool_steps))]
    i = 0
    for e in cb.pool.entries:
        if e.kind == pl.UTF8:
            out.append('%d utf8 %s' % (i, quoted(e.text)))
        elif e.kind == pl.CLASS:
            out.append('%d class %d' % (i, e.a))
        elif e.kind == pl.NAT:
            out.append('%d nat %d %d' % (i, e.a, e.b))
        elif e.kind == pl.REF:
            out.append('%d ref %d %d' % (i, e.a, e.b))
        else:
            out.append('%d rid %d 0x%08X' % (i, e.a, e.b))
        i += 1
    rows = cb.rows
    out.append('clss interface %d name %s super %s interfaces [%s]' % (
        rows.interface, _index(rows.name), _index(rows.super),
        ' '.join('%d' % x for x in rows.interfaces)))
    for vec, mark in ((rows.static_fields, 's'), (rows.instance_fields, 'i')):
        for r in vec:
            out.append('field %s flags %X name %d type %d tree %s' % (
                mark, r.flags, r.name, r.type, _index(r.tree)))
    for vec, mark in ((rows.static_methods, 's'), (rows.instance_methods, 'i')):
        for r in vec:
            out.append('method %s flags %X name %d desc %d tree %s locals %d' % (
                mark, r.flags, r.name, r.desc, _index(r.tree), r.locals))
    return '\n'.join(out) + '\n'


def dump_classes(builds):
    """dump_class for every class of a source, in order."""
    return ''.join(dump_class(cb) for cb in builds)
