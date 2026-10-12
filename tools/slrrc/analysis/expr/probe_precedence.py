#!/usr/bin/env python3
"""probe_precedence.py - which precedence and associativity facts does the
corpus exercise?

check_expr.py reproduces every tree with a parser that uses Java's operator
precedence (jparse.BINARY_LEVELS) and associativity.  That proves the
compiler's grammar agrees with Java's wherever two operators meet WITHOUT
parentheses in a pair source.  This script counts those meetings, so that the
notes can say which relations are proven and which never occur:

  - parent operator level / child operator level, for a child that is a
    direct, unparenthesised operand (left or right)
  - same-level chains (a - b - c): left child = left associative
  - unary operand of a binary, cast operand forms, assignment chains,
    conditional operands

    python -I probe_precedence.py [--json FILE]
"""
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', '..', 'lib'))
for p in (_LIB, _HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import oracle   # noqa: E402
import jparse   # noqa: E402

LEVEL = {}
for i, ops in enumerate(jparse.BINARY_LEVELS):
    for o in ops:
        LEVEL[o] = i
NAMES = ['||', '&&', '|', '^', '&', '== !=', '< > <= >= instanceof', '<< >>', '+ -', '* / %']


def level_of(e):
    if e.k == 'binary':
        return LEVEL[e.op]
    if e.k == 'instanceof':
        return LEVEL['instanceof']
    return None


def main(argv):
    pair = Counter()
    same = Counter()
    other = Counter()
    for e in oracle.iter_pairs(build=None, consistent=True):
        u = jparse.parse(oracle.load_source(e))
        for n in jparse.walk(u):
            if n.k in ('binary', 'instanceof'):
                pl = level_of(n)
                kids = [('left', n.l)] if n.k == 'binary' else [('left', n.x)]
                if n.k == 'binary':
                    kids.append(('right', n.r))
                for side, c in kids:
                    cl = level_of(c)
                    if cl is not None:
                        if cl == pl:
                            same[(NAMES[pl], side)] += 1
                        else:
                            pair[(NAMES[pl], NAMES[cl], side)] += 1
                    elif c.k == 'unary':
                        other['unary %s as %s operand of a binary operator' % (c.op, side)] += 1
                    elif c.k == 'cast':
                        other['cast as %s operand of a binary operator' % side] += 1
                    elif c.k == 'assign':
                        other['assignment as operand of a binary operator (unparenthesised)'] += 1
                    elif c.k == 'cond':
                        other['conditional as operand of a binary operator (unparenthesised)'] += 1
            elif n.k == 'unary':
                c = n.x
                if c.k in ('binary', 'instanceof', 'cond', 'assign'):
                    other['unary %s applied to an unparenthesised %s' % (n.op, c.k)] += 1
                elif c.k == 'unary':
                    other['unary %s applied to unary %s' % (n.op, c.op)] += 1
                elif c.k == 'cast':
                    other['unary %s applied to a cast' % n.op] += 1
                elif c.k in ('field', 'index', 'call'):
                    other['unary %s applied to a postfix form (%s)' % (n.op, c.k)] += 1
            elif n.k == 'cast':
                c = n.x
                other['cast operand: %s' % (c.k if c.k != 'unary' else 'unary ' + c.op)] += 1
            elif n.k == 'assign':
                if n.r.k == 'assign':
                    other['assignment chain a = b = c (right associative)'] += 1
                if n.r.k == 'cond':
                    other['conditional as right side of an assignment'] += 1
                if n.r.k in ('binary', 'instanceof'):
                    other['binary as right side of an assignment'] += 1
            elif n.k == 'cond':
                for nm, c in (('condition', n.c), ('then', n.a), ('else', n.b)):
                    other['conditional %s: %s' % (nm, c.k)] += 1
            elif n.k == 'postfix':
                other['postfix %s on %s' % (n.op, n.x.k)] += 1
    print('operator meetings without parentheses (parent level <- child level, side):')
    for (pn, cn, side), v in sorted(pair.items(), key=lambda kv: (NAMES.index(kv[0][0]),
                                                                    NAMES.index(kv[0][1]), kv[0][2])):
        rel = 'child binds tighter' if NAMES.index(cn) > NAMES.index(pn) else \
            'CHILD BINDS LOOSER'
        print('  %-24s <- %-24s %-5s %6d  %s' % (pn, cn, side, v, rel))
    print('same-level chains:')
    for (pn, side), v in sorted(same.items(), key=lambda kv: (NAMES.index(kv[0][0]), kv[0][1])):
        print('  %-24s child on the %-5s %6d  %s' % (
            pn, side, v, 'left associative' if side == 'left' else 'RIGHT NESTING'))
    print('other forms:')
    for k, v in sorted(other.items()):
        print('  %6d  %s' % (v, k))
    if '--json' in argv:
        import json
        with open(argv[argv.index('--json') + 1], 'w', encoding='utf-8') as f:
            json.dump({'pairs': [[a, b, c, v] for (a, b, c), v in pair.items()],
                       'same': [[a, b, v] for (a, b), v in same.items()],
                       'other': dict(other), 'levels': NAMES}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
