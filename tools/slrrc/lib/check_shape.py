#!/usr/bin/env python3
"""check_shape.py - proof that the trees parser.py builds are the trees the
game's compiler built.

For every tree of every source/class pair (field initialisers and method
bodies) the postfix sequence of operators, literals, types, calls, declares
and jumps is derived from the AST (shape.method_events) and read from the
class file (shape.tree_events) and the two are compared event by event.  See
shape.py for what an event is.

    python -I check_shape.py [--json FILE] [--corpus DIR] [--show N] [--only KEY]

Exit status 0 when every tree of every consistent pair has the structure the
AST predicts.
"""
import json
import os
import sys
import time
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer    # noqa: E402
import oracle   # noqa: E402
import parser   # noqa: E402
import shape    # noqa: E402
import symbols  # noqa: E402
import tufa     # noqa: E402


def fmt(e):
    return '(' + ', '.join(repr(x) if isinstance(x, str) else str(x) for x in e) + ')'


def context(src, cls, i, width=4):
    lo = max(0, i - width)
    rows = []
    for k in range(lo, i + width + 1):
        if k >= len(src) and k >= len(cls):
            break
        a = fmt(src[k]) if k < len(src) else '-'
        b = fmt(cls[k]) if k < len(cls) else '-'
        rows.append('      %s %4d  source %-44s class %s' % ('>>' if k == i else '  ', k, a, b))
    return '\n'.join(rows)


def strip_payload(events):
    """The events without the value of literals: what is left is the
    structure alone."""
    return [(e[0], e[1]) + tuple(e[3:]) if e[0] == 'lit' else e for e in events]


_CACHE = {}


def check_heads(src_heads, cls_heads, res, locals_, st, where, examples):
    """Name paths of one tree that agrees: what the first identifier of the
    source turned out to be in the class file."""
    for i, h in enumerate(cls_heads):
        if h is None:
            continue
        s = src_heads.get(i)
        if s is None:
            continue
        first, n = s
        if first is None:
            st['bare call f(...)'] += 1
            st['bare call f(...) compiled as ' + (
                'a method ref' if h[0] == 'method' else 'a path of %d starting with %s' % (h[-1], h[0]))] += 1
            continue
        kind = h[0]
        st['name paths'] += 1
        st['name paths starting with a %s node' % kind] += 1
        full = res.resolve(first)
        segments = h[-1] if len(h) == 3 else 1
        if segments != n:
            st['name paths whose node count differs from the number of identifiers'] += 1
            if len(examples) < 40:
                examples.append('%s: %s..., %d identifiers, %d segments' % (where, first, n, segments))
        if kind == 'class':
            if full == h[1]:
                st['class prefix = the first identifier resolved as a type name'] += 1
            else:
                st['class prefix that is another class than the resolver finds'] += 1
                if len(examples) < 40:
                    examples.append('%s: %s -> class node %s, resolver %s' % (where, first, h[1], full))
        else:
            if full is not None:
                st['first identifier is also a class name but compiled as a variable'] += 1
                if len(examples) < 40:
                    examples.append('%s: %s is class %s but compiled as %s' % (where, first, full, kind))


def literal_stats(root_node, st):
    """Count the literals of a tree that agrees: their values were compared
    with the class file (floats by kind only)."""
    for n in root_node.walk():
        if n.kind != 'Literal':
            continue
        lit, tx = n.lit, n.text
        st['literals'] += 1
        st['%s literals' % lit] += 1
        if lit == 'int':
            if tx[:2] in ('0x', '0X'):
                st['int literals written in hex'] += 1
                if int(tx, 16) > 0x7FFFFFFF:
                    st['int literals above 0x7FFFFFFF (hex)'] += 1
        elif lit == 'str':
            esc = lexer.string_escapes(tx)
            if esc:
                st['str literals with an escape'] += 1
            for c in esc:
                st['escapes \\%s' % c] += 1
            if '\t' in tx:
                st['str literals holding a raw TAB'] += 1
        elif lit == 'rid':
            st['rid literals with %d hex digits' % (len(tx.rpartition(':')[2]) - 3)] += 1
        elif lit == 'float':
            st['float literals (kind compared, bits not)'] += 1


def _load(e, root):
    """(source text, blobs, class events per blob and tree), cached: the
    sensitivity check walks the corpus once per mutation."""
    r = _CACHE.get(e.key)
    if r is None:
        blobs = oracle.load_class(e, root)
        r = (oracle.load_source(e, root), blobs, [{} for _ in blobs])
        _CACHE[e.key] = r
    return r


def run(root=None, only=None, parser_class=None, consistent_only=False):
    """Compare every tree.  parser_class: a subclass of parser.Parser to
    parse with (check_sensitivity.py); None is the real grammar."""
    uni = symbols.Universe.from_index()
    stats = Counter()
    events = Counter()
    bad = []
    per_cat = {}
    type_steps = Counter({'first found in nowhere': 0,
                          'with candidates in more than one package': 0})
    head_stats = Counter({
        'name paths whose node count differs from the number of identifiers': 0,
        'class prefix that is another class than the resolver finds': 0,
        'first identifier is also a class name but compiled as a variable': 0})
    head_examples = []
    lit_stats = Counter()
    for e in oracle.iter_pairs(build=None):
        if only and only.lower() not in e.key.lower():
            continue
        if consistent_only and not e.consistent:
            continue
        cat = e.category + ('' if e.consistent else ' (stale)')
        pc = per_cat.setdefault(cat, Counter())
        pc['pairs'] += 1
        text, blobs, cls_cache = _load(e, root)
        try:
            unit = parser.parse(text, e.source, parser_class)
        except (parser.ParseError, lexer.LexError) as x:
            n = sum(len(b.trees) for b in blobs)
            stats['sources that do not parse'] += 1
            stats['trees'] += n
            stats['trees of sources that do not parse'] += n
            pc['pairs different'] += 1
            pc['trees'] += n
            pc['different'] += n
            bad.append((cat, e.key, 'does not parse: %s' % x, '', 'parse'))
            continue
        res = symbols.Resolver(unit, uni)
        models = symbols.class_models(unit, res)
        file_ok = True
        if parser_class is None:
            for n in unit.walk():
                if n.kind == 'Type' and not n.primitive:
                    if '.' in n.name:
                        c = [('dotted', n.name)]
                    else:
                        c = res.candidates(n.name)
                    type_steps['class type names'] += 1
                    type_steps['first found in ' + (c[0][0] if c else 'nowhere')] += 1
                    if len(set(full for _, full in c)) > 1:
                        type_steps['with candidates in more than one package'] += 1
        for bi, (m, b) in enumerate(zip(models, blobs)):
            jobs = []
            src_heads = {}
            for f in m.fields():
                if f.init is not None:
                    jobs.append(('field %s' % f.name, f.tree, f.declarator.line,
                                 lambda f=f: shape.initialiser_events(f.init, res, src_heads),
                                 f.init))
            for r in m.methods():
                if r.tree == tufa.NONE:
                    continue
                if symbols.has_code(r.decl):
                    jobs.append(('method %s%s' % (r.name, r.desc), r.tree, r.decl.line,
                                 lambda r=r: shape.method_events(r.decl, res, src_heads),
                                 r.decl.body))
                else:
                    jobs.append(('method %s%s' % (r.name, r.desc), r.tree, r.decl.line,
                                 lambda: [], None))
            for label, ti, line, make, ast in jobs:
                stats['trees'] += 1
                pc['trees'] += 1
                kind = label.split(' ')[0]
                stats['trees: ' + kind] += 1
                where = '%s (%s) %s, line %d, tree %d' % (e.key, m.simple_name, label, line, ti)
                try:
                    cached = cls_cache[bi].get(ti)
                    if cached is None:
                        cls_heads = []
                        cached = (shape.tree_events(b, b.trees[ti], cls_heads), cls_heads)
                        cls_cache[bi][ti] = cached
                    cls_ev, cls_heads = cached
                    src_heads.clear()
                    src_ev = make()
                except (ValueError, IndexError, KeyError) as x:
                    bad.append((cat, where, 'error: %s' % x, '', 'error'))
                    pc['different'] += 1
                    file_ok = False
                    continue
                d = shape.first_difference(src_ev, cls_ev)
                stats['events'] += len(cls_ev)
                if d is None:
                    stats['trees equal'] += 1
                    pc['equal'] += 1
                    if parser_class is None:
                        for ev in cls_ev:
                            events[ev[0] if ev[0] != 'op' else 'op ' + ev[1] + ' ' + ev[2]] += 1
                        if e.consistent:
                            check_heads(src_heads, cls_heads, res, None, head_stats,
                                        where, head_examples)
                            if ast is not None:
                                literal_stats(ast, lit_stats)
                else:
                    pc['different'] += 1
                    file_ok = False
                    how = 'structure'
                    if shape.first_difference(strip_payload(src_ev),
                                              strip_payload(cls_ev)) is None:
                        how = 'literal'
                        pc['different in a literal only'] += 1
                        stats['trees that differ in the value of a literal only'] += 1
                    bad.append((cat, where,
                                'event %d of %d / %d' % (d, len(src_ev), len(cls_ev)),
                                context(src_ev, cls_ev, d), how))
        pc['pairs equal' if file_ok else 'pairs different'] += 1
    return {'stats': stats, 'bad': bad, 'per_cat': per_cat, 'events': events,
            'type_steps': type_steps, 'head_stats': head_stats,
            'head_examples': head_examples, 'lit_stats': lit_stats}


def main(argv):
    root = None
    jpath = None
    show = 40
    only = None
    i = 1
    while i < len(argv):
        if argv[i] == '--corpus':
            root = argv[i + 1]
            i += 2
        elif argv[i] == '--json':
            jpath = argv[i + 1]
            i += 2
        elif argv[i] == '--show':
            show = int(argv[i + 1])
            i += 2
        elif argv[i] == '--only':
            only = argv[i + 1]
            i += 2
        else:
            print(__doc__)
            return 2
    t0 = time.time()
    r = run(root, only)
    s = r['stats']
    print('check_shape: postfix shape of every tree, AST against class file')
    print('corpus %s' % (root or oracle.corpus_root()))
    print()
    print('trees compared %d (field initialisers %d, methods %d), events %d'
          % (s['trees'], s['trees: field'], s['trees: method'], s['events']))
    print('trees with the same shape: %d of %d' % (s['trees equal'], s['trees']))
    print()
    print('%-20s %6s %12s %7s %7s %10s %22s' % (
        'category', 'pairs', 'pairs equal', 'trees', 'equal', 'different',
        'of these: literal only'))
    fail = 0
    for cat in sorted(r['per_cat']):
        c = r['per_cat'][cat]
        print('%-20s %6d %12d %7d %7d %10d %22d' % (
            cat, c['pairs'], c['pairs equal'], c['trees'], c['equal'], c['different'],
            c['different in a literal only']))
        if 'stale' not in cat:
            fail += c['different'] - c['different in a literal only']
    print()
    print('"literal only": the sequences are equal once the value of each literal is')
    print('ignored, so the structure agrees and only a string, a number or a resource')
    print('id differs.')
    print()
    if r['bad']:
        print('Trees that differ (%d):' % len(r['bad']))
        for cat, where, what, ctx, how in r['bad'][:show]:
            print('  [%s] [%s] %s: %s' % (cat, how, where, what))
            if ctx:
                print(ctx)
        if len(r['bad']) > show:
            print('  ... %d more' % (len(r['bad']) - show))
        print()
    ts = r['type_steps']
    print('Class type names in the sources of the pairs (declarations and bodies): %d'
          % ts['class type names'])
    for k in sorted(ts):
        if k != 'class type names':
            print('  %-45s %d' % (k, ts[k]))
    print()
    hs = r['head_stats']
    print('Name paths (a, a.b.c, a.b.f(...)) in the trees of consistent pairs that agree:')
    for k in sorted(hs):
        print('  %8d  %s' % (hs[k], k))
    for x in r['head_examples'][:12]:
        print('      %s' % x)
    print()
    ls = r['lit_stats']
    print('Literals in the trees of consistent pairs that agree (value compared):')
    for k in sorted(ls):
        print('  %8d  %s' % (ls[k], k))
    print()
    print('Events of the trees that agree, by kind:')
    for k, n in sorted(r['events'].items()):
        print('  %8d  %s' % (n, k))
    print()
    print('%d of %d trees agree; %d trees of consistent pairs differ in structure (%.1f s)'
          % (s['trees equal'], s['trees'], fail, time.time() - t0))
    if jpath:
        doc = {'stats': dict(s),
               'per_category': {k: dict(v) for k, v in r['per_cat'].items()},
               'different': [{'category': c, 'where': w, 'what': x, 'context': ctx,
                              'how': how}
                             for c, w, x, ctx, how in r['bad']],
               'type_steps': dict(r['type_steps']),
               'head_stats': dict(r['head_stats']),
               'lit_stats': dict(r['lit_stats']),
               'head_examples': r['head_examples'],
               'events': dict(r['events'])}
        with open(jpath, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    return 0 if fail == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
