#!/usr/bin/env python3
"""t2_parse.py - milestone M2: every source parses.

    python -I tests/t2_parse.py [--only TEXT] [-v] [--update]

A source passes when
  a  slrrc.parser accepts it without a diagnostic in strict mode (so no
     construct outside the proven subset is in it);
  b  every token is consumed, every node of the tree is reachable from the
     root exactly once, and the token ranges nest: a child lies inside its
     parent, siblings are in source order and do not overlap;
  c  the tokens regenerated from the AST (slrrc.dump.ast_tokens) equal the
     tokens read: the tree loses nothing but comments and white space;
  d  the modifier bits of every declaration are the OR of its modifier
     keywords;
  e  the tree is the tree of lib/parser.py (the parser the corpus proofs were
     made with): the same nodes in the same order with the same first and
     last token, operator token and name token, and the same names,
     modifiers, dimensions, literals and operators.

Headline:  M2 parse         3454/3454 sources
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import parser as libparser  # noqa: E402  lib/parser.py, the oracle

from slrrc import dump      # noqa: E402
from slrrc import parser as ps      # noqa: E402
from slrrc import scanner as sc     # noqa: E402
from slrrc.diag import Diagnostics  # noqa: E402

NONE = ps.NONE
LIB_FLAGS = {'public': 0x1, 'protected': 0x2, 'private': 0x4, 'static': 0x8,
             'abstract': 0x10, 'final': 0x20, 'native': 0x40, 'synchronized': 0x80}
LIT_NAMES = {sc.T_INT: 'int', sc.T_FLOAT: 'float', sc.T_STR: 'str', sc.T_RID: 'rid',
             sc.KW_NULL: 'null', sc.KW_TRUE: 'bool', sc.KW_FALSE: 'bool'}
WITH_OP = ('Binary', 'InstanceOf', 'Assign', 'Cond', 'Unary', 'Postfix')
WITH_NAME = ('Field', 'Call')
WITH_MODS = (ps.CLASS, ps.FIELD_DECL, ps.METHOD_DECL, ps.PARAM, ps.LOCAL_VAR)


def nests(ast):
    """Token ranges: children inside the parent, in order, not overlapping;
    every node reachable exactly once.  Returns a reason or None."""
    seen = 0
    stack = [ast.root]
    while stack:
        n = stack.pop()
        seen += 1
        if ast.nodes[n.index] is not n:
            return 'node %r has a wrong index' % n
        last = n.tok0 - 1
        for c in ps.children(n):
            if c.tok0 > c.tok1 or c.tok0 < n.tok0 or c.tok1 > n.tok1 or c.tok0 <= last:
                return 'token range of %r in %r' % (c, n)
            last = c.tok1
            stack.append(c)
    if seen != len(ast.nodes):
        return '%d nodes made, %d reachable' % (len(ast.nodes), seen)
    return None


def modifier_bits(ast):
    """Every declaration: flags = OR of the modifier keywords in front."""
    for n in ast.nodes:
        if n.kind in WITH_MODS:
            mask = 0
            i = n.tok0
            while ast.toks[i].kind in ps.MODIFIER_BITS:
                mask |= ps.MODIFIER_BITS[ast.toks[i].kind]
                i += 1
            if mask != (n.flags & ps.ACC_MASK):
                return 'modifier bits of %r' % n
    return None


def mine(ast):
    """The tree as a list of comparable tuples, parents first, source order.
    The helper nodes lib/parser.py does not have are left out: LIST (its
    items take its place), QNAME, and the TYPE of extends / implements."""
    out = []
    text = ast.text

    def walk(n, parent):
        k = n.kind
        if k == ps.LIST:
            for x in n.items:
                walk(x, parent)
            return
        if k == ps.QNAME or (k == ps.TYPE and parent == ps.CLASS):
            return
        name = ps.KIND_NAMES[k]
        op = n.op_tok if name in WITH_OP else NONE
        nt = n.name_tok if name in WITH_NAME else NONE
        x = None
        if k == ps.UNIT:
            x = ast.dotted(n.a.tok0, n.a.tok1) if n.a is not None else None
        elif k == ps.IMPORT:
            x = (ast.dotted(n.a.tok0, n.a.tok1), bool(n.flags & ps.F_STAR),
                 text(n.tok0 + 1) if n.flags & ps.F_COLON else None)
        elif k == ps.CLASS:
            x = (text(n.name_tok), bool(n.flags & ps.F_INTERFACE), n.flags & ps.ACC_MASK,
                 ast.type_name(n.a) if n.a is not None else None,
                 [ast.type_name(t) for t in n.b.items] if n.b is not None else [])
        elif k == ps.FIELD_DECL or k == ps.LOCAL_VAR:
            x = n.flags & ps.ACC_MASK
        elif k == ps.METHOD_DECL:
            x = (text(n.name_tok), bool(n.flags & ps.F_CTOR), n.flags & ps.ACC_MASK,
                 n.b is not None)
        elif k == ps.PARAM:
            x = (text(n.name_tok), n.dims(), n.flags & ps.ACC_MASK)
        elif k == ps.DECLARATOR:
            x = (text(n.name_tok), n.dims(), n.a is not None)
        elif k == ps.TYPE:
            x = (ast.type_name(n), n.dims(), ps.is_primitive(ast, n))
        elif k == ps.LITERAL:
            x = (LIT_NAMES[ast.toks[n.tok0].kind], text(n.tok0))
        elif k == ps.NAME:
            x = text(n.name_tok)
        elif k == ps.FIELD or k == ps.CALL:
            x = text(n.name_tok)
        elif k == ps.CTOR_CALL:
            x = text(n.tok0)
        elif k == ps.UNARY or k == ps.POSTFIX or k == ps.BINARY or k == ps.ASSIGN:
            x = text(n.op_tok)
        out.append((name, n.tok0, n.tok1, op, nt, x))
        for c in ps.children(n):
            walk(c, k)

    walk(ast.root, 0)
    return out


def lib_mask(mods):
    m = 0
    for x in mods:
        m |= LIB_FLAGS[x]
    return m


def lib_walk(unit):
    """The nodes of a lib/parser.py Unit, parents first, source order.
    (Unit keeps imports and types in two lists; an import behind a type, which
    no shipped source has, would come out of Node.walk() in front of it.)"""
    yield unit
    for top in sorted(unit.imports + unit.types, key=lambda n: n.tok0):
        for n in top.walk():
            yield n


def theirs(unit):
    """The tree of lib/parser.py in the same form."""
    out = []
    for n in lib_walk(unit):
        k = n.kind
        op = getattr(n, 'op_tok', NONE) if k in WITH_OP else NONE
        nt = getattr(n, 'name_tok', NONE) if k in WITH_NAME else NONE
        x = None
        if k == 'Unit':
            x = n.package
        elif k == 'Import':
            x = (n.name, bool(n.star), n.pack)
        elif k == 'Class':
            x = (n.name, bool(n.interface), lib_mask(n.modifiers), n.extends,
                 list(n.implements))
        elif k in ('FieldDecl', 'LocalVar'):
            x = lib_mask(n.modifiers)
        elif k == 'MethodDecl':
            x = (n.name, bool(n.constructor), lib_mask(n.modifiers), n.body is not None)
        elif k == 'Param':
            x = (n.name, n.dims, lib_mask(n.modifiers))
        elif k == 'Declarator':
            x = (n.name, n.dims, n.init is not None)
        elif k == 'Type':
            x = (n.name, n.dims, bool(n.primitive))
        elif k == 'Literal':
            x = (n.lit, n.text)
        elif k == 'Name':
            x = n.name
        elif k in ('Field', 'Call'):
            x = n.name
        elif k == 'CtorCall':
            x = n.which
        elif k in ('Unary', 'Postfix', 'Binary', 'Assign'):
            x = n.op
        out.append((k, n.tok0, n.tok1, op, nt, x))
    return out


def check(text, name):
    """(reason of failure or None, number of AST nodes)."""
    diags = Diagnostics(strict=True)
    toks = sc.scan(text, diags)
    ast = ps.parse(text, toks, diags) if not diags.errors else None
    if diags.items:
        d = diags.items[0]
        return 'line %d column %d: %s %s' % (d.line, d.column, d.code, d.text), 0
    if ast.root is None:
        return 'no tree and no diagnostic', 0
    n = len(ast.nodes)
    if ast.root.tok0 != 0 or ast.root.tok1 != len(toks) - 2:
        return 'not every token was consumed', n
    why = nests(ast) or modifier_bits(ast)
    if why:
        return why, n
    regen = dump.ast_tokens(ast)
    orig = [text[t.pos:t.pos + t.length] for t in toks[:-1]]
    if regen != orig:
        k = 0
        while k < min(len(regen), len(orig)) and regen[k] == orig[k]:
            k += 1
        return 'token %d (line %d): read %r, regenerated %r' % (
            k, toks[min(k, len(toks) - 1)].line, orig[k:k + 4], regen[k:k + 4]), n
    try:
        ref = theirs(libparser.parse(text, name))
    except (libparser.ParseError, libparser.lexer.LexError) as x:
        return 'lib/parser.py rejects it: %s' % x, n
    got = mine(ast)
    if got != ref:
        k = 0
        while k < min(len(got), len(ref)) and got[k] == ref[k]:
            k += 1
        a = got[k] if k < len(got) else None
        b = ref[k] if k < len(ref) else None
        line = toks[(a or b)[1]].line
        return 'line %d: node %d is %r, lib/parser.py has %r' % (line, k, a, b), n
    return None, n


def run(opts):
    oc = harness.Outcome('M2 parse')
    entries = harness.selected(harness.all_sources(), opts)
    passed = []
    nodes = 0
    for e in entries:
        why, n = check(harness.load_source(e), e.source)
        nodes += n
        if why is None:
            passed.append(e.source)
        else:
            oc.failures.append('%s: %s' % (e.source, why))
    oc.text = '%d/%d sources' % (len(passed), len(entries))
    oc.ok = len(passed) == len(entries) == 3454
    oc.passed['m2.pass'] = passed
    oc.notes.append('%d AST nodes' % nodes)
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
