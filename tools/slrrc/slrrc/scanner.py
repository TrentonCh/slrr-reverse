"""scanner.py - the tokens of one source text.

    toks = scan(source, diags)            # list of Token, the last one is T_EOF
    toks = scan(source, diags, comments)  # comments: list that receives (pos, length)

The source is a str with one character per byte of the file (latin-1), CR LF
kept; nothing is decoded (compiler_spec.md, "Encoding").  Lines are counted by
LF, CR is white space.  The rules are those of notes/grammar.md section 2,
which hold for all 852,211 tokens of the shipped sources.

A Token is (kind, pos, length, line); its text is source[pos:pos + length].
Every keyword and every operator has a kind of its own, so the parser never
compares text.

The scanner stops at the first error (E_LEX, or E_UNSUPPORTED for a literal
form the dialect has no rule for) and returns the tokens read so far, closed
by T_EOF; the caller looks at diags.errors.

Literal values are functions of the token text:
    int_value(text)      unsigned 32-bit value (wraps modulo 2**32)
    string_value(text)   text after escape processing
    rid_parts(text)      (pack name as written, local id)
    float_digits(text)   (integer digits, fraction digits)

NUMBERS.  Offsets, lengths and lines are unsigned (C++: uint32_t / size_t).
"Not found" is never a negative number: a search answers the length of the
text (_find), and a function that returns an offset behind something answers
0 when there is nothing (_rid_tail, _string_end; no token ends at offset 0).

C++ (sc_lexer.cpp): struct Token { uint8_t kind; uint32_t pos, len, line; };
the character sets below become 256-entry tables.
"""
from .diag import E_LEX, E_LIMIT, E_UNSUPPORTED, W_BYTE

# ---------------------------------------------------------------------------
# token kinds
# ---------------------------------------------------------------------------

T_EOF = 0
T_ID = 1
T_INT = 2
T_FLOAT = 3
T_STR = 4
T_RID = 5

# keywords (all 35 words the scanner reserves; notes/grammar.md 2.3)
KW_PACKAGE = 10
KW_IMPORT = 11
KW_CLASS = 12
KW_INTERFACE = 13
KW_EXTENDS = 14
KW_IMPLEMENTS = 15
KW_PUBLIC = 16
KW_PRIVATE = 17
KW_PROTECTED = 18
KW_STATIC = 19
KW_FINAL = 20
KW_ABSTRACT = 21
KW_NATIVE = 22
KW_SYNCHRONIZED = 23
KW_NEW = 24
KW_RETURN = 25
KW_IF = 26
KW_ELSE = 27
KW_FOR = 28
KW_WHILE = 29
KW_DO = 30
KW_SWITCH = 31
KW_CASE = 32
KW_DEFAULT = 33
KW_BREAK = 34
KW_CONTINUE = 35
KW_THIS = 36
KW_SUPER = 37
KW_NULL = 38
KW_TRUE = 39
KW_FALSE = 40
KW_INSTANCEOF = 41
KW_VOID = 42
KW_INT = 43
KW_FLOAT = 44

# operators and punctuation (notes/grammar.md 2.8)
P_SEMI = 50             # ;
P_COMMA = 51            # ,
P_DOT = 52              # .
P_LPAREN = 53           # (
P_RPAREN = 54           # )
P_LBRACE = 55           # {
P_RBRACE = 56           # }
P_LBRACKET = 57         # [
P_RBRACKET = 58         # ]
P_ASSIGN = 59           # =
P_ADD_ASSIGN = 60       # +=
P_SUB_ASSIGN = 61       # -=
P_MUL_ASSIGN = 62       # *=
P_DIV_ASSIGN = 63       # /=
P_MOD_ASSIGN = 64       # %=
P_AND_ASSIGN = 65       # &=
P_OR_ASSIGN = 66        # |=
P_XOR_ASSIGN = 67       # ^=
P_SHL_ASSIGN = 68       # <<=
P_SHR_ASSIGN = 69       # >>=
P_USHR_ASSIGN = 70      # >>>=
P_QUESTION = 71         # ?
P_COLON = 72            # :
P_LOR = 73              # ||
P_LAND = 74             # &&
P_OR = 75               # |
P_XOR = 76              # ^
P_AND = 77              # &
P_EQ = 78               # ==
P_NE = 79               # !=
P_LT = 80               # <
P_GT = 81               # >
P_LE = 82               # <=
P_GE = 83               # >=
P_SHL = 84              # <<
P_SHR = 85              # >>
P_USHR = 86             # >>>
P_ADD = 87              # +
P_SUB = 88              # -
P_MUL = 89              # *
P_DIV = 90              # /
P_MOD = 91              # %
P_NOT = 92              # !
P_BITNOT = 93           # ~
P_INC = 94              # ++
P_DEC = 95              # --

KEYWORDS = {
    'package': KW_PACKAGE, 'import': KW_IMPORT, 'class': KW_CLASS,
    'interface': KW_INTERFACE, 'extends': KW_EXTENDS, 'implements': KW_IMPLEMENTS,
    'public': KW_PUBLIC, 'private': KW_PRIVATE, 'protected': KW_PROTECTED,
    'static': KW_STATIC, 'final': KW_FINAL, 'abstract': KW_ABSTRACT,
    'native': KW_NATIVE, 'synchronized': KW_SYNCHRONIZED, 'new': KW_NEW,
    'return': KW_RETURN, 'if': KW_IF, 'else': KW_ELSE, 'for': KW_FOR,
    'while': KW_WHILE, 'do': KW_DO, 'switch': KW_SWITCH, 'case': KW_CASE,
    'default': KW_DEFAULT, 'break': KW_BREAK, 'continue': KW_CONTINUE,
    'this': KW_THIS, 'super': KW_SUPER, 'null': KW_NULL, 'true': KW_TRUE,
    'false': KW_FALSE, 'instanceof': KW_INSTANCEOF, 'void': KW_VOID,
    'int': KW_INT, 'float': KW_FLOAT,
}

OPERATORS = {
    ';': P_SEMI, ',': P_COMMA, '.': P_DOT, '(': P_LPAREN, ')': P_RPAREN,
    '{': P_LBRACE, '}': P_RBRACE, '[': P_LBRACKET, ']': P_RBRACKET,
    '=': P_ASSIGN, '+=': P_ADD_ASSIGN, '-=': P_SUB_ASSIGN, '*=': P_MUL_ASSIGN,
    '/=': P_DIV_ASSIGN, '%=': P_MOD_ASSIGN, '&=': P_AND_ASSIGN, '|=': P_OR_ASSIGN,
    '^=': P_XOR_ASSIGN, '<<=': P_SHL_ASSIGN, '>>=': P_SHR_ASSIGN,
    '>>>=': P_USHR_ASSIGN, '?': P_QUESTION, ':': P_COLON, '||': P_LOR,
    '&&': P_LAND, '|': P_OR, '^': P_XOR, '&': P_AND, '==': P_EQ, '!=': P_NE,
    '<': P_LT, '>': P_GT, '<=': P_LE, '>=': P_GE, '<<': P_SHL, '>>': P_SHR,
    '>>>': P_USHR, '+': P_ADD, '-': P_SUB, '*': P_MUL, '/': P_DIV, '%': P_MOD,
    '!': P_NOT, '~': P_BITNOT, '++': P_INC, '--': P_DEC,
}

# kind -> name, for dumps and messages
KIND_NAMES = {T_EOF: 'eof', T_ID: 'id', T_INT: 'int', T_FLOAT: 'float',
              T_STR: 'str', T_RID: 'rid'}
# kind -> the one text a keyword or operator token has
FIXED_TEXT = {}
for _text, _kind in KEYWORDS.items():
    KIND_NAMES[_kind] = 'kw'
    FIXED_TEXT[_kind] = _text
for _text, _kind in OPERATORS.items():
    KIND_NAMES[_kind] = 'op'
    FIXED_TEXT[_kind] = _text

MAX_LINE = 0xFFFF           # the line of a tree node is a u16

_WHITE = frozenset(' \t\r\f\v')         # LF is handled apart: it counts lines
_DIGITS = frozenset('0123456789')
_HEX = frozenset('0123456789ABCDEFabcdef')
_ID_START = frozenset('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_')
_ID_CHAR = frozenset('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789')
_FLOAT_SUFFIX = frozenset('fFdD')


class Token(object):
    __slots__ = ('kind', 'pos', 'length', 'line')

    def __init__(self, kind, pos, length, line):
        self.kind = kind
        self.pos = pos              # offset of the first character
        self.length = length
        self.line = line            # 1-based line of the first character


def column_at(source, pos):
    """1-based column of the character at pos (a TAB counts as one)."""
    k = pos
    while k > 0 and source[k - 1] != '\n':
        k -= 1
    return pos - k + 1


def _find(source, what, pos):
    """Offset of the first occurrence of `what` at or behind pos; the length
    of the source when there is none.  (C++: std::string::find, with npos
    turned into the length.)"""
    k = source.find(what, pos)
    if k < 0:
        return len(source)
    return k


# ---------------------------------------------------------------------------
# the scanner
# ---------------------------------------------------------------------------

def scan(source, diags, comments=None):
    """Tokens of the source, closed by a T_EOF token."""
    toks = []
    n = len(source)
    pos = 0
    line = 1
    chain_end = 0               # end of the dotted name looked at last
    errors_before = diags.errors
    if source.startswith('\xef\xbb\xbf'):
        diags.warning(W_BYTE, 1, 1, 'UTF-8 byte order mark skipped')
        pos = 3
    while pos < n and diags.errors == errors_before:
        ch = source[pos]
        if ch == '\n':
            line += 1
            pos += 1
            continue
        if ch in _WHITE:
            pos += 1
            continue
        if ch in _ID_START:
            end = pos + 1
            while end < n and source[end] in _ID_CHAR:
                end += 1
            rid_end = 0
            if end > chain_end:
                # A resource id is a dotted name with ":0x..r" behind it.  A
                # dotted name without that tail holds no resource id, so the
                # identifiers inside it are not looked at again.
                chain_end = _dotted_end(source, end, n)
                rid_end = _rid_tail(source, chain_end, n)
            if rid_end > 0:
                _check_rid(source, pos, rid_end, line, diags)
                toks.append(Token(T_RID, pos, rid_end - pos, line))
                pos = rid_end
                continue
            kind = KEYWORDS.get(source[pos:end], T_ID)
            toks.append(Token(kind, pos, end - pos, line))
            pos = end
            continue
        if ch in _DIGITS or (ch == '.' and pos + 1 < n and source[pos + 1] in _DIGITS):
            kind, end = _number(source, pos, n)
            if kind == T_INT:
                _check_int(source, pos, end, line, diags)
            elif kind == T_FLOAT:
                _check_float(source, pos, end, line, diags)
            elif ch in _DIGITS:
                end = pos
                while end < n and end < pos + 40 and (source[end] in _ID_CHAR or
                                                      source[end] == '.'):
                    end += 1
                diags.error(E_LEX, line, column_at(source, pos),
                            'malformed number %s' % _show(source[pos:end]))
                break
            if kind != T_EOF:
                toks.append(Token(kind, pos, end - pos, line))
                pos = end
                continue
            # a '.' that starts no number is the operator
        if ch == '/' and pos + 1 < n:
            nx = source[pos + 1]
            if nx == '/':
                end = _find(source, '\n', pos)
                _comment(source, pos, end, line, diags, comments)
                pos = end
                continue
            if nx == '*':
                end = _find(source, '*/', pos + 2)
                if end == n:
                    diags.error(E_LEX, line, column_at(source, pos),
                                'block comment is not closed')
                    break
                end += 2
                _comment(source, pos, end, line, diags, comments)
                line += source.count('\n', pos, end)
                pos = end
                continue
        if ch == '"':
            end = _string_end(source, pos, n)
            if end == 0:
                diags.error(E_LEX, line, column_at(source, pos),
                            'string literal is not closed on its line')
                break
            _check_string(source, pos, end, line, diags)
            toks.append(Token(T_STR, pos, end - pos, line))
            pos = end
            continue
        if ch == "'":
            diags.error(E_UNSUPPORTED, line, column_at(source, pos),
                        'character literal (the dialect has none)')
            break
        # operators, longest match first
        kind = T_EOF
        length = 4 if pos + 4 <= n else n - pos
        while length > 0:
            kind = OPERATORS.get(source[pos:pos + length], T_EOF)
            if kind != T_EOF:
                break
            length -= 1
        if kind == T_EOF:
            diags.error(E_LEX, line, column_at(source, pos),
                        'character %s is not part of any token' % _show(ch))
            break
        toks.append(Token(kind, pos, length, line))
        pos += length
    if diags.errors == errors_before and toks and toks[len(toks) - 1].line > MAX_LINE:
        # lines only grow, so the last token tells; what follows it (white
        # space, comments) stamps no node
        t = toks[len(toks) - 1]
        diags.error(E_LIMIT, t.line, column_at(source, t.pos),
                    'token behind line %d (the line of a node is 16 bits wide)' % MAX_LINE)
    toks.append(Token(T_EOF, pos if pos < n else n, 0, line))
    return toks


def _show(text):
    """A piece of source for a message: quoted, non-printing bytes escaped."""
    out = ['"']
    for ch in text:
        o = ord(ch)
        if ch == '"' or ch == '\\':
            out.append('\\' + ch)
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append('\\x%02x' % o)
    out.append('"')
    return ''.join(out)


def _comment(source, pos, end, line, diags, comments):
    if comments is not None:
        comments.append((pos, end - pos))
    if not source[pos:end].isascii():
        diags.warning(W_BYTE, line, column_at(source, pos),
                      'byte above 0x7F in a comment')


def _dotted_end(source, end, n):
    """end is the offset behind an identifier; returns the offset behind
    the  { "." Identifier }  that follows it without white space."""
    k = end
    while k + 1 < n and source[k] == '.' and source[k + 1] in _ID_START:
        k += 2
        while k < n and source[k] in _ID_CHAR:
            k += 1
    return k


def _rid_tail(source, k, n):
    """A resource id literal is  Identifier { "." Identifier } ":" "0" ("x" |
    "X") HexDigit { HexDigit } "r"  with no identifier character behind it.
    k is the offset behind the dotted name; returns the offset behind the
    literal, or 0 when no such tail follows."""
    if not (k + 2 < n and source[k] == ':' and source[k + 1] == '0' and
            (source[k + 2] == 'x' or source[k + 2] == 'X')):
        return 0
    h = k + 3
    while h < n and source[h] in _HEX:
        h += 1
    if h == k + 3 or h >= n or source[h] != 'r':
        return 0
    if h + 1 < n and source[h + 1] in _ID_CHAR:
        return 0
    return h + 1


def _string_end(source, pos, n):
    """Offset behind the closing quote of the string literal that opens at
    pos, or 0 when the line or the file ends first.  A backslash takes the
    next character with it."""
    j = pos + 1
    while j < n:
        ch = source[j]
        if ch == '"':
            return j + 1
        if ch == '\n':
            return 0
        if ch == '\\':
            if j + 1 >= n or source[j + 1] == '\n':
                return 0
            j += 2
        else:
            j += 1
    return 0


def _exponent_end(source, j, n):
    """Offset behind  ("e" | "E") ["+" | "-"] Digit { Digit }  at j, or j."""
    if j < n and (source[j] == 'e' or source[j] == 'E'):
        k = j + 1
        if k < n and (source[k] == '+' or source[k] == '-'):
            k += 1
        d = k
        while k < n and source[k] in _DIGITS:
            k += 1
        if k > d:
            return k
    return j


def _float_tail_ok(source, j, n):
    """Nothing that could continue a number stands at j."""
    return not (j < n and (source[j] in _ID_CHAR or source[j] == '.'))


def _number(source, pos, n):
    """(kind, end) of the number at pos: T_FLOAT, T_INT, or (T_EOF, pos)
    when no rule matches.  The alternatives are tried in this order, each
    with its longest match:
        digits "." [digits] [exponent] [suffix]  |  "." digits [exponent] [suffix]
        digits exponent [suffix]
        digits suffix
        "0" ("x" | "X") hex digits
        digits
    A float must not be followed by a letter, digit, "_" or "."; an int not
    by a letter, digit or "_".  The corpus uses  digits "." digits ["f"],
    digits "f"  and both int forms only."""
    i = pos
    while i < n and source[i] in _DIGITS:
        i += 1
    j = 0                               # offset behind "." digits; 0: no point
    if i > pos:
        if i < n and source[i] == '.':
            j = i + 1
            while j < n and source[j] in _DIGITS:
                j += 1
    else:
        j = pos + 1                     # source[pos] is ".", a digit follows
        while j < n and source[j] in _DIGITS:
            j += 1
    if j > 0:
        j = _exponent_end(source, j, n)
        if j < n and source[j] in _FLOAT_SUFFIX:
            j += 1
        if _float_tail_ok(source, j, n):
            return T_FLOAT, j
    if i == pos:
        return T_EOF, pos
    j = _exponent_end(source, i, n)
    if j > i:
        if j < n and source[j] in _FLOAT_SUFFIX:
            j += 1
        if _float_tail_ok(source, j, n):
            return T_FLOAT, j
    if i < n and source[i] in _FLOAT_SUFFIX and _float_tail_ok(source, i + 1, n):
        return T_FLOAT, i + 1
    if source[pos] == '0' and pos + 1 < n and (source[pos + 1] == 'x' or source[pos + 1] == 'X'):
        j = pos + 2
        while j < n and source[j] in _HEX:
            j += 1
        if j > pos + 2 and not (j < n and source[j] in _ID_CHAR):
            return T_INT, j
    if not (i < n and source[i] in _ID_CHAR):
        return T_INT, i
    return T_EOF, pos


# ---------------------------------------------------------------------------
# literal forms no shipped source uses
# ---------------------------------------------------------------------------

def _check_int(source, pos, end, line, diags):
    text = source[pos:end]
    if text[:2] == '0x' or text[:2] == '0X':
        digits = text[2:].lstrip('0')
        if len(digits) > 8:
            diags.unverified(line, column_at(source, pos),
                             'int literal above 32 bits, taken modulo 2**32')
        return
    if len(text) > 1 and text[0] == '0':
        diags.unverified(line, column_at(source, pos),
                         'decimal int literal with a leading zero, read as decimal')
    digits = text.lstrip('0')
    if len(digits) > 10 or (len(digits) == 10 and digits > '2147483647'):
        diags.unverified(line, column_at(source, pos),
                         'decimal int literal above 2147483647, taken modulo 2**32')


def _check_float(source, pos, end, line, diags):
    text = source[pos:end]
    col = column_at(source, pos)
    last = text[len(text) - 1]
    if last == 'd' or last == 'D':
        diags.error(E_UNSUPPORTED, line, col, 'float literal with suffix %s' % last)
        return
    body = text[:len(text) - 1] if (last == 'f' or last == 'F') else text
    if 'e' in body or 'E' in body:
        diags.error(E_UNSUPPORTED, line, col, 'float literal with an exponent')
        return
    if last == 'F':
        diags.unverified(line, col, 'float literal with suffix F')
    if body[0] == '.':
        diags.unverified(line, col, 'float literal without digits before the point')
    elif body[len(body) - 1] == '.':
        diags.unverified(line, col, 'float literal without digits after the point')


def _check_string(source, pos, end, line, diags):
    text = source[pos:end]
    if not text.isascii():
        diags.warning(W_BYTE, line, column_at(source, pos),
                      'byte above 0x7F in a string literal')
    # A control character other than TAB written raw into a literal (51
    # shipped literals hold a TAB, none any other): a NUL would end the utf8
    # entry early for a reader that goes by the terminator.
    for ch in text:
        o = ord(ch)
        if (o < 0x20 and ch != '\t') or o == 0x7F:
            diags.unverified(line, column_at(source, pos),
                             'control character 0x%02X in a string literal' % o)
            break
    i = 1
    last = len(text) - 1
    while i < last:
        if text[i] == '\\' and i + 1 < last:
            i += 1
            e = text[i]
            if not (e == 'n' or e == 't' or e == '"' or e == '\\'):
                diags.unverified(line, column_at(source, pos),
                                 'escape \\%s in a string literal' % e)
        i += 1


def _check_rid(source, pos, end, line, diags):
    text = source[pos:end]
    digits = text[text.rfind(':') + 3:len(text) - 1].lstrip('0')
    if len(digits) > 8:
        diags.unverified(line, column_at(source, pos),
                         'resource id above 32 bits, taken modulo 2**32')


# ---------------------------------------------------------------------------
# literal values
# ---------------------------------------------------------------------------

_HEX_VALUE = {}
for _i, _c in enumerate('0123456789abcdef'):
    _HEX_VALUE[_c] = _i
    _HEX_VALUE[_c.upper()] = _i


def int_value(text):
    """Value of an int token as an unsigned 32-bit number: the literal modulo
    2**32, decimal or hexadecimal (25,940 of 25,940 literals of the pairs)."""
    v = 0
    if text[:2] == '0x' or text[:2] == '0X':
        for ch in text[2:]:
            v = ((v << 4) | _HEX_VALUE[ch]) & 0xFFFFFFFF
        return v
    for ch in text:
        v = (v * 10 + _HEX_VALUE[ch]) & 0xFFFFFFFF
    return v


def string_value(text):
    """Text of a str token after escape processing.  Proven: \\n is LF, \\t is
    TAB, \\" and \\\\ stand for the character (12,753 of 12,753 literals).
    Unproven (W_UNVERIFIED at scan time): \\r is CR, \\0 is NUL, any other
    escaped character stands for itself."""
    out = []
    i = 1
    last = len(text) - 1
    while i < last:
        ch = text[i]
        if ch == '\\' and i + 1 < last:
            i += 1
            e = text[i]
            if e == 'n':
                out.append('\n')
            elif e == 't':
                out.append('\t')
            elif e == 'r':
                out.append('\r')
            elif e == '0':
                out.append('\0')
            else:
                out.append(e)
        else:
            out.append(ch)
        i += 1
    return ''.join(out)


def rid_parts(text):
    """('cars.racers.Axis', 0xFFB4) for 'cars.racers.Axis:0x0000FFB4r': the
    pack name as written and the local id (modulo 2**32)."""
    colon = text.rfind(':')
    v = 0
    for ch in text[colon + 3:len(text) - 1]:
        v = ((v << 4) | _HEX_VALUE[ch]) & 0xFFFFFFFF
    return text[:colon], v


def float_digits(text):
    """('12', '50') for '12.50f': the digits before and behind the point,
    without the suffix.  Either part may be empty."""
    n = len(text)
    if text[n - 1] == 'f' or text[n - 1] == 'F':
        n -= 1
    point = 0
    while point < n and text[point] != '.':
        point += 1
    if point == n:
        return text[:n], ''
    return text[:point], text[point + 1:n]
