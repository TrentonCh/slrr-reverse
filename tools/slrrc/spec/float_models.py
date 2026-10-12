#!/usr/bin/env python3
"""float_models.py - which float conversion explains the 0x200A nodes, per file.

    python -I float_models.py [--set oracle|other|all] [--json OUT]

The foundation left the bit pattern of a float literal open: two single
precision conversions and correct rounding together explain all but 6 of
35,084 nodes, and the variant seems chosen per compilation.  This script tests
one hypothesis that would make it a single piece of code: the scanner converts
the literal with x87 arithmetic, and the x87 precision control of the thread
that compiles is either 53 bits (the Windows default) or 24 bits (what
Direct3D sets when a device is created without D3DCREATE_FPU_PRESERVE).

A candidate is a small program (how the fraction is accumulated) run under a
precision control PC in {24, 53, 64} with float or double variables.  All
arithmetic is done on exact fractions and rounded explicitly, so that double
rounding is modelled and not approximated.

For every class file the script finds the candidates that reproduce EVERY
float node of the file, and then the smallest sets of candidates that cover
all files.  It prints nothing it has not computed.
"""
import json
import os
import struct
import sys
from collections import Counter, defaultdict
from fractions import Fraction

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB = os.path.normpath(os.path.join(_HERE, '..', 'lib'))
if _LIB not in sys.path:
    sys.path.insert(0, _LIB)

import oracle   # noqa: E402
import srclex   # noqa: E402
import tufa     # noqa: E402


def rnd(x, p):
    """Round the fraction x to a binary float with a p-bit significand,
    round to nearest even, unbounded exponent."""
    if x == 0 or p is None:
        return x
    neg = x < 0
    if neg:
        x = -x
    n, d = x.numerator, x.denominator
    e = n.bit_length() - d.bit_length()          # 2**e <= x < 2**(e+2) roughly
    # scale so that the significand is an integer of p bits
    sh = p - 1 - e
    if sh >= 0:
        num, den = n << sh, d
    else:
        num, den = n, d << (-sh)
    q, r = divmod(num, den)
    if q.bit_length() > p:                       # one bit too many
        sh -= 1
        if sh >= 0:
            num, den = n << sh, d
        else:
            num, den = n, d << (-sh)
        q, r = divmod(num, den)
    elif q.bit_length() < p:
        sh += 1
        if sh >= 0:
            num, den = n << sh, d
        else:
            num, den = n, d << (-sh)
        q, r = divmod(num, den)
    assert q.bit_length() == p, (x, p, q)
    if 2 * r > den or (2 * r == den and (q & 1)):
        q += 1
    res = Fraction(q, 1) / (Fraction(2) ** sh) if sh >= 0 else Fraction(q << (-sh), 1)
    return -res if neg else res


def bits_of(x):
    """Bit pattern of the float nearest to the fraction x (x is rounded to 24
    bits first, exactly)."""
    y = rnd(x, 24)
    return struct.unpack('<I', struct.pack('<f', float(y)))[0]


F01_24 = rnd(Fraction(1, 10), 24)
F01_53 = rnd(Fraction(1, 10), 53)
F01_64 = rnd(Fraction(1, 10), 64)
TEN = Fraction(10)


def split(text):
    t = text.rstrip('fFdD')
    ip, _, fp = t.partition('.')
    return (int(ip) if ip else 0), fp


class Cand(object):
    """prog: 'div'   s = 1;   loop: s = s / 10; v = v + d * s
             'diva'  s = 1/10 (rounded as a constant); loop: v = v + d * s; s = s / 10
             'mul'   s = c;   loop: v = v + d * s; s = s * c
             'mulb'  s = 1;   loop: s = s * c; v = v + d * s
             'int'   v = v * 10 + d over all digits, then v = v / 10**k (k divisions by 10)
             'intm'  the same, then k multiplications by c
             'intp'  the same, then one division by the exactly known 10**k
             'frac'  f = digits as an integer (exact), v = ip + f / 10**k in one division
       pc:   precision control of the x87 (24, 53, 64): every result is rounded to it
       vt:   precision the variable v is stored with (24 float, 53 double, None = register)
       st:   the same for the scale s
       ct:   precision of the constant 0.1 (24 = 0.1f, 53 = 0.1)
       wrap: integer part modulo 2**32
    """

    def __init__(self, prog, pc, vt, st, ct, wrap=True):
        self.prog, self.pc, self.vt, self.st, self.ct, self.wrap = prog, pc, vt, st, ct, wrap
        self.name = '%s pc%d v%s s%s c%s' % (prog, pc, vt or 'r', st or 'r', ct)

    def run(self, text):
        ip, fp = split(text)
        if self.wrap:
            ip &= 0xFFFFFFFF
        pc = self.pc

        def R(x):
            return rnd(x, pc)

        def V(x):
            return rnd(x, self.vt) if self.vt else x

        def S(x):
            return rnd(x, self.st) if self.st else x

        c = {24: F01_24, 53: F01_53, 64: F01_64}[self.ct]
        v = V(R(Fraction(ip)))
        prog = self.prog
        if prog == 'div':
            s = Fraction(1)
            for ch in fp:
                s = S(R(s / TEN))
                v = V(R(v + R(int(ch) * s)))
        elif prog == 'diva':
            s = S(c)
            for ch in fp:
                v = V(R(v + R(int(ch) * s)))
                s = S(R(s / TEN))
        elif prog == 'mul':
            s = S(c)
            for ch in fp:
                v = V(R(v + R(int(ch) * s)))
                s = S(R(s * c))
        elif prog == 'mulb':
            s = Fraction(1)
            for ch in fp:
                s = S(R(s * c))
                v = V(R(v + R(int(ch) * s)))
        elif prog in ('int', 'intm', 'intp'):
            for ch in fp:
                v = V(R(R(v * TEN) + int(ch)))
            if prog == 'int':
                for ch in fp:
                    v = V(R(v / TEN))
            elif prog == 'intm':
                for ch in fp:
                    v = V(R(v * c))
            else:
                v = V(R(v / (TEN ** len(fp))))
        elif prog == 'frac':
            f = Fraction(int(fp)) if fp else Fraction(0)
            v = V(R(v + R(f / (TEN ** len(fp)))))
        else:
            raise ValueError(prog)
        return bits_of(v)


def candidates():
    out = []
    for prog in ('div', 'diva', 'mul', 'mulb', 'int', 'intm', 'intp', 'frac'):
        for pc in (24, 53, 64):
            for vt in (24, 53, None):
                for st in (24, 53, None):
                    if prog in ('int', 'intm', 'intp', 'frac') and st != 24:
                        continue                      # no scale variable
                    for ct in (24, 53):
                        if prog in ('div', 'int', 'intp', 'frac') and ct != 24:
                            continue                  # no constant 0.1
                        out.append(Cand(prog, pc, vt, st, ct))
    return out


def exact_bits(text):
    ip, fp = split(text)
    ip &= 0xFFFFFFFF
    return bits_of(Fraction(ip) + (Fraction(int(fp), 10 ** len(fp)) if fp else 0))


def samples(which):
    if which == 'oracle':
        ents = list(oracle.iter_oracle_pairs())
    elif which == 'other':
        ents = [e for e in oracle.iter_entries('pair_other') if e.consistent]
    else:
        ents = [e for e in oracle.iter_entries() if e.category in ('pair_940', 'pair_other')
                and e.consistent]
    for e in ents:
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
                    d = abs(exact_bits(tk) - imm)
                    if d <= 2 and (best is None or d < best[0]):
                        best = (d, k)
                if best is None:
                    yield e, line, None, imm
                else:
                    yield e, line, free.pop(best[1]), imm


def main(argv):
    which = 'oracle'
    if '--set' in argv:
        which = argv[argv.index('--set') + 1]
    out_json = argv[argv.index('--json') + 1] if '--json' in argv else None
    cands = candidates()
    per_file = defaultdict(list)
    unmatched = 0
    n = 0
    for e, line, tk, imm in samples(which):
        n += 1
        if tk is None:
            unmatched += 1
            continue
        per_file[e.cls].append((tk, imm, line))
    print('set %s: %d files with float nodes, %d nodes, %d not matched to a token'
          % (which, len(per_file), n, unmatched))
    # distinct (text) -> candidate results, cached
    texts = set(tk for v in per_file.values() for tk, _, _ in v)
    print('distinct literal texts: %d; candidates: %d' % (len(texts), len(cands)))
    import pickle
    cache = os.path.join(_HERE, 'out', 'float_res_%s.pickle' % which)
    res = {}
    if os.path.exists(cache):
        with open(cache, 'rb') as f:
            names, res = pickle.load(f)
        if names != [c.name for c in cands]:
            res = {}
    for tk in texts:
        if tk not in res:
            res[tk] = [c.run(tk) for c in cands]
    with open(cache, 'wb') as f:
        pickle.dump(([c.name for c in cands], res), f, pickle.HIGHEST_PROTOCOL)
    with open(os.path.join(_HERE, 'out', 'float_samples_%s.pickle' % which), 'wb') as f:
        pickle.dump(dict(per_file), f, pickle.HIGHEST_PROTOCOL)
    exact = {tk: exact_bits(tk) for tk in texts}
    # collapse candidates that give the same result on every text
    sig = defaultdict(list)
    for k, c in enumerate(cands):
        sig[tuple(res[tk][k] for tk in sorted(texts))].append(k)
    groups = sorted(sig.values(), key=lambda g: g[0])
    print('candidates that differ on at least one text: %d groups' % len(groups))
    rep = [g[0] for g in groups]
    # node hits
    hits = Counter()
    for f, v in per_file.items():
        for tk, imm, _ in v:
            for k in rep:
                if res[tk][k] == imm:
                    hits[k] += 1
            if exact[tk] == imm:
                hits['exact'] += 1
    total = sum(len(v) for v in per_file.values())
    print()
    print('nodes reproduced, best 12 candidate groups (of %d nodes):' % total)
    print('  %-34s %d' % ('correctly rounded', hits['exact']))
    for k in sorted(rep, key=lambda k: -hits[k])[:12]:
        g = [gg for gg in groups if gg[0] == k][0]
        print('  %-34s %d   (%d equivalent settings, e.g. %s)' % (
            cands[k].name, hits[k], len(g), '; '.join(cands[j].name for j in g[1:3])))
    # files fully explained by each candidate
    full = defaultdict(set)
    for f, v in per_file.items():
        for k in rep:
            if all(res[tk][k] == imm for tk, imm, _ in v):
                full[k].add(f)
        if all(exact[tk] == imm for tk, imm, _ in v):
            full['exact'].add(f)
    allf = set(per_file)
    print()
    print('files in which EVERY float node is reproduced (of %d):' % len(allf))
    print('  %-34s %d' % ('correctly rounded', len(full['exact'])))
    order = sorted(rep, key=lambda k: -len(full[k]))
    for k in order[:12]:
        print('  %-34s %d' % (cands[k].name, len(full[k])))
    # best pairs and triples of candidates
    keys = order[:40] + ['exact']
    best2 = []
    for i, a in enumerate(keys):
        for b in keys[i + 1:]:
            best2.append((len(full[a] | full[b]), a, b))
    best2.sort(key=lambda t: -t[0])
    print()
    print('best two settings together:')

    def nm(k):
        return 'correctly rounded' if k == 'exact' else cands[k].name
    for cnt, a, b in best2[:8]:
        print('  %4d of %d files   %s  +  %s' % (cnt, len(allf), nm(a), nm(b)))
    best3 = []
    top = [k for _, a, b in best2[:30] for k in (a, b)]
    top = list(dict.fromkeys(top))
    for i, a in enumerate(top):
        for j, b in enumerate(top[i + 1:], i + 1):
            for c in top[j + 1:]:
                best3.append((len(full[a] | full[b] | full[c]), a, b, c))
    best3.sort(key=lambda t: -t[0])
    print('best three settings together:')
    for cnt, a, b, c in best3[:6]:
        print('  %4d of %d files   %s  +  %s  +  %s' % (cnt, len(allf), nm(a), nm(b), nm(c)))
    # the files the best pair leaves
    if best2:
        cnt, a, b = best2[0]
        left = sorted(allf - full[a] - full[b])
        print()
        print('files the best pair does not explain: %d' % len(left))
        for f in left[:40]:
            v = per_file[f]
            bad_a = [(tk, imm) for tk, imm, _ in v if a == 'exact' and exact[tk] != imm
                     or a != 'exact' and res[tk][a] != imm]
            bad_b = [(tk, imm) for tk, imm, _ in v if b == 'exact' and exact[tk] != imm
                     or b != 'exact' and res[tk][b] != imm]
            print('  %-90s nodes %3d  wrong under A %2d, under B %2d  %s' % (
                f, len(v), len(bad_a), len(bad_b),
                ', '.join('%s->%08X' % t for t in (bad_a if len(bad_a) <= len(bad_b) else bad_b)[:3])))
        # which setting does each file need
        need = Counter()
        for f in allf:
            ina, inb = f in full[a], f in full[b]
            need['both' if ina and inb else 'A only' if ina else 'B only' if inb else 'neither'] += 1
        print('files by the setting they need: A = %s, B = %s: %s' % (nm(a), nm(b), dict(need)))
    # greedy cover with every candidate (not only one per group)
    fullc = {}
    for k, c in enumerate(cands):
        fullc[k] = set(f for f, v in per_file.items()
                       if all(res[tk][k] == imm for tk, imm, _ in v))
    rest = set(allf)
    cover = []
    while rest:
        k = max(fullc, key=lambda k: (len(fullc[k] & rest), -k))
        got = fullc[k] & rest
        if not got:
            break
        cover.append((cands[k].name, len(got)))
        rest -= got
    print()
    print('greedy cover of the files, one setting per file:')
    for name, cnt in cover:
        print('  %-34s %d more files' % (name, cnt))
    print('  files no setting reproduces: %d %s' % (len(rest), sorted(rest)[:10]))
    # one piece of code under the three precision states
    codes = defaultdict(dict)
    for k, c in enumerate(cands):
        codes[(c.prog, c.vt, c.st, c.ct)][c.pc] = fullc[k]
    rows = []
    for code, d in codes.items():
        u = set().union(*d.values())
        rows.append((len(u), '%s v%s s%s c%s' % (code[0], code[1] or 'r', code[2] or 'r', code[3]),
                     {pc: len(x) for pc, x in sorted(d.items())}))
    rows.sort(key=lambda r: -r[0])
    print()
    print('one piece of code, precision control free per file (files reproduced of %d):' % len(allf))
    for r in rows[:6]:
        print('  %4d  %-24s alone per state: %s' % r)
    cl = list(codes.items())
    best = (0, None, None)
    for i, (c1, d1) in enumerate(cl):
        u1 = set().union(*d1.values())
        for c2, d2 in cl[i + 1:]:
            n2 = len(u1 | set().union(*d2.values()))
            if n2 > best[0]:
                best = (n2, c1, c2)
    print('two pieces of code, precision control free per file: best %d of %d (%s and %s)'
          % (best[0], len(allf), best[1], best[2]))
    if out_json:
        doc = {
            'cover': cover, 'cover_left': sorted(rest),
            'one_code': rows[:10], 'two_codes': [best[0], str(best[1]), str(best[2])],
            'set': which, 'files': len(allf), 'nodes': total,
            'node_hits': {nm(k): hits[k] for k in rep + ['exact']},
            'file_full': {nm(k): len(full[k]) for k in rep + ['exact']},
            'best2': [(cnt, nm(a), nm(b)) for cnt, a, b in best2[:20]],
            'best3': [(cnt, nm(a), nm(b), nm(c)) for cnt, a, b, c in best3[:20]],
        }
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
