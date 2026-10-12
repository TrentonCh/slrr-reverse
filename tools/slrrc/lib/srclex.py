#!/usr/bin/env python3
"""srclex.py - a light tokenizer for the SLRR script dialect (.java sources).

This is an ANALYSIS lexer, not the compiler's flex scanner: it exists so that
corpus checks can count and locate tokens.  Its token classes were tuned until
the per-file token counts agree with the node counts of the compiled classes
(check_opcodes.py, section "token census"), which is the evidence that it
splits the corpus sources the way the compiler does.

    import srclex
    for tok in srclex.tokenize(text):      # text as returned by oracle.load_source
        tok.kind, tok.text, tok.line, tok.pos

Token kinds
    id      identifier
    kw      keyword (KEYWORDS)
    int     integer literal: decimal or 0x hex
    float   floating literal: has a '.', an exponent or an f/F/d/D suffix
    str     string literal, text includes the quotes
    chr     character literal, text includes the quotes
    rid     resource id literal  pack.path:0xHEXr
    op      operator or punctuation (longest match)

Comments and white space are dropped.  Lines are 1-based and count "\\n".
"""
import re
from collections import namedtuple

Token = namedtuple('Token', 'kind text line pos')

KEYWORDS = frozenset('''
    package import class interface extends implements
    public private protected static final abstract native synchronized
    new return if else for while do switch case default break continue
    this super null true false instanceof void int float
'''.split())

_OPS = ['>>>=', '<<=', '>>=', '>>>', '++', '--', '&&', '||', '==', '!=', '<=',
        '>=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<', '>>']
_OP_RE = '|'.join(re.escape(o) for o in _OPS) + r'|[-+*/%&|^~!<>=?:;,.(){}\[\]@#$\\`]'

_MASTER = re.compile(r'''
    (?P<ws>[ \t\r\n\f\v]+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])*')
  | (?P<rid>[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)*:0[xX][0-9A-Fa-f]+r(?![A-Za-z0-9_]))
  | (?P<float>(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?[fFdD]?
             |[0-9]+[eE][+-]?[0-9]+[fFdD]?
             |[0-9]+[fFdD](?![A-Za-z0-9_]))
  | (?P<int>0[xX][0-9A-Fa-f]+|[0-9]+)
  | (?P<id>[A-Za-z_$][A-Za-z0-9_$]*)
  | (?P<op>%s)
''' % _OP_RE, re.S | re.X)


class LexError(ValueError):
    pass


def tokenize(text, strict=True):
    """Yield Token for every token of the text.  strict=True raises LexError
    on a character no rule accepts (the corpus has none)."""
    pos = 0
    line = 1
    n = len(text)
    while pos < n:
        m = _MASTER.match(text, pos)
        if m is None:
            if strict:
                raise LexError('line %d: cannot tokenize %r' % (line, text[pos:pos + 20]))
            yield Token('bad', text[pos], line, pos)
            if text[pos] == '\n':
                line += 1
            pos += 1
            continue
        kind = m.lastgroup
        tx = m.group(0)
        if kind not in ('ws', 'lc', 'bc'):
            if kind == 'id' and tx in KEYWORDS:
                kind = 'kw'
            yield Token(kind, tx, line, pos)
        line += tx.count('\n')
        pos = m.end()


def tokens(text, strict=True):
    return list(tokenize(text, strict))


def int_value(tok_text):
    """Value of an int token as an unsigned 32-bit number."""
    t = tok_text
    if t[:2] in ('0x', '0X'):
        return int(t, 16) & 0xFFFFFFFF
    return int(t) & 0xFFFFFFFF


def string_value(tok_text):
    """Text of a str token after escape processing (\\n \\t \\r \\\\ \\" \\'
    \\0; any other escaped character stands for itself)."""
    body = tok_text[1:-1]
    out = []
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == '\\' and i + 1 < len(body):
            i += 1
            out.append({'n': '\n', 't': '\t', 'r': '\r', '0': '\0'}.get(body[i], body[i]))
        else:
            out.append(ch)
        i += 1
    return ''.join(out)


def rid_parts(tok_text):
    """('cars.racers.Axis', 0xFFB4) for 'cars.racers.Axis:0x0000FFB4r'."""
    pack, _, rest = tok_text.rpartition(':')
    return pack, int(rest[:-1], 16)


def rid_pack_file(pack):
    """Pack name of a rid literal as the compiler stores it in the pool:
    dots become backslashes and '.rpk' is appended."""
    return pack.replace('.', '\\') + '.rpk'
