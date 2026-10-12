#!/usr/bin/env python3
"""census.py - how often every construct of the dialect occurs.

Tokenizes and parses all 3,454 sources (lexer.py, parser.py) and counts
tokens, literal forms, declarations, statements and expressions, plus the
constructs a Java compiler would reject.  The numbers of notes/grammar.md come
from here (gen_grammar_md.py fills them in).

    python -I census.py [--json FILE] [--corpus DIR]

Output: a text report on stdout and, with --json, the machine-readable form
    {'counts': {key: n}, 'examples': {key: [text, ...]}, 'tables': {...}}
"""
import json
import os
import re
import sys
import time
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer    # noqa: E402
import oracle   # noqa: E402
import parser   # noqa: E402
import symbols  # noqa: E402

MAX_EXAMPLES = 3

JAVA_KEYWORDS_NOT_IN_DIALECT = frozenset('''
    boolean byte char short long double try catch finally throw throws
    volatile transient goto const strictfp assert enum
'''.split())


class Census(object):
    def __init__(self):
        self.c = Counter()
        self.ex = defaultdict(list)
        self.files = defaultdict(set)      # key -> set of files
        self.packs = Counter()             # pack name as written -> literals
        self.rel = None
        self.lines = None

    def add(self, key, line=None, n=1, text=None):
        self.c[key] += n
        self.files[key].add(self.rel)
        if line is None:
            return
        ex = self.ex[key]
        mine = self.rel + ':'
        if len(ex) >= MAX_EXAMPLES:
            # full: a new file replaces the last example of a file that
            # already has another one, so examples come from different files
            if any(x.startswith(mine) for x in ex):
                return
            dup = None
            for i in range(len(ex) - 1, 0, -1):
                f = ex[i].split(':', 1)[0] + ':'
                if any(y.startswith(f) for y in ex[:i]):
                    dup = i
                    break
            if dup is None:
                return
            del ex[dup]
        if text is None:
            text = self.lines[line - 1].strip()
        text = re.sub(r'\s+', ' ', text)
        if len(text) > 110:
            text = text[:107] + '...'
        ex.append('%s:%d: %s' % (self.rel, line, text))


def kind_of_target(e):
    """Name of the receiver form of a call, field access or index."""
    if e is None:
        return 'none'
    k = e.kind
    if k == 'Name':
        return 'name'
    if k == 'Field':
        return 'name chain' if is_chain(e) else 'field of ' + kind_of_target(e.target)
    if k == 'This':
        return 'this'
    if k == 'Super':
        return 'super'
    if k == 'Call':
        return 'call result'
    if k == 'Index':
        return 'array element'
    if k == 'Paren':
        inner = e.expr
        return 'parenthesised ' + ('cast' if inner.kind == 'Cast' else inner.kind.lower())
    if k == 'New':
        return 'new object'
    if k == 'Literal':
        return e.lit + ' literal'
    return k.lower()


def is_chain(e):
    while e.kind == 'Field':
        e = e.target
    return e.kind == 'Name'


def expr_class(e):
    """Coarse class of an expression, for conditions and statement kinds."""
    k = e.kind
    if k == 'Paren':
        return expr_class(e.expr)
    if k == 'Binary':
        if e.op in ('&&', '||'):
            return 'logical && ||'
        if e.op in ('==', '!=', '<', '>', '<=', '>='):
            return 'comparison'
        return 'arithmetic or bitwise operator'
    if k == 'Unary':
        return 'negation !' if e.op == '!' else 'unary ' + e.op
    if k in ('Name', 'Field'):
        return 'name or field'
    if k == 'Call':
        return 'call'
    if k == 'Index':
        return 'array element'
    if k == 'Literal':
        return e.lit + ' literal'
    if k == 'InstanceOf':
        return 'instanceof'
    if k == 'Assign':
        return 'assignment'
    return k.lower()


def type_class(t):
    if t.primitive:
        base = t.name
    elif '.' in t.name:
        base = 'dotted class name'
    else:
        base = 'class'
    return base + '[]' * t.dims


class Walker(object):
    """Counts the constructs of one compilation unit."""

    def __init__(self, cen, unit, resolver, universe, rel):
        self.cen = cen
        self.u = unit
        self.res = resolver
        self.uni = universe
        self.rel = rel
        self.toks = unit.tokens
        self.method = None
        self.scopes = []
        self.loops = []

    def add(self, key, node=None, text=None):
        self.cen.add(key, node.line if node is not None else None, text=text)

    # -- unit ------------------------------------------------------------------
    def unit(self):
        u = self.u
        a = self.add
        a('unit')
        if u.package:
            a('unit.package')
            self.cen.c['package ' + u.package] += 1
            if self.toks[0].text != 'package':
                a('unit.package not first', u)
        a('unit.types %d' % len(u.types))
        if len(u.types) > 1:
            a('unit.several top-level types', u.types[1])
        base = os.path.basename(self.rel)[:-5]
        if base not in [c.name for c in u.types]:
            a('unit.no type named like the file', u.types[0] if u.types else u)
        if u.types and u.types[0].name != base:
            a('unit.first type not named like the file', u.types[0])
        publics = [c for c in u.types if 'public' in c.modifiers]
        if len(publics) > 1:
            a('unit.several public types', publics[1])
        for c in publics:
            if c.name != base:
                a('unit.public type not named like the file', c)
        seen = set()
        for imp in u.imports:
            a('import')
            a('import.' + ('star' if imp.star else 'single'), imp)
            if imp.pack:
                a('import.colon', imp)
            key = (imp.pack, imp.name, imp.star)
            if key in seen:
                a('import.repeated', imp)
            seen.add(key)
            exists = imp.name in self.uni.packages
            self.cen.c['import %s%s%s' % ((imp.pack + ':') if imp.pack else '', imp.name,
                                          '.*' if imp.star else '')] += 1
            if not exists:
                a('import.of a package without classes', imp)
                if imp.name in self.uni.classes:
                    a('import.of the members of a class', imp)
            if imp.name == 'java.lang':
                a('import.java.lang explicit', imp)
            if imp.name == u.package:
                a('import.own package', imp)
        a('unit.imports %d' % len(u.imports))
        for cls in u.types:
            self.cls(cls)

    # -- classes ---------------------------------------------------------------
    def cls(self, cls):
        a = self.add
        a('class')
        a('class.' + ('interface' if cls.interface else 'class'), cls)
        a('class.modifiers [%s]' % ' '.join(cls.modifiers), cls)
        if cls.extends:
            a('class.extends', cls)
            if '.' in cls.extends:
                a('class.extends a dotted name', cls)
        else:
            a('class.no extends', cls)
        if cls.interface and (cls.extends or cls.implements):
            a('class.interface with extends or implements', cls)
        if cls.interface and cls.members:
            a('class.interface with members', cls)
        if cls.implements:
            a('class.implements', cls)
            a('class.implements %d interfaces' % len(cls.implements), cls)
        fields = set()
        methods = set()
        ctors = 0
        for m in cls.members:
            if m.kind == 'FieldDecl':
                self.field(cls, m)
                for d in m.declarators:
                    if d.name in fields:
                        a('odd.field declared twice in one class', d)
                    fields.add(d.name)
            elif m.kind == 'MethodDecl':
                sig = (m.name, self.res.method_descriptor(m).split(')')[0])
                if sig in methods:
                    a('odd.method declared twice with the same parameter types', m)
                methods.add(sig)
                if m.constructor:
                    ctors += 1
                self.method_decl(cls, m)
            else:
                a('member.initializer block', m)
        a('class.constructors %d' % min(ctors, 3) + ('+' if ctors >= 3 else ''))
        if not ctors and not cls.interface:
            a('class.without constructor', cls)

    def field(self, cls, m):
        a = self.add
        a('field.declaration', m)
        a('field.declarators per declaration %d' % len(m.declarators), m)
        a('field.modifiers [%s]' % ' '.join(m.modifiers), m)
        for mod in m.modifiers:
            a('field.modifier ' + mod)
        if cls.interface:
            a('field.in interface', m)
        for d in m.declarators:
            a('field')
            a('field.' + ('static' if 'static' in m.modifiers else 'instance'))
            a('field.type ' + type_class(m.type) + '[]' * d.dims, d)
            if d.dims:
                a('array.brackets after the field name', d)
            if m.type.dims:
                a('array.brackets after the field type', d)
            if d.dims and m.type.dims:
                a('array.brackets after both type and name', d)
            if d.init is not None:
                a('field.with initialiser', d)
                a('field.initialiser is ' + expr_class(d.init), d)
                self.method = None
                self.scopes = []
                self.expr(d.init, 'field initialiser')
            else:
                a('field.without initialiser')
            if 'final' in m.modifiers and d.init is None:
                a('odd.final field without initialiser', d)

    def method_decl(self, cls, m):
        a = self.add
        what = 'constructor' if m.constructor else 'method'
        a(what)
        a(what + '.modifiers [%s]' % ' '.join(m.modifiers), m)
        for mod in m.modifiers:
            a(what + '.modifier ' + mod)
        a(what + '.parameters %d' % len(m.params), m)
        if m.body is None:
            form = '";"'
        elif not m.body.stmts:
            form = '"{}"'
        else:
            form = '"{ ... }"'
        a(what + '.body ' + form, m)
        if 'abstract' in m.modifiers:
            a('method.abstract', m)
        if 'native' in m.modifiers:
            a('method.native', m)
            a('method.native ' + ('static' if 'static' in m.modifiers else 'instance'), m)
        if not m.constructor:
            a('method.return type ' + type_class(m.type), m)
            if m.name == cls.name:
                a('odd.method with a return type named like its class', m)
            if cls.interface:
                a('method.in interface', m)
        for p in m.params:
            a('param')
            a('param.type ' + type_class(p.type) + '[]' * p.dims, p)
            if p.dims:
                a('array.brackets after the parameter name', p)
            if p.type.dims:
                a('array.brackets after the parameter type', p)
            if p.modifiers:
                a('param.final', p)
        names = [p.name for p in m.params]
        if len(set(names)) != len(names):
            a('odd.two parameters with one name', m)
        if m.body is not None:
            self.method = m
            self.scopes = [dict((p.name, p) for p in m.params)]
            self.body(m)
            self.scopes = []
            self.method = None

    def body(self, m):
        a = self.add
        stmts = m.body.stmts
        first = stmts[0] if stmts else None
        explicit = first is not None and first.kind == 'ExprStmt' and \
            first.expr.kind == 'CtorCall'
        if m.constructor:
            if explicit:
                a('constructor.begins with %s(...)' % first.expr.which, first)
            else:
                a('constructor.without explicit this/super call (implicit super())', m)
        if stmts:
            last = stmts[-1]
            if last.kind == 'Return':
                a('body.ends with a return statement')
            else:
                a('body.does not end with a return statement')
                if not m.constructor and m.type.name != 'void':
                    a('odd.method with a result type whose body does not end with return', m)
        self.block_stmts(stmts, m.body, top=True)

    # -- scopes ----------------------------------------------------------------
    def declare(self, d):
        name = d.name
        if name in self.scopes[-1]:
            self.add('odd.local declared twice in one scope', d)
        else:
            for sc in self.scopes[:-1]:
                if name in sc:
                    self.add('odd.local hides an outer local or parameter', d)
                    break
        self.scopes[-1][name] = d

    # -- statements --------------------------------------------------------------
    def block_stmts(self, stmts, node, top=False):
        self.scopes.append({})
        dead = False
        for s in stmts:
            if dead:
                self.add('odd.statement after return / break / continue in one block', s)
                dead = False
            self.stmt(s)
            if s.kind in ('Return', 'Break', 'Continue'):
                dead = True
        self.scopes.pop()

    def stmt(self, s, sub=None):
        a = self.add
        k = s.kind
        if sub:
            a('substmt.%s body is %s' % (sub, 'a block' if k == 'Block' else
                                           'empty ";"' if k == 'Empty' else 'a single statement'),
              s)
        if k == 'Block':
            if sub is None:
                a('stmt.Block as a statement of its own', s)
            if not s.stmts:
                a('stmt.Block empty', s)
            self.block_stmts(s.stmts, s)
        elif k == 'Empty':
            a('stmt.empty statement', s)
        elif k == 'ExprStmt':
            e = s.expr
            ek = e.kind
            if ek == 'Assign':
                a('exprstmt.assignment ' + e.op)
            elif ek == 'Call':
                a('exprstmt.call')
            elif ek == 'CtorCall':
                a('exprstmt.%s(...)' % e.which)
                m = self.method
                if m is None or not m.constructor:
                    a('odd.this(...) or super(...) outside a constructor', s)
                elif m.body.stmts[0] is not s:
                    a('odd.this(...) or super(...) not as first statement', s)
            elif ek == 'Postfix':
                a('exprstmt.postfix ' + e.op)
            elif ek == 'Unary':
                a('exprstmt.prefix ' + e.op)
            elif ek == 'New':
                a('exprstmt.new object', s)
            else:
                a('exprstmt.other ' + ek, s)
            if ek == 'Paren':
                a('exprstmt.parenthesised', s)
            self.expr(e, 'statement')
        elif k == 'LocalVar':
            a('local.declaration')
            a('local.declarators per declaration %d' % len(s.declarators), s)
            if s.modifiers:
                a('local.with a modifier', s)
            for d in s.declarators:
                a('local')
                a('local.type ' + type_class(s.type) + '[]' * d.dims, d)
                if d.dims:
                    a('array.brackets after the local name', d)
                if s.type.dims:
                    a('array.brackets after the local type', d)
                if d.init is not None:
                    a('local.with initialiser')
                    self.expr(d.init, 'local initialiser')
                else:
                    a('local.without initialiser', d)
                self.declare(d)
        elif k == 'If':
            a('if.' + ('with else' if s.otherwise is not None else 'without else'))
            self.cond(s.cond, 'if')
            self.expr(s.cond, 'condition')
            self.stmt(s.then, 'if')
            if s.otherwise is not None:
                if s.otherwise.kind == 'If':
                    a('if.else if chain')
                    self.stmt(s.otherwise, None)
                else:
                    self.stmt(s.otherwise, 'else')
            if s.then.kind == 'If' and s.otherwise is not None:
                a('if.then-part is an if without braces, else follows', s)
            if s.then.kind == 'If' and s.then.otherwise is not None and s.otherwise is None:
                a('if.dangling else (bound to the inner if)', s)
        elif k == 'While':
            self.cond(s.cond, 'while')
            self.expr(s.cond, 'condition')
            self.loop(s.body, 'while')
        elif k == 'Do':
            self.loop(s.body, 'do')
            self.cond(s.cond, 'do-while')
            self.expr(s.cond, 'condition')
        elif k == 'For':
            self.scopes.append({})
            if isinstance(s.init, list):
                a('for.init %s' % ('empty' if not s.init else
                                   '%d expression%s' % (len(s.init), 's' if len(s.init) > 1 else '')),
                  s)
                if len(s.init) > 1:
                    a('for.init with several expressions', s)
                for e in s.init:
                    a('for.init expression is ' + expr_class(e))
                    self.expr(e, 'statement')
            else:
                a('for.init declaration with %d declarator%s' % (
                    len(s.init.declarators), 's' if len(s.init.declarators) > 1 else ''), s)
                a('for.init declaration of type ' + type_class(s.init.type))
                for d in s.init.declarators:
                    a('local')
                    a('local.in for header')
                    if d.init is not None:
                        a('local.with initialiser')
                        self.expr(d.init, 'local initialiser')
                    else:
                        a('local.without initialiser', d)
                    self.declare(d)
            if s.cond is None:
                a('for.without condition', s)
            else:
                self.cond(s.cond, 'for')
                self.expr(s.cond, 'condition')
            a('for.update %s' % ('empty' if not s.update else
                                 '%d expression%s' % (len(s.update), 's' if len(s.update) > 1 else '')),
              s)
            if len(s.update) > 1:
                a('for.update with several expressions', s)
            if s.body.kind == 'Empty':
                a('for.with an empty statement as body', s)
            for e in s.update:
                a('for.update expression is ' + (
                    e.kind + ' ' + e.op if e.kind in ('Postfix', 'Unary', 'Assign') else e.kind))
                self.expr(e, 'statement')
            self.loop(s.body, 'for')
            self.scopes.pop()
        elif k == 'Switch':
            self.switch(s)
        elif k in ('Break', 'Continue'):
            if s.label:
                a('stmt.%s with label' % k, s)
            if k == 'Continue':
                a('continue.in a %s loop' % (self.loops[-1] if self.loops else '?'), s)
        elif k == 'Return':
            m = self.method
            if s.value is not None:
                a('return.with value')
                self.expr(s.value, 'return value')
                if m is not None and m.constructor:
                    a('odd.return with a value in a constructor', s)
                elif m is not None and m.type.name == 'void':
                    a('odd.return with a value in a void method', s)
            else:
                a('return.without value')
                if m is not None and not m.constructor and m.type.name != 'void':
                    a('odd.return without a value in a method with a result type', s)
        elif k == 'Labeled':
            a('stmt.labelled', s)
            self.stmt(s.body)

    def loop(self, body, what):
        self.loops.append(what)
        self.stmt(body, what)
        self.loops.pop()

    def cond(self, e, what):
        self.add('cond.%s: %s' % (what, expr_class(e)), e)
        self.add('cond.all: ' + expr_class(e))

    def switch(self, s):
        a = self.add
        self.expr(s.expr, 'switch subject')
        a('switch.subject is ' + expr_class(s.expr))
        a('switch.groups %s' % ('0' if not s.groups else '1+'))
        if not s.groups:
            a('switch.empty body', s)
        nd = 0
        declared = set()
        for gi, g in enumerate(s.groups):
            a('switch.group')
            mine = set()
            for st in g.stmts:
                if st.kind == 'LocalVar':
                    for d in st.declarators:
                        if d.name in declared:
                            a('odd.local declared in two groups of one switch', d)
                        mine.add(d.name)
            declared |= mine
            if mine:
                a('switch.group that declares a local', g)
            a('switch.labels per group %d' % len(g.labels), g)
            if len(g.labels) > 1:
                a('switch.group with several labels', g)
            for lab in g.labels:
                if lab.kind == 'DefaultLabel':
                    nd += 1
                    a('switch.default label', lab)
                    if gi != len(s.groups) - 1:
                        a('switch.default not in the last group', lab)
                    elif g.labels[-1] is not lab:
                        a('switch.default not the last label', lab)
                else:
                    a('switch.case label')
                    v = lab.value
                    a('switch.case value written ' + (
                        'in parentheses' if v.kind == 'Paren' else 'plain'), lab)
                    while v.kind == 'Paren':
                        v = v.expr
                    if v.kind == 'Literal':
                        a('switch.case value is ' + v.lit + ' literal', lab)
                    elif v.kind == 'Unary' and v.operand.kind == 'Literal':
                        a('switch.case value is signed literal', lab)
                    elif v.kind in ('Name', 'Field'):
                        a('switch.case value is a name' +
                          (' (dotted)' if v.kind == 'Field' else ''), lab)
                    else:
                        a('switch.case value is ' + v.kind, lab)
                    self.expr(lab.value, 'case value')
            if not g.stmts:
                a('switch.group without statements (labels before the closing brace)', g)
            else:
                last = g.stmts[-1]
                if last.kind in ('Break', 'Return', 'Continue'):
                    a('switch.group ends with ' + last.kind.lower())
                elif gi == len(s.groups) - 1:
                    a('switch.last group ends without break', g)
                else:
                    a('switch.group falls through into the next', last)
            self.block_stmts(g.stmts, g)
        a('switch.%s default' % ('with' if nd else 'without'))

    # -- expressions -------------------------------------------------------------
    def expr(self, e, ctx):
        a = self.add
        k = e.kind
        if k == 'Literal':
            a('literal.' + e.lit)
        elif k == 'Paren':
            a('paren.around ' + e.expr.kind)
            if e.expr.kind == 'Paren':
                a('paren.double', e)
            self.expr(e.expr, ctx)
        elif k == 'Name':
            a('chain.name chains')
            a('chain.of 1 identifier')
        elif k == 'Field':
            if is_chain(e):
                n = 1
                x = e
                while x.kind == 'Field':
                    x = x.target
                    n += 1
                a('chain.name chains')
                a('chain.of %d identifiers' % n, e)
            elif e.target.kind == 'Super':
                a('super.field access', e)
            else:
                a('fieldaccess.on ' + kind_of_target(e.target), e)
                self.expr(e.target, 'receiver')
        elif k == 'Index':
            a('index.of ' + kind_of_target(e.target), e)
            if e.target.kind == 'Index':
                a('index.of an array element', e)
            self.expr(e.index, 'index')
            self.expr(e.target, 'array')
        elif k == 'Call':
            a('call.on ' + kind_of_target(e.target), e)
            if e.target is not None and is_chain(e.target):
                n = 1
                x = e.target
                while x.kind == 'Field':
                    x = x.target
                    n += 1
                a('call.path of %d identifiers before the method name' % n, e)
            a('call.arguments %d' % min(len(e.args), 10) +
              ('+' if len(e.args) >= 10 else ''))
            for x in e.args:
                self.expr(x, 'argument')
            if e.target is not None and not is_chain(e.target) and e.target.kind != 'Super':
                self.expr(e.target, 'receiver')
        elif k == 'CtorCall':
            a('ctorcall.' + e.which)
            if ctx != 'statement':
                a('odd.this(...) or super(...) inside an expression', e)
            for x in e.args:
                self.expr(x, 'argument')
        elif k == 'New':
            a('new.object of ' + ('dotted class name' if '.' in e.type.name else 'class'), e)
            a('new.arguments %d' % min(len(e.args), 10) + ('+' if len(e.args) >= 10 else ''))
            for x in e.args:
                self.expr(x, 'argument')
            if ctx == 'statement':
                a('new.as a statement', e)
        elif k == 'NewArray':
            a('newarray.of ' + type_class(e.type), e)
            a('newarray.sizes %d, empty brackets %d' % (len(e.sizes), e.dims), e)
            if len(e.sizes) != 1 or e.dims:
                a('newarray.not exactly one size', e)
            for x in e.sizes:
                a('newarray.size is ' + expr_class(x))
                self.expr(x, 'array size')
        elif k == 'Unary':
            a('unary.' + e.op)
            o = e.operand
            if e.op in ('-', '+'):
                a('unary.%s applied to %s' % (e.op, (o.lit + ' literal') if o.kind == 'Literal'
                                                else 'an expression'), e)
            if o.kind == 'Unary':
                a('unary.%s applied to unary %s' % (e.op, o.op), e)
            if e.op in ('++', '--'):
                a('incdec.prefix %s on %s' % (e.op, o.kind))
                a('incdec.%s as %s' % (e.op + 'x', 'statement' if ctx == 'statement' else 'value'), e)
            self.expr(o, 'operand')
        elif k == 'Postfix':
            a('incdec.postfix %s on %s' % (e.op, e.operand.kind))
            a('incdec.%s as %s' % ('x' + e.op, 'statement' if ctx == 'statement' else 'value'), e)
            self.expr(e.operand, 'operand')
        elif k == 'Binary':
            a('binary.' + e.op, e)
            self.expr(e.left, 'operand')
            self.expr(e.right, 'operand')
        elif k == 'Assign':
            a('assign.' + e.op, e)
            a('assign.target ' + e.target.kind)
            if ctx != 'statement':
                a('assign.%s used as a value' % e.op, e)
                if e.op != '=':
                    a('assign.compound operator used as a value', e)
                a('assign.as a value in ' + ctx, e)
                if ctx == 'condition':
                    a('odd.assignment as a condition', e)
            self.expr(e.value, 'assigned value')
            self.expr(e.target, 'assignment target')
        elif k == 'Cond':
            a('cond-expr', e)
            if e.then.kind == 'Cond' or e.otherwise.kind == 'Cond' or e.cond.kind == 'Cond':
                a('cond-expr.nested', e)
            self.expr(e.cond, 'condition')
            self.expr(e.then, 'operand')
            self.expr(e.otherwise, 'operand')
        elif k == 'Cast':
            a('cast.to ' + type_class(e.type), e)
            if e.type.dims:
                a('cast.to an array type', e)
            a('cast.operand is ' + kind_of_target(e.operand))
            nx = self.toks[e.type.tok1 + 2]
            a('cast.followed by %s' % (nx.kind if nx.kind != 'op' and nx.kind != 'kw'
                                       else repr(nx.text)), e)
            self.expr(e.operand, 'operand')
        elif k == 'InstanceOf':
            a('instanceof.' + type_class(e.type), e)
            if e.type.dims or e.type.primitive:
                a('instanceof.array or primitive type', e)
            self.expr(e.operand, 'operand')
        elif k == 'This':
            a('this.' + ctx, e)


def scan_tokens(cen, unit, text):
    """Lexical census of one source."""
    a = cen.add
    for t in unit.tokens:
        k = t.kind
        if k == 'eof':
            continue
        cen.c['token'] += 1
        cen.c['token.' + k] += 1
        if k == 'kw':
            cen.c['keyword ' + t.text] += 1
            cen.files['keyword ' + t.text].add(cen.rel)
        elif k == 'op':
            cen.c['operator ' + t.text] += 1
        elif k == 'id':
            if t.text in JAVA_KEYWORDS_NOT_IN_DIALECT:
                a('odd.Java keyword used as an identifier: ' + t.text, t.line)
            if t.text[0] == '_':
                cen.c['identifier starting with _'] += 1
        elif k in ('int', 'float', 'rid'):
            a('form.' + lexer.literal_form(t), t.line, text=t.text)
            if k == 'int':
                v = int(t.text, 16) if t.text[:2] in ('0x', '0X') else int(t.text)
                if t.text[:2] not in ('0x', '0X') and len(t.text) > 1 and t.text[0] == '0':
                    a('int.decimal with a leading zero', t.line, text=t.text)
                if t.text[:2] not in ('0x', '0X') and v > 0x7FFFFFFF:
                    a('odd.decimal int literal above 2147483647', t.line, text=t.text)
                if v > 0xFFFFFFFF:
                    a('odd.int literal above 32 bits', t.line, text=t.text)
                if t.text[:2] in ('0x', '0X'):
                    cen.c['int hex with %d digits' % (len(t.text) - 2)] += 1
                    if v > 0x7FFFFFFF:
                        a('int hex above 0x7FFFFFFF', t.line, text=t.text)
            elif k == 'float':
                ip, fp, ex, sf = lexer.float_parts(t.text)
                if ex is not None:
                    a('float.with an exponent', t.line, text=t.text)
                if not ip:
                    a('float.without digits before the point', t.line, text=t.text)
                if fp == '':
                    a('float.without digits after the point', t.line, text=t.text)
                if sf and sf != 'f':
                    a('float.with a suffix other than f', t.line, text=t.text)
                if sf == 'f':
                    cen.c['float.with suffix f'] += 1
                cen.c['float integer digits %d' % min(len(ip), 11)] += 1
                cen.c['float fraction digits %d' % min(len(fp or ''), 11)] += 1
                if len(ip) > 9:
                    a('float with more than 9 integer digits', t.line, text=t.text)
                if len(ip) > 1 and ip[0] == '0':
                    a('float with a leading zero', t.line, text=t.text)
            else:
                pack, lid = lexer.rid_parts(t.text)
                cen.c['rid pack ' + pack] += 1
                cen.packs[pack] += 1
                cen.c['rid pack segments %d' % (pack.count('.') + 1)] += 1
                if lid > 0xFFFF:
                    a('rid local id above 0xFFFF', t.line, text=t.text)
                hexpart = t.text.rpartition(':')[2][2:-1]
                if len(hexpart) > 8:
                    a('rid with more than 8 hex digits', t.line, text=t.text)
                if hexpart != hexpart.upper():
                    a('rid with lower-case hex digits', t.line, text=t.text)
        elif k == 'str':
            cen.c['string'] += 1
            if t.text == '""':
                cen.c['string empty'] += 1
            for esc in lexer.string_escapes(t.text):
                a('string escape \\' + (esc if esc not in ('\\',) else '\\'), t.line, text=t.text)
                if esc not in '"\\nt':
                    a('string.with an escape other than quote, backslash, n, t', t.line,
                      text=t.text)
            if any(ord(ch) > 126 or (ord(ch) < 32 and ch != '\t') for ch in t.text):
                a('string.with a character outside printable ASCII and TAB', t.line,
                  text=repr(t.text))
            if '\t' in t.text:
                a('string with a raw TAB', t.line, text=t.text.replace('\t', '<TAB>'))
            if '//' in t.text or '/*' in t.text:
                a('string holding // or /*', t.line, text=t.text)
            cen.c['string length %s' % ('0' if len(t.text) == 2 else '1-99' if len(t.text) < 102
                                        else '100-999' if len(t.text) < 1002 else '1000+')] += 1
        elif k == 'chr':
            a('char literal', t.line, text=t.text)
    for c in unit.comments:
        if c.kind == 'line':
            cen.c['comment.line'] += 1
            if '/*' in c.text:
                a('comment.line holding /*', c.line, text=c.text)
            if c.text.rstrip('\r').endswith('\\'):
                a('comment.line ending in a backslash', c.line, text=c.text)
        else:
            cen.c['comment.block'] += 1
            if '\n' in c.text:
                cen.c['comment.block over several lines'] += 1
            if c.text.startswith('/**') and len(c.text) > 4:
                a('comment.block starting with /**', c.line, text=c.text.split('\n')[0])
            if '/*' in c.text[2:]:
                a('comment.block holding /*', c.line, text=c.text.split('\n')[0])
            if '//' in c.text:
                cen.c['comment.block holding //'] += 1
            # a block comment inside an expression or statement
    cen.c['lines'] += text.count('\n') + (0 if text.endswith('\n') else 1)
    cen.c['bytes'] += len(text)


def run(root=None):
    root = root or oracle.corpus_root()
    cen = Census()
    uni = symbols.Universe.from_index()
    failures = []
    cats = Counter()
    type_res = Counter({'unknown': 0, 'written with dots': 0,
                        'with candidates in more than one package': 0})
    unknown_types = Counter()
    unknown_examples = {}
    notes = Counter()
    cat_of = {}
    for e in oracle.iter_entries():
        if e.source is None:
            continue
        rel = e.source
        cat_of[rel] = e.category
        text = oracle.load_source(e, root)
        cen.rel = rel
        cen.lines = text.split('\n')
        cats[e.category] += 1
        try:
            unit = parser.parse(text, rel)
        except (parser.ParseError, lexer.LexError) as x:
            failures.append('%s: %s' % (rel, x))
            continue
        cen.c['sources parsed'] += 1
        scan_tokens(cen, unit, text)
        for n in unit.walk():
            cen.c['node.' + n.kind] += 1
            cen.files['node.' + n.kind].add(rel)
        res = symbols.Resolver(unit, uni)
        Walker(cen, unit, res, uni, rel).unit()
        for code, line, tx in unit.notes:
            cen.add('note.' + code, line)
            notes[code] += 1
        # type names
        names = []
        for cls in unit.types:
            if cls.extends:
                names.append((cls.extends, cls.line))
            names.extend((x, cls.line) for x in cls.implements)
        for n in unit.walk():
            if n.kind == 'Type' and not n.primitive:
                names.append((n.name, n.line))
        for name, line in names:
            type_res['type names'] += 1
            type_res['type names in ' + e.category] += 1
            if '.' in name:
                type_res['written with dots'] += 1
                cen.add('type.written with dots', line)
                how = res.resolve_how(name)[0]
            else:
                c = res.candidates(name)
                how = c[0][0] if c else None
                if len(set(f for _, f in c)) > 1:
                    type_res['with candidates in more than one package'] += 1
            if how is None:
                type_res['unknown'] += 1
                type_res['unknown in ' + e.category] += 1
                unknown_types[name] += 1
                unknown_examples.setdefault(name, '%s:%d' % (rel, line))
            else:
                type_res['found: ' + how] += 1
    # constructs no pair exercises
    only_source = {}
    for key, files in cen.files.items():
        if key.startswith(('unit.imports ', 'unit.types ')):
            continue                # size buckets, not constructs
        if files and all(cat_of.get(f) == 'source_only' for f in files):
            only_source[key] = cen.c[key]
    cen.c['census keys with a source file'] = len(cen.files)
    # resource packs: does the file the literal names exist?
    packs = {}
    lower = {}
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if not d.lower().startswith('tree_rpk_scan_boot')]
        for f in fn:
            if f.lower().endswith('.rpk'):
                relp = os.path.relpath(os.path.join(dp, f), root)
                lower[relp.lower()] = relp
    for pack, n in cen.packs.items():
        want = lexer.rid_pack_file(pack)
        have = lower.get(want.lower())
        packs[pack] = {'literals': n, 'file': have, 'same_case': have == want}
        cen.c['rid packs written (distinct spellings)'] += 1
        if have is None:
            cen.c['rid packs without a file on disk'] += 1
            cen.c['rid literals of packs without a file on disk'] += n
        else:
            cen.c['rid literals of packs with a file on disk'] += n
            if have != want:
                cen.c['rid packs spelled in another letter case than the file'] += 1
                cen.c['rid literals spelled in another letter case than the file'] += n
    cen.c['rid packs on disk (distinct files named)'] = len(
        set(v['file'] for v in packs.values() if v['file']))
    cen.c['rpk files on disk'] = len(lower)
    return {'census': cen, 'failures': failures, 'categories': cats, 'packs': packs,
            'only_source': only_source,
            'type_res': type_res, 'unknown_types': unknown_types,
            'unknown_examples': unknown_examples, 'notes': notes}


def report(r, out):
    cen = r['census']
    c = cen.c
    w = out.write
    w('census: constructs of the dialect in the shipped sources\n')
    w('sources parsed %d, failures %d\n\n' % (c['sources parsed'], len(r['failures'])))
    for f in r['failures']:
        w('  FAILURE %s\n' % f)
    groups = defaultdict(list)
    for k in c:
        head = k.split('.', 1)[0] if '.' in k.split(' ')[0] else k.split(' ')[0]
        groups[head].append(k)
    for head in sorted(groups):
        w('[%s]\n' % head)
        for k in sorted(groups[head]):
            nf = len(cen.files.get(k, ()))
            w('  %8d  %s%s\n' % (c[k], k, ('   (%d files)' % nf) if nf else ''))
            for x in cen.ex.get(k, []):
                w('              %s\n' % x)
        w('\n')
    w('[constructs that occur only in sources without a class file]\n')
    for k in sorted(r['only_source']):
        w('  %8d  %s\n' % (r['only_source'][k], k))
        for x in cen.ex.get(k, []):
            w('              %s\n' % x)
    if not r['only_source']:
        w('  none\n')
    w('\n')
    w('[resource packs named by literals]\n')
    for pack in sorted(r['packs'], key=str.lower):
        v = r['packs'][pack]
        w('  %6d  %-55s %s\n' % (v['literals'], pack, v['file'] or 'NO SUCH FILE'))
    w('\n')
    w('[type name resolution]\n')
    for k in sorted(r['type_res']):
        w('  %8d  %s\n' % (r['type_res'][k], k))
    for name, n in r['unknown_types'].most_common():
        w('      unknown: %s (%d uses, first %s)\n' % (name, n, r['unknown_examples'][name]))


def main(argv):
    root = None
    jpath = None
    i = 1
    while i < len(argv):
        if argv[i] == '--corpus':
            root = argv[i + 1]
            i += 2
        elif argv[i] == '--json':
            jpath = argv[i + 1]
            i += 2
        else:
            print(__doc__)
            return 2
    t0 = time.time()
    r = run(root)
    report(r, sys.stdout)
    print('done in %.1f s' % (time.time() - t0))
    if jpath:
        cen = r['census']
        doc = {'counts': dict(cen.c),
               'examples': {k: v for k, v in cen.ex.items()},
               'files': {k: len(v) for k, v in cen.files.items()},
               'failures': r['failures'],
               'packs': r['packs'],
               'only_source': r['only_source'],
               'categories': dict(r['categories']),
               'type_res': dict(r['type_res']),
               'unknown_types': dict(r['unknown_types']),
               'unknown_examples': r['unknown_examples'],
               'notes': dict(r['notes'])}
        with open(jpath, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    return 0 if not r['failures'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
