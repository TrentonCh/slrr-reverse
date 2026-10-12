"""emit.py - the emitter: the AST of a method or of a field initialiser ->
the node list of its tree.

    trees = emit_class(scope, build, packs, float_mode)     # one Tree per tree owner

The rules are those of notes/statements.md (statements, locals, jumps, lines)
and notes/expressions.md (expressions, names, calls); together they reproduce
7,568 of the 7,570 trees of the oracle pairs node for node, the other two
being trees that were compiled against another version of a class they call.

THE OUTPUT

A Tree is a list of SymNode(op, line, kind, value) and a table of operands.
op is the op byte of the class file, line the source line the node carries.
An operand that is a number (local index, count, int value, float bits, kind
of expression, jump offset) is in the node; an operand that names a pool
entry is kept BY CONTENT in tree.operands (a text; a class name; a class, a
member name and a descriptor; a pack file name and a local id) and the node
holds its index there.  Pool indices do not exist yet: the pool is filled by
two passes over the finished trees of the whole class (the writer).

WHAT THE EMITTER KEEPS WHILE IT WALKS A BODY

    locals      one list for the frame: `this` (not in a static method), the
                parameters in REVERSE order, then every declared local.  A
                declaration takes the next index; the end of its scope (block,
                for statement, case group, body) releases it.  break, continue
                and return emit releases without closing the scope.
    flow        the enclosing loops and switches: where break and continue
                jump to, and how many locals were live at the entry
    labels      positions that jumps name; a jump holds a label until the
                tree is finished and then the offset in nodes, relative to
                the jump node itself

THE ORDER OF THE NODES is the order of execution, which is not the order of
the source: the value of an assignment before its target, the index before
the array, the arguments before a computed receiver, the update of a for
behind its body, the case values of a switch before its groups.

THE LINE OF A NODE is the line of the token the game's parser had read when
it built the node (notes/statements.md 8): the last token of a construct that
needs no lookahead (a literal, a name, the ")" of a call, the "]" of an
index), the token BEHIND a construct that ends in an operand (operators,
casts, assignments, ?:, instanceof, new T[n]).  The jumps of if, while and
for carry the line of end_tok(): the lookahead rule of an LALR(1) parser for
the grammar of the Java specification.  Every put_*() below takes the index
of that token.  Per kind of node:

    expr literal and its carrier                the literal
    local, field (a simple name)                the identifier
    path n of a name                            its last identifier
    segments of a path                          the identifier of the segment
    getfield                                    the member name
    index                                       the "]"
    push_int, call, call_on, call_super,
      expr call / stmt call                     the ")" of the call
    path n of a call, method / name             the callee name
    expr new, its push_int, new_init            the ")"
    type carrier of new                         the "(" behind the class name
    expr newarray and its type carrier          the token behind the "]"
    - ! ~ ++x --x                               the token behind the operand
    x++ x--                                     the operator
    binary operators; and_test / or_test,
      their jump, expr land / lor               the token behind the right operand
    assignment                                  the token behind the value
    ?:                                          the token behind the third operand
    cast and its carrier                        the token behind the operand
    instanceof and its type carrier             the token behind the type
    this(..) / super(..): push_int,
      this_init / super_init, stmt ctor         the ";"
    the implicit super call                     the first token behind the "{"
    declare, type, init, store                  the token behind the declared name
    release at the end of a block               its "}"
    jump_false and jump of if, while, for;
      release at the end of a for               end_tok(statement)
    jump_true of do                             its ";"
    case, pop, jump of a switch                 the "}" of the switch
    release at the end of a case group          the token behind the group
    release and jump of break / continue        its ";"
    value / push_void / ret, release,
      ret_value of a return                     its ";"
    implicit return, releases at the end
      of a body                                 the "}" of the body
    end of a field initialiser                  the token behind the declarator

For 14 of these kinds two candidate tokens stand on one line in every shipped
source (statements.md 8.1, "not ruled out"); the token taken here is the one
the grammar suggests.

NAMES (expressions.md 5).  The first identifier of a name is a local, else a
field of the class or a superclass, else a class.  a.b.c with such a head is
one `path n` with n segments; every further segment is a field ref.  A field
ref names the declaring class for a static field and the static type of the
receiver for an instance field (the class being compiled for a simple name).
A member of anything else (this, an element, a call, a cast, a parenthesis)
is fetched by name: getfield.

CALLS (expressions.md 6).  f(..) and a.b.f(..) end in a method ref when every
argument is a literal, a local, a field written as a simple name or `this`,
and the scored lookup (classes.find_method) on the receiver class finds a
method; the ref names the receiver class and carries the found descriptor.
In every other case the call ends in the bare name, behind a `path 1` header
when it has no receiver.  The compiler knows a type only for those four
forms; it never derives the type of an operator, a call, an element or a new.

LITERALS.  An int is its value modulo 2**32, a string its text after escape
processing, true and false a carrier of their own.  Two literals depend on
state of the game's exe that no source holds, and neither is decided here: the
bits of a float come from floatlit.float_bits() under the conversion the
caller selects (four occur in the shipped classes), and the file name of the
pack of a resource id comes from the PackNames the caller passes in.  A pack
for which it answers "no file" is compiled to system.rpk id 0, the form one
shipped class shows, and reported (W_PACK) at every such literal, in both
modes: the resource is not the one the source names.

DIAGNOSTICS.  An identifier that is no local, field or class is E_NAME, a
member that is not found in a dotted name is E_MEMBER; the tree of a source
with an error is never used.  Constructs no shipped source uses are compiled
by the plain extension of the rules and reported through diags.unverified()
(notes/compiler_spec.md appendix B): a warning, or an error in strict mode.
Every diagnostic is a function of the source and of the answers of the two
interfaces; none depends on what an earlier method happened to look up.
The emitter never raises for bad input.

NUMBERS.  Local indices, counts, token indices and node positions are
unsigned (C++: uint32_t).  No loop counts below zero, and "no local" is NONE,
not a negative number.  The one place that needs a signed difference is the
jump offset, which finish() forms modulo 2**32.

C++ (sc_emit.cpp): the same class with the same methods; a Tree is two
vectors; child nodes are indices.
"""
from . import classes
from . import floatlit
from . import packs as pk
from . import scanner as sc
from .diag import E_MEMBER, E_NAME, E_SYNTAX, E_UNSUPPORTED, W_PACK
from .parser import (
    ACC_STATIC, F_CTOR,
    BLOCK, LOCAL_VAR, EXPR_STMT, IF, WHILE, DO, FOR, SWITCH, CASE_LABEL,
    BREAK, CONTINUE, RETURN, EMPTY, LITERAL, NAME, THIS, SUPER, FIELD, INDEX,
    CALL, CTOR_CALL, NEW, NEW_ARRAY, UNARY, POSTFIX, BINARY, ASSIGN, COND,
    CAST, INSTANCEOF, PAREN)
from .parser import is_primitive
from .scanner import (
    T_INT, T_FLOAT, T_STR, T_RID, KW_THIS, KW_NULL, KW_TRUE, KW_FALSE, KW_VOID,
    P_ASSIGN, P_ADD_ASSIGN, P_SUB_ASSIGN, P_MUL_ASSIGN, P_DIV_ASSIGN,
    P_MOD_ASSIGN, P_AND_ASSIGN, P_OR_ASSIGN, P_XOR_ASSIGN, P_LOR, P_LAND,
    P_OR, P_XOR, P_AND, P_EQ, P_NE, P_LT, P_GT, P_LE, P_GE, P_SHL, P_SHR,
    P_ADD, P_SUB, P_MUL, P_DIV, P_MOD, P_NOT, P_BITNOT, P_INC, P_DEC)

# ---------------------------------------------------------------------------
# op bytes (notes/opcodes.md; the 16-bit opcode of the VM is in the comment)
# ---------------------------------------------------------------------------

OP_LOCAL = 0x01         # 0x1001  local i
OP_DECLARE = 0x02       # 0x1002  declare i; a type carrier follows
OP_RELEASE = 0x03       # 0x1003  release i
OP_AND_TEST = 0x04      # 0x0004  test of &&; a jump follows
OP_OR_TEST = 0x05       # 0x0005  test of ||; a jump follows
OP_TYPE = 0x06          # 0x1006  carrier: utf8 type descriptor
OP_EXPR = 0x07          # 0x1007  expression of kind imm, value used
OP_STMT = 0x08          # 0x1008  expression of kind imm, as a statement
OP_STR = 0x09           # 0x4009  carrier: string literal (utf8)
OP_FLOAT = 0x0A         # 0x200a  carrier: float literal (bits)
OP_INT = 0x0B           # 0x100b  carrier: int literal
OP_NULL = 0x0C          # 0x000c  carrier: null
OP_BOOL = 0x0D          # 0x100d  carrier: true (1) / false (0)
OP_RID = 0x0E           # 0x100e  carrier: resource id (rid entry)
OP_END = 0x10           # 0x0010  end of a field initialiser
OP_PATH = 0x11          # 0x1011  name path of imm segments
OP_NAME = 0x12          # 0x4012  segment: method by name (utf8)
OP_JUMP = 0x14          # 0x1014
OP_JUMP_FALSE = 0x15    # 0x1015
OP_JUMP_TRUE = 0x16     # 0x1016
OP_CASE = 0x17          # 0x1017  compare with the switch subject, jump when equal
OP_POP = 0x18           # 0x0018  drop the switch subject
OP_CLASS = 0x19         # 0x1019  segment / carrier: class entry
OP_METHOD = 0x1A        # 0x101a  segment: method ref
OP_FIELD = 0x1B         # 0x101b  field ref (a node of its own, or a segment)
OP_GETFIELD = 0x1C      # 0x401c  member of a computed value, by name (utf8)
OP_INDEX = 0x20         # 0x0020  array element
OP_NEW_INIT = 0x21      # 0x0021  constructor call of new
OP_THIS_INIT = 0x22     # 0x0022  this(..)
OP_SUPER_INIT = 0x23    # 0x0023  super(..)
OP_CALL = 0x24          # 0x0024  call; a method ref or a path follows
OP_CALL_ON = 0x25       # 0x4025  call on a computed receiver, by name (utf8)
OP_CALL_SUPER = 0x26    # 0x4026  super.f(..), by name (utf8)
OP_PUSH_INT = 0x27      # 0x1027  argument count
OP_VALUE = 0x28         # 0x0028  detach the return value from its variable
OP_PUSH_VOID = 0x29     # 0x0029  the value of return;
OP_RET = 0x2A           # 0x002a  return from a constructor
OP_RET_VALUE = 0x2B     # 0x002b  return
OP_INIT = 0x2C          # 0x002c  marker: the declaration has an initialiser
OP_STORE = 0x2D         # 0x002d  store the initialiser

OP_NAMES = {
    OP_LOCAL: 'local', OP_DECLARE: 'declare', OP_RELEASE: 'release',
    OP_AND_TEST: 'and_test', OP_OR_TEST: 'or_test', OP_TYPE: 'type',
    OP_EXPR: 'expr', OP_STMT: 'stmt', OP_STR: 'str', OP_FLOAT: 'float',
    OP_INT: 'int', OP_NULL: 'null', OP_BOOL: 'bool', OP_RID: 'rid',
    OP_END: 'end', OP_PATH: 'path', OP_NAME: 'name', OP_JUMP: 'jump',
    OP_JUMP_FALSE: 'jump_false', OP_JUMP_TRUE: 'jump_true', OP_CASE: 'case',
    OP_POP: 'pop', OP_CLASS: 'class', OP_METHOD: 'method', OP_FIELD: 'field',
    OP_GETFIELD: 'getfield', OP_INDEX: 'index', OP_NEW_INIT: 'new_init',
    OP_THIS_INIT: 'this_init', OP_SUPER_INIT: 'super_init', OP_CALL: 'call',
    OP_CALL_ON: 'call_on', OP_CALL_SUPER: 'call_super', OP_PUSH_INT: 'push_int',
    OP_VALUE: 'value', OP_PUSH_VOID: 'push_void', OP_RET: 'ret',
    OP_RET_VALUE: 'ret_value', OP_INIT: 'init', OP_STORE: 'store',
}

# kinds of expression: the immediate of OP_EXPR / OP_STMT (expressions.md 4)
X_COND = 0x00
X_LOR = 0x01
X_LAND = 0x02
X_NEG = 0x16
X_LNOT = 0x15
X_BNOT = 0x14
X_POSTINC = 0x17
X_POSTDEC = 0x18
X_PREINC = 0x19
X_PREDEC = 0x1A
X_CALL = 0x1B
X_LITERAL = 0x1C
X_NEW = 0x1E
X_NEWARRAY = 0x1F
X_INSTANCEOF = 0x20
X_CAST = 0x21
X_CTOR = 0x22

BINARY_KIND = {
    P_OR: 0x03, P_XOR: 0x04, P_AND: 0x05, P_NE: 0x06, P_EQ: 0x07, P_GE: 0x08,
    P_LE: 0x09, P_GT: 0x0A, P_LT: 0x0B, P_SHR: 0x0D, P_SHL: 0x0E, P_SUB: 0x0F,
    P_ADD: 0x10, P_MOD: 0x11, P_DIV: 0x12, P_MUL: 0x13,
}
ASSIGN_KIND = {
    P_ASSIGN: 0x23, P_MUL_ASSIGN: 0x24, P_DIV_ASSIGN: 0x25, P_MOD_ASSIGN: 0x26,
    P_ADD_ASSIGN: 0x27, P_SUB_ASSIGN: 0x28, P_AND_ASSIGN: 0x2C,
    P_XOR_ASSIGN: 0x2D, P_OR_ASSIGN: 0x2E,
}

# kind -> name, for dumps.  0x0C, 0x1D and 0x29 to 0x2B are never emitted
# (by their place in the table: >>>, -, <<=, >>=, >>>=).
X_NAMES = {
    0x00: 'cond', 0x01: 'lor', 0x02: 'land', 0x03: 'bor', 0x04: 'bxor',
    0x05: 'band', 0x06: 'ne', 0x07: 'eq', 0x08: 'ge', 0x09: 'le', 0x0A: 'gt',
    0x0B: 'lt', 0x0D: 'shr', 0x0E: 'shl', 0x0F: 'sub', 0x10: 'add',
    0x11: 'mod', 0x12: 'div', 0x13: 'mul', 0x14: 'bnot', 0x15: 'lnot',
    0x16: 'neg', 0x17: 'postinc', 0x18: 'postdec', 0x19: 'preinc',
    0x1A: 'predec', 0x1B: 'call', 0x1C: 'literal', 0x1E: 'new',
    0x1F: 'newarray', 0x20: 'instanceof', 0x21: 'cast', 0x22: 'ctor',
    0x23: 'assign', 0x24: 'mul_assign', 0x25: 'div_assign',
    0x26: 'mod_assign', 0x27: 'add_assign', 0x28: 'sub_assign',
    0x2C: 'and_assign', 0x2D: 'xor_assign', 0x2E: 'or_assign',
}

# ---------------------------------------------------------------------------
# the output
# ---------------------------------------------------------------------------

# what the value of a node is
K_NONE = 0          # the node has no operand (3 bytes in the file)
K_IMM = 1           # a number
K_LABEL = 2         # a label; only while the tree is being emitted
K_UTF8 = 3          # index into tree.operands: a text
K_CLASS = 4         # index into tree.operands: a class
K_REF = 5           # index into tree.operands: a field or a method of a class
K_RID = 6           # index into tree.operands: a resource

NONE = 0xFFFFFFFF


class SymNode(object):
    __slots__ = ('op', 'line', 'kind', 'value')

    def __init__(self, op, line, kind, value):
        self.op = op
        self.line = line
        self.kind = kind
        self.value = value


class Operand(object):
    """What a node names in the pool, by content.

        K_UTF8      text
        K_CLASS     text = the class name
        K_REF       text = the class name, name, desc
        K_RID       text = the pack file name, number = the local id"""
    __slots__ = ('text', 'name', 'desc', 'number')

    def __init__(self, text, name, desc, number):
        self.text = text
        self.name = name
        self.desc = desc
        self.number = number


class Tree(object):
    __slots__ = ('nodes', 'operands', 'max_locals')

    def __init__(self):
        self.nodes = []             # SymNode
        self.operands = []          # Operand
        self.max_locals = 0         # the most locals alive at one time; 0 for an empty tree


# ---------------------------------------------------------------------------
# what the emitter keeps
# ---------------------------------------------------------------------------

class Flow(object):
    """An enclosing loop or switch."""
    __slots__ = ('is_loop', 'brk', 'cont', 'mark', 'cont_proven')

    def __init__(self, is_loop, brk, cont, mark, cont_proven):
        self.is_loop = is_loop
        self.brk = brk              # label behind the statement
        self.cont = cont            # label continue jumps to (loops)
        self.mark = mark            # number of live locals at the entry
        self.cont_proven = cont_proven  # a for with an update: the one place
        #                             a shipped source has a continue in


SEG_LOCAL = 0
SEG_FIELD = 1
SEG_CLASS = 2


class Segment(object):
    """One identifier of a dotted name, resolved."""
    __slots__ = ('kind', 'index', 'cls', 'name', 'desc')

    def __init__(self, kind, index, cls, name, desc):
        self.kind = kind
        self.index = index          # SEG_LOCAL: the local
        self.cls = cls              # SEG_FIELD: the class of the ref; SEG_CLASS: the class
        self.name = name            # SEG_FIELD
        self.desc = desc            # SEG_FIELD


class Walk(object):
    """A dotted name, resolved identifier by identifier."""
    __slots__ = ('ok', 'segs', 'type', 'cls')

    def __init__(self):
        self.ok = True
        self.segs = []
        self.type = None            # descriptor of the value the name stands for
        self.cls = None             # the class, when the name is a class name


def is_name(e):
    """Is the expression a name: an identifier, or a name "." identifier?"""
    while e.kind == FIELD:
        e = e.a
    return e.kind == NAME


def name_toks(e):
    """The identifier tokens of a name, first to last."""
    out = []
    while e.kind == FIELD:
        out.append(e.name_tok)
        e = e.a
    out.append(e.tok0)
    out.reverse()
    return out


def strip_parens(e):
    while e.kind == PAREN:
        e = e.a
    return e


def is_ctor_call(s):
    """Is the statement a written this(..) or super(..)?"""
    return s.kind == EXPR_STMT and s.a.kind == CTOR_CALL


class Emitter(object):
    """Emits the trees of one class."""

    def __init__(self, scope, info, packs, float_mode):
        self.scope = scope
        self.ast = scope.ast
        self.toks = scope.ast.toks
        self.diags = scope.diags
        self.cls = info.name                    # the class being compiled
        self.this_desc = 'L' + info.name + ';'
        self.packs = packs
        self.float_mode = float_mode
        # the tree being emitted
        self.tree = None
        self.labels = []            # label -> node position, NONE until placed
        self.local_names = []       # the live locals; the index is the local
        self.local_types = []       # their descriptors
        self.declaring = NONE       # the local whose initialiser is being compiled
        self.flow = []
        self.is_ctor = False
        self.is_static = False
        self.returns_value = False  # a method with a return type other than void
        self.in_field = False

    # -- trees ---------------------------------------------------------------

    def begin(self):
        self.tree = Tree()
        self.labels = []
        self.local_names = []
        self.local_types = []
        self.declaring = NONE
        self.flow = []
        self.is_ctor = False
        self.is_static = False
        self.returns_value = False
        self.in_field = False

    def finish(self):
        """Turn the labels into offsets: target position - position of the
        jump, in nodes, as an unsigned 32-bit number."""
        tree = self.tree
        self.tree = None
        i = 0
        for n in tree.nodes:
            if n.kind == K_LABEL:
                target = self.labels[n.value]
                if target == NONE:              # only after an error
                    target = len(tree.nodes)
                n.value = (target - i) & 0xFFFFFFFF
                n.kind = K_IMM
            i += 1
        return tree

    def field_tree(self, d, is_static):
        """The tree of a field initialiser (DECLARATOR d with an
        initialiser):  E(init), end."""
        self.begin()
        self.is_static = is_static
        self.in_field = True
        self.expr(d.a, OP_EXPR)
        self.put(OP_END, d.tok1 + 1)
        return self.finish()

    def method_tree(self, m):
        """The tree of a method or constructor (METHOD_DECL m).

            constructor     the this(..) / super(..) call; when none is
                            written: push_int 0, super_init, stmt ctor
            all             the statements of the body
            void method     push_void, releases, ret_value    (implicit return)
            constructor     ret, releases                     (implicit return)
            all             releases of the locals of the body, then of the
                            parameters and this

        A body written ";" gives an empty tree, and so does "{}" on a method;
        a constructor written "{}" still holds the super call and the return."""
        self.begin()
        self.is_ctor = (m.flags & F_CTOR) != 0
        self.is_static = (m.flags & ACC_STATIC) != 0
        body = m.b
        if body is None:
            return self.finish()
        if not self.is_ctor and not body.items:
            return self.finish()
        self.returns_value = (not self.is_ctor and
                              self.toks[m.a.tok0].kind != KW_VOID)
        if not self.is_static:
            self.declare('this', self.this_desc)
        k = len(m.items)
        while k > 0:                            # the last parameter first
            k -= 1
            p = m.items[k]
            self.declare(self.ast.text(p.name_tok),
                         classes.descriptor(self.scope, p.a, p.dims()))
        frame = len(self.local_names)           # this + parameters
        close = body.tok1                       # the "}" of the body
        if self.is_ctor and not (body.items and is_ctor_call(body.items[0])):
            # built when the parser has read the first token of the body and
            # sees that it is not this( / super(
            first = body.tok0 + 1
            self.put_imm(OP_PUSH_INT, 0, first)
            self.put(OP_SUPER_INIT, first)
            self.put_imm(OP_STMT, X_CTOR, first)
        before = len(self.tree.nodes)
        errors = self.diags.errors
        for s in body.items:
            self.stmt(s, False)
        if (not self.is_ctor and len(self.tree.nodes) == before and
                self.diags.errors == errors):
            # "{ ; }" or "{ { } }": whether the game's compiler takes such a
            # body for an empty one is not known; it is not taken for one here
            self.unverified(body.tok0, 'method body whose statements emit no node')
        # The implicit return: constructors and void methods have it, value
        # methods do not.  No shipped source tells "by the kind of method"
        # from "unless the last statement is a return"; the kind decides here.
        last = body.items[len(body.items) - 1] if body.items else None
        if self.returns_value:
            if last.kind != RETURN:
                self.unverified(close, 'method with a return type that does not end in a '
                                       'return statement')
        else:
            if last is not None and last.kind == RETURN:
                self.unverified(last.tok0, 'return as the last statement of a void method '
                                           'or a constructor')
            self.ret(None, close)
        self.releases(frame, close, True)       # end of the body block
        self.releases(0, close, True)           # end of the method
        return self.finish()

    # -- nodes ---------------------------------------------------------------

    def line(self, tok):
        if tok >= len(self.toks):
            tok = len(self.toks) - 1
        return self.toks[tok].line

    def put(self, op, tok):
        self.tree.nodes.append(SymNode(op, self.line(tok), K_NONE, 0))

    def put_imm(self, op, value, tok):
        self.tree.nodes.append(SymNode(op, self.line(tok), K_IMM, value))

    def put_jump(self, op, label, tok):
        self.tree.nodes.append(SymNode(op, self.line(tok), K_LABEL, label))

    def put_operand(self, op, kind, operand, tok):
        self.tree.nodes.append(SymNode(op, self.line(tok), kind, len(self.tree.operands)))
        self.tree.operands.append(operand)

    def put_utf8(self, op, text, tok):
        self.put_operand(op, K_UTF8, Operand(text, '', '', 0), tok)

    def put_class(self, op, name, tok):
        self.put_operand(op, K_CLASS, Operand(name, '', '', 0), tok)

    def put_ref(self, op, cls, name, desc, tok):
        self.put_operand(op, K_REF, Operand(cls, name, desc, 0), tok)

    def new_label(self):
        self.labels.append(NONE)
        return len(self.labels) - 1

    def place(self, label):
        self.labels[label] = len(self.tree.nodes)

    # -- diagnostics ---------------------------------------------------------

    def error(self, code, tok, text):
        t = self.toks[tok]
        self.diags.error(code, t.line, sc.column_at(self.ast.source, t.pos), text)

    def unverified(self, tok, text):
        t = self.toks[tok]
        self.diags.unverified(t.line, sc.column_at(self.ast.source, t.pos), text)

    # -- locals --------------------------------------------------------------

    def declare(self, name, desc):
        self.local_names.append(name)
        self.local_types.append(desc)
        if len(self.local_names) > self.tree.max_locals:
            self.tree.max_locals = len(self.local_names)
        return len(self.local_names) - 1

    def lookup_local(self, name):
        """Index of the live local of that name, the innermost first; NONE
        when there is none."""
        k = len(self.local_names)
        while k > 0:
            k -= 1
            if self.local_names[k] == name:
                return k
        return NONE

    def releases(self, down_to, tok, close):
        """release nodes for the locals from the highest down to `down_to`.
        close: the scope ends, the locals are gone; otherwise (break,
        continue, return) the code behind is compiled as if nothing had
        happened."""
        k = len(self.local_names)
        while k > down_to:
            k -= 1
            self.put_imm(OP_RELEASE, k, tok)
        if close:
            del self.local_names[down_to:]
            del self.local_types[down_to:]

    # -- statements ----------------------------------------------------------

    def end_tok(self, s, dual):
        """The token the game's parser had read when it reduced statement s
        (notes/statements.md 8.2).  dual: s stands where the grammar allows
        both Statement and StatementNoShortIf, that is in the then-branch of
        an if, and in the else-branch or loop body of a statement that itself
        stands in such a place.  There the parser reads one token past a
        finished statement before it reduces it; an if without else always
        costs that token."""
        while True:
            k = s.kind
            if k == IF:
                if s.c is None:
                    return s.tok1 + 1
                s = s.c
            elif k == WHILE or k == FOR:
                s = s.b
            else:
                return s.tok1 + 1 if dual else s.tok1

    def stmt(self, s, dual):
        k = s.kind
        if k == EXPR_STMT:
            self.expr(s.a, OP_STMT)
        elif k == IF:
            # E(c), jump_false X, [then], X:
            # E(c), jump_false X, [then], jump Y, X: [else], Y:
            end = self.end_tok(s, dual)
            self.expr(s.a, OP_EXPR)
            l_else = self.new_label()
            self.put_jump(OP_JUMP_FALSE, l_else, end)
            self.stmt(s.b, True)
            if s.c is None:
                self.place(l_else)
            else:
                l_end = self.new_label()
                self.put_jump(OP_JUMP, l_end, end)
                self.place(l_else)
                self.stmt(s.c, dual)
                self.place(l_end)
        elif k == BLOCK:
            mark = len(self.local_names)
            for x in s.items:
                self.stmt(x, False)
            self.releases(mark, s.tok1, True)
        elif k == LOCAL_VAR:
            self.local_decl(s)
        elif k == RETURN:
            self.return_stmt(s)
        elif k == FOR:
            self.for_stmt(s, dual)
        elif k == WHILE:
            # T: E(c), jump_false X, [body], jump T, X:
            end = self.end_tok(s, dual)
            l_top = self.new_label()
            l_end = self.new_label()
            self.place(l_top)
            self.expr(s.a, OP_EXPR)
            self.put_jump(OP_JUMP_FALSE, l_end, end)
            self.flow.append(Flow(True, l_end, l_top, len(self.local_names), False))
            self.stmt(s.b, dual)
            self.flow.pop()
            self.put_jump(OP_JUMP, l_top, end)
            self.place(l_end)
        elif k == DO:
            # T: [body], C: E(c), jump_true T, X:
            l_top = self.new_label()
            l_cont = self.new_label()
            l_end = self.new_label()
            self.place(l_top)
            self.flow.append(Flow(True, l_end, l_cont, len(self.local_names), False))
            self.stmt(s.a, False)
            self.flow.pop()
            self.place(l_cont)
            self.expr(s.b, OP_EXPR)
            self.put_jump(OP_JUMP_TRUE, l_top, s.tok1)
            self.place(l_end)
        elif k == SWITCH:
            self.switch_stmt(s)
        elif k == BREAK:
            # releases of the locals declared since the loop or switch was
            # entered, jump behind it
            if not self.flow:
                self.error(E_SYNTAX, s.tok0, 'break outside a loop or a switch')
                return
            f = self.flow[len(self.flow) - 1]
            self.releases(f.mark, s.tok1, False)
            self.put_jump(OP_JUMP, f.brk, s.tok1)
        elif k == CONTINUE:
            self.continue_stmt(s)
        elif k == EMPTY:
            pass
        else:
            self.error(E_UNSUPPORTED, s.tok0, 'statement the emitter has no rule for')

    def local_decl(self, s):
        """T v;      declare i, type T
           T v = e;  declare i, type T, init, E(e), store
        One sequence per declarator.  All its nodes carry the line of the
        token behind the declared name.  The local exists before its
        initialiser is compiled; no shipped initialiser names its own
        variable, so that order is not confirmed (self.declaring)."""
        for d in s.items:
            tok = d.name_tok + 1 + 2 * d.dims()
            desc = classes.descriptor(self.scope, s.a, d.dims())
            self.put_imm(OP_DECLARE, len(self.local_names), tok)
            self.put_utf8(OP_TYPE, desc, tok)
            local = self.declare(self.ast.text(d.name_tok), desc)
            if d.a is not None:
                self.put(OP_INIT, tok)
                self.declaring = local
                self.expr(d.a, OP_EXPR)
                self.declaring = NONE
                self.put(OP_STORE, tok)

    def for_stmt(self, s, dual):
        """[init], T: E(c), jump_false X, [body], C: S(update), jump T,
        X: releases of the locals of the init.
        Without a condition there is no test; without an update C is the
        closing jump."""
        end = self.end_tok(s, dual)
        mark = len(self.local_names)
        init = s.c
        if init is not None:
            if init.kind == LOCAL_VAR:
                self.local_decl(init)
            else:
                for x in init.items:
                    self.expr(x, OP_STMT)
        l_top = self.new_label()
        l_cont = self.new_label()
        l_end = self.new_label()
        self.place(l_top)
        if s.a is not None:
            self.expr(s.a, OP_EXPR)
            self.put_jump(OP_JUMP_FALSE, l_end, end)
        self.flow.append(Flow(True, l_end, l_cont, len(self.local_names), len(s.items) > 0))
        self.stmt(s.b, dual)
        self.flow.pop()
        self.place(l_cont)
        for x in s.items:
            self.expr(x, OP_STMT)
        self.put_jump(OP_JUMP, l_top, end)
        self.place(l_end)
        self.releases(mark, end, True)

    def switch_stmt(self, s):
        """E(subject); per case label: E(value), case G; pop; jump to the
        default group or behind the switch; then the groups in source order,
        each closed by the releases of its own locals (a group is a scope).
        Nothing separates the groups: one that does not end in break runs
        into the next."""
        close = s.tok1                          # the "}" of the switch
        self.expr(s.a, OP_EXPR)
        l_end = self.new_label()
        l_default = NONE
        group_labels = []
        for g in s.items:
            lab = self.new_label()
            group_labels.append(lab)
            for c in g.a.items:
                if c.kind == CASE_LABEL:
                    self.expr(c.a, OP_EXPR)
                    self.put_jump(OP_CASE, lab, close)
                else:
                    l_default = lab
        self.put(OP_POP, close)
        self.put_jump(OP_JUMP, l_end if l_default == NONE else l_default, close)
        mark = len(self.local_names)
        self.flow.append(Flow(False, l_end, NONE, mark, False))
        k = 0
        for g in s.items:
            self.place(group_labels[k])
            k += 1
            for x in g.items:
                self.stmt(x, False)
            # the releases carry the line of the token behind the group: the
            # next label, or the "}"
            self.releases(mark, g.tok1 + 1, True)
        self.flow.pop()
        self.place(l_end)

    def continue_stmt(self, s):
        """releases, then a jump to the update of the for (to the condition
        of a while or do)."""
        f = None                        # the innermost enclosing loop
        through_switch = False
        k = len(self.flow)
        while k > 0:
            k -= 1
            if self.flow[k].is_loop:
                f = self.flow[k]
                break
            through_switch = True
        if f is None:
            self.error(E_SYNTAX, s.tok0, 'continue outside a loop')
            return
        if through_switch or not f.cont_proven:
            self.unverified(s.tok0, 'continue in a while or do loop, in a for without '
                                    'update, or inside a switch')
        self.releases(f.mark, s.tok1, False)
        self.put_jump(OP_JUMP, f.cont, s.tok1)

    def return_stmt(self, s):
        """Every return of the shipped sources fits its method: a value in a
        method with a return type, none in a void method or a constructor."""
        if s.a is not None:
            if self.is_ctor:
                self.error(E_UNSUPPORTED, s.tok0, 'return with a value in a constructor '
                                                  '(no shipped source uses it)')
                return
            if not self.returns_value:
                self.unverified(s.tok0, 'return with a value in a void method')
        elif self.returns_value:
            self.unverified(s.tok0, 'return without a value in a method with a return type')
        self.ret(s.a, s.tok1)

    def ret(self, value, tok):
        """return e;                E(e), value, releases, ret_value
        return;                     push_void, releases, ret_value
        return; in a constructor    ret, releases
        Every live local is released, down to this; the scope stays open."""
        if self.is_ctor:
            self.put(OP_RET, tok)
            self.releases(0, tok, False)
            return
        if value is not None:
            self.expr(value, OP_EXPR)
            self.put(OP_VALUE, tok)
        else:
            self.put(OP_PUSH_VOID, tok)
        self.releases(0, tok, False)
        self.put(OP_RET_VALUE, tok)

    # -- expressions ---------------------------------------------------------

    def expr(self, e, top):
        """Emit expression e.  top is OP_EXPR when its value is used and
        OP_STMT when e is an expression statement (or an element of a for
        header): the node of the outermost construct is written with it,
        nothing else differs."""
        k = e.kind
        if k == LITERAL:
            self.literal(e, top)
        elif k == NAME:
            self.name(e)
        elif k == FIELD:
            if is_name(e):
                self.name(e)
            else:
                # a member of a computed value: by name
                name = self.ast.text(e.name_tok)
                self.receiver_check(e.a)
                if name == 'length':
                    # inside a name the length of an array is a field ref
                    # of the array type; here the type is not known
                    self.unverified(e.name_tok, 'member length of a computed value')
                self.expr(e.a, OP_EXPR)
                self.put_utf8(OP_GETFIELD, name, e.tok1)
        elif k == CALL:
            self.call(e, top)
        elif k == ASSIGN:
            # the value first, then the target
            op = self.toks[e.op_tok].kind
            if op != P_ASSIGN and top == OP_EXPR:
                self.unverified(e.op_tok, 'compound assignment used as a value')
            self.expr(e.b, OP_EXPR)
            self.expr(e.a, OP_EXPR)
            self.put_imm(top, ASSIGN_KIND[op], e.tok1 + 1)
        elif k == BINARY:
            op = self.toks[e.op_tok].kind
            behind = e.tok1 + 1
            if op == P_LAND or op == P_LOR:
                # E(x), test, jump Z, E(y), expr land / lor, Z:
                self.expr(e.a, OP_EXPR)
                self.put(OP_AND_TEST if op == P_LAND else OP_OR_TEST, behind)
                skip = self.new_label()
                self.put_jump(OP_JUMP, skip, behind)
                self.expr(e.b, OP_EXPR)
                self.put_imm(top, X_LAND if op == P_LAND else X_LOR, behind)
                self.place(skip)
            else:
                self.expr(e.a, OP_EXPR)
                self.expr(e.b, OP_EXPR)
                self.put_imm(top, BINARY_KIND[op], behind)
        elif k == PAREN:
            self.expr(e.a, top)
        elif k == INDEX:
            # the index first, then the array
            self.expr(e.b, OP_EXPR)
            self.expr(e.a, OP_EXPR)
            self.put(OP_INDEX, e.tok1)
        elif k == UNARY:
            op = self.toks[e.op_tok].kind
            self.expr(e.a, OP_EXPR)
            if op == P_SUB:
                self.put_imm(top, X_NEG, e.tok1 + 1)
            elif op == P_NOT:
                self.put_imm(top, X_LNOT, e.tok1 + 1)
            elif op == P_BITNOT:
                self.put_imm(top, X_BNOT, e.tok1 + 1)
            elif op == P_INC:
                self.put_imm(top, X_PREINC, e.tok1 + 1)
            elif op == P_DEC:
                self.put_imm(top, X_PREDEC, e.tok1 + 1)
            # a unary plus has no node
        elif k == POSTFIX:
            self.expr(e.a, OP_EXPR)
            self.put_imm(top, X_POSTINC if self.toks[e.op_tok].kind == P_INC else X_POSTDEC,
                         e.tok1)
        elif k == NEW:
            # expr new, type, arguments, count, new_init; the type carrier is
            # built when the class name is reduced: line of the "("
            self.put_imm(top, X_NEW, e.tok1)
            self.put_utf8(OP_TYPE, classes.descriptor(self.scope, e.a, 0), e.a.tok1 + 1)
            for a in e.items:
                self.expr(a, OP_EXPR)
            self.put_imm(OP_PUSH_INT, len(e.items), e.tok1)
            self.put(OP_NEW_INIT, e.tok1)
        elif k == NEW_ARRAY:
            self.expr(e.b, OP_EXPR)
            self.put_imm(top, X_NEWARRAY, e.tok1 + 1)
            self.put_utf8(OP_TYPE, classes.descriptor(self.scope, e.a, 1), e.tok1 + 1)
        elif k == THIS:
            if self.is_static:
                self.unverified(e.tok0, 'this in a static method or in the initialiser of '
                                        'a static field')
            elif self.in_field:
                self.unverified(e.tok0, 'this in a field initialiser')
            self.put_imm(OP_LOCAL, 0, e.tok0)
        elif k == CAST:
            # a cast to a class carries a class entry, a cast to int or
            # float a type descriptor
            self.expr(e.b, OP_EXPR)
            self.put_imm(top, X_CAST, e.tok1 + 1)
            desc = classes.descriptor(self.scope, e.a, 0)
            if is_primitive(self.ast, e.a):
                self.put_utf8(OP_TYPE, desc, e.tok1 + 1)
            else:
                self.put_class(OP_CLASS, desc[1:len(desc) - 1], e.tok1 + 1)
        elif k == INSTANCEOF:
            self.expr(e.a, OP_EXPR)
            self.put_imm(top, X_INSTANCEOF, e.tok1 + 1)
            self.put_utf8(OP_TYPE, classes.descriptor(self.scope, e.b, 0), e.tok1 + 1)
        elif k == COND:
            # no jump: both branches are evaluated
            self.expr(e.a, OP_EXPR)
            self.expr(e.b, OP_EXPR)
            self.expr(e.c, OP_EXPR)
            self.put_imm(top, X_COND, e.tok1 + 1)
        elif k == CTOR_CALL:
            # this(..) / super(..): arguments, count, this_init / super_init,
            # stmt ctor; the three nodes carry the line of the ";"
            for a in e.items:
                self.expr(a, OP_EXPR)
            semi = e.tok1 + 1
            self.put_imm(OP_PUSH_INT, len(e.items), semi)
            self.put(OP_THIS_INIT if self.toks[e.tok0].kind == KW_THIS else OP_SUPER_INIT, semi)
            self.put_imm(OP_STMT, X_CTOR, semi)
        else:
            self.error(E_UNSUPPORTED, e.tok0, 'expression the emitter has no rule for')

    def literal(self, e, top):
        """expr literal and its carrier, on the line of the literal."""
        tok = e.tok0
        kind = self.toks[tok].kind
        text = self.ast.text(tok)
        self.put_imm(top, X_LITERAL, tok)
        if kind == T_INT:
            self.put_imm(OP_INT, sc.int_value(text), tok)
        elif kind == T_FLOAT:
            digits = sc.float_digits(text)
            self.put_imm(OP_FLOAT, floatlit.float_bits(self.float_mode, digits[0], digits[1]),
                         tok)
        elif kind == T_STR:
            self.put_utf8(OP_STR, sc.string_value(text), tok)
        elif kind == T_RID:
            parts = sc.rid_parts(text)
            name = self.packs.canonical(pk.pack_path(parts[0]))
            number = parts[1]
            if name is None:
                # A pack without a file (observed once, Build 940: the two
                # literals of TimeAttackTest).  The resource the class will
                # name is not the one the source names, so this is said at
                # every such literal, and in strict mode too: a warning,
                # because the shipped source has to compile.
                t = self.toks[tok]
                self.diags.warning(W_PACK, t.line, sc.column_at(self.ast.source, t.pos),
                                   'pack %s has no file: the resource id is compiled to %s '
                                   'id %d' % (parts[0], pk.NO_FILE_PACK, pk.NO_FILE_ID))
                name = pk.NO_FILE_PACK
                number = pk.NO_FILE_ID
            self.put_operand(OP_RID, K_RID, Operand(name, '', '', number), tok)
        elif kind == KW_NULL:
            self.put(OP_NULL, tok)
        else:
            self.put_imm(OP_BOOL, 1 if kind == KW_TRUE else 0, tok)

    # -- names ---------------------------------------------------------------

    def walk(self, toks, count):
        """Resolve the first `count` identifiers of a dotted name.  The first
        one is a local, else a field of the class or a superclass, else a
        class; every further one is a field of the type reached so far
        (length for an array)."""
        w = Walk()
        name = self.ast.text(toks[0])
        local = self.lookup_local(name)
        hit = None
        if local == NONE:
            hit = classes.find_field(self.scope, self.cls, name)
        if local != NONE or hit is not None:
            # No shipped source has a name that is a variable and a class,
            # so the place of class names in this order is not known.  The
            # question is decided from the source alone: a class the source
            # declares, or a name it writes as a type anywhere.  (The
            # provider is not asked about every variable, and what an
            # earlier method happened to resolve plays no part.  A class the
            # source uses only as the head of a name, Util.abs(a), is not
            # seen here.)
            if name in self.scope.own_by_simple or name in self.scope.type_names:
                self.unverified(toks[0], '%s is a variable and the name of a class; the '
                                         'variable is taken' % name)
        if local != NONE:
            if local == self.declaring:
                self.unverified(toks[0], 'local variable %s used in its own initialiser'
                                % name)
            w.segs.append(Segment(SEG_LOCAL, local, '', '', ''))
            w.type = self.local_types[local]
        elif hit is not None:
            f = hit.field
            if self.is_static and not f.is_static:
                # no static method and no initialiser of a static field of
                # the shipped sources names an instance field
                self.unverified(toks[0], 'instance field %s used in a static method or in '
                                         'the initialiser of a static field' % name)
            w.segs.append(Segment(SEG_FIELD, 0, hit.owner if f.is_static else self.cls,
                                  f.name, f.desc))
            w.type = f.desc
        else:
            full = classes.resolve_type(self.scope, name)
            if full is None:
                self.error(E_NAME, toks[0], '%s is no local variable, field or class' % name)
                w.ok = False
                return w
            w.segs.append(Segment(SEG_CLASS, 0, full, '', ''))
            w.cls = full
        k = 1
        while k < count:
            name = self.ast.text(toks[k])
            cls = w.cls
            if cls is None:
                cls = classes.class_of_desc(w.type)
            if cls is not None and cls[:1] == '[':
                # the one member of an array; the ref names the array type
                if name != 'length':
                    self.error(E_MEMBER, toks[k], 'an array has no member %s' % name)
                    w.ok = False
                    return w
                w.segs.append(Segment(SEG_FIELD, 0, cls, 'length', 'I'))
                w.type = 'I'
            else:
                hit = None
                if cls is not None:
                    hit = classes.find_field(self.scope, cls, name)
                if hit is None:
                    self.error(E_MEMBER, toks[k], 'no field %s in %s' % (
                        name, cls if cls else 'a value of type %s' % w.type))
                    w.ok = False
                    return w
                f = hit.field
                w.segs.append(Segment(SEG_FIELD, 0, hit.owner if f.is_static else cls,
                                      f.name, f.desc))
                w.type = f.desc
            w.cls = None
            k += 1
        return w

    def put_segment(self, seg, tok):
        if seg.kind == SEG_LOCAL:
            self.put_imm(OP_LOCAL, seg.index, tok)
        elif seg.kind == SEG_FIELD:
            self.put_ref(OP_FIELD, seg.cls, seg.name, seg.desc, tok)
        else:
            self.put_class(OP_CLASS, seg.cls, tok)

    def name(self, e):
        """A name in value position (read or written alike):
        x        local i  /  field [C.x:T]
        a.b.c    path 3, head, field [..], field [..]"""
        toks = name_toks(e)
        w = self.walk(toks, len(toks))
        if not w.ok:
            return
        if w.cls is not None:
            self.error(E_NAME, toks[0], 'the class name %s is used as a value'
                       % self.ast.text(toks[0]))
            return
        n = len(w.segs)
        if n == 1:
            self.put_segment(w.segs[0], toks[0])
            return
        self.put_imm(OP_PATH, n, toks[n - 1])
        k = 0
        while k < n:
            self.put_segment(w.segs[k], toks[k])
            k += 1

    def receiver_check(self, x):
        """A parenthesised name as the receiver of a member or a call: no
        shipped source has one.  It is compiled as any computed receiver."""
        if x.kind == PAREN and is_name(strip_parens(x)):
            self.unverified(x.tok0, 'parenthesised name as a receiver')

    # -- calls ---------------------------------------------------------------

    def call(self, e, top):
        """f(..)          args, count, call, method [ref]  |  path 1, name "f"
        a.b.f(..)         args, count, call, path n, segments, method [ref] | name "f"
        X.f(..)           args, count, E(X), call_on "f"       (X computed)
        super.f(..)       args, count, call_super "f"
        closed by expr call / stmt call.  The count and the closing nodes
        carry the line of the ")", the path the line of the callee name."""
        for a in e.items:
            self.expr(a, OP_EXPR)
        close = e.tok1
        argc = len(e.items)
        target = e.a
        name = self.ast.text(e.name_tok)
        self.put_imm(OP_PUSH_INT, argc, close)
        if target is None:
            self.put(OP_CALL, close)
            # One class file without source shows a local in the place of
            # the method (Texture.<init>(I)): the game's compiler seems to
            # look the callee up as a variable first.  No source has such a
            # clash; the call is compiled as a call.
            if self.lookup_local(name) != NONE:
                self.unverified(e.name_tok, 'call of %s while a local variable of that name '
                                            'is in scope' % name)
            elif classes.find_field(self.scope, self.cls, name) is not None:
                self.unverified(e.name_tok, 'call of %s while the class has a field of that '
                                            'name' % name)
            self.callee(self.cls, e, True)
        elif target.kind == SUPER:
            self.put_utf8(OP_CALL_SUPER, name, close)
        elif is_name(target):
            self.put(OP_CALL, close)
            toks = name_toks(target)
            w = self.walk(toks, len(toks))
            if w.ok:
                n = len(w.segs)
                self.put_imm(OP_PATH, n + 1, e.name_tok)
                k = 0
                while k < n:
                    self.put_segment(w.segs[k], toks[k])
                    k += 1
                cls = w.cls
                if cls is None:
                    cls = classes.class_of_desc(w.type)
                self.callee(cls, e, False)
        else:
            # the receiver is computed behind the arguments and the count
            self.receiver_check(target)
            self.expr(target, OP_EXPR)
            self.put_utf8(OP_CALL_ON, name, close)
        self.put_imm(top, X_CALL, close)

    def callee(self, cls, e, lone):
        """The last segment of a call path: a method ref when every argument
        has a typed form and the lookup on the receiver class cls finds a
        method, else the name (behind a `path 1` header for a call without
        receiver)."""
        name = self.ast.text(e.name_tok)
        argc = len(e.items)
        known = cls is not None and cls[:1] != '['
        typed = True
        unknown_form = False
        types = []
        for a in e.items:
            if self.is_typed(a):
                types.append(self.arg_type(a))
            else:
                typed = False
            if self.form_unknown(a):
                unknown_form = True
        if unknown_form and known and classes.has_method(self.scope, cls, name, argc):
            self.unverified(e.name_tok, 'argument true, false, (x), !x, ~x, ?:, instanceof or '
                                        'new T[n] in a call the receiver class has a method for')
        if typed and known:
            hit = classes.lookup_method(self.scope, cls, name, types)
            if hit.method is not None:
                if hit.tie:
                    # which of them the game's lookup meets first is not known
                    self.unverified(e.name_tok, 'two methods %s fit the arguments equally '
                                                'well' % name)
                self.put_ref(OP_METHOD, cls, name, hit.method.desc, e.name_tok)
                return
        if lone:
            self.put_imm(OP_PATH, 1, e.name_tok)
        self.put_utf8(OP_NAME, name, e.name_tok)

    def is_typed(self, a):
        """Does the compiler know the type of an argument of this form?  Only
        for a literal, a local, a field written as a simple name and this; a
        parenthesised one does not count."""
        k = a.kind
        return k == LITERAL or k == NAME or k == THIS

    def arg_type(self, a):
        """Type of an argument of a typed form: a descriptor, NULL_TYPE, or
        None when the name is no variable."""
        k = a.kind
        if k == LITERAL:
            t = self.toks[a.tok0].kind
            if t == T_FLOAT:
                return 'F'
            if t == T_STR:
                return classes.STRING_DESC
            if t == KW_NULL:
                return classes.NULL_TYPE
            return 'I'                          # int, resource id, true, false
        if k == THIS:
            return self.this_desc
        name = self.ast.text(a.tok0)
        local = self.lookup_local(name)
        if local != NONE:
            return self.local_types[local]
        hit = classes.find_field(self.scope, self.cls, name)
        if hit is not None:
            return hit.field.desc
        return None

    def form_unknown(self, a):
        """Argument forms no shipped source has in a call that could be
        resolved (expressions.md 6.2): whether the game's compiler knows
        their type is not known.  true and false are taken as typed (int),
        the others as untyped."""
        k = a.kind
        if k == LITERAL:
            t = self.toks[a.tok0].kind
            return t == KW_TRUE or t == KW_FALSE
        if k == PAREN:
            x = strip_parens(a)
            return x.kind == LITERAL or x.kind == NAME or x.kind == THIS
        if k == UNARY:
            t = self.toks[a.op_tok].kind
            return t == P_NOT or t == P_BITNOT
        return k == COND or k == INSTANCEOF or k == NEW_ARRAY


def emit_class(scope, build, packs, float_mode):
    """The trees of one class, in tree order: one Tree per tree owner of
    build.rows (a field with an initialiser, a method that is not abstract)."""
    em = Emitter(scope, build.info, packs, float_mode)
    trees = []
    for owner in build.rows.owners:
        if owner.is_field:
            trees.append(em.field_tree(owner.node, owner.is_static))
        else:
            trees.append(em.method_tree(owner.node))
    return trees
