#!/usr/bin/env python3
"""study_float_literals.py - how does the compiler turn a float token into the
32-bit pattern of a 0x200A node?  (OPEN QUESTION; this script holds the data.)

    python -I study_float_literals.py [--json OUT] [--show N]

Method.  For every Build 940 pair that passes paircheck, each 0x200A node is
matched with a float token on the source line the node records (nearest value,
within 2 ulp, one token per node).  The matched (token text, bit pattern)
samples are then compared with candidate conversions:

    exact     correctly rounded float32 of the decimal text (C strtof)
    div10     32-bit float arithmetic:  v = integer part (as a float);
              s = 1; for each fraction digit d: s = s / 10; v = v + d * s
    mul01     the same with s = 0.1f; ... v = v + d * s; s = s * 0.1f

All three use the integer part modulo 2**32 (see below).

What the corpus shows (numbers in notes/foundation.md, regenerate with this
script):
  * The conversion is NOT correctly rounded: about one sample in nine is 1 ulp
    (rarely 2) away from the nearest float.
  * It is NOT a function of the token text alone: the same text compiles to
    different patterns in different class files (0.0001 is 0x38D1B716 in one
    file and 0x38D1B718 in another; the nearest float is 0x38D1B717).
  * div10 and mul01 together explain almost every sample, and a class file
    follows one of them throughout: the choice is made per compilation, not
    per literal.  Whether that is x87 precision state at compile time or two
    compiler builds carrying the same build number is unknown.
  * The integer part is accumulated in a 32-bit unsigned integer and wraps:
    1000000000000000.0 compiles to 2764472320.0 (= 10**15 mod 2**32).
"""
import json
import os
import struct
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import oracle   # noqa: E402
import srclex   # noqa: E402
import tufa     # noqa: E402


def r32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


F01 = r32(0.1)


def _split(text):
    t = text.rstrip('fFdD')
    ip, _, fp = t.partition('.')
    return (int(ip) if ip else 0) & 0xFFFFFFFF, fp, (int(ip) if ip else 0)


def conv_exact(text):
    whole, fp, _ = _split(text)
    return bits(whole + (float('0.' + fp) if fp else 0.0))


def conv_div10(text):
    whole, fp, _ = _split(text)
    v = r32(float(whole))
    s = 1.0
    for ch in fp:
        s = r32(s / 10.0)
        v = r32(v + r32(int(ch) * s))
    return bits(v)


def conv_mul01(text):
    whole, fp, _ = _split(text)
    v = r32(float(whole))
    s = F01
    for ch in fp:
        v = r32(v + r32(int(ch) * s))
        s = r32(s * F01)
    return bits(v)


CANDIDATES = (('exact', conv_exact), ('div10', conv_div10), ('mul01', conv_mul01))


def samples():
    """Yield (entry, line, token text, node bits) for every matched node."""
    for e in oracle.iter_oracle_pairs():
        text = oracle.load_source(e)
        by = defaultdict(list)
        for t in srclex.tokenize(text):
            if t.kind == 'float':
                by[t.line].append(t.text)
        nodes = defaultdict(list)
        for b in oracle.load_class(e):
            for tree in b.trees:
                for nd in tree:
                    if tufa.OP_FLAGS[nd.op] == 0x200a:
                        nodes[nd.slot].append(nd.imm)
        for line, imms in nodes.items():
            free = list(by.get(line, ()))
            for imm in imms:
                best = None
                for k, tk in enumerate(free):
                    d = abs(conv_exact(tk) - imm)
                    if d <= 2 and (best is None or d < best[0]):
                        best = (d, k)
                if best is None:
                    yield e, line, None, imm
                else:
                    yield e, line, free.pop(best[1]), imm


def main(argv):
    out_json = None
    show = 12
    if '--json' in argv:
        out_json = argv[argv.index('--json') + 1]
    if '--show' in argv:
        show = int(argv[argv.index('--show') + 1])
    n = 0
    unmatched = 0
    delta = Counter()
    hit = Counter()
    by_text = defaultdict(Counter)
    where = {}
    per_file = defaultdict(Counter)
    wraps = []
    forms = Counter()
    for e, line, tk, imm in samples():
        n += 1
        if tk is None:
            unmatched += 1
            continue
        forms['suffix f' if tk[-1] in 'fF' else 'plain'] += 1
        ex = conv_exact(tk)
        delta[imm - ex] += 1
        a = conv_div10(tk) == imm
        b = conv_mul01(tk) == imm
        c = ex == imm
        hit['exact'] += c
        hit['div10'] += a
        hit['mul01'] += b
        hit['div10 or mul01'] += (a or b)
        hit['none of the three'] += not (a or b or c)
        key = 'both' if (a and b) else 'div10 only' if a else 'mul01 only' if b else 'neither'
        per_file[e.cls][key] += 1
        by_text[tk][imm] += 1
        where.setdefault((tk, imm), '%s:%d' % (e.source, line))
        if _split(tk)[2] >= 1 << 32:
            wraps.append((e.source, line, tk, '0x%08X' % imm))
    files = Counter()
    for f, c in per_file.items():
        if c['div10 only'] and c['mul01 only']:
            files['mixed'] += 1
        elif c['div10 only']:
            files['div10'] += 1
        elif c['mul01 only']:
            files['mul01'] += 1
        else:
            files['undetermined (both agree on every literal)'] += 1
        if c['neither']:
            files['with a literal neither explains'] += 1
    multi = {t: c for t, c in by_text.items() if len(c) > 1}
    print('Build 940 pairs consistent with their source: %d files with float '
          'literals' % len(per_file))
    print('float nodes                     %d' % n)
    print('  matched with a token          %d' % (n - unmatched))
    print('  not matched (within 2 ulp)    %d' % unmatched)
    print('token forms: %s' % dict(forms))
    print('difference from the correctly rounded value, in ulp:')
    for k in sorted(delta):
        print('  %+d   %d' % (k, delta[k]))
    print('samples reproduced by a candidate conversion:')
    for k in ('exact', 'div10', 'mul01', 'div10 or mul01', 'none of the three'):
        print('  %-18s %d of %d' % (k, hit[k], n - unmatched))
    print('class files by which candidate their literals need:')
    for k, v in sorted(files.items()):
        print('  %-44s %d' % (k, v))
    print('distinct token texts %d; texts compiled to more than one pattern: %d'
          % (len(by_text), len(multi)))
    for t, c in sorted(multi.items(), key=lambda kv: -sum(kv[1].values()))[:show]:
        print('  %-10s nearest 0x%08X : %s' % (t, conv_exact(t), ', '.join(
            '0x%08X x%d (%s)' % (b, v, where[(t, b)]) for b, v in c.most_common())))
    print('integer part >= 2**32 (wraps): %d' % len(wraps))
    for w in wraps[:show]:
        print('  %s:%d %s -> %s' % w)
    neither = [(f, c['neither']) for f, c in sorted(per_file.items()) if c['neither']]
    print('files with literals neither div10 nor mul01 reproduces: %d' % len(neither))
    for f, k in neither[:show]:
        print('  %s (%d)' % (f, k))
    if out_json:
        doc = {
            'nodes': n, 'unmatched': unmatched,
            'delta_ulp': {str(k): v for k, v in delta.items()},
            'reproduced': dict(hit), 'files': dict(files),
            'texts': {t: {('0x%08X' % b): {'count': v, 'first': where[(t, b)]}
                          for b, v in c.items()} for t, c in sorted(by_text.items())},
            'per_file': {f: dict(c) for f, c in sorted(per_file.items())},
            'wraps': wraps,
        }
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
