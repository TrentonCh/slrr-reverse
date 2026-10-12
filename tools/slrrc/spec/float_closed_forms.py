#!/usr/bin/env python3
"""float_closed_forms.py - the four float conversions as the compiler will
implement them (plain float / double operations), checked against the exact
simulation of float_models.py for every literal text of the pairs.

    python -I float_closed_forms.py

float_models.py works on exact fractions with explicit rounding, which is the
reference but far too slow for a compiler.  The functions below use only what
C gives: IEEE double arithmetic and the conversion double -> float.  A float
operation is written r32(a op b) on doubles; that equals the single precision
operation exactly, because rounding a sum, product or quotient of two floats
to 53 bits and then to 24 gives the same result as rounding once.

In C (compile with /fp:precise, SSE2):

  A   float v = (float)ip, s = 1.0f;
      for each digit d:  s = s / 10.0f;  v = v + (float)d * s;
  B   float v = (float)ip, s = 0.1f;
      for each digit d:  v = v + (float)d * s;  s = s * 0.1f;
  C   float v = (float)ip, s = 1.0f;
      for each digit d:  s = (float)((double)s / 10.0);
                         v = (float)((double)v + (double)d * (double)s);
  D   double v = (double)ip, s = (double)0.1f;
      for each digit d:  v = v + (double)d * s;  s = s / 10.0;
      result (float)v

ip is the integer part accumulated in a 32-bit unsigned integer (it wraps).
"""
import os
import pickle
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))

NAMES = {'A': 'div pc24 v24 s24 c24', 'B': 'mul pc24 v24 s24 c24',
         'C': 'div pc53 v24 s24 c24', 'D': 'diva pc53 v53 s53 c24'}


def r32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


F01 = r32(0.1)


def split(text):
    t = text.rstrip('fFdD')
    ip, _, fp = t.partition('.')
    return (int(ip) if ip else 0) & 0xFFFFFFFF, fp


def conv_a(text):
    ip, fp = split(text)
    v, s = r32(float(ip)), 1.0
    for ch in fp:
        s = r32(s / 10.0)
        v = r32(v + r32(int(ch) * s))
    return bits(v)


def conv_b(text):
    ip, fp = split(text)
    v, s = r32(float(ip)), F01
    for ch in fp:
        v = r32(v + r32(int(ch) * s))
        s = r32(s * F01)
    return bits(v)


def conv_c(text):
    ip, fp = split(text)
    v, s = r32(float(ip)), 1.0
    for ch in fp:
        s = r32(s / 10.0)
        v = r32(v + int(ch) * s)
    return bits(v)


def conv_d(text):
    ip, fp = split(text)
    v, s = float(ip), F01
    for ch in fp:
        v = v + int(ch) * s
        s = s / 10.0
    return bits(v)


CONV = {'A': conv_a, 'B': conv_b, 'C': conv_c, 'D': conv_d}


def main(argv):
    total = 0
    bad = {k: 0 for k in CONV}
    first = {}
    texts = set()
    for which in ('oracle', 'other'):
        p = os.path.join(_HERE, 'out', 'float_res_%s.pickle' % which)
        if not os.path.exists(p):
            print('run float_models.py --set %s first' % which)
            return 1
        with open(p, 'rb') as f:
            names, res = pickle.load(f)
        idx = {n: k for k, n in enumerate(names)}
        for tk, v in res.items():
            if tk in texts:
                continue
            texts.add(tk)
            total += 1
            for ch, fn in CONV.items():
                want = v[idx[NAMES[ch]]]
                got = fn(tk)
                if got != want:
                    bad[ch] += 1
                    first.setdefault(ch, (tk, got, want))
    print('distinct float literal texts of the consistent pairs: %d' % total)
    for ch in 'ABCD':
        print('  setting %s: closed form equals the exact simulation for %d of %d texts%s' % (
            ch, total - bad[ch], total,
            '' if not bad[ch] else '   first difference: %s -> %08X, simulation %08X' % first[ch]))
    return 1 if any(bad.values()) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
