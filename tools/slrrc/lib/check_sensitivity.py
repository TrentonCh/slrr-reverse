#!/usr/bin/env python3
"""check_sensitivity.py - does the shape proof notice a wrong grammar?

check_shape.py finds that the trees of parser.py have the shape of the trees
in the class files.  That is only worth something when a different grammar
would have been caught.  This script parses the consistent pairs again with
deliberately wrong grammars (one decision changed at a time) and counts the
trees whose shape then differs from the class file.  A count of 0 means the
corpus cannot tell the two grammars apart: that decision is then not proven,
only assumed.

    python -I check_sensitivity.py [--json FILE] [--corpus DIR]

It also counts, in the ASTs of the pairs, the places where each decision
matters (operators of two precedence levels sharing an operand without
parentheses, chains of one level, an else after a nested if, ...).
"""
import json
import os
import sys
import time
from collections import Counter, OrderedDict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import check_shape  # noqa: E402
import oracle       # noqa: E402
import parser       # noqa: E402

LEVELS = OrderedDict([
    (1, '||'), (2, '&&'), (3, '|'), (4, '^'), (5, '&'), (6, '== !='),
    (7, '< > <= >= instanceof'), (8, '<< >> >>>'), (9, '+ -'), (10, '* / %'),
])


def with_prec(table):
    return type('P', (parser.Parser,), {'PREC': table})


def swapped(a, b):
    """Precedence table with the levels a and b exchanged."""
    t = {}
    for op, lv in parser.BINARY_PREC.items():
        t[op] = b if lv == a else a if lv == b else lv
    return t


class ElseOuter(parser.Parser):
    """An else belongs to the OUTER if when an if is the direct body of an
    if:  if (a) if (b) x; else y;  reads as  if (a) { if (b) x; } else y;"""
    deny = False

    def if_statement(self):
        deny, self.deny = self.deny, False
        t = self.expect('if')
        n = self.node('If', t, cond=None, then=None, otherwise=None)
        self.expect('(', 'if')
        n.cond = self.expression()
        self.expect(')', 'if')
        if self.at('if'):
            self.deny = True
        n.then = self.statement()
        self.deny = False
        if not deny and self.accept('else'):
            n.otherwise = self.statement()
        return self.done(n)

    def block(self):
        deny, self.deny = self.deny, False
        try:
            return parser.Parser.block(self)
        finally:
            self.deny = deny


class NoNameCast(parser.Parser):
    """'(' Name ')' never starts a cast; only (int) and (float) do."""

    def at_cast(self):
        t = self.toks[self.i + 1]
        if t.kind == 'kw' and t.text in ('int', 'float'):
            return parser.Parser.at_cast(self)
        return False


class CastBeforeSign(parser.Parser):
    """'(' Name ')' also starts a cast when + or - follows:  (a) - b  reads
    as the cast of -b to a."""

    def at_cast(self):
        if parser.Parser.at_cast(self):
            return True
        toks = self.toks
        j = self.i + 1
        if toks[j].kind != 'id':
            return False
        j += 1
        while toks[j].text == '.' and toks[j + 1].kind == 'id':
            j += 2
        return toks[j].text == ')' and toks[j + 1].kind == 'op' and \
            toks[j + 1].text in ('+', '-')


class LooseUnary(parser.Parser):
    """Prefix operators and casts take a whole product as operand:
    -a * b  reads as  -(a * b),  (float)a / b  as  (float)(a / b)."""
    OPERAND_LEVEL = 10

    def unary(self):
        t = self.tok
        if t.kind == 'op':
            if t.text in ('+', '-', '!', '~'):
                self.i += 1
                n = self.node('Unary', t, op=t.text, operand=None)
                n.operand = self.binary(self.OPERAND_LEVEL)
                return self.done(n)
            if t.text == '(' and self.at_cast():
                self.i += 1
                n = self.node('Cast', t, type=None, operand=None)
                n.type = self.type('cast')
                self.expect(')', 'cast')
                n.operand = self.binary(self.OPERAND_LEVEL)
                return self.done(n)
        return parser.Parser.unary(self)


class LoosestUnary(LooseUnary):
    """Prefix operators and casts take the whole binary expression that
    follows:  !a && b  reads as  !(a && b),  -a + b  as  -(a + b)."""
    OPERAND_LEVEL = 1


class CastOfPrimaryOnly(parser.Parser):
    """A cast applies to the first name only:  (T)a.b  reads as  ((T)a).b"""

    def unary(self):
        t = self.tok
        if t.kind == 'op' and t.text == '(' and self.at_cast():
            self.i += 1
            n = self.node('Cast', t, type=None, operand=None)
            n.type = self.type('cast')
            self.expect(')', 'cast')
            n.operand = self.primary()
            self.done(n)
            return self.postfix_from(n, t)
        return parser.Parser.unary(self)

    def postfix_from(self, e, start):
        while True:
            t = self.tok
            if t.kind == 'op' and t.text == '.':
                self.i += 1
                name = self.ident('member access')
                if self.at('('):
                    n = self.node('Call', start, target=e, name=name.text, args=None)
                    n.args = self.arguments()
                else:
                    n = self.node('Field', start, target=e, name=name.text)
                e = self.done(n)
            elif t.kind == 'op' and t.text == '[':
                self.i += 1
                n = self.node('Index', start, target=e, index=None)
                n.index = self.expression()
                self.expect(']', 'array access')
                e = self.done(n)
            else:
                return e


class LeftAssign(parser.Parser):
    """Assignment groups to the left:  a = b = c  reads as  (a = b) = c"""

    def expression(self):
        start = self.tok
        left = self.conditional()
        while self.tok.kind == 'op' and self.tok.text in parser.ASSIGN_OPS:
            t = self.tok
            self.i += 1
            n = self.node('Assign', start, op=t.text, target=left, value=None)
            n.value = self.conditional()
            left = self.done(n)
        return left


class CondBindsTight(parser.Parser):
    """The condition of ?: is a single operand:  a < b ? c : d  reads as
    a < (b ? c : d)"""

    def unary(self):
        start = self.tok
        e = parser.Parser.unary(self)
        if self.at('?'):
            self.i += 1
            n = self.node('Cond', start, cond=e, then=None, otherwise=None)
            n.then = self.expression()
            self.expect(':', 'conditional expression')
            n.otherwise = self.conditional()
            return self.done(n)
        return e


def mutations():
    out = []
    flat = {op: 1 for op in parser.BINARY_PREC}
    out.append(('flat', 'all binary operators on one level, grouped left to right',
                with_prec(flat)))
    out.append(('right', 'every binary operator groups to the right',
                type('P', (parser.Parser,),
                     {'RIGHT_ASSOC': frozenset(parser.BINARY_PREC)})))
    for lv, ops in LEVELS.items():
        these = frozenset(op for op, l in parser.BINARY_PREC.items() if l == lv)
        out.append(('right %d' % lv, 'level %d (%s) groups to the right' % (lv, ops),
                    type('P', (parser.Parser,), {'RIGHT_ASSOC': these})))
    lv = list(LEVELS)
    for i in range(len(lv) - 1):
        a, b = lv[i], lv[i + 1]
        out.append(('swap %d %d' % (a, b),
                    'neighbouring levels %d (%s) and %d (%s) exchanged' % (
                        a, LEVELS[a], b, LEVELS[b]),
                    with_prec(swapped(a, b))))

    def doc(cls):
        return ' '.join(x.strip() for x in cls.__doc__.split('\n'))
    out.append(('else outer', doc(ElseOuter), ElseOuter))
    out.append(('no name cast', doc(NoNameCast), NoNameCast))
    out.append(('cast before sign', doc(CastBeforeSign), CastBeforeSign))
    out.append(('loose unary', doc(LooseUnary), LooseUnary))
    out.append(('loosest unary', doc(LoosestUnary), LoosestUnary))
    out.append(('cast of primary', doc(CastOfPrimaryOnly), CastOfPrimaryOnly))
    out.append(('left assign', doc(LeftAssign), LeftAssign))
    out.append(('cond tight', doc(CondBindsTight), CondBindsTight))
    return out


# ---------------------------------------------------------------------------
# places where a decision matters, counted in the ASTs
# ---------------------------------------------------------------------------

def level_of(n):
    if n.kind == 'Binary':
        return parser.BINARY_PREC[n.op]
    if n.kind == 'InstanceOf':
        return 7
    return None


def census(root=None):
    c = Counter()
    pair = Counter()            # (outer level, inner level) -> count
    for e in oracle.iter_pairs(build=None, consistent=True):
        unit = parser.parse(oracle.load_source(e, root), e.source)
        for n in unit.walk():
            k = n.kind
            lv = level_of(n)
            if lv is not None:
                left = n.left if k == 'Binary' else n.operand
                ll = level_of(left)
                if ll is not None:
                    if ll == lv:
                        c['chain of level %d' % lv] += 1
                    else:
                        pair[(lv, ll)] += 1
                if k == 'Binary':
                    rl = level_of(n.right)
                    if rl is not None:
                        pair[(lv, rl)] += 1
                if left.kind == 'Unary' and left.op in ('-', '+', '!', '~'):
                    c['prefix operator as left operand of a binary operator'] += 1
                if left.kind == 'Cast':
                    c['cast as left operand of a binary operator'] += 1
            elif k == 'If':
                if n.otherwise is not None and n.then.kind == 'If':
                    c['if with else whose then-part is an if without braces'] += 1
                if n.otherwise is None and n.then.kind == 'If' and \
                        n.then.otherwise is not None:
                    c['if without else whose then-part is an if-else without braces'] += 1
            elif k == 'Cast':
                c['cast'] += 1
                c['cast to ' + ('a primitive type' if n.type.primitive else 'a class')] += 1
                if n.operand.kind in ('Field', 'Index', 'Call'):
                    c['cast whose operand has a "." "[" or "(" suffix'] += 1
            elif k == 'Assign':
                if n.value.kind == 'Assign':
                    c['assignment whose value is an assignment (a = b = c)'] += 1
            elif k == 'Cond':
                c['conditional expression'] += 1
                if level_of(n.cond) is not None:
                    c['conditional whose condition is a binary expression without parentheses'] += 1
            elif k == 'Paren':
                if n.expr.kind == 'Name':
                    c['parenthesised single name'] += 1
    return c, pair


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
    print('check_sensitivity: wrong grammars against the class files')
    print('corpus %s' % (root or oracle.corpus_root()))
    print()
    base = check_shape.run(root, consistent_only=True)
    bs = base['stats']
    base_bad = set(w for _, w, _, _, _ in base['bad'])
    print('Real grammar: %d trees of %d consistent pairs, %d differ (%s)' % (
        bs['trees'], sum(c['pairs'] for c in base['per_cat'].values()),
        len(base_bad), 'the known resource id of a missing pack' if len(base_bad) == 1
        else 'see check_shape.py'))
    print()
    print('%-18s %9s %9s %9s  %s' % ('mutation', 'no parse', 'trees', 'pairs', 'what was changed'))
    rows = []
    for key, text, cls in mutations():
        r = check_shape.run(root, consistent_only=True, parser_class=cls)
        s = r['stats']
        new_bad = [b for b in r['bad'] if b[1] not in base_bad]
        trees = len([b for b in new_bad if b[4] != 'parse']) + \
            s['trees of sources that do not parse']
        pairs = sum(c['pairs different'] for c in r['per_cat'].values()) - \
            sum(c['pairs different'] for c in base['per_cat'].values())
        example = ''
        for b in new_bad:
            if b[4] != 'parse':
                example = b[1]
                break
        if not example and new_bad:
            example = new_bad[0][1] + ': ' + new_bad[0][2][:120]
        rows.append({'key': key, 'text': text, 'no_parse': s['sources that do not parse'],
                     'trees': trees, 'pairs': pairs, 'example': example})
        print('%-18s %9d %9d %9d  %s' % (key, s['sources that do not parse'], trees, pairs, text))
    print()
    print('"no parse": sources the wrong grammar cannot parse at all; "trees": trees')
    print('whose shape differs from the class file (all trees of a source that does')
    print('not parse count); "pairs": source files with at least one such tree.')
    print()
    print('Exchanging two neighbouring levels changes nothing else, so a count of 0')
    print('there means the corpus has no expression that tells the two apart.  For')
    print('levels that are not neighbours see the table of shared operands below.')
    print()
    c, pair = census(root)
    print('Places where a decision matters, counted in the ASTs of the consistent pairs:')
    for k in sorted(c):
        print('  %7d  %s' % (c[k], k))
    print()
    print('Operators of two levels sharing an operand without parentheses')
    print('(rows: the operator applied last; columns: the operator applied first):')
    lv = list(LEVELS)
    print('  %-26s' % 'outer \\ inner' + ''.join('%7d' % b for b in lv))
    for a in lv:
        print('  %2d %-23s' % (a, LEVELS[a]) + ''.join(
            '%7s' % (pair[(a, b)] if pair[(a, b)] else ('.' if a != b else '-')) for b in lv))
    print()
    print('done in %.1f s' % (time.time() - t0))
    if jpath:
        doc = {'mutations': rows, 'census': dict(c),
               'adjacent_levels': [{'outer': a, 'inner': b, 'count': n}
                                   for (a, b), n in sorted(pair.items())],
               'levels': {str(k): v for k, v in LEVELS.items()},
               'base_trees': bs['trees'], 'base_different': sorted(base_bad)}
        with open(jpath, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
