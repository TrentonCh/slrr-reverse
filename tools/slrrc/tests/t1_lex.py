#!/usr/bin/env python3
"""t1_lex.py - milestone M1: every source tokenises.

    python -I tests/t1_lex.py [--only TEXT] [-v] [--update]

A source passes when
  a  slrrc.scanner reads it without a diagnostic in strict mode (no error, no
     construct outside the proven subset);
  b  tokens, comments and white space tile the file: every byte belongs to
     exactly one of them;
  c  the token stream equals the one of lib/lexer.py (the scanner the corpus
     proofs were made with): kind, position, text and line of every token;
  d  the literal values agree with lib/lexer.py: int values, string texts
     after escape processing, pack name and local id of resource ids, and the
     digits of floats.

Headline:  M1 tokenise      3454/3454 sources, 852211 tokens
(the end marker of each source is not counted).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402
import lexer    # lib/lexer.py, the oracle

from slrrc import scanner
from slrrc.diag import Diagnostics

WS = ' \t\r\n\f\v'


def tiles(text, toks, comments):
    spans = [(t.pos, t.pos + t.length) for t in toks if t.kind != scanner.T_EOF]
    spans += [(p, p + n) for p, n in comments]
    spans.sort()
    pos = 0
    for a, b in spans:
        if a < pos or text[pos:a].strip(WS):
            return False
        pos = b
    return not text[pos:].strip(WS)


def lib_kind(kind):
    return scanner.KIND_NAMES[kind]


def check(text, name):
    """(reason of failure or None, number of tokens)."""
    diags = Diagnostics(strict=True)
    comments = []
    toks = scanner.scan(text, diags, comments)
    if diags.items:
        d = diags.items[0]
        return 'line %d column %d: %s %s' % (d.line, d.column, d.code, d.text), 0
    if toks[-1].kind != scanner.T_EOF:
        return 'no end marker', 0
    n = len(toks) - 1
    if not tiles(text, toks, comments):
        return 'tokens, comments and white space do not tile the file', n
    ref_comments = []
    try:
        ref = lexer.tokenize(text, ref_comments, name)
    except lexer.LexError as x:
        return 'lib/lexer.py rejects it: %s' % x, n
    if len(ref) != len(toks):
        return '%d tokens, lib/lexer.py reads %d' % (n, len(ref) - 1), n
    for t, r in zip(toks, ref):
        tx = text[t.pos:t.pos + t.length]
        if lib_kind(t.kind) != r.kind or t.pos != r.pos or tx != r.text or t.line != r.line:
            return 'line %d: token %r (%s) / lib/lexer.py %r (%s) line %d' % (
                t.line, tx, lib_kind(t.kind), r.text, r.kind, r.line), n
        k = t.kind
        if k >= scanner.KW_PACKAGE and scanner.FIXED_TEXT[k] != tx:
            return 'line %d: kind %d does not belong to %r' % (t.line, k, tx), n
        if k == scanner.T_INT:
            if scanner.int_value(tx) != lexer.int_value(tx):
                return 'line %d: int value of %s' % (t.line, tx), n
        elif k == scanner.T_STR:
            if scanner.string_value(tx) != lexer.string_value(tx):
                return 'line %d: string value of %s' % (t.line, tx), n
        elif k == scanner.T_RID:
            if scanner.rid_parts(tx) != lexer.rid_parts(tx):
                return 'line %d: resource id %s' % (t.line, tx), n
        elif k == scanner.T_FLOAT:
            ip, fp, ex, sf = lexer.float_parts(tx)
            if scanner.float_digits(tx) != (ip, fp or '') or ex is not None:
                return 'line %d: float digits of %s' % (t.line, tx), n
    if [(p, n_) for p, n_ in comments] != [(c.pos, len(c.text)) for c in ref_comments]:
        return 'comments differ from lib/lexer.py', n
    return None, n


def run(opts):
    oc = harness.Outcome('M1 tokenise')
    entries = harness.selected(harness.all_sources(), opts)
    passed = []
    tokens = 0
    for e in entries:
        why, n = check(harness.load_source(e), e.source)
        tokens += n
        if why is None:
            passed.append(e.source)
        else:
            oc.failures.append('%s: %s' % (e.source, why))
    oc.text = '%d/%d sources, %d tokens' % (len(passed), len(entries), tokens)
    oc.ok = len(passed) == len(entries) == 3454 and tokens == 852211
    oc.passed['m1.pass'] = passed
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
