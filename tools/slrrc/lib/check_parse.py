#!/usr/bin/env python3
"""check_parse.py - proof that lexer.py and parser.py accept the whole corpus.

    python -I check_parse.py [--json FILE] [--corpus DIR]

For every one of the 3,454 sources:

  L1  the lexer tokenizes it (no character outside a token, a comment or
      white space);
  L2  tokens, comments and white space tile the file: every byte belongs to
      exactly one of them;
  L3  the token stream equals the one of srclex.py, the independent light
      tokenizer of the foundation whose token counts were matched against the
      node counts of the class files;
  P1  the parser accepts it;
  P2  the parser consumed every token, and the token ranges of the AST nodes
      nest (a child lies inside its parent, siblings do not overlap);
  P3  the tokens regenerated from the AST (unparse.py) equal the tokens read:
      the AST loses nothing but comments and white space;
  P4  the text regenerated from the AST parses again to the same AST;
  P5  package and top-level types agree with the corpus index (the light scan
      of oracle.py, which was itself checked against the blob names).

And a measurement that the parser is strict, not merely tolerant:

  N1  a source with one of its  ; { } ( ) [ ]  tokens blanked out is rejected.
      Sample: every 7th source, every 11th such token.  No source has a
      redundant one of these tokens, so a deletion that is accepted must
      leave another valid program; those are listed (one exists:
      "x = a.value" + "((T)b).f(...);" without the ";" between them reads as
      the call a.value(...)).  N1 is reported, it does not decide the exit
      status.

Failures are listed with file, line, column and the construct the parser was
in.  Exit status 0 when every rule holds for every source.
"""
import json
import os
import sys
import time
from collections import Counter, OrderedDict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer    # noqa: E402
import oracle   # noqa: E402
import parser   # noqa: E402
import srclex   # noqa: E402
import unparse  # noqa: E402

STRICT_TOKENS = frozenset((';', '{', '}', '(', ')', '[', ']'))
SAMPLE_SOURCE = 7
SAMPLE_TOKEN = 11


def strictness(text, toks, rel, counter):
    """Blank out sampled punctuation tokens one at a time; return the list of
    deletions the parser accepted."""
    accepted = []
    k = 0
    for t in toks:
        if t.kind != 'op' or t.text not in STRICT_TOKENS:
            continue
        k += 1
        if k % SAMPLE_TOKEN:
            continue
        counter['deletions tried'] += 1
        damaged = text[:t.pos] + ' ' + text[t.pos + 1:]
        try:
            parser.parse(damaged, rel)
        except (parser.ParseError, lexer.LexError):
            counter['deletions rejected'] += 1
            continue
        accepted.append('%s:%d:%d: %r' % (rel, t.line, t.col, t.text))
    return accepted


RULES = OrderedDict([
    ('L1', 'the lexer tokenizes the source'),
    ('L2', 'tokens, comments and white space tile the file'),
    ('L3', 'same token stream as srclex.py'),
    ('P1', 'the parser accepts the source'),
    ('P2', 'every token consumed; token ranges of the nodes nest'),
    ('P3', 'tokens regenerated from the AST = tokens read'),
    ('P4', 'the regenerated text parses to the same AST'),
    ('P5', 'package and top-level types = corpus index'),
])

WS = ' \t\r\n\f\v'


def tiles(text, toks, comments):
    """True when tokens and comments are disjoint, in order, and everything
    between them is white space."""
    spans = [(t.pos, t.pos + len(t.text)) for t in toks if t.kind != 'eof']
    spans += [(c.pos, c.pos + len(c.text)) for c in comments]
    spans.sort()
    pos = 0
    for a, b in spans:
        if a < pos:
            return False
        if text[pos:a].strip(WS):
            return False
        pos = b
    return not text[pos:].strip(WS)


def nests(node):
    """Token ranges: children inside the parent, in order, not overlapping."""
    stack = [node]
    while stack:
        n = stack.pop()
        last = n.tok0 - 1
        kids = sorted(n.children(), key=lambda c: c.tok0)
        for c in kids:
            if c.tok0 > c.tok1 or c.tok0 < n.tok0 or c.tok1 > n.tok1:
                return False
            if c.tok0 <= last:
                return False
            last = c.tok1
            stack.append(c)
    return True


def plain(node):
    """Structure of an AST without positions, for comparing two parses."""
    d = node.to_dict()

    def strip(x):
        if isinstance(x, dict):
            return {k: strip(v) for k, v in x.items()
                    if k not in ('line', 'op_tok', 'name_tok')}
        if isinstance(x, list):
            return [strip(v) for v in x]
        return x
    return strip(d)


def run(root=None):
    good = Counter()
    total = Counter()
    bad = {}
    stats = Counter()
    constructs = Counter()

    def check(rule, ok, rel, detail=''):
        total[rule] += 1
        if ok:
            good[rule] += 1
        else:
            bad.setdefault(rule, []).append('%s%s' % (rel, (': ' + detail) if detail else ''))
        return ok

    index = dict((e.source, e) for e in oracle.iter_entries() if e.source)
    accepted = []
    for si, rel in enumerate(oracle.find_source_files(root)):
        stats['sources'] += 1
        text = oracle.load_source(rel, root)
        stats['bytes'] += len(text)
        comments = []
        try:
            toks = lexer.tokenize(text, comments, rel)
        except lexer.LexError as x:
            check('L1', False, rel, 'line %d column %d: %s' % (x.line, x.col, x.message))
            continue
        check('L1', True, rel)
        stats['tokens'] += len(toks) - 1
        stats['comments'] += len(comments)
        check('L2', tiles(text, toks, comments), rel)
        try:
            ref = [(t.kind, t.text, t.line) for t in srclex.tokenize(text)]
            mine = [(t.kind, t.text, t.line) for t in toks[:-1]]
            check('L3', ref == mine, rel)
        except srclex.LexError as x:
            check('L3', False, rel, str(x))
        try:
            unit = parser.parse(text, rel)
        except parser.ParseError as x:
            constructs[x.construct or '?'] += 1
            check('P1', False, rel, 'line %d column %d: %s at %r, in: %s' % (
                x.line, x.col, x.message, x.tok.text, x.construct or '?'))
            continue
        check('P1', True, rel)
        for n in unit.walk():
            stats['nodes'] += 1
        check('P2', unit.tok0 == 0 and unit.tok1 == len(toks) - 2 and nests(unit), rel)
        regen = unparse.tokens(unit)
        orig = [t.text for t in toks[:-1]]
        ok3 = regen == orig
        if not ok3:
            k = 0
            while k < min(len(regen), len(orig)) and regen[k] == orig[k]:
                k += 1
            detail = 'token %d (line %d): read %r, regenerated %r' % (
                k, toks[min(k, len(toks) - 1)].line, orig[k:k + 4], regen[k:k + 4])
        check('P3', ok3, rel, '' if ok3 else detail)
        try:
            again = parser.parse(unparse.text(unit), rel)
            check('P4', plain(again) == plain(unit), rel)
        except (parser.ParseError, lexer.LexError) as x:
            check('P4', False, rel, str(x))
        if si % SAMPLE_SOURCE == 0:
            stats['sources sampled for N1'] += 1
            accepted.extend(strictness(text, toks, rel, stats))
        e = index.get(rel)
        if e is not None:
            want = [(d['kind'], d['name'], d['extends'], list(d['implements']),
                     list(d['modifiers'])) for d in e.declared]
            have = [('interface' if c.interface else 'class', c.name,
                     (c.extends_list[0] if len(c.extends_list) == 1 else c.extends_list)
                     if c.extends_list else None,
                     list(c.implements), list(c.modifiers)) for c in unit.types]
            check('P5', e.package == unit.package and want == have, rel,
                  '%s %s / %s %s' % (unit.package, have, e.package, want))
        else:
            check('P5', False, rel, 'not in the index')
    return {'good': good, 'total': total, 'bad': bad, 'stats': stats,
            'constructs': constructs, 'accepted': accepted}


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
    s = r['stats']
    print('check_parse: lexer.py and parser.py over every source of the corpus')
    print('corpus %s' % (root or oracle.corpus_root()))
    print()
    print('sources %d, bytes %d, tokens %d, comments %d, AST nodes %d' % (
        s['sources'], s['bytes'], s['tokens'], s['comments'], s['nodes']))
    print()
    failed = 0
    for rule, text in RULES.items():
        g, t = r['good'][rule], r['total'][rule]
        ok = g == t and t == s['sources']
        if not ok:
            failed += 1
        print('%s %-3s %5d of %-5d %s' % ('ok  ' if ok else 'FAIL', rule, g, s['sources'], text))
        for x in r['bad'].get(rule, [])[:50]:
            print('          failure: %s' % x)
    print('info N1   %5d of %-5d a source with one ; { } ( ) [ ] token blanked out is '
          'rejected (%d sources sampled)' % (s['deletions rejected'], s['deletions tried'],
                                             s['sources sampled for N1']))
    for x in r['accepted'][:30]:
        print('          still a valid program without: %s' % x)
    print()
    n = s['sources']
    print('tokenize rate %d of %d (%.2f%%), parse rate %d of %d (%.2f%%)' % (
        r['good']['L1'], n, 100.0 * r['good']['L1'] / n,
        r['good']['P1'], n, 100.0 * r['good']['P1'] / n))
    if r['constructs']:
        print('parse failures by construct: %s' % dict(r['constructs']))
    print('%d of %d rules hold for every source (%.1f s)' % (
        len(RULES) - failed, len(RULES), time.time() - t0))
    if jpath:
        doc = {'rules': [{'id': k, 'text': v, 'holds': r['good'][k], 'of': s['sources'],
                          'failures': r['bad'].get(k, [])} for k, v in RULES.items()],
               'stats': dict(s)}
        doc['rules'].append({
            'id': 'N1', 'holds': s['deletions rejected'], 'of': s['deletions tried'],
            'text': 'a source with one of its ; { } ( ) [ ] tokens blanked out is rejected '
                    '(every %dth source, every %dth such token)' % (SAMPLE_SOURCE, SAMPLE_TOKEN),
            'failures': r['accepted']})
        with open(jpath, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1, sort_keys=True)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
