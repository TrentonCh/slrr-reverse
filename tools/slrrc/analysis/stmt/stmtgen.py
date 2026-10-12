#!/usr/bin/env python3
"""stmtgen.py - the statement compiler of the SLRR script dialect, as a model.

Given the syntax tree of a method (sparse.py) this module EMITS the node
sequence the game's compiler writes into the TREE chunk.  Everything that
belongs to statements, locals and control flow is predicted from the rules
written down in notes/statements.md:

    which nodes a statement form produces and in which order,
    every jump offset,
    every local index of declare / release / local,
    the implicit super call, the implicit return, the epilogue,
    the source line of every node (the u16 the old documents call "slot").

What belongs to the EXPRESSION analyst (symbol resolution, pool indices) is
taken from the class file being checked, through a narrow "oracle" window:

    * whether a name compiles to `local`, `field`, or `path n` + segments,
      and the pool index of every segment;
    * whether a call path is a lone `method` node or `path n` + segments;
    * the pool index of string / type / class / resource-id operands and the
      bit pattern of a float literal.

The oracle is only ever asked about the node at the position the emitter has
reached, and only inside those constructs; the emitter still checks what it
is given (a local index must be the one the scope model predicts, a member
name must be the identifier of the source, a descriptor must fit the declared
type).  A tree that the emitter reproduces node for node therefore proves the
statement rules for every statement in it.

    gen = Gen(unit, blob, actual_nodes, cfg)
    gen.method(member)        or      gen.field_init(decl)
    gen.finish()              # resolve labels
    gen.out                   # [G]  predicted nodes

Raises Mismatch when the oracle window shows something no rule allows.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for _p in (_LIB, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import srclex  # noqa: E402
import tufa    # noqa: E402

# opcodes (16-bit codes of tufa.OP_FLAGS)
LOCAL, DECLARE, RELEASE = 0x1001, 0x1002, 0x1003
AND_TEST, OR_TEST = 0x0004, 0x0005
TYPE, EXPR, STMT = 0x1006, 0x1007, 0x1008
STR, FLOAT, INT, NULL, BOOL, RID = 0x4009, 0x200a, 0x100b, 0x000c, 0x100d, 0x100e
END, PATH, NAME = 0x0010, 0x1011, 0x4012
JUMP, JUMP_FALSE, JUMP_TRUE, CASE, POP = 0x1014, 0x1015, 0x1016, 0x1017, 0x0018
CLASS, METHOD, FIELD, GETFIELD = 0x1019, 0x101a, 0x101b, 0x401c
INDEX, NEW_INIT, THIS_INIT, SUPER_INIT, CALL = 0x0020, 0x0021, 0x0022, 0x0023, 0x0024
CALL_ON, CALL_SUPER, PUSH_INT = 0x4025, 0x4026, 0x1027
VALUE, PUSH_VOID, RET, RET_VALUE, INIT, STORE = 0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d

RAW_OF = {}
for _raw, _code in enumerate(tufa.OP_FLAGS):
    if _code and _code not in RAW_OF:
        RAW_OF[_code] = _raw

T_COND, T_LOR, T_LAND = 0x00, 0x01, 0x02
T_CALL, T_LITERAL, T_NEW, T_NEWARRAY, T_INSTANCEOF, T_CAST, T_CTOR = \
    0x1B, 0x1C, 0x1E, 0x1F, 0x20, 0x21, 0x22
BIN_TAG = {'|': 0x03, '^': 0x04, '&': 0x05, '!=': 0x06, '==': 0x07, '>=': 0x08,
           '<=': 0x09, '>': 0x0A, '<': 0x0B, '>>>': 0x0C, '>>': 0x0D, '<<': 0x0E,
           '-': 0x0F, '+': 0x10, '%': 0x11, '/': 0x12, '*': 0x13}
ASSIGN_TAG = {'=': 0x23, '*=': 0x24, '/=': 0x25, '%=': 0x26, '+=': 0x27,
              '-=': 0x28, '<<=': 0x29, '>>=': 0x2A, '>>>=': 0x2B, '&=': 0x2C,
              '^=': 0x2D, '|=': 0x2E}
UN_TAG = {'~': 0x14, '!': 0x15, '-': 0x16, '++': 0x19, '--': 0x1A}
POST_TAG = {'++': 0x17, '--': 0x18}


class Mismatch(Exception):
    def __init__(self, pos, why):
        Exception.__init__(self, 'node %d: %s' % (pos, why))
        self.pos = pos
        self.why = why


class Label(object):
    __slots__ = ('pos',)

    def __init__(self):
        self.pos = None


class G(object):
    """One predicted node."""
    __slots__ = ('code', 'imm', 'src', 'cat', 'lt', 'ln', 'label', 'alts')

    def __init__(self, code, imm, src, cat, lt, ln, alts=None):
        self.code = code      # 16-bit opcode
        self.imm = imm        # immediate (None on 3-byte nodes)
        self.src = src        # 'rule' = predicted, 'oracle' = copied
        self.cat = cat        # category label for the statistics
        self.lt = lt          # token index whose line is the predicted line
        self.ln = ln          # predicted line (filled from lt)
        self.label = None     # jump target Label
        self.alts = alts      # ((label, line), ...) rival line rules


class Cfg(object):
    """Switches for the hypotheses that were tried while the rules were being
    established.  The defaults are the rules that hold (statements.md)."""
    pass


class Gen(object):
    def __init__(self, unit, blob, actual, cfg=None):
        self.toks = unit.toks
        self.blob = blob
        # free mode (actual is None): no class to look at.  Names and call
        # paths are emitted as place holders (one node per identifier); the
        # statement skeleton and every line are still predicted.  Used by
        # line_risk.py on the sources that have no class.
        self.free = actual is None
        self.act = actual or []
        self.acode = [tufa.OP_FLAGS[n.op] for n in self.act]
        self.out = []
        self.locals = []          # names; index = local slot
        self.max_locals = 0
        self.nparam_slots = 0     # this + parameters
        self.nparams = 0          # parameters only
        self.used_index = set()   # indices handed out by declare so far
        self.spans = []           # (form, statement N, first node, end node)
        self.flow = []            # enclosing loops / switches
        self.is_ctor = False
        self.is_static = False
        self.ret_void = True
        self.cfg = cfg or Cfg()
        self.notes = []           # soft findings (cat, text)
        self.stats = {}           # statement form -> count (this tree)

    # ---- low level ------------------------------------------------------
    def line_of(self, ti):
        if ti is None:
            return None
        if ti >= len(self.toks):
            ti = len(self.toks) - 1
        if ti < 0:
            ti = 0
        return self.toks[ti].line

    def emit(self, code, imm=None, cat='', lt=None, src='rule', alts=None):
        if alts:
            alts = tuple((lab, self.line_of(t)) for lab, t in alts)
        g = G(code, imm, src, cat, lt, self.line_of(lt), alts)
        self.out.append(g)
        return g

    def count(self, form, k=1):
        self.stats[form] = self.stats.get(form, 0) + k

    def pos(self):
        return len(self.out)

    def peek(self, k=0):
        i = len(self.out) + k
        if i < len(self.act):
            return self.act[i]
        return None

    def peek_code(self, k=0):
        i = len(self.out) + k
        if i < len(self.acode):
            return self.acode[i]
        return None

    def oracle(self, code, cat, lt, alts=None):
        """Copy the immediate of the actual node at the cursor, which must
        have opcode `code`."""
        if self.free:
            return self.emit(code, None, cat, lt, src='oracle', alts=alts)
        a = self.peek()
        if a is None or tufa.OP_FLAGS[a.op] != code:
            raise Mismatch(self.pos(), '%s: expected opcode 0x%04X, class has %s'
                           % (cat, code, 'end of tree' if a is None else
                              '0x%04X' % tufa.OP_FLAGS[a.op]))
        return self.emit(code, a.imm, cat, lt, src='oracle', alts=alts)

    def jump(self, code, label, cat, lt, alts=None):
        g = self.emit(code, None, cat, lt, alts=alts)
        g.label = label
        return g

    def place(self, label):
        label.pos = len(self.out)

    def finish(self):
        for i, g in enumerate(self.out):
            if g.label is not None:
                if g.label.pos is None:
                    raise Mismatch(i, 'label never placed (%s)' % g.cat)
                g.imm = (g.label.pos - i) & 0xFFFFFFFF

    def note(self, cat, text):
        self.notes.append((cat, self.pos(), text))

    def text(self, ti):
        return self.toks[ti].text

    # ---- locals ---------------------------------------------------------
    def declare_local(self, name):
        self.locals.append(name)
        if len(self.locals) > self.max_locals:
            self.max_locals = len(self.locals)
        return len(self.locals) - 1

    def count_local(self, idx):
        """Statistics for a `local idx` node the scope model predicted."""
        base = 0 if self.is_static else 1
        if idx < base:
            self.count('local node: this')
        elif idx < self.nparam_slots:
            self.count('local node: parameter')
            # the index this parameter would have if parameters were numbered
            # in source order
            pos_rev = idx - base                       # 0 = last parameter
            fwd = base + (self.nparams - 1 - pos_rev)
            if fwd != idx:
                self.count('local node: parameter whose index differs under forward numbering')
        else:
            self.count('local node: declared local')

    def lookup_local(self, name):
        for i in range(len(self.locals) - 1, -1, -1):
            if self.locals[i] == name:
                return i
        return None

    def releases(self, down_to, cat, lt, pop, alts=None):
        """release nodes for the locals above `down_to`, highest first."""
        for i in range(len(self.locals) - 1, down_to - 1, -1):
            self.emit(RELEASE, i, cat, lt, alts=alts)
        if pop:
            del self.locals[down_to:]

    # ---- types ----------------------------------------------------------
    def check_type(self, desc, typ, extra_dims=0, what='type'):
        """Does descriptor `desc` (pool text) fit the source type?"""
        dims = typ.dims + extra_dims
        base = typ.parts
        want_prefix = '[' * dims
        if not desc.startswith(want_prefix) or desc[dims:dims + 1] == '[':
            self.note('typecheck', '%s: descriptor %r does not have %d dimension(s)'
                      % (what, desc, dims))
            return
        core = desc[dims:]
        if base == ['int']:
            ok = core == 'I'
        elif base == ['float']:
            ok = core == 'F'
        elif base == ['void']:
            ok = core == 'V'
        else:
            dotted = '.'.join(base)
            ok = core.startswith('L') and core.endswith(';') and (
                core[1:-1] == dotted or core[1:-1].endswith('.' + dotted))
        if not ok:
            self.note('typecheck', '%s: descriptor %r does not fit source type %s%s'
                      % (what, desc, '.'.join(base), '[]' * dims))

    def type_carrier(self, typ, cat, lt, extra_dims=0, alts=None):
        g = self.oracle(TYPE, cat, lt, alts)
        if self.free:
            return g
        try:
            self.check_type(self.blob.utf8(g.imm), typ, extra_dims, cat)
        except tufa.TufaError as x:
            self.note('typecheck', str(x))
        return g

    # ---- expressions ----------------------------------------------------
    # Line rule of an expression node (statements.md, "lines"): the node is
    # built when the grammar rule of its construct is reduced, and carries the
    # line the scanner has reached then.  Constructs that end in a closing
    # token (a literal, `)` of a call, `]` of an index, the member name of
    # `x.name`, `++` behind an operand) are reduced without lookahead: line
    # of their LAST token.  Constructs that end in an operand (unary and
    # binary operators, casts, assignments, ?:, instanceof, new T[n]) are
    # reduced after the parser has read the token behind the operand: line of
    # the NEXT token.
    def LAST(self, e):
        return e.t1

    def NEXT(self, e):
        return e.t1 + 1

    def a_last(self, e):
        """Rival rules for a node whose rule says NEXT."""
        return (('last', e.t1), ('first', e.t0))

    def a_next(self, e):
        """Rival rules for a node whose rule says LAST."""
        return (('next', e.t1 + 1), ('first', e.t0))

    def expr(self, e, stmt=False):
        """Emit expression e.  stmt=True: e is an expression statement (or a
        for-init / for-update element), its top node is `stmt` not `expr`."""
        k = e.k
        top = STMT if stmt else EXPR
        if k == 'paren':
            return self.expr(e.e, stmt)
        if k == 'lit':
            if stmt:
                self.note('odd', 'literal as a statement')
            self.emit(top, T_LITERAL, 'lit', e.t1, alts=self.a_next(e))
            lk = e.lk
            tx = self.text(e.tok)
            al = self.a_next(e)
            if lk == 'int':
                self.emit(INT, srclex.int_value(tx), 'lit.int', e.t1, alts=al)
            elif lk == 'float':
                self.oracle(FLOAT, 'lit.float', e.t1, al)
            elif lk == 'str':
                g = self.oracle(STR, 'lit.str', e.t1, al)
                if not self.free and self.blob.utf8(g.imm) != srclex.string_value(tx):
                    self.note('litcheck', 'string %r is not the source literal %s'
                              % (self.blob.utf8(g.imm), tx))
            elif lk == 'rid':
                self.oracle(RID, 'lit.rid', e.t1, al)
            elif lk == 'null':
                self.emit(NULL, None, 'lit.null', e.t1, alts=al)
            elif lk == 'bool':
                self.emit(BOOL, 1 if tx == 'true' else 0, 'lit.bool', e.t1, alts=al)
            else:
                raise Mismatch(self.pos(), 'literal kind %s' % lk)
            return
        if k == 'name':
            if stmt:
                self.note('odd', 'name as a statement')
            return self.name(e)
        if k == 'this':
            if self.is_static:
                self.note('odd', 'this in a static method')
            self.emit(LOCAL, 0, 'this', e.t1, alts=self.a_next(e))
            return
        if k == 'field':
            self.expr(e.obj)
            g = self.oracle(GETFIELD, 'getfield', e.t1, self.a_next(e))
            if not self.free and self.blob.utf8(g.imm) != self.text(e.name_tok):
                self.note('namecheck', 'getfield %r for source name %s'
                          % (self.blob.utf8(g.imm), self.text(e.name_tok)))
            return
        if k == 'call':
            al = self.a_next(e)
            for a in e.args:
                self.expr(a)
            self.emit(PUSH_INT, len(e.args), 'call.argc', e.t1, alts=al)
            self.emit(CALL, None, 'call', e.t1, alts=al)
            self.call_path(e)
            self.emit(top, T_CALL, 'call.end', e.t1, alts=al)
            return
        if k == 'mcall':
            al = self.a_next(e)
            for a in e.args:
                self.expr(a)
            self.emit(PUSH_INT, len(e.args), 'mcall.argc', e.t1, alts=al)
            self.expr(e.obj)
            g = self.oracle(CALL_ON, 'mcall.call_on', e.t1, al)
            if not self.free and self.blob.utf8(g.imm) != self.text(e.name_tok):
                self.note('namecheck', 'call_on %r for source name %s'
                          % (self.blob.utf8(g.imm), self.text(e.name_tok)))
            self.emit(top, T_CALL, 'mcall.end', e.t1, alts=al)
            return
        if k == 'scall':
            al = self.a_next(e)
            for a in e.args:
                self.expr(a)
            self.emit(PUSH_INT, len(e.args), 'scall.argc', e.t1, alts=al)
            g = self.oracle(CALL_SUPER, 'scall.call_super', e.t1, al)
            if not self.free and self.blob.utf8(g.imm) != self.text(e.name_tok):
                self.note('namecheck', 'call_super %r for source name %s'
                          % (self.blob.utf8(g.imm), self.text(e.name_tok)))
            self.emit(top, T_CALL, 'scall.end', e.t1, alts=al)
            return
        if k == 'ctor':
            if not stmt:
                self.note('odd', 'this()/super() inside an expression')
            al = self.a_next(e)
            for a in e.args:
                self.expr(a)
            al = self.a_last(e) + (('after', e.t1 + 2),)
            self.emit(PUSH_INT, len(e.args), 'ctor.argc', e.t1 + 1, alts=al)
            self.emit(THIS_INIT if e.which == 'this' else SUPER_INIT, None,
                      'ctor.init', e.t1 + 1, alts=al)
            self.emit(STMT, T_CTOR, 'ctor.end', e.t1 + 1, alts=al)
            return
        if k == 'index':
            self.expr(e.idx)
            self.expr(e.arr)
            self.emit(INDEX, None, 'index', e.t1, alts=self.a_next(e))
            return
        if k == 'new':
            al = self.a_next(e) + (('type', e.type.t1), ('lparen', e.type.t1 + 1))
            self.emit(top, T_NEW, 'new', e.t1, alts=al)
            # the type carrier is built when the class name is reduced: the
            # parser has read the `(` behind it by then
            self.type_carrier(e.type, 'new.type', e.type.t1 + 1,
                              alts=self.a_next(e) + (('type', e.type.t1), ('rparen', e.t1)))
            for a in e.args:
                self.expr(a)
            self.emit(PUSH_INT, len(e.args), 'new.argc', e.t1, alts=al)
            self.emit(NEW_INIT, None, 'new.init', e.t1, alts=al)
            return
        if k == 'newarr':
            if len(e.sizes) != 1:
                self.note('odd', 'new array with %d sizes' % len(e.sizes))
            for s in e.sizes:
                self.expr(s)
            self.emit(top, T_NEWARRAY, 'newarr', e.t1 + 1, alts=self.a_last(e))
            self.type_carrier(e.type, 'newarr.type', e.t1 + 1, alts=self.a_last(e))
            return
        if k == 'un':
            self.expr(e.e)
            if e.op == '+':
                return
            if e.op in ('++', '--'):
                self.emit(top, UN_TAG[e.op], 'preinc', e.t1 + 1, alts=self.a_last(e))
            else:
                if stmt:
                    self.note('odd', 'unary %s as a statement' % e.op)
                self.emit(top, UN_TAG[e.op], 'un', e.t1 + 1, alts=self.a_last(e))
            return
        if k == 'post':
            self.expr(e.e)
            self.emit(top, POST_TAG[e.op], 'postinc', e.t1, alts=self.a_next(e))
            return
        if k == 'cast':
            self.expr(e.e)
            al = self.a_last(e) + (('rparen', e.rparen),)
            self.emit(top, T_CAST, 'cast', e.t1 + 1, alts=al)
            c = self.peek_code()
            if self.free:
                self.oracle(TYPE, 'cast.type', e.t1 + 1, al)
            elif c == CLASS:
                g = self.oracle(CLASS, 'cast.class', e.t1 + 1, al)
                nm = self.blob.class_name(g.imm)
                dotted = '.'.join(e.type.parts)
                if not (nm == dotted or nm.endswith('.' + dotted)) or e.type.dims:
                    self.note('typecheck', 'cast class %r for source type %s'
                              % (nm, dotted))
                self.count('cast to a class (class carrier)')
            else:
                self.type_carrier(e.type, 'cast.type', e.t1 + 1, alts=al)
                self.count('cast to a primitive (type carrier)')
            return
        if k == 'bin':
            al = self.a_last(e) + (('op', e.op_tok), ('left', e.l.t1 + 1))
            if e.op in ('&&', '||'):
                self.expr(e.l)
                self.emit(AND_TEST if e.op == '&&' else OR_TEST, None,
                          'sc.test', e.t1 + 1, alts=al)
                lab = Label()
                self.jump(JUMP, lab, 'sc.jump', e.t1 + 1, alts=al)
                self.expr(e.r)
                self.emit(top, T_LAND if e.op == '&&' else T_LOR, 'sc.op',
                          e.t1 + 1, alts=al)
                self.place(lab)
                self.count('expr && / ||')
                return
            self.expr(e.l)
            self.expr(e.r)
            self.emit(top, BIN_TAG[e.op], 'bin', e.t1 + 1, alts=al)
            return
        if k == 'instanceof':
            self.expr(e.e)
            self.emit(top, T_INSTANCEOF, 'instanceof', e.t1 + 1, alts=self.a_last(e))
            self.type_carrier(e.type, 'instanceof.type', e.t1 + 1, alts=self.a_last(e))
            return
        if k == 'cond':
            self.expr(e.c)
            self.expr(e.a)
            self.expr(e.b)
            self.emit(top, T_COND, 'cond', e.t1 + 1, alts=self.a_last(e))
            self.count('expr ?:')
            return
        if k == 'assign':
            self.expr(e.r)
            self.expr(e.l)
            self.emit(top, ASSIGN_TAG[e.op], 'assign', e.t1 + 1,
                      alts=self.a_last(e) + (('op', e.op_tok),))
            return
        raise Mismatch(self.pos(), 'expression kind %s has no rule' % k)

    def _ref_name(self, imm):
        try:
            return self.blob.ref(imm)[1]
        except tufa.TufaError:
            return None

    def _segments(self, n, e, cat, call, extra_alts=()):
        """Copy n path segments from the class and check them against the
        identifiers of the source name e (a sparse 'name' N)."""
        parts = e.parts
        names = [self.text(t) for t in parts]
        peeked = []
        for j in range(n):
            peeked.append(self.peek_code(j))
        nmem = n - (1 if peeked and peeked[0] == CLASS else 0)
        # identifier token of each segment: the member segments are the last
        # identifiers of the name, a leading class segment is the identifier
        # in front of them
        seg_tok = []
        for j in range(n):
            k = len(parts) - n + j
            seg_tok.append(parts[k] if 0 <= k < len(parts) else parts[0])
        first = True
        segs = []
        for j in range(n):
            c = self.peek_code()
            last = j == n - 1
            lt = seg_tok[j]
            al = (('name_last', e.t1), ('name_next', e.t1 + 1),
                  ('name_first', e.t0)) + tuple(extra_alts)
            if c == LOCAL and first:
                a = self.peek()
                want = self.lookup_local(names[0])
                if want is None or want != a.imm:
                    raise Mismatch(self.pos(), '%s: path starts with local %d, '
                                   'scope model says %s is %s'
                                   % (cat, a.imm, names[0], want))
                self.emit(LOCAL, want, cat + '.seg.local', lt, alts=al)
                self.count_local(want)
                segs.append(('local', names[0]))
            elif c == FIELD:
                g = self.oracle(FIELD, cat + '.seg.field', lt, al)
                segs.append(('field', self._ref_name(g.imm)))
            elif c == CLASS and first and n >= 2:
                g = self.oracle(CLASS, cat + '.seg.class', lt, al)
                segs.append(('class', self.blob.class_name(g.imm)))
            elif c == NAME and (call or not first):
                g = self.oracle(NAME, cat + '.seg.name', lt, al)
                segs.append(('name', self.blob.utf8(g.imm)))
            elif c == METHOD and call and last:
                g = self.oracle(METHOD, cat + '.seg.method', lt, al)
                segs.append(('method', self._ref_name(g.imm)))
            else:
                raise Mismatch(self.pos(), '%s: segment %d of %d is %s'
                               % (cat, j, n, 'missing' if c is None else '0x%04X' % c))
            first = False
        # the member segments (all but a leading class) are the last
        # identifiers of the source name, one each
        mem = [s for s in segs if s[0] != 'class']
        tail = names[len(names) - len(mem):] if len(mem) <= len(names) else None
        if tail is None or [s[1] for s in mem] != tail:
            self.note('namecheck', '%s: segments %r for source name %s'
                      % (cat, segs, '.'.join(names)))
        elif segs and segs[0][0] == 'class':
            head = '.'.join(names[:len(names) - len(mem)])
            cn = segs[0][1]
            if not head or not (cn == head or cn.endswith('.' + head)):
                self.note('namecheck', '%s: class segment %r for source prefix %r'
                          % (cat, cn, head))
        elif len(mem) != len(names):
            self.note('namecheck', '%s: %d segments for source name %s'
                      % (cat, len(mem), '.'.join(names)))
        return segs

    def _free_segments(self, e, cat, extra_alts=()):
        """Free mode: one place holder per identifier of the name."""
        for t in e.parts:
            al = (('name_last', e.t1), ('name_next', e.t1 + 1),
                  ('name_first', e.t0)) + tuple(extra_alts)
            self.emit(FIELD, None, cat + '.seg.field', t, src='oracle', alts=al)

    def name(self, e):
        """A (dotted) name in value position."""
        names = [self.text(t) for t in e.parts]
        c = self.peek_code()
        lt = e.t1
        al = (('name_next', e.t1 + 1), ('name_first', e.t0))
        if self.free:
            if len(names) == 1:
                self.emit(LOCAL, None, 'name.local', lt, src='oracle', alts=al)
            else:
                self.emit(PATH, None, 'name.path', lt, src='oracle', alts=al)
                self._free_segments(e, 'name.path')
            return
        if c == LOCAL:
            a = self.peek()
            want = self.lookup_local(names[0])
            if len(names) != 1 or want is None or want != a.imm:
                raise Mismatch(self.pos(), 'name %s: class has local %d, scope '
                               'model says %s' % ('.'.join(names), a.imm, want))
            self.emit(LOCAL, want, 'name.local', lt, alts=al)
            self.count_local(want)
            return
        if c == FIELD:
            g = self.oracle(FIELD, 'name.field', lt, al)
            if len(names) != 1 or self._ref_name(g.imm) != names[0]:
                self.note('namecheck', 'field %r for source name %s'
                          % (self.blob.describe(g.imm), '.'.join(names)))
            if self.lookup_local(names[0]) is not None:
                self.note('namecheck', 'field node for %s although a local of '
                          'that name is live' % names[0])
            return
        if c == PATH:
            g = self.oracle(PATH, 'name.path', lt, al)
            self._segments(g.imm, e, 'name.path', call=False)
            return
        raise Mismatch(self.pos(), 'name %s: class has %s' % (
            '.'.join(names), 'end of tree' if c is None else '0x%04X' % c))

    def call_path(self, e):
        """The name path behind a `call` node; e is the sparse 'call' N."""
        nm = e.name
        c = self.peek_code()
        al = (('name_next', nm.t1 + 1), ('name_first', nm.t0), ('rparen', e.t1),
              ('call_next', e.t1 + 1))
        if self.free:
            if len(nm.parts) == 1:
                self.emit(METHOD, None, 'call.method', nm.t1, src='oracle', alts=al)
            else:
                self.emit(PATH, None, 'call.path', nm.t1, src='oracle', alts=al)
                self._free_segments(nm, 'call.path', (('rparen', e.t1),))
            return
        if c == METHOD:
            g = self.oracle(METHOD, 'call.method', nm.t1, al)
            if len(nm.parts) != 1 or self._ref_name(g.imm) != self.text(nm.parts[0]):
                self.note('namecheck', 'lone method ref %r for source call %s'
                          % (self.blob.describe(g.imm),
                             '.'.join(self.text(t) for t in nm.parts)))
            return
        if c == PATH:
            g = self.oracle(PATH, 'call.path', nm.t1, al)
            self._segments(g.imm, nm, 'call.path', call=True,
                           extra_alts=(('rparen', e.t1),))
            return
        raise Mismatch(self.pos(), 'call %s: class has %s after call' % (
            '.'.join(self.text(t) for t in nm.parts),
            'end of tree' if c is None else '0x%04X' % c))

    # ---- statements -----------------------------------------------------
    # Which token has the scanner reached when statement s is reduced?
    # The grammar is the LALR(1) grammar of the Java language specification
    # with its Statement / StatementNoShortIf pair.  `dual` is True where both
    # nonterminals are possible: in the then-branch of an `if`, and in the
    # tail (else-branch, loop body) of a statement that is itself in such a
    # place.  There the parser has to read one token past a finished
    # statement before it can tell which of the two it was (an `else` behind
    # it or not); everywhere else a statement that ends in `;` or `}` is
    # reduced at once.  An `if` without `else` always costs that token.
    def end_tok(self, s, dual):
        k = s.k
        if k == 'if':
            if s.els is None:
                return s.t1 + 1
            return self.end_tok(s.els, dual)
        if k in ('while', 'for'):
            return self.end_tok(s.body, dual)
        return s.t1 + 1 if dual else s.t1

    def end_tok_simple(self, s):
        """The rule without the dual context (rival, for the statistics)."""
        return self.end_tok(s, False)

    def stmts(self, lst):
        for s in lst:
            self.stmt(s, False)

    def stmt(self, s, dual=False):
        a = len(self.out)
        self._stmt(s, dual)
        k = s.k
        form = k
        if k == 'if':
            form = 'if-else' if s.els is not None else 'if'
        elif k == 'return':
            form = 'return expr' if s.e is not None else 'return'
        elif k == 'local':
            form = 'local'
        elif k == 'for':
            form = 'for'
        self.spans.append((form, s, a, len(self.out)))

    def _stmt(self, s, dual=False):
        k = s.k
        if k == 'block':
            mark = len(self.locals)
            self.stmts(s.stmts)
            self.count('block')
            if len(self.locals) > mark:
                self.count('block with locals')
            self.releases(mark, 'block.release', s.t1, pop=True,
                          alts=(('next', s.t1 + 1),))
            return
        if k == 'empty':
            self.count('empty statement')
            return
        if k == 'local':
            self.local_decl(s)
            if dual:
                self.note('odd', 'declaration as the body of an if')
            return
        if k == 'expr':
            self.expr(s.e, stmt=True)
            self.count('expression statement')
            return
        if k == 'if':
            end = self.end_tok(s, dual)
            al = (('simple', self.end_tok_simple(s)), ('last', s.t1), ('next', s.t1 + 1))
            self.expr(s.cond)
            l_else = Label()
            self.jump(JUMP_FALSE, l_else, 'if.jump_false', end, alts=al)
            self.stmt(s.then, True)
            if s.els is None:
                self.place(l_else)
                self.count('if')
            else:
                l_end = Label()
                self.jump(JUMP, l_end, 'if.jump_else', end, alts=al)
                self.place(l_else)
                self.stmt(s.els, dual)
                self.place(l_end)
                self.count('if-else')
            return
        if k == 'while':
            end = self.end_tok(s, dual)
            al = (('simple', self.end_tok_simple(s)), ('last', s.t1), ('next', s.t1 + 1))
            l_top, l_end = Label(), Label()
            self.place(l_top)
            self.expr(s.cond)
            self.jump(JUMP_FALSE, l_end, 'while.jump_false', end, alts=al)
            self.flow.append({'kind': 'loop', 'brk': l_end, 'cont': l_top,
                              'mark': len(self.locals)})
            self.stmt(s.body, dual)
            self.flow.pop()
            self.jump(JUMP, l_top, 'while.jump_back', end, alts=al)
            self.place(l_end)
            self.count('while')
            return
        if k == 'do':
            l_top, l_cont, l_end = Label(), Label(), Label()
            self.place(l_top)
            self.flow.append({'kind': 'loop', 'brk': l_end, 'cont': l_cont,
                              'mark': len(self.locals)})
            self.stmt(s.body, False)
            self.flow.pop()
            self.place(l_cont)
            self.expr(s.cond)
            self.jump(JUMP_TRUE, l_top, 'do.jump_true', s.t1,
                      alts=(('next', s.t1 + 1), ('rparen', s.rparen)))
            self.place(l_end)
            self.count('do-while')
            return
        if k == 'for':
            end = self.end_tok(s, dual)
            al = (('simple', self.end_tok_simple(s)), ('last', s.t1), ('next', s.t1 + 1))
            mark = len(self.locals)
            if s.init is None:
                self.count('for without init')
            elif isinstance(s.init, list):
                for x in s.init:
                    self.expr(x, stmt=True)
                self.count('for with expression init')
            else:
                self.local_decl(s.init)
                self.count('for with declaration init')
            l_top, l_cont, l_end = Label(), Label(), Label()
            self.place(l_top)
            if s.cond is not None:
                self.expr(s.cond)
                self.jump(JUMP_FALSE, l_end, 'for.jump_false', end, alts=al)
            else:
                self.count('for without condition')
            self.flow.append({'kind': 'loop', 'brk': l_end, 'cont': l_cont,
                              'mark': len(self.locals)})
            self.stmt(s.body, dual)
            self.flow.pop()
            self.place(l_cont)
            for x in s.update:
                self.expr(x, stmt=True)
            if not s.update:
                self.count('for without update')
            self.jump(JUMP, l_top, 'for.jump_back', end, alts=al)
            self.place(l_end)
            self.releases(mark, 'for.release', end, pop=True, alts=al)
            self.count('for')
            return
        if k == 'switch':
            self.switch(s)
            return
        if k == 'break' or k == 'continue':
            if not self.flow:
                raise Mismatch(self.pos(), '%s outside a loop or switch' % k)
            if k == 'break':
                f = self.flow[-1]
                lab = f['brk']
                self.count('break in a %s' % f['kind'])
            else:
                f = None
                for x in reversed(self.flow):
                    if x['kind'] == 'loop':
                        f = x
                        break
                if f is None:
                    raise Mismatch(self.pos(), 'continue outside a loop')
                lab = f['cont']
            al = (('next', s.t1 + 1), ('first', s.t0))
            if len(self.locals) > f['mark']:
                self.count('%s that releases locals' % k)
            self.releases(f['mark'], k + '.release', s.t1, pop=False, alts=al)
            self.jump(JUMP, lab, k + '.jump', s.t1, alts=al)
            self.count(k)
            return
        if k == 'return':
            self.ret(s.e, s.t1, 'return', alts=(('next', s.t1 + 1), ('first', s.t0)))
            self.count('return expr' if s.e is not None else 'return')
            return
        raise Mismatch(self.pos(), 'statement kind %s has no rule' % k)

    def local_decl(self, s):
        for d in s.decls:
            idx = len(self.locals)
            if idx in self.used_index:
                self.count('declare that reuses the index of a closed scope')
            self.used_index.add(idx)
            # all four nodes of a declarator are built when its name (with
            # any [] behind it) is reduced, i.e. when the parser has read the
            # token behind the name: `=`, `,` or `;`
            lt = d.name_tok + 1 + 2 * d.dims
            al = (('decl_next', d.t1 + 1), ('stmt_end', s.t1), ('name', d.name_tok),
                  ('first', s.t0))
            self.emit(DECLARE, idx, 'decl.declare', lt, alts=al)
            self.type_carrier(s.type, 'decl.type', lt, extra_dims=d.dims, alts=al)
            self.declare_local(d.name)
            if d.init is not None:
                self.emit(INIT, None, 'decl.init', lt, alts=al)
                self.expr(d.init)
                self.emit(STORE, None, 'decl.store', lt, alts=al)
                self.count('local with initialiser')
            else:
                self.count('local without initialiser')

    def ret(self, e, lt, cat, alts=None):
        if self.is_ctor:
            if e is not None:
                self.note('odd', 'return with a value in a constructor')
            self.emit(RET, None, cat + '.ret', lt, alts=alts)
            self.releases(0, cat + '.release', lt, pop=False, alts=alts)
            return
        if e is not None:
            self.expr(e)
            self.emit(VALUE, None, cat + '.value', lt, alts=alts)
        else:
            self.emit(PUSH_VOID, None, cat + '.push_void', lt, alts=alts)
        self.releases(0, cat + '.release', lt, pop=False, alts=alts)
        self.emit(RET_VALUE, None, cat + '.ret_value', lt, alts=alts)

    def switch(self, s):
        self.expr(s.subj)
        l_end = Label()
        labs = []
        l_default = None
        ncase = 0
        for g in s.groups:
            lab = Label()
            labs.append(lab)
            for c in g.labels:
                if c.k == 'case':
                    self.expr(c.e)
                    self.jump(CASE, lab, 'switch.case', s.t1,
                              alts=(('colon', c.t1), ('label_next', c.t1 + 1),
                                    ('next', s.t1 + 1)))
                    ncase += 1
                else:
                    l_default = lab
        al = (('next', s.t1 + 1), ('lbrace', s.lbrace))
        self.emit(POP, None, 'switch.pop', s.t1, alts=al)
        self.jump(JUMP, l_default if l_default is not None else l_end,
                  'switch.jump_default', s.t1, alts=al)
        self.flow.append({'kind': 'switch', 'brk': l_end, 'cont': None,
                          'mark': len(self.locals)})
        mark = len(self.locals)
        prev_ended = True
        for g, lab in zip(s.groups, labs):
            self.place(lab)
            if not g.stmts:
                self.count('case group without statements')
            for x in g.stmts:
                self.stmt(x, False)
            # a case group is a scope of its own: its locals are released
            # where the next label (or the closing brace) was read
            if len(self.locals) > mark:
                self.count('case group with locals')
            self.releases(mark, 'switch.group_release', g.t1 + 1, pop=True,
                          alts=(('last', g.t1), ('switch_end', s.t1)))
            last = g.stmts[-1] if g.stmts else None
            if g is not s.groups[-1] and (last is None or last.k not in
                                          ('break', 'return', 'continue')):
                self.count('case group that falls through')
        self.flow.pop()
        self.place(l_end)
        self.count('switch')
        self.count('case label', ncase)
        if len(s.groups) and all(len(g.labels) == 1 for g in s.groups):
            pass
        for g in s.groups:
            if len(g.labels) > 1:
                self.count('case group with several labels')
        if l_default is not None:
            self.count('default label')
            if s.groups and s.groups[-1].labels[-1].k != 'default':
                self.count('default label not last')
        else:
            self.count('switch without default')

    # ---- trees ----------------------------------------------------------
    def field_init(self, d):
        self.expr(d.init)
        self.emit(END, None, 'fieldinit.end', d.t1 + 1, alts=(('last', d.t1),))
        self.count('field initialiser')

    def method(self, m):
        """Emit the tree of method / constructor m (a sparse 'method' N)."""
        self.is_ctor = bool(m.ctor)
        self.is_static = 'static' in m.mods
        self.ret_void = m.ret is None or (m.ret.parts == ['void'] and not m.ret.dims)
        if m.body is None:
            self.count('body ";" (%s)' % ('native' if 'native' in m.mods else
                                         'constructor' if m.ctor else 'method'))
            return
        if 'native' in m.mods or 'abstract' in m.mods:
            self.note('odd', 'native/abstract method with a body')
        body = m.body
        if not self.is_static:
            self.declare_local('this')
        for p in reversed(m.params):
            self.declare_local(p.name)
        nparams = len(self.locals)
        self.nparam_slots = nparams
        self.nparams = len(m.params)
        if not body.stmts and not self.is_ctor:
            self.count('body "{}" of a method')
            self.locals = []
            return
        rb = body.t1
        first = body.stmts[0] if body.stmts else None
        if self.is_ctor:
            explicit = first is not None and first.k == 'expr' and first.e.k == 'ctor'
            if not explicit:
                # built when the parser has read the first token of the body
                # and knows that it is not this( / super(
                lt = body.t0 + 1
                al = (('lbrace', body.t0), ('first_stmt_end',
                      self.end_tok(first, False) if first is not None else rb))
                self.emit(PUSH_INT, 0, 'implicit_super.argc', lt, alts=al)
                self.emit(SUPER_INIT, None, 'implicit_super.init', lt, alts=al)
                self.emit(STMT, T_CTOR, 'implicit_super.end', lt, alts=al)
                self.count('implicit super()')
            else:
                self.count('explicit %s()' % first.e.which)
            if not body.stmts:
                self.count('body "{}" of a constructor')
        self.stmts(body.stmts)
        # the implicit return at the closing brace: constructors and void
        # methods only
        if self.is_ctor or self.ret_void:
            self.ret(None, rb, 'implicit_return', alts=(('next', rb + 1),))
            self.count('implicit return')
        else:
            self.count('value method: no implicit return')
            last = body.stmts[-1] if body.stmts else None
            if last is None or last.k != 'return':
                self.count('value method not ending in return')
        # end of the body block, then of the method
        self.releases(nparams, 'body.release', rb, pop=True, alts=(('next', rb + 1),))
        self.releases(0, 'method.release', rb, pop=True, alts=(('next', rb + 1),))
        self.count('constructor body' if self.is_ctor else 'method body')
