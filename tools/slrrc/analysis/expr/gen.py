#!/usr/bin/env python3
"""gen.py - a model of the compiler's expression emission, in symbolic form.

    import gen
    model = gen.Model(db)                      # classdb.DB
    for uc in model.compile_unit(unit):        # jparse unit
        uc.name                                # 'java.game.Trigger'
        uc.trees                               # [Tree] in source order, like TREE
        t.nodes                                # [(mnemonic, operand)]
        t.src                                  # per node: the AST node it comes from
        t.rules                                # [(rule key, AST node, first, end)]

A predicted node is a tuple (mnemonic, operand):

    ('local', 3) ('declare', 3) ('type', 'I') ('init', None) ('store', None)
    ('expr', 'add') ('stmt', 'assign')        tag names as in lib/opcodes.py
    ('str', text) ('int', value & 0xFFFFFFFF) ('float', token text)
    ('null', None) ('bool', 0|1) ('rid', (pack, local id))
    ('field', (class, name, desc)) ('method', (class, name, desc))
    ('name', utf8) ('class', dotted name) ('getfield', utf8)
    ('call', None) ('call_on', utf8) ('call_super', utf8) ('path', n)
    ('push_int', n) ('index', None) ('new_init', None) ('this_init', None)
    ('super_init', None) ('and_test', None) ('or_test', None)
    ('jump', +k)                               only the one behind and/or_test

Statement-level control nodes (jump, jump_false, jump_true, case, pop, value,
push_void, ret, ret_value, release, end) are NOT predicted: the comparison
filters them out of the class side (symbolic.filter_nodes).  Everything an
expression or a declaration contributes is predicted, with the local numbering.

THE RULES (each is counted by check_expr.py; see notes/expressions.md)

  literal            expr literal ; carrier
  local / parameter  local i            (i: this 0, parameters in reverse, then
                                         declared locals; innermost wins)
  this               local 0
  simple name        field ref          (a field of the class or a superclass)
  a.b.c              path n ; head ; field ref ...   head = local | field | class
  X.f (computed X)   <X> ; getfield "f"
  a[i]               <i> ; <a> ; index
  op x               <x> ; expr op      (unary + emits nothing)
  x op y             <x> ; <y> ; expr op
  x && y             <x> ; and_test ; jump +(len(y)+2) ; <y> ; expr land
  x || y             <x> ; or_test ; jump +(len(y)+2) ; <y> ; expr lor
  c ? a : b          <c> ; <a> ; <b> ; expr cond
  x = y, x op= y     <y> ; <x> ; expr|stmt assign-op
  x++ ++x x-- --x    <x> ; expr|stmt postinc ...
  (T) x              <x> ; expr cast ; class T | type desc
  x instanceof T     <x> ; expr instanceof ; type "LT;"
  new T(a, b)        expr new ; type "LT;" ; <a> ; <b> ; push_int 2 ; new_init
  new T[n]           <n> ; expr newarray ; type "[T"
  f(a, b)            <a> ; <b> ; push_int 2 ; call ; method ref
                                                   | path 1 ; name "f"
  p.q.f(a)           <a> ; push_int 1 ; call ; path n ; segments ; method ref | name "f"
  X.f(a) (computed)  <a> ; push_int 1 ; <X> ; call_on "f"
  super.f(a)         <a> ; push_int 1 ; call_super "f"
  ... followed by expr call (value used) or stmt call (statement)

A member ref names the class that DECLARES a static field, and the static type
of the receiver (the class itself for a simple name) for an instance field and
for every method.  A call ends in a method ref when the compiler knows the type
of every argument (literal, local, simple-name field, this: nothing else) and
its method lookup (Model.find_method, the VM's Class::getMethod) finds a
method; in every other case it ends in the bare name.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import srclex    # noqa: E402
import classdb   # noqa: E402

STRING = 'Ljava.lang.String;'
OBJECT = 'Ljava.lang.Object;'

BINOP = {'||': 'lor', '&&': 'land', '|': 'bor', '^': 'bxor', '&': 'band',
         '!=': 'ne', '==': 'eq', '>=': 'ge', '<=': 'le', '>': 'gt', '<': 'lt',
         '>>>': 'tag0c', '>>': 'shr', '<<': 'shl', '-': 'sub', '+': 'add',
         '%': 'mod', '/': 'div', '*': 'mul'}
ASSIGNOP = {'=': 'assign', '*=': 'mul_assign', '/=': 'div_assign',
            '%=': 'mod_assign', '+=': 'add_assign', '-=': 'sub_assign',
            '<<=': 'tag29', '>>=': 'tag2a', '>>>=': 'tag2b',
            '&=': 'and_assign', '^=': 'xor_assign', '|=': 'or_assign'}
UNOP = {'-': 'neg', '!': 'lnot', '~': 'bnot'}
PREOP = {'++': 'preinc', '--': 'predec'}
POSTOP = {'++': 'postinc', '--': 'postdec'}


# Which token gives a node its line number.  The compiler stamps a node with
# the line the scanner had reached when the construct was reduced:
#   own    the node's own token (identifier, literal, closing bracket ...)
#   after  the token behind the own token
#   last   the last token of the construct
#   next   the token behind the construct (the parser's lookahead)
# Keys are roles (see Emitter.put); 'role: mnemonic' entries win.
LINE_RULE = {
    'literal': 'own',
    'simple name': 'own',
    'this': 'own',
    'field path header': 'own',
    'field path segment': 'own',
    'call path header': 'own',
    'call path segment': 'own',
    'callee': 'own',
    'call': 'own',                  # the closing parenthesis
    'getfield': 'own',
    'index': 'own',                 # the closing bracket
    'binary operator': 'next',
    'short-circuit test': 'next',
    'assignment': 'next',
    'prefix operator': 'next',
    'postfix operator': 'own',
    'conditional': 'next',
    'cast': 'next',
    'instanceof': 'next',
    'new': 'own',                   # push_int, new_init: the closing parenthesis
    'new: expr': 'last',            # ... and so is the node that opens it
    'new: stmt': 'last',
    'new: type': 'after',           # the token behind the type name
    'newarray': 'own',
    'declaration': 'own',           # the declared name
    'this()/super()': 'last',
    'implicit super()': 'after',    # the first token of the body
}


class Tree(object):
    __slots__ = ('owner', 'kind', 'nodes', 'src', 'anchor', 'lines', 'rules', 'line',
                 'decl', 'notes')

    def __init__(self, owner, kind, line, decl, em=None):
        self.owner = owner      # 'field NAME' / 'method NAME'
        self.kind = kind        # 'field' / 'method'
        self.line = line
        self.decl = decl
        self.nodes = em.out if em else []
        self.src = em.src if em else []
        self.anchor = em.anchor if em else []
        self.lines = em.lines if em else []
        self.rules = em.rules if em else []
        self.notes = em.notes if em else []


class UnitClass(object):
    __slots__ = ('name', 'trees', 'decl')

    def __init__(self, name, decl):
        self.name = name
        self.decl = decl
        self.trees = []


def strip_paren(e):
    while e.k == 'paren':
        e = e.x
    return e


def form_of(e):
    """Short name of the syntactic form of an expression (for the tables)."""
    p = '(paren) ' if e.k == 'paren' else ''
    e = strip_paren(e)
    k = e.k
    if k == 'lit':
        return p + 'literal'
    return p + k


class Emitter(object):
    """Emits the nodes of one method body or field initialiser."""

    def __init__(self, model, cls, resolver, static, toks=None):
        self.toks = toks                # the unit's tokens, for the line numbers
        self.lines = []                 # parallel to out: predicted source line
        self.m = model
        self.db = model.db
        self.opt = model.opt
        self.cls = cls                  # fully qualified name of the class
        self.r = resolver
        self.static = static
        self.out = []
        self.src = []                   # parallel to out: AST node that made it
        self.anchor = []                # parallel to out: (token index, role)
        self.rules = []                 # (key, AST node, first node, end node)
        self.locals = []                # [(name, desc)] index = slot
        self.scopes = []
        self.notes = []

    # ---- output ---------------------------------------------------------
    def put(self, mn, operand=None, e=None, tok=None, role=None):
        """Append a node.  e is the construct it belongs to, tok the index of
        the token the node stands for (identifier, literal, operator; None
        when it has none) and role a label for the line study."""
        self.out.append((mn, operand))
        self.src.append(e)
        self.anchor.append((tok, role))
        self.lines.append(self.line_of(mn, e, tok, role))

    def line_of(self, mn, e, tok, role):
        """The source line the compiler records in the node (LINE_RULE)."""
        toks = self.toks
        if toks is None or e is None or role is None:
            return None
        cand = LINE_RULE.get('%s: %s' % (role, mn)) or LINE_RULE[role]
        n = len(toks)
        if cand == 'own':
            i = tok
        elif cand == 'after':
            i = tok + 1
        elif cand == 'last':
            i = e.t1 - 1
        else:
            i = e.t1
        if i >= n:
            i = n - 1
        return toks[i].line

    def rule(self, key, e, start):
        self.rules.append((key, e, start, len(self.out)))

    # ---- locals -----------------------------------------------------------
    def declare(self, name, desc):
        self.locals.append((name, desc))
        return len(self.locals) - 1

    def lookup_local(self, name):
        for i in range(len(self.locals) - 1, -1, -1):
            if self.locals[i][0] == name:
                return i
        return None

    def open_scope(self):
        self.scopes.append(len(self.locals))

    def close_scope(self):
        del self.locals[self.scopes.pop():]

    # ---- types ------------------------------------------------------------
    @staticmethod
    def class_of_desc(desc):
        """Class name whose members a value of this type has (pool spelling):
        'Lpkg.Name;' -> 'pkg.Name', '[I' -> '[I', '[Lpkg.Name;' -> '[pkg.Name'."""
        if desc is None:
            return None
        if desc.startswith('L') and desc.endswith(';'):
            return desc[1:-1]
        if desc.startswith('['):
            el = desc[1:]
            if el.startswith('L') and el.endswith(';'):
                return '[' + el[1:-1]
            return '[' + el
        return None

    # ---- statements ---------------------------------------------------------
    def body(self, block, is_ctor):
        if is_ctor:
            first = block.stmts[0] if block.stmts else None
            if first is None or first.k != 'ctorcall':
                at = len(self.out)
                self.put('push_int', 0, block, tok=block.t0, role='implicit super()')
                self.put('super_init', None, block, tok=block.t0, role='implicit super()')
                self.put('stmt', 'ctor', block, tok=block.t0, role='implicit super()')
                self.rule('ctor.implicit super()', block, at)
        # the method body block shares the scope of the parameters
        for s in block.stmts:
            self.stmt(s)

    def stmt(self, s):
        k = s.k
        if k == 'block':
            self.open_scope()
            for x in s.stmts:
                self.stmt(x)
            self.close_scope()
        elif k == 'empty' or k == 'break' or k == 'continue':
            pass
        elif k == 'local':
            self.local_decl(s)
        elif k == 'expr':
            self.expr(s.e, value=False)
        elif k == 'if':
            self.expr(s.c)
            self.sub(s.a)
            if s.b is not None:
                self.sub(s.b)
        elif k == 'while':
            self.expr(s.c)
            self.sub(s.body)
        elif k == 'do':
            self.sub(s.body)
            self.expr(s.c)
        elif k == 'for':
            self.open_scope()
            for x in s.init:
                self.stmt(x)
            if s.cond is not None:
                self.expr(s.cond)
            self.sub(s.body)
            for x in s.update:
                self.stmt(x)
            self.close_scope()
        elif k == 'switch':
            self.expr(s.e)
            for it in s.items:
                if it.k == 'case':
                    self.expr(it.e)
            # every case section is a scope of its own: the locals declared
            # in it are released at the next label
            self.open_scope()
            for it in s.items:
                if it.k in ('case', 'default'):
                    self.close_scope()
                    self.open_scope()
                else:
                    self.stmt(it)
            self.close_scope()
        elif k == 'return':
            if s.e is not None:
                self.expr(s.e)
        elif k == 'ctorcall':
            at = len(self.out)
            for a in s.args:
                self.expr(a)
            self.put('push_int', len(s.args), s, role='this()/super()')
            self.put('this_init' if s.which == 'this' else 'super_init', None, s,
                     role='this()/super()')
            self.put('stmt', 'ctor', s, role='this()/super()')
            self.rule('ctor.%s(...)' % s.which, s, at)
        else:
            raise ValueError('statement kind %r' % k)

    def sub(self, s):
        """A statement that is the body of if / loop: its own scope."""
        if s.k == 'block':
            self.stmt(s)
        else:
            self.open_scope()
            self.stmt(s)
            self.close_scope()

    def local_decl(self, s):
        for d in s.decls:
            desc = self.r.descriptor(s.type, d.dims)
            if desc is None:
                desc = 'L?%s;' % s.type.name
                self.notes.append('unknown type %s' % s.type.name)
            at = len(self.out)
            idx = self.declare(d.name, desc)
            self.put('declare', idx, d, tok=d.t0, role='declaration')
            self.put('type', desc, d, tok=d.t0, role='declaration')
            if d.init is None:
                self.rule('decl.without initialiser', d, at)
            else:
                self.put('init', None, d, tok=d.t0, role='declaration')
                self.observe_store('initialiser', desc, self.expr(d.init), d, at)
                self.put('store', None, d, tok=d.t0, role='declaration')
                self.rule('decl.with initialiser', d, at)

    # ---- expressions --------------------------------------------------------
    def expr(self, e, value=True):
        """Emit e; value=False in statement position.  Returns the static
        type as a descriptor, 'null' or None (unknown)."""
        k = e.k
        ev = 'expr' if value else 'stmt'
        at = len(self.out)
        if k == 'paren':
            t = self.expr(e.x, value)
            self.rule('paren.(x) (no node)', e, at)
            return t
        if k == 'lit':
            self.put('expr', 'literal', e, tok=e.t0, role='literal')
            if e.kind == 'int':
                self.put('int', srclex.int_value(e.text), e, tok=e.t0, role='literal')
                self.rule('lit.int ' + ('hex' if e.text[:2] in ('0x', '0X') else 'decimal'), e, at)
                return 'I'
            if e.kind == 'float':
                self.put('float', e.text, e, tok=e.t0, role='literal')
                self.rule('lit.float' + (' with f suffix' if e.text[-1] in 'fF' else ''), e, at)
                return 'F'
            if e.kind == 'str':
                self.put('str', srclex.string_value(e.text), e, tok=e.t0, role='literal')
                self.rule('lit.string', e, at)
                return STRING
            if e.kind == 'rid':
                self.put('rid', srclex.rid_parts(e.text), e, tok=e.t0, role='literal')
                self.rule('lit.rid', e, at)
                return 'I'
            if e.kind == 'null':
                self.put('null', None, e, tok=e.t0, role='literal')
                self.rule('lit.null', e, at)
                return 'null'
            if e.kind == 'bool':
                self.put('bool', 1 if e.text == 'true' else 0, e, tok=e.t0, role='literal')
                self.rule('lit.' + e.text, e, at)
                return 'I'
            raise ValueError(e.kind)
        if k == 'this':
            self.put('local', 0, e, tok=e.t0, role='this')
            self.rule('name.this', e, at)
            return 'L%s;' % self.cls
        if k == 'name' or k == 'field':
            return self.access(e)
        if k == 'index':
            self.expr(e.idx)
            t = self.expr(e.arr)
            self.put('index', None, e, tok=e.t1 - 1, role='index')
            self.rule('index.array is ' + form_of(e.arr), e, at)
            if t and t.startswith('['):
                return t[1:]
            return None
        if k == 'call':
            return self.call(e, value)
        if k == 'new':
            desc = self.r.descriptor(e.type)
            if desc is None:
                desc = 'L?%s;' % e.type.name
                self.notes.append('unknown type %s' % e.type.name)
            self.put(ev, 'new', e, tok=e.type.t1 - 1, role='new')
            self.put('type', desc, e, tok=e.type.t1 - 1, role='new')
            for a in e.args:
                self.expr(a)
            self.put('push_int', len(e.args), e, tok=e.t1 - 1, role='new')
            self.put('new_init', None, e, tok=e.t1 - 1, role='new')
            self.rule('new.object (%s)' % ev, e, at)
            return desc
        if k == 'newarray':
            self.expr(e.size)
            desc = self.r.descriptor(e.type, 1 + e.extra)
            if desc is None:
                desc = '[L?%s;' % e.type.name
                self.notes.append('unknown type %s' % e.type.name)
            self.put(ev, 'newarray', e, tok=e.t1 - 1, role='newarray')
            self.put('type', desc, e, tok=e.t1 - 1, role='newarray')
            self.rule('new.array of %s' % ('int' if desc == '[I' else 'float' if desc == '[F'
                                           else 'objects'), e, at)
            return desc
        if k == 'unary':
            if e.op == '+':
                t = self.expr(e.x, value)
                self.rule('unary.+ (no node)', e, at)
                return t
            t = self.expr(e.x)
            if e.op in PREOP:
                self.put(ev, PREOP[e.op], e, tok=e.t0, role='prefix operator')
                self.rule('incdec.%s (%s)' % (PREOP[e.op], ev), e, at)
                return t
            self.put(ev, UNOP[e.op], e, tok=e.t0, role='prefix operator')
            self.rule('unary.%s' % UNOP[e.op], e, at)
            return 'I' if e.op in ('!', '~') else t
        if k == 'postfix':
            t = self.expr(e.x)
            self.put(ev, POSTOP[e.op], e, tok=e.t1 - 1, role='postfix operator')
            self.rule('incdec.%s (%s)' % (POSTOP[e.op], ev), e, at)
            return t
        if k == 'binary':
            if e.op in ('&&', '||'):
                self.expr(e.l)
                self.put('and_test' if e.op == '&&' else 'or_test', None, e, tok=e.optok,
                         role='short-circuit test')
                jat = len(self.out)
                self.put('jump', None, e, tok=e.optok, role='short-circuit test')
                self.expr(e.r)
                self.put(ev, BINOP[e.op], e, tok=e.optok, role='binary operator')
                self.out[jat] = ('jump', len(self.out) - jat)
                self.rule('shortcircuit.%s' % BINOP[e.op], e, at)
                return 'I'
            tl = self.expr(e.l)
            tr = self.expr(e.r)
            self.put(ev, BINOP[e.op], e, tok=e.optok, role='binary operator')
            self.rule('binary.%s' % BINOP[e.op], e, at)
            self.observe_binary(e, tl, tr, at)
            return self.binary_type(e.op, tl, tr)
        if k == 'assign':
            tr = self.expr(e.r)
            tl = self.expr(e.l)
            self.put(ev, ASSIGNOP[e.op], e, tok=e.optok, role='assignment')
            if e.op == '=':
                self.observe_store('assignment', tl, tr, e, at)
            self.rule('assign.%s (%s)' % (ASSIGNOP[e.op], ev), e, at)
            self.rule('assign target.%s' % self.target_form(e.l), e, at)
            return tl
        if k == 'cond':
            self.expr(e.c)
            ta = self.expr(e.a)
            tb = self.expr(e.b)
            self.put(ev, 'cond', e, tok=e.t1 - 1, role='conditional')
            self.rule('cond.?:', e, at)
            return ta if ta == tb else (ta or tb)
        if k == 'cast':
            self.expr(e.x)
            self.put(ev, 'cast', e, tok=e.type.t1, role='cast')
            if e.type.name in classdb.PRIMS or e.type.dims:
                desc = self.r.descriptor(e.type)
                self.put('type', desc, e, tok=e.type.t1, role='cast')
                self.rule('cast.to %s' % ('array' if e.type.dims else e.type.name), e, at)
                return desc
            full = self.r.resolve(e.type.name)
            if full is None:
                full = '?' + e.type.name
                self.notes.append('unknown type %s' % e.type.name)
            self.put('class', full, e, tok=e.type.t1, role='cast')
            self.rule('cast.to class', e, at)
            return 'L%s;' % full
        if k == 'instanceof':
            self.expr(e.x)
            desc = self.r.descriptor(e.type)
            if desc is None:
                desc = 'L?%s;' % e.type.name
                self.notes.append('unknown type %s' % e.type.name)
            self.put(ev, 'instanceof', e, tok=e.t1 - 1, role='instanceof')
            self.put('type', desc, e, tok=e.t1 - 1, role='instanceof')
            self.rule('instanceof.x instanceof T', e, at)
            return 'I'
        raise ValueError('expression kind %r' % k)

    @staticmethod
    def type_class(t):
        if t is None:
            return None
        if t == 'null':
            return 'null'
        if t == STRING:
            return 'String'
        if t in ('I', 'F'):
            return {'I': 'int', 'F': 'float'}[t]
        return 'array' if t.startswith('[') else 'object'

    def observe_binary(self, e, tl, tr, at):
        """Typed side counts: they show that operand types change nothing in
        what is emitted (no conversion node, no special compare or concat)."""
        a, b = self.type_class(tl), self.type_class(tr)
        if a is None or b is None:
            return
        pair = ' and '.join(sorted((a, b)))
        if e.op in ('==', '!='):
            self.rule('types.%s on %s' % (e.op, pair), e, at)
        elif e.op == '+' and 'String' in (a, b):
            self.rule('types.+ on %s (concatenation)' % pair, e, at)
        elif e.op in ('+', '-', '*', '/', '%', '<', '>', '<=', '>=') and \
                (a, b) in (('int', 'float'), ('float', 'int')):
            self.rule('types.arithmetic or comparison mixing int and float', e, at)

    def observe_store(self, what, tl, tr, e, at):
        a, b = self.type_class(tl), self.type_class(tr)
        if a is None or b is None or a == b or b == 'null':
            return
        if (a, b) in (('int', 'float'), ('float', 'int')):
            self.rule('types.%s of %s value to %s variable' % (what, b, a), e, at)
        elif a in ('int', 'float') or b in ('int', 'float'):
            self.rule('types.%s of %s value to %s variable' % (what, b, a), e, at)

    def target_form(self, l):
        l = strip_paren(l)
        if l.k == 'name':
            return 'local' if self.lookup_local(l.id) is not None else 'field (simple name)'
        if l.k == 'field':
            return 'path' if self.chain(l) is not None else 'getfield'
        return l.k

    def binary_type(self, op, tl, tr):
        if op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
            return 'I'
        if op == '+' and (tl == STRING or tr == STRING):
            return STRING
        if tl == 'F' or tr == 'F':
            return 'F'
        if tl == 'I' and tr == 'I':
            return 'I'
        return tl or tr

    # ---- names and member access --------------------------------------------
    @staticmethod
    def chain(e):
        """[id, id, ...] when e is a pure dotted name a.b.c, else None."""
        names = []
        x = e
        while x.k == 'field':
            names.append(x.name)
            x = x.obj
        if x.k != 'name':
            return None
        names.append(x.id)
        names.reverse()
        return names

    @staticmethod
    def chain_toks(e, nsegs):
        """Token index of the identifier behind each of the nsegs segments of
        the dotted name e (a package-qualified class name is one segment: its
        last identifier)."""
        toks = []
        x = e
        while x.k == 'field':
            toks.append(x.t1 - 1)
            x = x.obj
        toks.append(x.t0)
        toks.reverse()
        return toks[len(toks) - nsegs:]

    def head(self, names):
        """Resolve the start of a dotted name.  Returns (segments, type desc
        or None, class-context or None, names consumed, head kind).  The
        class-context is set while the walk stands on a class (static access).
        Order of the lookup: local, field of the class chain, class name."""
        n0 = names[0]
        order = self.opt.get('head_order', 'local,field,class').split(',')
        for what in order:
            if what == 'local':
                idx = self.lookup_local(n0)
                if idx is not None:
                    return [('local', idx)], self.locals[idx][1], None, 1, 'local'
            elif what == 'field':
                f = self.db.find_field(self.cls, n0)
                if f is not None:
                    return ([('field', (self.ref_class(self.cls, f), f.name, f.desc))],
                            f.desc, None, 1,
                            'static field' if f.static else 'instance field')
            else:
                # a class name, possibly package qualified (longest prefix first)
                for cut in range(len(names), 0, -1):
                    dotted = '.'.join(names[:cut])
                    full = self.r.resolve(dotted) if cut == 1 else (
                        dotted if dotted in self.db.classes else None)
                    if full is not None:
                        return [('class', full)], None, full, cut, 'class'
        return None, None, None, 0, None

    def ref_class(self, static_cls, member):
        """Which class a field ref names."""
        mode = self.opt.get('ref_class', 'mixed')
        if mode == 'owner':
            return member.owner
        if mode == 'static':
            return static_cls
        # a static field is named by the class that declares it, an instance
        # field by the static type of the receiver
        return member.owner if member.static else static_cls

    def walk(self, names, upto):
        """Segments for names[:upto] of a dotted name.  Returns (segments,
        type desc of the last one, class-context, description) or
        (None, None, None, None) when the head cannot be resolved."""
        segs, t, cctx, used, hk = self.head(names)
        if segs is None:
            return None, None, None, None
        what = [hk]
        for nm in names[used:upto]:
            cls = cctx if cctx is not None else self.class_of_desc(t)
            f = None
            if cls is not None:
                if cls.startswith('['):
                    if nm == 'length':
                        segs.append(('field', (cls, 'length', 'I')))
                        t, cctx = 'I', None
                        what.append('array length')
                        continue
                else:
                    f = self.db.find_field(cls, nm)
            if f is not None:
                segs.append(('field', (self.ref_class(cls, f), f.name, f.desc)))
                t = f.desc
                what.append('static field' if f.static else 'instance field')
                if f.owner != cls:
                    what.append('inherited ' + ('static' if f.static else 'instance'))
            else:
                segs.append(('name', nm))
                t = None
                what.append('unknown member')
            cctx = None
        return segs, t, cctx, what

    def access(self, e):
        """A name or a member access in value position."""
        at = len(self.out)
        if e.k == 'name':
            segs, t, cctx, used, hk = self.head([e.id])
            if segs is None or cctx is not None:
                self.notes.append('unresolved name %s' % e.id)
                self.put('unresolved', e.id, e)
                return None
            self.put(segs[0][0], segs[0][1], e, tok=e.t0, role='simple name')
            if hk == 'local':
                self.rule('name.local or parameter', e, at)
            elif hk == 'instance field':
                f = self.db.find_field(self.cls, e.id)
                self.rule('name.instance field (%s)' % (
                    'own class' if f.owner == self.cls else 'inherited'), e, at)
            else:
                self.rule('name.static field (%s)' % (
                    'own class' if segs[0][1][0] == self.cls else 'inherited'), e, at)
            if hk != 'local' and self.static:
                self.rule('name context.field by simple name inside a static method or '
                          'static initialiser', e, at)
            return t
        names = self.chain(e)
        if names is not None:
            segs, t, cctx, what = self.walk(names, len(names))
            if segs is not None and cctx is None:
                toks = self.chain_toks(e, len(segs))
                self.put('path', len(segs), e, tok=toks[0], role='field path header')
                for s, tk in zip(segs, toks):
                    self.put(s[0], s[1], e, tok=tk, role='field path segment')
                self.rule('path.%d segments, head %s' % (len(segs), what[0]), e, at)
                if 'array length' in what:
                    self.rule('path extra.ends in array length', e, at)
                if 'static field' in what[1:]:
                    self.rule('path extra.static field behind the head', e, at)
                if 'inherited instance' in what:
                    self.rule('path extra.instance field declared in a superclass of the '
                              'receiver type', e, at)
                if 'inherited static' in what:
                    self.rule('path extra.static field declared in a superclass of the '
                              'receiver type', e, at)
                if 'unknown member' in what:
                    self.notes.append('unknown member in path %s' % '.'.join(names))
                return t
            self.notes.append('unresolved path %s' % '.'.join(names))
            self.put('unresolved', '.'.join(names), e)
            return None
        # computed receiver
        t = self.expr(e.obj)
        self.put('getfield', e.name, e, tok=e.t1 - 1, role='getfield')
        self.rule('getfield.receiver is ' + form_of(e.obj), e, at)
        cls = self.class_of_desc(t)
        if cls is not None:
            if cls.startswith('[') and e.name == 'length':
                return 'I'
            f = self.db.find_field(cls, e.name)
            if f is not None:
                return f.desc
        return None

    # ---- calls ------------------------------------------------------------
    def call(self, e, value):
        ev = 'expr' if value else 'stmt'
        at = len(self.out)
        argt = [self.expr(a) for a in e.args]
        self.put('push_int', len(e.args), e, tok=e.t1 - 1, role='call')
        ret = None
        self.name_tok = e.t0 if e.obj is None else e.obj.t1 + 1
        if e.obj is None:
            self.put('call', None, e, tok=e.t1 - 1, role='call')
            ret, how = self.callee(self.cls, e.name, argt, e, lone=True)
            key = 'call.no receiver, %s' % how
            # does a variable of the same name get in the way?  (It does not
            # in any pair; Texture.class, without source, suggests a local does.)
            if self.lookup_local(e.name) is not None:
                self.rule('call name clash.a local of the same name is in scope', e, at)
            if self.db.find_field(self.cls, e.name) is not None:
                self.rule('call name clash.a field of the same name exists', e, at)
        elif e.obj.k == 'super':
            self.put('call_super', e.name, e, tok=e.t1 - 1, role='call')
            ci = self.db.get(self.cls)
            if ci is not None and ci.super:
                ret = self.ret_type(ci.super, e.name, argt)
            key = 'call.super.f()'
        else:
            names = self.chain(e.obj)
            segs = None
            if names is not None:
                segs, t, cctx, what = self.walk(names, len(names))
            if segs is not None:
                toks = self.chain_toks(e.obj, len(segs))
                self.put('call', None, e, tok=e.t1 - 1, role='call')
                self.put('path', len(segs) + 1, e, tok=toks[0], role='call path header')
                for s, tk in zip(segs, toks):
                    self.put(s[0], s[1], e, tok=tk, role='call path segment')
                cls = cctx if cctx is not None else self.class_of_desc(t)
                ret, how = self.callee(cls, e.name, argt, e, lone=False)
                if cls is not None and not cls.startswith('[') and \
                        self.db.find_field(cls, e.name) is not None:
                    self.rule('call name clash.the receiver class has a field of the same name',
                              e, at)
                key = 'call.path, head %s, %s' % (what[0], how)
                self.rule('call path length.%d segments' % (len(segs) + 1), e, at)
                if 'unknown member' in what:
                    self.notes.append('unknown member in call path %s' % '.'.join(names))
            else:
                if names is not None:
                    self.notes.append('unresolved call path %s' % '.'.join(names))
                    self.put('unresolved', '.'.join(names), e)
                    t = None
                else:
                    t = self.expr(e.obj)
                self.put('call_on', e.name, e, tok=e.t1 - 1, role='call')
                cls = self.class_of_desc(t)
                if cls is not None:
                    ret = self.ret_type(cls, e.name, argt)
                key = 'call.computed receiver: ' + form_of(e.obj)
        self.put(ev, 'call', e, tok=e.t1 - 1, role='call')
        self.rule(key, e, at)
        self.rule('call result.%s call' % ev, e, at)
        return ret

    def arg_kind(self, a):
        p = 'paren:' if a.k == 'paren' else ''
        a = strip_paren(a)
        k = a.k
        if k == 'lit':
            return p + 'lit-' + a.kind
        if k == 'name':
            if self.lookup_local(a.id) is not None:
                return p + 'local'
            return p + 'field1'
        if k == 'field':
            return p + ('path' if self.chain(a) is not None else 'getfield')
        if k in ('binary', 'unary', 'assign', 'postfix'):
            return p + k + a.op
        return p + k

    def ret_type(self, cls, name, argt):
        if cls is None or cls.startswith('['):
            return None
        ms = self.db.methods_named(cls, name)
        ms = [m for m in ms if len(classdb.split_desc(m.desc)[0]) == len(argt)]
        rets = set(classdb.split_desc(m.desc)[1] for m in ms)
        if len(rets) == 1:
            return rets.pop()
        return None

    TYPED = ('lit-', 'local', 'field1', 'this')

    def arg_typed(self, kind):
        """Does the compiler know the type of an argument of this form?"""
        extra = self.opt.get('typed_extra')
        if extra and kind.startswith(tuple(extra.split(','))):
            return True
        return kind.startswith(self.TYPED)

    def callee(self, cls, name, argt, e, lone):
        """The last segment of a call path.  With option callee=wild it is
        the wildcard ('callee?', info); otherwise the model's prediction: a
        method ref when the compile-time lookup finds a method, else the name
        (behind a `path 1` header when the call has no receiver).  Returns
        (return type, description)."""
        cands = []
        if cls is not None and not cls.startswith('['):
            cands = self.db.methods_named(cls, name)
        argk = tuple(self.arg_kind(a) for a in e.args)
        info = {'cls': cls, 'name': name, 'argt': tuple(argt), 'cands': cands,
                'lone': lone, 'self_static': self.static, 'self_cls': self.cls,
                'argk': argk}
        if self.opt.get('callee') == 'wild':
            self.put('callee?', info, e, tok=self.name_tok, role='callee')
            return self.ret_type(cls, name, argt), 'wildcard'
        m = None
        typed = all(self.arg_typed(k) for k in argk)
        score = None
        if cls is not None and not cls.startswith('[') and typed:
            m, score, tie = self.m.find_method_ex(cls, name, argt)
            if tie:
                self.rule('method ref tie.two overloads share the best score', e, len(self.out))
        info['found'] = m
        info['typed'] = typed
        if m is not None:
            at = len(self.out)
            self.put('method', (cls, name, m.desc), e, tok=self.name_tok, role='callee')
            self.src[-1] = (e, info)
            same = [c for c in cands if len(classdb.split_desc(c.desc)[0]) == len(argt)]
            descs = set(c.desc for c in same)
            self.rule('method ref.%s, %s' % (
                'the only method of that name and argument count' if len(descs) == 1
                else 'chosen among %s' % ('2 overloads' if len(descs) == 2 else '3 or more overloads'),
                'exact types' if score == 0 else 'with conversions'), e, at)
            self.rule('method ref kind.%s method, declared in %s' % (
                'static' if m.static else 'instance',
                'the receiver class' if m.owner == cls else 'a superclass'), e, at)
            if score:
                self.rule('method ref score.%d' % score, e, at)
            return classdb.split_desc(m.desc)[1] or None, 'method ref'
        if lone:
            self.put('path', 1, e, tok=self.name_tok, role='call path header')
        self.put('name', name, e, tok=self.name_tok, role='callee')
        self.src[-1] = (e, info)
        if not typed:
            why = 'an argument of unknown type'
        elif not cands:
            why = 'no method of that name'
        else:
            why = 'no method accepts the arguments'
        return self.ret_type(cls, name, argt), 'by name (%s)' % why


class Model(object):
    def __init__(self, db, **options):
        self.db = db
        self.opt = options
        self._rows = {}

    # ---- compile-time method lookup (Class::getMethod, 940 VA 0x54A150) ----
    def rows(self, ci):
        """The method rows of a class in the order the lookup scans them: the
        instance vector, then the static vector, each from its last row to its
        first (decompilation of 0x54A150; the corpus does not constrain it)."""
        r = self._rows.get(ci.name)
        if r is None:
            inst = [m for m in ci.methods if not m.static]
            stat = [m for m in ci.methods if m.static]
            first, second = (inst, stat)
            if self.opt.get('vec_order') == 'static_first':
                first, second = stat, inst
            r = list(reversed(first)) + list(reversed(second))
            if self.opt.get('row_order') == 'forward':
                r = list(first) + list(second)
            self._rows[ci.name] = r
        return r

    def distance(self, arg_cls, par_cls):
        """Superclass steps from arg_cls up to par_cls, -1 when unrelated.  An
        interface the class (or a superclass) implements counts as reachable."""
        d = 0
        for ci in self.db.chain(arg_cls):
            if ci.name == par_cls:
                return d
            if self.opt.get('ifaces', 'yes') == 'yes' and par_cls in ci.interfaces:
                return d + 1
            d += 1
        return -1

    def type_score(self, par, arg):
        """Score of passing a value of type arg for a parameter of type par
        (0x55BD80): 0 identical, -1 impossible, else a penalty."""
        if par == arg:
            return 0
        if arg is None:
            return -1
        p0 = par[0]
        # the type of null, and of a class name the compiler could not
        # resolve, is the descriptor "L;" (Thesaurus.class declares such a
        # parameter): it is accepted for every object or array parameter
        a0 = arg[0] if arg not in ('null', 'L;') else 'N'
        if p0 == 'F':
            return 101 if a0 == 'I' else -1
        if p0 == 'I':
            return 100 if a0 == 'F' else -1
        if p0 in 'L[':
            if a0 in 'IF':
                mode = self.opt.get('prim_to_obj', 'string')
                if mode == 'string':
                    return 100 if par == STRING else -1
                if mode == 'string_or_super':
                    return 100 if par in (STRING, OBJECT) else -1
                return -1
            if a0 == 'N':
                return 100
            if a0 in 'L[':
                if p0 == '[' or a0 == '[':
                    if p0 == 'L' and par == OBJECT and \
                            self.opt.get('array_to_object', 'yes') == 'yes':
                        return 1
                    return -1
                return self.distance(arg[1:-1], par[1:-1])
        return -1

    def find_method(self, cls, name, argt, with_score=False):
        """The method a call `name(argt)` on class cls resolves to at compile
        time, or None.  Walks the superclass chain; in each class the rows are
        scanned and scored; score 0 wins at once, otherwise the lowest score,
        the first one found on a tie."""
        m, sc, tie = self.find_method_ex(cls, name, argt)
        return (m, sc) if with_score else m

    def find_method_ex(self, cls, name, argt):
        """(method or None, score, True when another method reached the same
        best score)."""
        tie = False
        best = None
        best_score = 1000000
        n = len(argt)
        for ci in self.db.chain(cls):
            for m in self.rows(ci):
                if m.name != name:
                    continue
                ps = classdb.split_desc(m.desc)[0]
                if len(ps) != n:
                    continue
                score = 0
                for p, a in zip(ps, argt):
                    sc = self.type_score(p, a)
                    if sc < 0:
                        score = -1
                        break
                    score += sc
                if score < 0:
                    continue
                if self.opt.get('score_mode') == 'first':
                    return m, score, False      # control: no scoring at all
                if score == 0:
                    return m, 0, False
                if self.opt.get('score_mode') == 'exact_else_first':
                    if best is None:            # control: exact, else the first
                        best, best_score = m, score
                    continue
                if self.opt.get('score_mode') == 'exact_else_last':
                    best, best_score = m, score  # control: exact, else the last
                    continue
                if score < best_score:
                    best, best_score, tie = m, score, False
                elif score == best_score and m.desc != best.desc:
                    tie = True
        return best, (best_score if best is not None else None), tie

    # ---- one source file ----------------------------------------------------
    def compile_unit(self, unit):
        out = []
        r = classdb.Resolver(self.db, unit.package, unit.imports)
        for td in unit.types:
            full = (unit.package + '.' if unit.package else '') + td.name
            uc = UnitClass(full, td)
            for m in td.members:
                if m.k == 'fielddecl':
                    if m.init is None:
                        continue
                    static = 'static' in m.mods
                    em = Emitter(self, full, r, static, getattr(unit, 'toks', None))
                    if not static:
                        em.declare('this', 'L%s;' % full)
                    em.expr(m.init)
                    uc.trees.append(Tree('field %s' % m.name, 'field', m.line, m, em))
                else:
                    if m.body is None:
                        if 'abstract' in m.mods:
                            continue
                        uc.trees.append(Tree('method %s' % m.name, 'method', m.line, m))
                        continue
                    static = 'static' in m.mods
                    em = Emitter(self, full, r, static, getattr(unit, 'toks', None))
                    if not static:
                        em.declare('this', 'L%s;' % full)
                    for p in reversed(m.params):
                        d = r.descriptor(p.type, p.dims)
                        if d is None:
                            d = 'L?%s;' % p.type.name
                            em.notes.append('unknown type %s' % p.type.name)
                        em.declare(p.name, d)
                    em.body(m.body, m.name == '<init>')
                    uc.trees.append(Tree('method %s' % m.name, 'method', m.line, m, em))
            out.append(uc)
        return out
