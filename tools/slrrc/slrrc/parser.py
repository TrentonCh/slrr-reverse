"""parser.py - recursive descent parser: tokens -> AST.

    ast = parse(source, toks, diags)      # Ast; ast.root is None after an error

The grammar is the one of notes/grammar.md section 3 (all 3,454 shipped
sources); binary operators by precedence climbing, the ten levels of Java.

THE AST

One Node type for every construct (C++: struct Node in sc_ast.hpp):

    kind                node kind, one of the constants below
    index               position in ast.nodes (creation order)
    tok0, tok1          index of its first and last token
    op_tok, name_tok    index of an operator / a name token, or NONE
    a, b, c             child nodes or None (meaning per kind)
    items               list of child nodes (meaning per kind)
    flags               modifier bits and other small facts (per kind)

Names are not copied into the tree: the name of a node is the text of its
name token (ast.text(n.name_tok)), a literal is the text of its token, an
operator is the kind of its token.  The token indices are kept because the
compiler stamps every tree node with the line its parser had reached, which is
a function of token positions (notes/statements.md 8).

The children per kind ("-" = unused: None or an empty list):

    kind            a               b               c               items
    UNIT            package QNAME   -               -               IMPORT and CLASS nodes
    QNAME           -               -               -               -
    IMPORT          QNAME           -               -               -
    CLASS           extends TYPE    LIST of TYPE    -               members
                                    (implements)
    FIELD_DECL      TYPE            -               -               DECLARATORs
    DECLARATOR      initialiser     -               -               -
    METHOD_DECL     return TYPE     body BLOCK      -               PARAMs
    PARAM           TYPE            -               -               -
    TYPE            -               -               -               -
    LIST            -               -               -               the nodes
    BLOCK           -               -               -               statements
    LOCAL_VAR       TYPE            -               -               DECLARATORs
    EXPR_STMT       expression      -               -               -
    IF              condition       then            else            -
    WHILE           condition       body            -               -
    DO              body            condition       -               -
    FOR             condition       body            init            update expressions
    SWITCH          subject         -               -               SWITCH_GROUPs
    SWITCH_GROUP    LIST of labels  -               -               statements
    CASE_LABEL      value           -               -               -
    DEFAULT_LABEL, BREAK, CONTINUE, EMPTY, LITERAL, NAME, THIS, SUPER: none
    RETURN          value           -               -               -
    FIELD           target          -               -               -
    INDEX           target          index           -               -
    CALL            target          -               -               arguments
    CTOR_CALL       -               -               -               arguments
    NEW             TYPE            -               -               arguments
    NEW_ARRAY       element TYPE    size            -               -
    UNARY, POSTFIX  operand         -               -               -
    BINARY          left            right           -               -
    ASSIGN          target          value           -               -
    COND            condition       then            else            -
    CAST            TYPE            operand         -               -
    INSTANCEOF      operand         TYPE            -               -
    PAREN           expression      -               -               -

    METHOD_DECL: a is None for a constructor, b is None for a body written ";".
    FOR: c is a LOCAL_VAR, a LIST of expressions, or None; a may be None.
    IF: c may be None.  RETURN, DECLARATOR, CALL, UNIT, CLASS: a may be None.

The tokens and flags per kind:

    kind            tok0 .. tok1                name_tok        op_tok      flags
    UNIT            the whole source            -               -           -
    QNAME           id { . id }                 the last id     -           -
    IMPORT          import .. ;                 -               the ":"     F_STAR, F_COLON
    CLASS           first modifier .. }         the class name  -           modifiers, F_INTERFACE
    FIELD_DECL      first modifier .. ;         -               -           modifiers
    DECLARATOR      name .. end of initialiser  the name        the "="     dims
    METHOD_DECL     first modifier .. } or ;    the method name -           modifiers, F_CTOR
    PARAM           [final] type name           the name        -           modifiers, dims
    TYPE            name { [ ] }                last id of the  -           dims
                                                name
    LIST            first .. last item          -               -           -
    BLOCK           { .. }                      -               -           -
    LOCAL_VAR       [final] type .. [;]         -               -           modifiers
    EXPR_STMT       expression ;                -               -           -
    IF, WHILE, FOR  keyword .. end of the body  -               -           -
    DO              do .. ;                     -               -           -
    SWITCH          switch .. }                 -               -           -
    SWITCH_GROUP    first label .. last token   -               -           -
                    before the next label
    CASE_LABEL      case .. :                   -               -           -
    DEFAULT_LABEL   default :                   -               -           -
    BREAK, CONTINUE, RETURN   keyword .. ;      -               -           -
    EMPTY           ;                           -               -           -
    LITERAL         the literal token           -               -           -
    NAME            the identifier              = tok0          -           -
    THIS, SUPER     the keyword                 -               -           -
    FIELD           target . name               the name        -           -
    INDEX           target [ index ]            -               -           -
    CALL            [ target . ] name ( .. )    the name        -           -
    CTOR_CALL       this ( .. ) / super ( .. )  -               -           -
    NEW             new T ( .. )                -               -           -
    NEW_ARRAY       new T [ n ]                 -               -           -
    UNARY           op operand                  -               the op      -
    POSTFIX         operand op                  -               the op      -
    BINARY, ASSIGN  left op right               -               the op      -
    COND            c ? x : y                   -               the "?"     -
    CAST            ( T ) operand               -               -           -
    INSTANCEOF      x instanceof T              -               the keyword -
    PAREN           ( e )                       -               -           -

    The start of an expression node is the first token of its leftmost
    operand: a.b[i].f(x) + 1 and all its left parts start at "a".

A LOCAL_VAR that is a statement ends at its ";", one in a for header at its
last declarator.  A name chain a.b.c is FIELD(FIELD(NAME a, b), c): the
parser does not decide what a name is.  Parentheses are kept (PAREN).

Constructs outside the proven subset (notes/compiler_spec.md appendix B):
diags.unverified() where a default is stated (an error in strict mode),
E_UNSUPPORTED where none is, E_SYNTAX for everything outside the grammar.

HOW THE PARSER STOPS AT AN ERROR.  It stops at the first error, and it does
so by unwinding: fail() records the diagnostic and raises the private _Stop,
which parse() catches; nothing else raises it and nothing else catches it.
Every function of the parser is written on that assumption:

    never return        fail, error, unsupported, too_deep
    return only when    expect, ident (the token is there), enter (the
    all is well         nesting is within the limit), done (the tree is not
                        too high), unverified (default mode; in strict mode
                        the construct is an error and the call does not
                        return)

The code behind such a call relies on it: type_decl() reads `interface` after
error(), block() loops until "}" and leaves an unclosed block through
error(), switch_statement() uses `group` after error().  A port has to keep
this: ONE exception type of the parser, thrown in fail() and in the strict
branch of unverified() and caught in parse(), which returns the diagnostics.
It never crosses the interface of the compiler (the engine is built with
exceptions enabled; its pack reader uses try / catch).  Turning fail() into
"record and return" is wrong for about 130 call sites.  The class file
reader (classfile.py: _Bad, caught in read()) follows the same rule.

NODES ARE NUMBERED IN THE ORDER THEY ARE MADE (node()): dump_ast() prints
that number, and two implementations are compared through it.  A node made
for a construct that starts with an operand or a type is made AFTER that
part (BINARY, ASSIGN, COND and INSTANCEOF after their left operand; FIELD,
INDEX, POSTFIX and a CALL with a receiver after their target; FIELD_DECL, a
METHOD_DECL with a return type, PARAM, LOCAL_VAR, NEW and NEW_ARRAY after
their TYPE), every other node BEFORE its children (UNIT, IMPORT, CLASS, a
constructor, DECLARATOR, BLOCK, EXPR_STMT, IF, WHILE, DO, FOR, SWITCH,
SWITCH_GROUP and its LIST, CASE_LABEL, RETURN, PAREN, UNARY, CAST, a CALL
without receiver, CTOR_CALL, LIST).

EVERY NODE IS CLOSED EXACTLY ONCE (done()), after its last child and its last
item, and before any token behind it is read.  Between node() and done() the
items of a node grow while other nodes, with items of their own, are made (a
switch group gets its statements while the statements are parsed; a FOR gets
its update expressions after its init and condition exist).

C++ (sc_parser.cpp): children are node indices.  The items of a node are
collected in a vector of the function that parses the construct and copied
into Ast::lists by done(), where they become the slice (list, count) of the
node; because every node is closed once and after its items, the slices do
not interleave.  Appending to Ast::lists while a construct is still being
parsed would mix the items of a node with those of its children.
"""
from . import scanner as sc
from .diag import E_LIMIT, E_SYNTAX, E_UNSUPPORTED
from .scanner import (
    T_EOF, T_ID, T_INT, T_FLOAT, T_STR, T_RID,
    KW_PACKAGE, KW_IMPORT, KW_CLASS, KW_INTERFACE, KW_EXTENDS, KW_IMPLEMENTS,
    KW_PUBLIC, KW_PRIVATE, KW_PROTECTED, KW_STATIC, KW_FINAL, KW_ABSTRACT,
    KW_NATIVE, KW_SYNCHRONIZED, KW_NEW, KW_RETURN, KW_IF, KW_ELSE, KW_FOR,
    KW_WHILE, KW_DO, KW_SWITCH, KW_CASE, KW_DEFAULT, KW_BREAK, KW_CONTINUE,
    KW_THIS, KW_SUPER, KW_NULL, KW_TRUE, KW_FALSE, KW_INSTANCEOF, KW_VOID,
    KW_INT, KW_FLOAT,
    P_SEMI, P_COMMA, P_DOT, P_LPAREN, P_RPAREN, P_LBRACE, P_RBRACE,
    P_LBRACKET, P_RBRACKET, P_ASSIGN, P_ADD_ASSIGN, P_SUB_ASSIGN,
    P_MUL_ASSIGN, P_DIV_ASSIGN, P_MOD_ASSIGN, P_AND_ASSIGN, P_OR_ASSIGN,
    P_XOR_ASSIGN, P_SHL_ASSIGN, P_SHR_ASSIGN, P_USHR_ASSIGN, P_QUESTION,
    P_COLON, P_LOR, P_LAND, P_OR, P_XOR, P_AND, P_EQ, P_NE, P_LT, P_GT, P_LE,
    P_GE, P_SHL, P_SHR, P_USHR, P_ADD, P_SUB, P_MUL, P_DIV, P_MOD, P_NOT,
    P_BITNOT, P_INC, P_DEC)

NONE = 0xFFFFFFFF           # "no token" in op_tok / name_tok

# ---------------------------------------------------------------------------
# node kinds
# ---------------------------------------------------------------------------

UNIT = 1
QNAME = 2
IMPORT = 3
CLASS = 4
FIELD_DECL = 5
DECLARATOR = 6
METHOD_DECL = 7
PARAM = 8
TYPE = 9
LIST = 10
BLOCK = 11
LOCAL_VAR = 12
EXPR_STMT = 13
IF = 14
WHILE = 15
DO = 16
FOR = 17
SWITCH = 18
SWITCH_GROUP = 19
CASE_LABEL = 20
DEFAULT_LABEL = 21
BREAK = 22
CONTINUE = 23
RETURN = 24
EMPTY = 25
LITERAL = 26
NAME = 27
THIS = 28
SUPER = 29
FIELD = 30
INDEX = 31
CALL = 32
CTOR_CALL = 33
NEW = 34
NEW_ARRAY = 35
UNARY = 36
POSTFIX = 37
BINARY = 38
ASSIGN = 39
COND = 40
CAST = 41
INSTANCEOF = 42
PAREN = 43

KIND_NAMES = {
    UNIT: 'Unit', QNAME: 'QName', IMPORT: 'Import', CLASS: 'Class',
    FIELD_DECL: 'FieldDecl', DECLARATOR: 'Declarator', METHOD_DECL: 'MethodDecl',
    PARAM: 'Param', TYPE: 'Type', LIST: 'List', BLOCK: 'Block',
    LOCAL_VAR: 'LocalVar', EXPR_STMT: 'ExprStmt', IF: 'If', WHILE: 'While',
    DO: 'Do', FOR: 'For', SWITCH: 'Switch', SWITCH_GROUP: 'SwitchGroup',
    CASE_LABEL: 'CaseLabel', DEFAULT_LABEL: 'DefaultLabel', BREAK: 'Break',
    CONTINUE: 'Continue', RETURN: 'Return', EMPTY: 'Empty', LITERAL: 'Literal',
    NAME: 'Name', THIS: 'This', SUPER: 'Super', FIELD: 'Field', INDEX: 'Index',
    CALL: 'Call', CTOR_CALL: 'CtorCall', NEW: 'New', NEW_ARRAY: 'NewArray',
    UNARY: 'Unary', POSTFIX: 'Postfix', BINARY: 'Binary', ASSIGN: 'Assign',
    COND: 'Cond', CAST: 'Cast', INSTANCEOF: 'InstanceOf', PAREN: 'Paren',
}

# ---------------------------------------------------------------------------
# flags
# ---------------------------------------------------------------------------

# The modifier bits are the bits of the flags word of a FILD / MTHD row
# (notes/declarations.md 4.2), so flags & ACC_MASK is that word.
ACC_PUBLIC = 0x01
ACC_PROTECTED = 0x02
ACC_PRIVATE = 0x04
ACC_STATIC = 0x08
ACC_ABSTRACT = 0x10
ACC_FINAL = 0x20
ACC_NATIVE = 0x40
ACC_SYNCHRONIZED = 0x80
ACC_MASK = 0xFF

F_INTERFACE = 0x0100        # CLASS: declared with the keyword interface
F_CTOR = 0x0200             # METHOD_DECL: a constructor (no return type)
F_STAR = 0x0400             # IMPORT: ends in ".*"
F_COLON = 0x0800            # IMPORT: "word:" in front of the name (tok0 + 1 is the word)
DIMS_SHIFT = 16             # TYPE, DECLARATOR, PARAM: number of "[" "]" pairs = flags >> 16

MODIFIER_BITS = {
    KW_PUBLIC: ACC_PUBLIC, KW_PROTECTED: ACC_PROTECTED, KW_PRIVATE: ACC_PRIVATE,
    KW_STATIC: ACC_STATIC, KW_ABSTRACT: ACC_ABSTRACT, KW_FINAL: ACC_FINAL,
    KW_NATIVE: ACC_NATIVE, KW_SYNCHRONIZED: ACC_SYNCHRONIZED,
}

# binary operators: token kind -> precedence (higher binds tighter); all
# group to the left.  instanceof sits with the relational operators.
BINARY_PREC = {
    P_LOR: 1,
    P_LAND: 2,
    P_OR: 3,
    P_XOR: 4,
    P_AND: 5,
    P_EQ: 6, P_NE: 6,
    P_LT: 7, P_GT: 7, P_LE: 7, P_GE: 7, KW_INSTANCEOF: 7,
    P_SHL: 8, P_SHR: 8, P_USHR: 8,
    P_ADD: 9, P_SUB: 9,
    P_MUL: 10, P_DIV: 10, P_MOD: 10,
}
ASSIGN_OPS = frozenset((
    P_ASSIGN, P_MUL_ASSIGN, P_DIV_ASSIGN, P_MOD_ASSIGN, P_ADD_ASSIGN,
    P_SUB_ASSIGN, P_SHL_ASSIGN, P_SHR_ASSIGN, P_USHR_ASSIGN, P_AND_ASSIGN,
    P_XOR_ASSIGN, P_OR_ASSIGN))
PREFIX_OPS = frozenset((P_ADD, P_SUB, P_NOT, P_BITNOT, P_INC, P_DEC))
LITERAL_TOKENS = frozenset((T_INT, T_FLOAT, T_STR, T_RID, KW_NULL, KW_TRUE, KW_FALSE))

# The limit on nesting; beyond it the source is refused with E_LIMIT.  It
# bounds two things: the recursion of the parser (one level per statement,
# per expression and per prefix operator or cast; the deepest shipped source,
# Garage.java, reaches 51) and the height of the tree (Garage.java: 56), which
# bounds the recursion of every pass behind the parser.  In the Python
# reference a parser level costs 3 to 15 interpreter frames, so this limit is
# the one that triggers only when the interpreter allows 4000 frames or more
# on top of the caller: the command line and the tests set 20000
# (cli.RECURSION_LIMIT), and a program that calls compile_source() itself has
# to do the same (sys.setrecursionlimit).  Under Python's default of 1000
# frames parentheses nested deeper than about 160 end in E_LIMIT a little
# earlier, and with another text (parse(), __init__._front, _emit).  The C++
# compiler has no second limit: its answer is the one of the reference under
# a raised limit.
MAX_DEPTH = 200


class Node(object):
    __slots__ = ('kind', 'index', 'tok0', 'tok1', 'op_tok', 'name_tok',
                 'a', 'b', 'c', 'items', 'flags')

    def __init__(self, kind, index, tok):
        self.kind = kind
        self.index = index
        self.tok0 = tok
        self.tok1 = tok
        self.op_tok = NONE
        self.name_tok = NONE
        self.a = None
        self.b = None
        self.c = None
        self.items = []
        self.flags = 0

    def dims(self):
        """TYPE, DECLARATOR, PARAM: the number of "[" "]" pairs."""
        return self.flags >> DIMS_SHIFT

    def __repr__(self):
        return '<%s #%d tokens %d..%d>' % (KIND_NAMES[self.kind], self.index,
                                           self.tok0, self.tok1)


class Ast(object):
    """The tree of one source with the tokens it refers to."""
    __slots__ = ('source', 'toks', 'nodes', 'root')

    def __init__(self, source, toks):
        self.source = source
        self.toks = toks
        self.nodes = []             # every node, in creation order
        self.root = None            # the UNIT node; None when the parse failed

    def text(self, tok):
        """Source text of token number tok."""
        t = self.toks[tok]
        return self.source[t.pos:t.pos + t.length]

    def line(self, tok):
        return self.toks[tok].line

    def dotted(self, tok0, tok1):
        """Text of the dotted name that spans the tokens tok0 .. tok1
        (identifiers with a "." between them): 'java.util.Vector'."""
        if tok0 == tok1:
            return self.text(tok0)
        parts = []
        k = tok0
        while k <= tok1:
            parts.append(self.text(k))
            k += 2
        return '.'.join(parts)

    def type_name(self, type_node):
        """The name a TYPE node writes: int, float, void, or the class name."""
        return self.dotted(type_node.tok0, type_node.name_tok)


def children(n):
    """The child nodes of a node in source order."""
    k = n.kind
    out = []
    if k == METHOD_DECL:
        if n.a is not None:
            out.append(n.a)
        out.extend(n.items)
        if n.b is not None:
            out.append(n.b)
        return out
    if k == FOR:
        if n.c is not None:
            out.append(n.c)
        if n.a is not None:
            out.append(n.a)
        out.extend(n.items)
        out.append(n.b)
        return out
    if n.a is not None:
        out.append(n.a)
    if n.b is not None:
        out.append(n.b)
    if n.c is not None:
        out.append(n.c)
    out.extend(n.items)
    return out


def is_primitive(ast, type_node):
    """True for a TYPE node that names int, float or void."""
    k = ast.toks[type_node.tok0].kind
    return k == KW_INT or k == KW_FLOAT or k == KW_VOID


class _Stop(Exception):
    """Unwinds the descent after the first error.  Raised in Parser.fail()
    and in the strict branch of Parser.unverified(), caught in parse(), and
    nowhere else (the module text says what depends on that)."""


class Parser(object):
    def __init__(self, source, toks, diags):
        self.source = source
        self.toks = toks
        self.diags = diags
        self.ast = Ast(source, toks)
        self.i = 0                  # index of the current token
        self.depth = 0              # recursion of the parser, see enter()
        self.heights = []           # per node: height of the tree below it, see done()
        self.deepest = []           # per node: first token of the deepest node below it
        self.cond_depth = 0         # > 0 inside the second or third operand of ?:
        self.conds = 0              # number of COND nodes made so far

    # -- tokens --------------------------------------------------------------

    def kind(self):
        return self.toks[self.i].kind

    def peek_kind(self, k):
        """Kind of the token k places ahead (the end marker repeats)."""
        j = self.i + k
        if j >= len(self.toks):
            j = len(self.toks) - 1
        return self.toks[j].kind

    def at(self, kind):
        return self.toks[self.i].kind == kind

    def accept(self, kind):
        if self.toks[self.i].kind == kind:
            self.i += 1
            return True
        return False

    def show(self, tok):
        """A token for a message."""
        t = self.toks[tok]
        if t.kind == T_EOF:
            return 'end of file'
        return '"%s"' % self.source[t.pos:t.pos + t.length]

    def fail(self, code, tok, text):
        """Record the error and stop the parse.  DOES NOT RETURN."""
        t = self.toks[tok]
        self.diags.error(code, t.line, sc.column_at(self.source, t.pos), text)
        raise _Stop()

    def error(self, text, tok=None):
        """The token sequence is outside the grammar.  Does not return."""
        if tok is None:
            tok = self.i
        self.fail(E_SYNTAX, tok, '%s (at %s)' % (text, self.show(tok)))

    def unsupported(self, tok, text):
        """A construct the compiler has no rule for, in either mode.  Does
        not return."""
        self.fail(E_UNSUPPORTED, tok, text + ' (no shipped source uses it)')

    def unverified(self, tok, text):
        """A construct of appendix B: a warning, after which the parse goes
        on; in strict mode an error, and the call does not return."""
        t = self.toks[tok]
        if self.diags.unverified(t.line, sc.column_at(self.source, t.pos), text):
            raise _Stop()

    def expect(self, kind, where):
        """Consume the token of that kind; returns only when it was there."""
        if self.toks[self.i].kind != kind:
            self.error('expected "%s" in %s' % (sc.FIXED_TEXT[kind], where))
        self.i += 1

    def ident(self, where):
        """Consume an identifier and return its token index; returns only
        when it was there."""
        if self.toks[self.i].kind != T_ID:
            self.error('expected an identifier in %s' % where)
        self.i += 1
        return self.i - 1

    # -- nodes ---------------------------------------------------------------

    def node(self, kind, tok):
        """Make a node.  Its number is the number of nodes made before it."""
        n = Node(kind, len(self.ast.nodes), tok)
        self.ast.nodes.append(n)
        self.heights.append(1)
        self.deepest.append(tok)
        return n

    def done(self, n):
        """Close a node: its last token is the one consumed last.  Called
        exactly once per node, after its last child and its last item.

        The height of the tree below the node is known here; a tree higher
        than MAX_DEPTH is refused (a chain a + b + c + ... is parsed in a
        loop but is as deep as it is long).  The error is reported where the
        nesting is: at the first token of the deepest node below this one
        (of several equally deep ones the first of a, b, c, items)."""
        n.tok1 = self.i - 1 if self.i > 0 else 0
        heights = self.heights
        h = 0
        deep = n.tok0
        if n.a is not None and heights[n.a.index] > h:
            h = heights[n.a.index]
            deep = self.deepest[n.a.index]
        if n.b is not None and heights[n.b.index] > h:
            h = heights[n.b.index]
            deep = self.deepest[n.b.index]
        if n.c is not None and heights[n.c.index] > h:
            h = heights[n.c.index]
            deep = self.deepest[n.c.index]
        for x in n.items:
            if heights[x.index] > h:
                h = heights[x.index]
                deep = self.deepest[x.index]
        heights[n.index] = h + 1
        self.deepest[n.index] = deep
        if h + 1 > MAX_DEPTH:
            self.too_deep(deep)
        return n

    def enter(self):
        """One level of recursion of the parser itself; returns only while
        the nesting is within MAX_DEPTH."""
        self.depth += 1
        if self.depth > MAX_DEPTH:
            self.too_deep(self.i)

    def leave(self):
        self.depth -= 1

    def too_deep(self, tok):
        """Does not return."""
        self.fail(E_LIMIT, tok, 'statements or expressions nested deeper than %d, '
                                'or an operator chain longer than that' % MAX_DEPTH)

    # -- names and types -----------------------------------------------------

    def qname(self, where):
        """QualifiedName = Identifier { "." Identifier }"""
        n = self.node(QNAME, self.i)
        n.name_tok = self.ident(where)
        while self.at(P_DOT) and self.peek_kind(1) == T_ID:
            self.i += 1
            n.name_tok = self.ident(where)
        return self.done(n)

    def dims(self):
        """Count "[" "]" pairs."""
        d = 0
        while self.at(P_LBRACKET) and self.peek_kind(1) == P_RBRACKET:
            self.i += 2
            d += 1
        return d

    def type_name(self, where):
        """The name part of a type: int, float, void or a class name.
        Returns the TYPE node, not yet closed."""
        t = self.i
        k = self.kind()
        n = self.node(TYPE, t)
        if k == KW_INT or k == KW_FLOAT or k == KW_VOID:
            self.i += 1
            n.name_tok = t
        elif k == T_ID:
            n.name_tok = self.ident(where)
            if self.at(P_DOT) and self.peek_kind(1) == T_ID:
                self.unverified(t, 'class name written with its package')
                while self.at(P_DOT) and self.peek_kind(1) == T_ID:
                    self.i += 1
                    n.name_tok = self.ident(where)
        else:
            self.error('expected a type in %s' % where)
        return n

    def type(self, where):
        """Type = ( "int" | "float" | "void" | Identifier ) [ "[" "]" ]"""
        n = self.type_name(where)
        d = self.dims()
        if d > 1:
            self.unsupported(n.tok0, 'array type with more than one dimension')
        if d and self.toks[n.tok0].kind == KW_VOID:
            self.unsupported(n.tok0, 'array of void')
        n.flags = d << DIMS_SHIFT
        return self.done(n)

    def class_type(self, where):
        """A class named behind extends, implements or instanceof."""
        if not self.at(T_ID):
            self.error('expected a class name in %s' % where)
        return self.done(self.type_name(where))

    def modifiers(self):
        """{ Modifier }: the OR of the modifier bits."""
        mods = 0
        while True:
            bit = MODIFIER_BITS.get(self.kind(), 0)
            if bit == 0:
                return mods
            if mods & bit:
                self.unverified(self.i, 'the same modifier twice')
            if bit == ACC_PROTECTED or bit == ACC_SYNCHRONIZED:
                self.unverified(self.i, 'modifier %s' % self.ast.text(self.i))
            mods |= bit
            self.i += 1

    # -- compilation unit ----------------------------------------------------

    def unit(self):
        """Unit = "package" QualifiedName ";" { Import } { TypeDecl }"""
        u = self.node(UNIT, self.i)
        if self.at(KW_PACKAGE):
            self.i += 1
            u.a = self.qname('the package declaration')
            self.expect(P_SEMI, 'the package declaration')
        else:
            self.unverified(self.i, 'source without a package declaration')
        types = 0
        while not self.at(T_EOF):
            if self.at(KW_IMPORT):
                if types:
                    self.unverified(self.i, 'import after a type declaration')
                u.items.append(self.import_decl())
            elif self.at(P_SEMI):
                self.unverified(self.i, '";" between top-level declarations')
                self.i += 1
            elif self.at(KW_PACKAGE):
                self.error('package declaration that is not at the start of the source')
            else:
                u.items.append(self.type_decl())
                types += 1
        return self.done(u)

    def import_decl(self):
        """Import = "import" [ Identifier ":" ] QualifiedName "." "*" ";" """
        n = self.node(IMPORT, self.i)
        self.i += 1
        if self.at(T_ID) and self.peek_kind(1) == P_COLON and self.peek_kind(2) == T_ID:
            # import parts:part.root.BodyPart.*;   (2 sources; it names nothing)
            n.flags |= F_COLON
            n.op_tok = self.i + 1
            self.i += 2
        q = self.node(QNAME, self.i)
        q.name_tok = self.ident('an import')
        while self.at(P_DOT):
            if self.peek_kind(1) == P_MUL:
                n.flags |= F_STAR
                break
            self.i += 1
            q.name_tok = self.ident('an import')
        self.done(q)
        n.a = q
        if n.flags & F_STAR:
            self.i += 2
        else:
            self.unverified(n.tok0, 'import of a single class')
        self.expect(P_SEMI, 'an import')
        return self.done(n)

    def type_decl(self):
        """TypeDecl = { Modifier } "class" Identifier [ "extends" Identifier ]
                      [ "implements" Identifier { "," Identifier } ] "{" { Member } "}"
                    | { Modifier } "interface" Identifier "{" "}" """
        start = self.i
        mods = self.modifiers()
        if self.at(KW_CLASS):
            interface = False
        elif self.at(KW_INTERFACE):
            interface = True
        else:
            self.error('expected class or interface')
        if mods & ~(ACC_PUBLIC | ACC_ABSTRACT):
            # the class file has no place for class modifiers
            self.unverified(start, 'modifier other than public and abstract on a class')
        self.i += 1
        n = self.node(CLASS, start)
        n.flags = mods | (F_INTERFACE if interface else 0)
        n.name_tok = self.ident('a type declaration')
        if self.at(KW_EXTENDS):
            if interface:
                self.unsupported(self.i, 'interface with extends')
            self.i += 1
            n.a = self.class_type('extends')
        if self.at(KW_IMPLEMENTS):
            if interface:
                self.unsupported(self.i, 'interface with implements')
            self.i += 1
            lst = self.node(LIST, self.i)
            lst.items.append(self.class_type('implements'))
            while self.accept(P_COMMA):
                lst.items.append(self.class_type('implements'))
            n.b = self.done(lst)
        self.expect(P_LBRACE, 'a class body')
        while not self.at(P_RBRACE):
            if self.at(T_EOF):
                self.error('class body is not closed')
            if self.at(P_SEMI):
                self.unverified(self.i, '";" between class members')
                self.i += 1
                continue
            if interface and not n.items:
                self.unverified(self.i, 'interface with members')
            n.items.append(self.member(n))
        self.i += 1
        return self.done(n)

    def member(self, cls):
        """Member          = FieldDecl | MethodDecl | ConstructorDecl
           FieldDecl       = { Modifier } Type Declarator { "," Declarator } ";"
           MethodDecl      = { Modifier } Type Identifier Params ( Block | ";" )
           ConstructorDecl = { Modifier } Identifier Params ( Block | ";" )
        A member that starts (after its modifiers) with  Identifier "("  is a
        constructor; every other member starts with a type."""
        start = self.i
        mods = self.modifiers()
        if self.at(P_LBRACE):
            self.unsupported(self.i, 'initialiser block in a class')
        if self.at(T_ID) and self.peek_kind(1) == P_LPAREN:
            if self.ast.text(self.i) != self.ast.text(cls.name_tok):
                self.unverified(self.i, 'constructor whose name is not the class name')
            if mods & ~ACC_PUBLIC:
                self.unverified(start, 'modifier other than public on a constructor')
            n = self.node(METHOD_DECL, start)
            n.flags = mods | F_CTOR
            n.name_tok = self.i
            self.i += 1
            self.method_rest(n)
            return self.done(n)
        k = self.kind()
        if not (k == T_ID or k == KW_INT or k == KW_FLOAT or k == KW_VOID):
            self.error('expected a member declaration')
        ty = self.type('a member declaration')
        name = self.ident('a member declaration')
        if self.at(P_LPAREN):
            n = self.node(METHOD_DECL, start)
            n.flags = mods
            n.a = ty
            n.name_tok = name
            self.method_rest(n)
            return self.done(n)
        if self.toks[ty.tok0].kind == KW_VOID:
            self.unsupported(ty.tok0, 'field of type void')
        n = self.node(FIELD_DECL, start)
        n.flags = mods
        n.a = ty
        self.i -= 1                     # back to the name of the first declarator
        self.declarators(n, ty, 'a field declaration')
        self.expect(P_SEMI, 'a field declaration')
        return self.done(n)

    def method_rest(self, n):
        """"(" [ Param { "," Param } ] ")" ( Block | ";" )"""
        self.expect(P_LPAREN, 'a parameter list')
        if not self.at(P_RPAREN):
            while True:
                n.items.append(self.param())
                if not self.accept(P_COMMA):
                    break
        self.expect(P_RPAREN, 'a parameter list')
        if self.at(P_LBRACKET):
            self.unsupported(self.i, 'brackets after a parameter list')
        if n.flags & ACC_ABSTRACT:
            self.unverified(n.tok0, 'abstract method')
        if n.flags & ACC_FINAL:
            self.unverified(n.tok0, 'final method')
        if self.at(P_LBRACE):
            if n.flags & ACC_NATIVE:
                self.unsupported(self.i, 'native method with a body')
            if n.flags & ACC_ABSTRACT:
                self.unsupported(self.i, 'abstract method with a body')
            n.b = self.block((n.flags & F_CTOR) != 0)
        else:
            self.expect(P_SEMI, 'a method body')

    def local_modifiers(self, what):
        """Modifiers of a parameter or a local variable: final at most."""
        start = self.i
        mods = self.modifiers()
        if mods & ~ACC_FINAL:
            self.unsupported(start, 'modifier other than final on %s' % what)
        if mods:
            self.unverified(start, 'final on %s' % what)
        return mods

    def param(self):
        """Param = Type Identifier"""
        start = self.i
        mods = self.local_modifiers('a parameter')
        ty = self.type('a parameter')
        if self.toks[ty.tok0].kind == KW_VOID:
            self.unsupported(ty.tok0, 'parameter of type void')
        n = self.node(PARAM, start)
        n.a = ty
        n.name_tok = self.ident('a parameter')
        n.flags = mods | (self.name_dims(ty) << DIMS_SHIFT)
        return self.done(n)

    def name_dims(self, ty):
        """Brackets behind a declared name: int a[]"""
        t = self.i
        d = self.dims()
        if d:
            if d + ty.dims() > 1:
                self.unsupported(t, 'array type with more than one dimension')
            self.unverified(t, 'brackets after a declared name')
        return d

    def declarators(self, decl, ty, where):
        """Declarator { "," Declarator };  Declarator = Identifier [ "=" Expression ]"""
        while True:
            d = self.node(DECLARATOR, self.i)
            d.name_tok = self.ident(where)
            d.flags = self.name_dims(ty) << DIMS_SHIFT
            if self.at(P_ASSIGN):
                d.op_tok = self.i
                self.i += 1
                if self.at(P_LBRACE):
                    self.unsupported(self.i, 'array initialiser')
                d.a = self.expression()
            decl.items.append(self.done(d))
            if not self.accept(P_COMMA):
                break

    # -- statements ----------------------------------------------------------

    def block(self, ctor_body=False):
        """Block = "{" { Statement } "}".  In the body of a constructor the
        first statement may be this(..) or super(..)."""
        n = self.node(BLOCK, self.i)
        self.expect(P_LBRACE, 'a block')
        while not self.at(P_RBRACE):
            if self.at(T_EOF):
                self.error('block is not closed')
            n.items.append(self.statement(ctor_body and not n.items))
        self.i += 1
        return self.done(n)

    def at_local_decl(self):
        """True when the tokens ahead read  Type Identifier  (a local
        variable declaration) and not an expression."""
        k = self.kind()
        if k == KW_INT or k == KW_FLOAT:
            return True
        if k in MODIFIER_BITS:
            return True
        if k != T_ID:
            return False
        toks = self.toks
        j = self.i + 1
        while toks[j].kind == P_DOT and toks[j + 1].kind == T_ID:
            j += 2
        while toks[j].kind == P_LBRACKET and toks[j + 1].kind == P_RBRACKET:
            j += 2
        return toks[j].kind == T_ID

    def local_decl(self):
        """LocalDecl = Type Declarator { "," Declarator }
        Returns the LOCAL_VAR node, not yet closed: a declaration that is a
        statement ends at its ";", one in a for header at its last
        declarator, and the caller closes it there."""
        start = self.i
        mods = self.local_modifiers('a local variable')
        ty = self.type('a local variable declaration')
        if self.toks[ty.tok0].kind == KW_VOID:
            self.unsupported(ty.tok0, 'local variable of type void')
        n = self.node(LOCAL_VAR, start)
        n.flags = mods
        n.a = ty
        self.declarators(n, ty, 'a local variable declaration')
        return n

    def substatement(self, where):
        """The statement that is the body of if / else / a loop."""
        if self.at_local_decl():
            self.unsupported(self.i, 'declaration as the body of %s' % where)
        return self.statement(False)

    def statement(self, ctor_first):
        self.enter()
        n = self.statement_1(ctor_first)
        self.leave()
        return n

    def statement_1(self, ctor_first):
        t = self.i
        k = self.kind()
        if k == P_LBRACE:
            return self.block()
        if k == P_SEMI:
            self.unverified(t, 'empty statement')
            self.i += 1
            return self.done(self.node(EMPTY, t))
        if k == KW_IF:
            return self.if_statement()
        if k == KW_WHILE:
            self.i += 1
            n = self.node(WHILE, t)
            self.expect(P_LPAREN, 'while')
            n.a = self.expression()
            self.expect(P_RPAREN, 'while')
            n.b = self.substatement('while')
            return self.done(n)
        if k == KW_DO:
            self.i += 1
            n = self.node(DO, t)
            n.a = self.substatement('do')
            self.expect(KW_WHILE, 'do')
            self.expect(P_LPAREN, 'do')
            n.b = self.expression()
            self.expect(P_RPAREN, 'do')
            self.expect(P_SEMI, 'do')
            return self.done(n)
        if k == KW_FOR:
            return self.for_statement()
        if k == KW_SWITCH:
            return self.switch_statement()
        if k == KW_RETURN:
            self.i += 1
            n = self.node(RETURN, t)
            if not self.at(P_SEMI):
                n.a = self.expression()
            self.expect(P_SEMI, 'return')
            return self.done(n)
        if k == KW_BREAK or k == KW_CONTINUE:
            self.i += 1
            n = self.node(BREAK if k == KW_BREAK else CONTINUE, t)
            if self.at(T_ID):
                self.unsupported(self.i, 'break or continue with a label')
            self.expect(P_SEMI, 'break' if k == KW_BREAK else 'continue')
            return self.done(n)
        if k == KW_CASE or k == KW_DEFAULT:
            self.error('case label outside a switch body')
        if k == KW_ELSE:
            self.error('else without if')
        if k == T_ID and self.peek_kind(1) == P_COLON:
            self.unsupported(t, 'labelled statement')
        if self.at_local_decl():
            n = self.local_decl()
            self.expect(P_SEMI, 'a local variable declaration')
            return self.done(n)
        n = self.node(EXPR_STMT, t)
        if (k == KW_THIS or k == KW_SUPER) and self.peek_kind(1) == P_LPAREN:
            # this(..) / super(..): the explicit constructor call
            if not ctor_first:
                self.unsupported(t, 'this(..) or super(..) elsewhere than as the '
                                    'first statement of a constructor')
            c = self.node(CTOR_CALL, t)
            self.i += 1
            self.arguments(c)
            n.a = self.done(c)
        else:
            n.a = self.expression()
            self.statement_expression(n.a)
        self.expect(P_SEMI, 'an expression statement')
        return self.done(n)

    def statement_expression(self, e):
        """StatementExpr = Assignment | ( "++" | "--" ) Unary
                         | Postfix ( "++" | "--" ) | MethodCall"""
        k = e.kind
        if k == ASSIGN or k == CALL or k == POSTFIX:
            return
        if k == UNARY:
            op = self.toks[e.op_tok].kind
            if op == P_INC or op == P_DEC:
                return
        if k == NEW:
            self.unverified(e.tok0, 'new T(..) as a statement')
            return
        self.error('not a statement: only an assignment, a call, ++ or -- can stand alone',
                   e.tok0)

    def if_statement(self):
        """"if" "(" Expression ")" Statement [ "else" Statement ]
        An else belongs to the nearest if that has none yet."""
        n = self.node(IF, self.i)
        self.i += 1
        self.expect(P_LPAREN, 'if')
        n.a = self.expression()
        self.expect(P_RPAREN, 'if')
        n.b = self.substatement('if')
        if self.accept(KW_ELSE):
            n.c = self.substatement('else')
        return self.done(n)

    def for_statement(self):
        """"for" "(" [ LocalDecl | StatementExpr ] ";" [ Expression ] ";"
        [ StatementExpr ] ")" Statement"""
        n = self.node(FOR, self.i)
        self.i += 1
        self.expect(P_LPAREN, 'for')
        if not self.at(P_SEMI):
            if self.at_local_decl():
                n.c = self.done(self.local_decl())
                if len(n.c.items) > 1:
                    # all 444 declarations in a for header declare one variable
                    self.unverified(n.c.tok0, 'several variables declared in a for header')
            else:
                lst = self.node(LIST, self.i)
                self.expression_list(lst)
                n.c = self.done(lst)
        self.expect(P_SEMI, 'for')
        if not self.at(P_SEMI):
            n.a = self.expression()
        self.expect(P_SEMI, 'for')
        if not self.at(P_RPAREN):
            self.expression_list(n)
        self.expect(P_RPAREN, 'for')
        n.b = self.substatement('for')
        return self.done(n)

    def expression_list(self, owner):
        """The statement expressions of a for header, into owner.items."""
        first = self.i
        while True:
            e = self.expression()
            self.statement_expression(e)
            owner.items.append(e)
            if not self.accept(P_COMMA):
                break
        if len(owner.items) > 1:
            self.unverified(first, 'several expressions in a for header')

    def switch_statement(self):
        """"switch" "(" Expression ")" "{" { SwitchGroup } "}"
        SwitchGroup = ( "case" Expression ":" | "default" ":" ) { Statement }
        A group is the statements from one label to the next; it is a scope."""
        n = self.node(SWITCH, self.i)
        self.i += 1
        self.expect(P_LPAREN, 'switch')
        n.a = self.expression()
        self.expect(P_RPAREN, 'switch')
        self.expect(P_LBRACE, 'switch')
        seen_default = False
        group = None
        while not self.at(P_RBRACE):
            t = self.i
            k = self.kind()
            if k == KW_CASE or k == KW_DEFAULT:
                if group is None or group.items:
                    if group is not None:
                        self.close_group(group, False)
                    group = self.node(SWITCH_GROUP, t)
                    group.a = self.node(LIST, t)
                    n.items.append(group)
                else:
                    self.unverified(t, 'several labels on one switch group')
                self.i += 1
                if k == KW_CASE:
                    lab = self.node(CASE_LABEL, t)
                    lab.a = self.expression()
                    self.expect(P_COLON, 'a case label')
                    if seen_default:
                        self.unverified(t, 'case label after the default label')
                else:
                    lab = self.node(DEFAULT_LABEL, t)
                    self.expect(P_COLON, 'the default label')
                    if seen_default:
                        self.unsupported(t, 'second default label')
                    seen_default = True
                group.a.items.append(self.done(lab))
            elif k == T_EOF:
                self.error('switch body is not closed')
            else:
                if group is None:
                    self.error('statement before the first case label')
                if not group.items:
                    # the labels of the group are complete: their list ends
                    # at the ":" read last
                    self.done(group.a)
                group.items.append(self.statement(False))
        if group is not None:
            self.close_group(group, True)
        else:
            self.unverified(n.tok0, 'switch without a case label')
        self.i += 1
        return self.done(n)

    def close_group(self, group, last):
        if not group.items:
            self.done(group.a)          # labels and no statement behind them
        self.done(group)
        if not group.items:
            self.unverified(group.tok0, 'switch group without statements')
        elif not last:
            k = group.items[len(group.items) - 1].kind
            if not (k == BREAK or k == RETURN or k == CONTINUE):
                self.unverified(group.tok0, 'switch group that falls through')

    # -- expressions ---------------------------------------------------------

    def expression(self):
        """Expression = Conditional | Assignment
           Assignment = Target AssignOp Expression        (groups to the right)"""
        self.enter()
        start = self.i
        left = self.conditional()
        t = self.i
        k = self.kind()
        if k in ASSIGN_OPS:
            self.i += 1
            if not (left.kind == NAME or left.kind == FIELD or left.kind == INDEX):
                self.unsupported(t, 'assignment to something that is not a name, '
                                    'a field or an array element')
            if k == P_SHL_ASSIGN or k == P_SHR_ASSIGN or k == P_USHR_ASSIGN:
                self.unsupported(t, 'operator %s' % sc.FIXED_TEXT[k])
            if k == P_MOD_ASSIGN:
                self.unverified(t, 'operator %=')
            n = self.node(ASSIGN, start)
            n.op_tok = t
            n.a = left
            n.b = self.expression()
            left = self.done(n)
        self.leave()
        return left

    def conditional(self):
        """Conditional = Binary [ "?" Expression ":" Conditional ]"""
        start = self.i
        conds = self.conds
        c = self.binary(1)
        if not self.at(P_QUESTION):
            return c
        if self.cond_depth or self.conds != conds:
            self.unverified(self.i, '?: inside ?:')
        self.conds += 1
        n = self.node(COND, start)
        n.op_tok = self.i
        n.a = c
        self.i += 1
        self.cond_depth += 1
        self.enter()                    # a ? b : c ? d : ... recurses here
        n.b = self.expression()
        self.expect(P_COLON, 'a conditional expression')
        n.c = self.conditional()
        self.leave()
        self.cond_depth -= 1
        return self.done(n)

    def binary(self, min_prec):
        """Binary = Unary { BinaryOp Unary | "instanceof" Identifier }
        by the levels of BINARY_PREC, every level grouping to the left."""
        start = self.i
        left = self.unary()
        while True:
            t = self.i
            k = self.kind()
            prec = BINARY_PREC.get(k, 0)
            if prec == 0 or prec < min_prec:
                return left
            self.i += 1
            if k == KW_INSTANCEOF:
                n = self.node(INSTANCEOF, start)
                n.op_tok = t
                n.a = left
                if self.at(KW_INT) or self.at(KW_FLOAT) or self.at(KW_VOID):
                    self.unsupported(self.i, 'instanceof with a primitive type')
                n.b = self.class_type('instanceof')
                if self.at(P_LBRACKET):
                    self.unsupported(self.i, 'instanceof with an array type')
            else:
                if k == P_USHR:
                    self.unsupported(t, 'operator >>>')
                n = self.node(BINARY, start)
                n.op_tok = t
                n.a = left
                n.b = self.binary(prec + 1)
            left = self.done(n)

    def at_cast(self):
        """At "(": does a cast follow?  True for  "(" ( "int" | "float" ) ")"
        and for  "(" Name ")"  followed by a token that can start an operand
        other than + and -."""
        toks = self.toks
        j = self.i + 1
        k = toks[j].kind
        if k == KW_INT or k == KW_FLOAT:
            j += 1
            while toks[j].kind == P_LBRACKET and toks[j + 1].kind == P_RBRACKET:
                j += 2
            return toks[j].kind == P_RPAREN
        if k != T_ID:
            return False
        j += 1
        while toks[j].kind == P_DOT and toks[j + 1].kind == T_ID:
            j += 2
        while toks[j].kind == P_LBRACKET and toks[j + 1].kind == P_RBRACKET:
            j += 2
        if toks[j].kind != P_RPAREN:
            return False
        k = toks[j + 1].kind
        return (k == T_ID or k == T_INT or k == T_FLOAT or k == T_STR or k == T_RID or
                k == KW_THIS or k == KW_SUPER or k == KW_NULL or k == KW_TRUE or
                k == KW_FALSE or k == KW_NEW or
                k == P_LPAREN or k == P_NOT or k == P_BITNOT)

    def unary(self):
        """Unary = ( "+" | "-" | "!" | "~" | "++" | "--" ) Unary
                 | "(" ( "int" | "float" ) ")" Unary
                 | "(" Identifier ")" UnaryNotPlusMinus
                 | Postfix [ "++" | "--" ]"""
        t = self.i
        k = self.kind()
        if k in PREFIX_OPS:
            self.enter()
            self.i += 1
            n = self.node(UNARY, t)
            n.op_tok = t
            n.a = self.unary()
            if k == P_INC or k == P_DEC:
                self.incdec_target(t, n.a)
            self.leave()
            return self.done(n)
        if k == P_LPAREN and self.at_cast():
            self.enter()
            self.i += 1
            n = self.node(CAST, t)
            n.a = self.type('a cast')
            if n.a.dims():
                self.unsupported(t, 'cast to an array type')
            self.expect(P_RPAREN, 'a cast')
            n.b = self.unary()
            self.leave()
            return self.done(n)
        return self.postfix()

    def incdec_target(self, tok, e):
        if not (e.kind == NAME or e.kind == FIELD or e.kind == INDEX):
            self.unsupported(tok, '++ or -- applied to something that is not a name, '
                                  'a field or an array element')

    def postfix(self):
        """Postfix = Primary { "." Identifier [ Arguments ] | "[" Expression "]" }
        and the postfix operators ++ and --."""
        start = self.i
        e = self.primary()
        while True:
            t = self.i
            k = self.kind()
            if k == P_DOT:
                self.i += 1
                name = self.ident('a member access')
                if self.at(P_LPAREN):
                    n = self.node(CALL, start)
                    n.a = e
                    n.name_tok = name
                    self.arguments(n)
                else:
                    if e.kind == SUPER:
                        self.unsupported(t, 'super.field')
                    n = self.node(FIELD, start)
                    n.a = e
                    n.name_tok = name
                e = self.done(n)
            elif k == P_LBRACKET:
                self.i += 1
                n = self.node(INDEX, start)
                n.a = e
                n.b = self.expression()
                self.expect(P_RBRACKET, 'an array access')
                e = self.done(n)
            elif k == P_INC or k == P_DEC:
                self.i += 1
                self.incdec_target(t, e)
                n = self.node(POSTFIX, start)
                n.op_tok = t
                n.a = e
                e = self.done(n)
            else:
                return e

    def arguments(self, owner):
        """Arguments = "(" [ Expression { "," Expression } ] ")", into owner.items."""
        self.expect(P_LPAREN, 'an argument list')
        if not self.at(P_RPAREN):
            while True:
                owner.items.append(self.expression())
                if not self.accept(P_COMMA):
                    break
        self.expect(P_RPAREN, 'an argument list')

    def primary(self):
        """Primary = Literal | Identifier [ Arguments ] | "this"
                   | "super" "." Identifier Arguments | "(" Expression ")"
                   | "new" Identifier Arguments
                   | "new" ( "int" | "float" | Identifier ) "[" Expression "]" """
        t = self.i
        k = self.kind()
        if k == T_ID:
            self.i += 1
            if self.at(P_LPAREN):
                n = self.node(CALL, t)
                n.name_tok = t
                self.arguments(n)
                return self.done(n)
            n = self.node(NAME, t)
            n.name_tok = t
            return self.done(n)
        if k in LITERAL_TOKENS:
            self.i += 1
            return self.done(self.node(LITERAL, t))
        if k == KW_THIS or k == KW_SUPER:
            self.i += 1
            if self.at(P_LPAREN):
                self.unsupported(t, 'this(..) or super(..) elsewhere than as the '
                                    'first statement of a constructor')
            if k == KW_SUPER and not self.at(P_DOT):
                self.error('super must be followed by "." or "("')
            return self.done(self.node(THIS if k == KW_THIS else SUPER, t))
        if k == KW_NEW:
            return self.new_expression()
        if k == P_LPAREN:
            self.i += 1
            n = self.node(PAREN, t)
            n.a = self.expression()
            self.expect(P_RPAREN, 'a parenthesised expression')
            return self.done(n)
        self.error('expected an expression')

    def new_expression(self):
        t = self.i
        self.i += 1
        k = self.kind()
        if not (k == KW_INT or k == KW_FLOAT or k == T_ID):
            self.error('expected a type after new')
        ty = self.done(self.type_name('new'))
        if self.at(P_LPAREN):
            if k != T_ID:
                self.error('expected "[" after the primitive type of new')
            n = self.node(NEW, t)
            n.a = ty
            self.arguments(n)
            return self.done(n)
        if self.at(P_LBRACKET):
            n = self.node(NEW_ARRAY, t)
            n.a = ty
            if self.peek_kind(1) == P_RBRACKET:
                self.unsupported(self.i, 'array creation without a size')
            self.i += 1
            n.b = self.expression()
            self.expect(P_RBRACKET, 'an array creation')
            if self.at(P_LBRACKET):
                self.unsupported(self.i, 'array creation with more than one dimension')
            if self.at(P_DOT):
                self.unsupported(self.i, 'member access directly on new T[n]')
            if self.at(P_LBRACE):
                self.unsupported(self.i, 'array initialiser')
            return self.done(n)
        self.error('expected "(" or "[" after the type of new')


def parse(source, toks, diags):
    """Parse the tokens of one source.  Returns the Ast; ast.root is None
    when the source does not fit the grammar (the diagnostics say why)."""
    p = Parser(source, toks, diags)
    try:
        root = p.unit()
    except _Stop:
        return p.ast
    except RecursionError:
        # a safety net behind MAX_DEPTH for the Python reference only
        t = toks[p.i]
        diags.error(E_LIMIT, t.line, sc.column_at(source, t.pos),
                    'statements or expressions nested too deep')
        return p.ast
    p.ast.root = root
    return p.ast
