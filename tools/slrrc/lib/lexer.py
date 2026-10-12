#!/usr/bin/env python3
"""lexer.py - tokenizer for the SLRR script dialect (the .java sources of
Street Legal Racing: Redline, Build 940).

This is the scanner of the compiler prototype.  It is strict: a character or a
literal form that no rule accepts raises LexError with file, line and column.
All 3,454 shipped sources tokenize (check_parse.py); notes/grammar.md section 2
lists every rule with the number of tokens it matched in the corpus.

    import lexer
    toks = lexer.tokenize(text)            # list of Token, the last one is 'eof'
    toks = lexer.tokenize(text, comments)  # comments: a list that receives Comment

Token fields
    kind    'id'     identifier
            'kw'     keyword (KEYWORDS)
            'int'    integer literal, decimal or hexadecimal
            'float'  floating literal
            'str'    string literal, text includes the quotes
            'chr'    character literal (the corpus has none)
            'rid'    resource id literal  pack.path:0xHEXr
            'op'     operator or punctuation, longest match
            'eof'    end of input (text '')
    text    the exact source text of the token
    line    1-based line of the first character (lines are counted by LF)
    col     1-based column of the first character (a TAB counts as one)
    pos     0-based character offset of the first character
    index   position of the token in the list

Text is expected the way oracle.load_source returns it: one character per
byte (latin-1), CRLF kept.  The lexer treats CR as white space.

Values of literal tokens (functions of the token text):
    int_value(text)      unsigned 32-bit value of an int token
    float_parts(text)    (integer digits, fraction digits, exponent, suffix)
    string_value(text)   text of a string literal after escape processing
    rid_parts(text)      (pack name as written, local id)
    literal_form(tok)    name of the lexical form, for the census
"""
import re
from collections import namedtuple

Token = namedtuple('Token', 'kind text line col pos index')
Comment = namedtuple('Comment', 'kind text line col pos')   # kind 'line' | 'block'

# Words the scanner returns as keywords.  Every one occurs in the shipped
# sources except 'protected' and 'synchronized', which are known from class
# files without source (method flags 0x2 and 0x80, notes/foundation.md).
KEYWORDS = frozenset('''
    package import class interface extends implements
    public private protected static final abstract native synchronized
    new return if else for while do switch case default break continue
    this super null true false instanceof void int float
'''.split())

MODIFIERS = ('public', 'private', 'protected', 'static', 'final', 'abstract',
             'native', 'synchronized')
PRIMITIVES = ('int', 'float', 'void')

# Words reserved by Java 1.0 that the dialect does not use.  The lexer returns
# them as plain identifiers; the census reports any use (there is none except
# as listed in notes/grammar.md).
JAVA_RESERVED_UNUSED = frozenset('''
    boolean byte char short long double try catch finally throw throws
    volatile transient goto const byvalue cast future generic inner operator
    outer rest var
'''.split())

OPERATORS = ['>>>=', '<<=', '>>=', '>>>', '++', '--', '&&', '||', '==', '!=',
             '<=', '>=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<',
             '>>',
             '+', '-', '*', '/', '%', '&', '|', '^', '~', '!', '<', '>', '=',
             '?', ':', ';', ',', '.', '(', ')', '{', '}', '[', ']']

_OP_RE = '|'.join(re.escape(o) for o in sorted(OPERATORS, key=len, reverse=True))

# Order matters: a resource id starts like an identifier, a float like an int.
_MASTER = re.compile(r'''
    (?P<ws>[ \t\r\n\f\v]+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>"(?:\\[^\n]|[^"\\\n])*")
  | (?P<chr>'(?:\\[^\n]|[^'\\\n])*')
  | (?P<rid>[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*:0[xX][0-9A-Fa-f]+r(?![A-Za-z0-9_]))
  | (?P<float>(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?[fFdD]?(?![A-Za-z0-9_.])
             |[0-9]+[eE][+-]?[0-9]+[fFdD]?(?![A-Za-z0-9_.])
             |[0-9]+[fFdD](?![A-Za-z0-9_.]))
  | (?P<int>(?:0[xX][0-9A-Fa-f]+|[0-9]+)(?![A-Za-z0-9_]))
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>%s)
''' % _OP_RE, re.S | re.X)


class LexError(ValueError):
    def __init__(self, message, line, col, name=None):
        ValueError.__init__(self, '%s:%d:%d: %s' % (name or '<source>', line, col, message))
        self.message = message
        self.line = line
        self.col = col
        self.name = name


def tokenize(text, comments=None, name=None):
    """Return the tokens of the text as a list that ends with an 'eof' token.
    comments, when given, is a list that receives a Comment per comment."""
    out = []
    add = out.append
    pos = 0
    line = 1
    bol = 0                     # offset of the first character of the line
    n = len(text)
    match = _MASTER.match
    while pos < n:
        m = match(text, pos)
        if m is None:
            col = pos - bol + 1
            ch = text[pos]
            if text.startswith('/*', pos):
                raise LexError('block comment is not closed', line, col, name)
            if ch == '"':
                raise LexError('string literal is not closed on its line', line, col, name)
            if ch == "'":
                raise LexError('character literal is not closed on its line', line, col, name)
            if ch.isdigit() or ch == '.':
                raise LexError('malformed number %r' % text[pos:pos + 16], line, col, name)
            raise LexError('character %r is not part of any token' % ch, line, col, name)
        kind = m.lastgroup
        tx = m.group(0)
        if kind == 'ws':
            nl = tx.count('\n')
            if nl:
                line += nl
                bol = pos + tx.rindex('\n') + 1
        elif kind == 'lc':
            if comments is not None:
                comments.append(Comment('line', tx, line, pos - bol + 1, pos))
        elif kind == 'bc':
            if comments is not None:
                comments.append(Comment('block', tx, line, pos - bol + 1, pos))
            nl = tx.count('\n')
            if nl:
                line += nl
                bol = pos + tx.rindex('\n') + 1
        else:
            if kind == 'id' and tx in KEYWORDS:
                kind = 'kw'
            add(Token(kind, tx, line, pos - bol + 1, pos, len(out)))
        pos = m.end()
    add(Token('eof', '', line, pos - bol + 1, pos, len(out)))
    return out


# ---------------------------------------------------------------------------
# literal values
# ---------------------------------------------------------------------------

def int_value(tok_text):
    """Value of an int token as an unsigned 32-bit number (wraps)."""
    t = tok_text
    if t[:2] in ('0x', '0X'):
        return int(t, 16) & 0xFFFFFFFF
    return int(t) & 0xFFFFFFFF


_FLOAT = re.compile(r'([0-9]*)(?:\.([0-9]*))?(?:[eE]([+-]?[0-9]+))?([fFdD]?)$')


def float_parts(tok_text):
    """('12', '50', None, 'f') for '12.50f': integer digits, fraction digits
    (None when there is no point), exponent text or None, suffix or ''."""
    m = _FLOAT.match(tok_text)
    if m is None:
        raise ValueError('not a float literal: %r' % tok_text)
    return m.group(1), m.group(2), m.group(3), m.group(4)


# The escapes the corpus uses are \" \\ \n \t; the other entries are the usual
# C ones and are a guess (no source exercises them).
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', 'b': '\b', 'f': '\f'}


def string_value(tok_text):
    """Text of a str token after escape processing.  An escaped character
    without a meaning stands for itself."""
    body = tok_text[1:-1]
    if '\\' not in body:
        return body
    out = []
    i = 0
    n = len(body)
    while i < n:
        ch = body[i]
        if ch == '\\' and i + 1 < n:
            i += 1
            out.append(ESCAPES.get(body[i], body[i]))
        else:
            out.append(ch)
        i += 1
    return ''.join(out)


def string_escapes(tok_text):
    """The escaped characters of a str token, in order ('n', '"', ...)."""
    return re.findall(r'\\(.)', tok_text[1:-1], re.S)


def rid_parts(tok_text):
    """('cars.racers.Axis', 0xFFB4) for 'cars.racers.Axis:0x0000FFB4r'."""
    pack, _, rest = tok_text.rpartition(':')
    return pack, int(rest[:-1], 16)


def rid_pack_file(pack):
    """Pack name of a rid literal as the compiler stores it in the pool, up
    to letter case: dots become backslashes and '.rpk' is appended."""
    return pack.replace('.', '\\') + '.rpk'


def literal_form(tok):
    """Name of the lexical form of a literal token, for the census."""
    k, t = tok.kind, tok.text
    if k == 'int':
        if t[:2] == '0x':
            return 'int hex 0x'
        if t[:2] == '0X':
            return 'int hex 0X'
        if len(t) > 1 and t[0] == '0':
            return 'int decimal with leading zero'
        return 'int decimal'
    if k == 'float':
        ip, fp, ex, sf = float_parts(t)
        form = ('d' if ip else '') + ('.' if fp is not None else '') + \
               ('d' if fp else '') + ('e' if ex is not None else '') + sf
        return 'float ' + form
    if k == 'rid':
        pack, _, rest = t.rpartition(':')
        return 'rid %s, %d hex digits' % (rest[:2], len(rest) - 3)
    return k


def main(argv):
    import sys
    if len(argv) != 2:
        print('usage: python -I lexer.py FILE.java', file=sys.stderr)
        return 2
    with open(argv[1], 'rb') as f:
        text = f.read().decode('latin-1')
    comments = []
    for t in tokenize(text, comments, argv[1]):
        print('%5d:%-3d %-5s %s' % (t.line, t.col, t.kind, t.text))
    print('%d comments' % len(comments))
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv))
