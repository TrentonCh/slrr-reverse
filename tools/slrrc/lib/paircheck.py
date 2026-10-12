#!/usr/bin/env python3
"""paircheck.py - does a class file still belong to its source?

A shipped .class was compiled from SOME version of its source.  When the
source was edited afterwards and the game never reloaded the class, the pair
is stale and useless as a byte-for-byte oracle.  Two independent checks find
such pairs without a compiler:

1. Token census.  The compiler emits exactly one node per operator, literal
   and keyword occurrence (nothing is folded or dropped; proven by
   check_opcodes.py), so per-file token counts of the source must equal
   per-file node counts of the class.  CENSUS lists the 54 token classes.

2. Line check.  The u16 of every node is the source line it came from, so the
   token a node carries must stand on that line: the text of a string
   literal, the value of an int / float / resource id literal, the name of a
   field or method (names may sit up to NAME_SLACK lines earlier: the line
   recorded is the one the parser had reached when it built the node, and a
   call can be split over lines).

    import paircheck
    r = paircheck.check_pair(source_text, blobs)
    r['consistent']      True when both checks pass
    r['census']          [(label, source count, class count), ...] disagreements
    r['lines']           {kind: count} of nodes whose token is not on the line
    r['examples']        up to 5 readable examples

This module does not import oracle (oracle imports it).
"""
import re
import struct
from collections import Counter

import srclex
import tufa

NONE = tufa.NONE
TAGGED = (0x1007, 0x1008)
NAME_SLACK = 6

# (label, key, what the class side counts)
CENSUS = [
    ('||', 'op:||', 'expr/stmt lor (tag 0x01)'),
    ('|| (tests)', 'op:||#test', '0x0005 or_test'),
    ('&&', 'op:&&', 'expr/stmt land (tag 0x02)'),
    ('&& (tests)', 'op:&&#test', '0x0004 and_test'),
    ('|', 'op:|', 'tag 0x03'),
    ('^', 'op:^', 'tag 0x04'),
    ('&', 'op:&', 'tag 0x05'),
    ('!=', 'op:!=', 'tag 0x06'),
    ('==', 'op:==', 'tag 0x07'),
    ('>=', 'op:>=', 'tag 0x08'),
    ('<=', 'op:<=', 'tag 0x09'),
    ('>', 'op:>', 'tag 0x0A'),
    ('<', 'op:<', 'tag 0x0B'),
    ('>>>', 'op:>>>', 'tag 0x0C'),
    ('>>', 'op:>>', 'tag 0x0D'),
    ('<<', 'op:<<', 'tag 0x0E'),
    ('- binary', 'op:-bin', 'tag 0x0F'),
    ('+ binary', 'op:+bin', 'tag 0x10'),
    ('%', 'op:%', 'tag 0x11'),
    ('/', 'op:/', 'tag 0x12'),
    ('* (outside import)', 'op:*', 'tag 0x13'),
    ('~', 'op:~', 'tag 0x14'),
    ('!', 'op:!', 'tag 0x15'),
    ('- unary', 'op:-un', 'tag 0x16'),
    ('+ unary', 'op:+un', 'no node'),
    ('x++', 'op:++post', 'tag 0x17'),
    ('x--', 'op:--post', 'tag 0x18'),
    ('++x', 'op:++pre', 'tag 0x19'),
    ('--x', 'op:--pre', 'tag 0x1A'),
    ('?', 'op:?', 'tag 0x00'),
    ('=', 'op:=', 'tag 0x23 + 0x002C init + field initialiser trees'),
    ('*=', 'op:*=', 'tag 0x24'),
    ('/=', 'op:/=', 'tag 0x25'),
    ('%=', 'op:%=', 'tag 0x26'),
    ('+=', 'op:+=', 'tag 0x27'),
    ('-=', 'op:-=', 'tag 0x28'),
    ('<<= >>= >>>=', 'op:shiftassign', 'tags 0x29-0x2B'),
    ('&=', 'op:&=', 'tag 0x2C'),
    ('^=', 'op:^=', 'tag 0x2D'),
    ('|=', 'op:|=', 'tag 0x2E'),
    ('string literal', 'str', '0x4009'),
    ('null', 'kw:null', '0x000C'),
    ('true / false', 'kw:bool', '0x100D'),
    ('resource id literal', 'rid', '0x100E'),
    ('int literal', 'int', '0x100B'),
    ('float literal', 'float', '0x200A'),
    ('instanceof', 'kw:instanceof', 'tag 0x20'),
    ('new', 'kw:new', 'tags 0x1E + 0x1F'),
    ('case', 'kw:case', '0x1017'),
    ('switch', 'kw:switch', '0x0018'),
    ('do', 'kw:do', '0x1016'),
    ('super.name(', 'super.call', '0x4026'),
    ('this(', 'this(', '0x0022'),
    ('[ expr ]', 'op:[idx', '0x0020 + tag 0x1F'),
    ('(Type) cast', 'cast', 'tag 0x21'),
]
CENSUS_KEYS = [k for _, k, _ in CENSUS]

# tokens after which an operator is a prefix operator
_PREFIX_AFTER_OPS = frozenset(
    '( [ { , ; = ? : ! ~ + - * / % & | ^ < > == != <= >= && || << >> >>> '
    '+= -= *= /= %= &= |= ^= <<= >>= >>>= ++ --'.split())
_PREFIX_AFTER_KW = frozenset(('return', 'case', 'new', 'instanceof', 'else', 'do'))
_OPERAND_KW = frozenset(('this', 'super', 'null', 'true', 'false', 'new'))


def _is_prefix_position(prev):
    if prev is None:
        return True
    if prev.kind == 'op':
        # after ++ / -- an operator is binary when they were postfix; the
        # corpus has no such sequence, so ++ / -- count as "operator before"
        return prev.text in _PREFIX_AFTER_OPS
    if prev.kind == 'kw':
        return prev.text in _PREFIX_AFTER_KW
    return False


def source_counts(text):
    """Token census of one source file: Counter keyed like CENSUS."""
    c = Counter()
    toks = srclex.tokens(text)
    n = len(toks)
    in_import = False
    for i, t in enumerate(toks):
        k, x = t.kind, t.text
        prev = toks[i - 1] if i else None
        if k == 'kw' and x in ('import', 'package'):
            in_import = True
        if k == 'op' and x == ';':
            in_import = False
        if k == 'op':
            if x == '*' and in_import:
                continue
            if x in ('<<=', '>>=', '>>>='):
                c['op:shiftassign'] += 1
            elif x == '[':
                # "[]" belongs to a type name; "[ expr ]" is an index or a size
                if not (i + 1 < n and toks[i + 1].text == ']'):
                    c['op:[idx'] += 1
            elif x in ('-', '+'):
                c['op:%s%s' % (x, 'un' if _is_prefix_position(prev) else 'bin')] += 1
            elif x in ('++', '--'):
                post = prev is not None and (prev.kind == 'id' or
                                             prev.text in (')', ']'))
                c['op:%s%s' % (x, 'post' if post else 'pre')] += 1
            elif x in ('&&', '||'):
                c['op:' + x] += 1
                c['op:' + x + '#test'] += 1
            elif x == '(':
                c['op:('] += 1
                # (Type) operand : a parenthesised type name in operand
                # position (not after if/while/for/switch or a callee name),
                # followed by the start of an operand
                j = i + 1
                if _is_prefix_position(prev) and j < n and (
                        toks[j].kind == 'id' or toks[j].text in ('int', 'float')):
                    j += 1
                    while j + 1 < n and toks[j].text == '.' and toks[j + 1].kind == 'id':
                        j += 2
                    while j + 1 < n and toks[j].text == '[' and toks[j + 1].text == ']':
                        j += 2
                    if j + 1 < n and toks[j].text == ')':
                        nx = toks[j + 1]
                        if nx.kind in ('id', 'int', 'float', 'str', 'rid', 'chr') or \
                                (nx.kind == 'kw' and nx.text in _OPERAND_KW) or \
                                (nx.kind == 'op' and nx.text in ('(', '!', '~')):
                            c['cast'] += 1
            else:
                c['op:' + x] += 1
        elif k == 'kw':
            if x in ('true', 'false'):
                c['kw:bool'] += 1
            else:
                c['kw:' + x] += 1
            if x == 'super' and i + 3 < n and toks[i + 1].text == '.' and \
                    toks[i + 2].kind == 'id' and toks[i + 3].text == '(':
                c['super.call'] += 1
            if x == 'this' and i + 1 < n and toks[i + 1].text == '(':
                c['this('] += 1
            if x == 'super' and i + 1 < n and toks[i + 1].text == '(':
                c['super('] += 1
        elif k == 'chr':
            c['int'] += 1
        else:
            c[k] += 1
    return c


def class_counts(blobs):
    """Node census of a class file, keyed like CENSUS."""
    c = Counter()
    for b in blobs:
        for st, i, f in b.fields():
            if f.init != NONE:
                c['field_init'] += 1
        for t in b.trees:
            for nd in t:
                code = tufa.OP_FLAGS[nd.op]
                c[code] += 1
                if code in TAGGED:
                    c[('tag', nd.imm)] += 1
    T = lambda *tags: sum(c[('tag', t)] for t in tags)
    return {
        'op:||': T(0x01), 'op:||#test': c[0x0005],
        'op:&&': T(0x02), 'op:&&#test': c[0x0004],
        'op:|': T(0x03), 'op:^': T(0x04), 'op:&': T(0x05), 'op:!=': T(0x06),
        'op:==': T(0x07), 'op:>=': T(0x08), 'op:<=': T(0x09), 'op:>': T(0x0A),
        'op:<': T(0x0B), 'op:>>>': T(0x0C), 'op:>>': T(0x0D), 'op:<<': T(0x0E),
        'op:-bin': T(0x0F), 'op:+bin': T(0x10), 'op:%': T(0x11), 'op:/': T(0x12),
        'op:*': T(0x13), 'op:~': T(0x14), 'op:!': T(0x15), 'op:-un': T(0x16),
        'op:+un': None,
        'op:++post': T(0x17), 'op:--post': T(0x18), 'op:++pre': T(0x19),
        'op:--pre': T(0x1A), 'op:?': T(0x00),
        'op:=': T(0x23) + c[0x002c] + c['field_init'],
        'op:*=': T(0x24), 'op:/=': T(0x25), 'op:%=': T(0x26), 'op:+=': T(0x27),
        'op:-=': T(0x28), 'op:shiftassign': T(0x29, 0x2A, 0x2B),
        'op:&=': T(0x2C), 'op:^=': T(0x2D), 'op:|=': T(0x2E),
        'str': c[0x4009], 'kw:null': c[0x000c], 'kw:bool': c[0x100d],
        'rid': c[0x100e], 'int': c[0x100b], 'float': c[0x200a],
        'kw:instanceof': T(0x20), 'kw:new': T(0x1E, 0x1F),
        'kw:case': c[0x1017], 'kw:switch': c[0x0018], 'kw:do': c[0x1016],
        'super.call': c[0x4026], 'this(': c[0x0022],
        'op:[idx': c[0x0020] + T(0x1F), 'cast': T(0x21),
    }


def census(text, blobs):
    """[(label, source count, class count)] for the token classes that
    disagree ('+ unary' has no node and never disagrees)."""
    sc = source_counts(text)
    cc = class_counts(blobs)
    out = []
    for label, key, _ in CENSUS:
        want = cc.get(key)
        if want is None:
            continue
        have = sc.get(key, 0)
        if have != want:
            out.append((label, have, want))
    return out


# ---------------------------------------------------------------------------
# line check
# ---------------------------------------------------------------------------

def _f32bits(x):
    try:
        return struct.unpack('<I', struct.pack('<f', x))[0]
    except (OverflowError, struct.error):
        return None


def float_token_bits(tok_text):
    """Approximate bit pattern the compiler gives a float token (exact to
    within FLOAT_ULP_SLACK).  The scanner accumulates the integer part in a
    32-bit unsigned integer, so it wraps modulo 2**32: 1000000000000000.0
    compiles to 2764472320.0 (corpus: Block_Inline.java line 57)."""
    t = tok_text.rstrip('fFdD')
    try:
        if 'e' in t.lower():
            return _f32bits(float(t))
        ip, _, fp = t.partition('.')
        whole = (int(ip) if ip else 0) & 0xFFFFFFFF
        frac = float('0.' + fp) if fp else 0.0
        return _f32bits(whole + frac)
    except ValueError:
        return None


def _tokens_by_line(text):
    by = {}
    for t in srclex.tokenize(text):
        by.setdefault(t.line, []).append(t)
        # a block comment or string cannot span lines in the corpus; tokens
        # are filed under the line they start on
    return by


FLOAT_ULP_SLACK = 2      # the compiler's conversion is off by up to 2 ulp


def line_check(text, blobs, max_examples=5):
    """Count the nodes whose token is not on the node's source line.
    Returns ({kind: count}, {kind: total}, [example strings])."""
    by = _tokens_by_line(text)
    nlines = text.count('\n') + 1
    bad = Counter()
    tot = Counter()
    examples = []

    def note(kind, b, ti, k, nd, detail):
        bad[kind] += 1
        if len(examples) < max_examples:
            examples.append('%s tree %d node %d line %d: %s %s' % (
                b.name, ti, k, nd.slot, kind, detail))

    for b in blobs:
        for ti, t in enumerate(b.trees):
            for k, nd in enumerate(t):
                code = tufa.OP_FLAGS[nd.op]
                if not 1 <= nd.slot <= nlines:
                    tot['line'] += 1
                    note('line', b, ti, k, nd, 'outside 1..%d' % nlines)
                    continue
                toks = by.get(nd.slot, ())
                if code == 0x4009:
                    tot['str'] += 1
                    want = b.utf8(nd.imm)
                    if not any(x.kind == 'str' and srclex.string_value(x.text) == want
                               for x in toks):
                        note('str', b, ti, k, nd, tufa.repr_text(want)[:60])
                elif code == 0x100b:
                    tot['int'] += 1
                    if not any(x.kind == 'int' and srclex.int_value(x.text) == nd.imm
                               for x in toks):
                        note('int', b, ti, k, nd, '%d' % nd.imm)
                elif code == 0x200a:
                    tot['float'] += 1
                    good = False
                    for x in toks:
                        if x.kind != 'float':
                            continue
                        fb = float_token_bits(x.text)
                        if fb is not None and abs(fb - nd.imm) <= FLOAT_ULP_SLACK:
                            good = True
                            break
                    if not good:
                        note('float', b, ti, k, nd, '0x%08X' % nd.imm)
                elif code == 0x100e:
                    tot['rid'] += 1
                    pack, local = b.rid(nd.imm)
                    good = False
                    for x in toks:
                        if x.kind != 'rid':
                            continue
                        p, l = srclex.rid_parts(x.text)
                        if l == local and srclex.rid_pack_file(p).lower() == pack.lower():
                            good = True
                        elif (pack.lower(), local) == ('system.rpk', 0):
                            good = True      # fallback for a pack that does not exist
                    if not good:
                        note('rid', b, ti, k, nd, '%s:0x%X' % (pack, local))
                elif code in (0x101a, 0x101b, 0x4012, 0x401c, 0x4025, 0x4026):
                    tot['name'] += 1
                    want = b.ref(nd.imm)[1] if code in (0x101a, 0x101b) else b.utf8(nd.imm)
                    good = False
                    for ln in range(nd.slot, max(0, nd.slot - NAME_SLACK - 1), -1):
                        if any(x.text == want and x.kind in ('id', 'kw')
                               for x in by.get(ln, ())):
                            good = True
                            break
                    if not good:
                        note('name', b, ti, k, nd, want)
    return dict(bad), dict(tot), examples


def check_pair(text, blobs):
    """Both checks for one pair.  See the module docstring."""
    cen = census(text, blobs)
    bad, tot, examples = line_check(text, blobs)
    return {
        'consistent': not cen and not bad,
        'census': cen,
        'lines': bad,
        'checked_nodes': sum(tot.values()),
        'examples': examples,
    }
