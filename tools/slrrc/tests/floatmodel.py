#!/usr/bin/env python3
"""floatmodel.py - the float literal conversions in exact arithmetic.

An oracle for slrrc/floatlit.py that shares no code with it: every value is a
fraction, and every rounding is written out (round to nearest, ties to even,
gradual underflow).  convert(mode, text) is what the C forms of the docstring
of floatlit.py compute under IEEE arithmetic with SSE2 and the default
rounding mode.

wrong_builds() are the same C forms under build settings a port could meet by
accident; each gives other bits for some literals.  The start-up self test of
the compiler (floatlit.SELF_TEST) has to hold a literal that tells each of
them from the right build; tests/t_review.py checks that it does.

    fma         v + d * s contracted to one fused multiply-add
    x87-53      x87 evaluation, precision control 53 bits: an expression is
                rounded to double and to single only when it is stored
    x87-64      the same with the full 64 bits of the x87
    x87-24      x87 with 24-bit precision control (what Direct3D sets on a
                thread unless the device is created with the FPU-preserve
                flag): every operation is rounded to single
    signed      the integer part converted from a signed 32-bit integer
    wide        the integer part accumulated in 64 bits (no wrap at 2**32)
    double-0.1  the constant 0.1 where the form says 0.1f
    ftz         flush to zero: a result below the smallest normal single is 0
    toward-0    rounding mode "toward zero" instead of "to nearest"
"""
import struct
from fractions import Fraction

MODES = ('A', 'B', 'C', 'D')
SINGLE = (24, -126)         # significand bits, exponent of the smallest normal
DOUBLE = (53, -1022)
EXTENDED = (64, -16382)
TENTH = Fraction(1, 10)
TEN = Fraction(10)


def round_to(x, fmt, toward_zero=False):
    """The fraction x rounded to a binary floating point format (bits,
    emin): to nearest, ties to even, with gradual underflow; exact."""
    if x == 0:
        return x
    p, emin = fmt
    neg = x < 0
    if neg:
        x = -x
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if Fraction(2) ** e > x:
        e -= 1                              # now 2**e <= x < 2**(e + 1)
    if e < emin:
        e = emin                            # below the normal range: fixed spacing
    unit = Fraction(2) ** (e - p + 1)       # one unit in the last place
    k = x / unit
    f = k.numerator // k.denominator
    r = k - f
    if not toward_zero and (r > Fraction(1, 2) or (r == Fraction(1, 2) and (f & 1))):
        f += 1
    y = f * unit
    return -y if neg else y


def bits(x):
    """Bit pattern of a value that is a single."""
    assert round_to(x, SINGLE) == x, x
    return struct.unpack('<I', struct.pack('<f', float(x)))[0]


def split(text):
    """(integer digits, fraction digits) of a literal text."""
    if text[-1] in 'fF':
        text = text[:-1]
    ip, _, fp = text.partition('.')
    return ip, fp


class Build(object):
    """How the four C forms are evaluated.  The defaults are the right build."""

    def __init__(self, name='right', fma=False, x87=None, signed=False, wide=False,
                 double_tenth=False, ftz=False, toward_zero=False):
        self.name = name
        self.fma = fma
        self.x87 = x87                      # None, or the format every x87 result has
        self.signed = signed
        self.wide = wide
        self.double_tenth = double_tenth
        self.ftz = ftz
        self.toward_zero = toward_zero

    # -- rounding ------------------------------------------------------------

    def rnd(self, x, fmt):
        y = round_to(x, fmt, self.toward_zero)
        if self.ftz and fmt == SINGLE and y != 0 and abs(y) < Fraction(2) ** -126:
            return Fraction(0)
        return y

    def S(self, x):
        """A value stored into a float variable."""
        return self.rnd(x, SINGLE)

    def D(self, x):
        """A value stored into a double variable."""
        return self.rnd(x, DOUBLE)

    def op_s(self, x):
        """The result of one operation on float operands, before it is
        stored: a single under SSE2, a register value on the x87."""
        if self.x87 is not None:
            return self.rnd(x, self.x87)
        return self.rnd(x, SINGLE)

    def op_d(self, x):
        """The result of one operation on double operands."""
        if self.x87 is not None:
            return self.rnd(x, self.x87)
        return self.rnd(x, DOUBLE)

    # -- the integer part ----------------------------------------------------

    def int_part(self, digits):
        v = int(digits) if digits else 0
        if not self.wide:
            v &= 0xFFFFFFFF
            if self.signed and v >= 0x80000000:
                v -= 0x100000000
        return Fraction(v)

    # -- the four forms ------------------------------------------------------

    def convert(self, mode, text):
        """The bits of the literal under conversion `mode`."""
        ip, fp = split(text)
        S, D, op_s, op_d = self.S, self.D, self.op_s, self.op_d
        ipv = self.int_part(ip)
        tenth_f = round_to(TENTH, SINGLE)           # the constant 0.1f
        if mode == 'A':
            # float v = (float)ip, s = 1.0f;  s = s / 10.0f;  v = v + (float)d * s;
            v = S(ipv)
            s = Fraction(1)
            for ch in fp:
                d = int(ch)
                s = S(op_s(s / TEN))
                if self.fma:
                    v = S(op_s(v + d * s))
                else:
                    v = S(op_s(v + op_s(d * s)))
            return bits(v)
        if mode == 'B':
            # float v = (float)ip, s = 0.1f;  v = v + (float)d * s;  s = s * 0.1f;
            v = S(ipv)
            s = tenth_f
            for ch in fp:
                d = int(ch)
                if self.fma:
                    v = S(op_s(v + d * s))
                else:
                    v = S(op_s(v + op_s(d * s)))
                if self.double_tenth:
                    s = S(op_d(s * round_to(TENTH, DOUBLE)))
                else:
                    s = S(op_s(s * tenth_f))
            return bits(v)
        if mode == 'C':
            # s = (float)((double)s / 10.0);
            # v = (float)((double)v + (double)d * (double)s);
            v = S(ipv)
            s = Fraction(1)
            for ch in fp:
                d = int(ch)
                s = S(op_d(s / TEN))
                if self.fma:
                    v = S(op_d(v + d * s))
                else:
                    v = S(op_d(v + op_d(d * s)))
            return bits(v)
        # D: double v = (double)ip, s = (double)0.1f;  v = v + (double)d * s;  s = s / 10.0;
        v = D(ipv)
        s = tenth_f
        for ch in fp:
            d = int(ch)
            if self.fma:
                v = D(op_d(v + d * s))
            else:
                v = D(op_d(v + op_d(d * s)))
            s = D(op_d(s / TEN))
        return bits(S(v))


RIGHT = Build()


def convert(mode, text):
    """The bits of a literal under conversion `mode`, right build."""
    return RIGHT.convert(mode, text)


def wrong_builds():
    return [
        Build('fma', fma=True),
        Build('x87-53', x87=DOUBLE),
        Build('x87-64', x87=EXTENDED),
        Build('x87-24', x87=SINGLE),
        Build('signed', signed=True),
        Build('wide', wide=True),
        Build('double-0.1', double_tenth=True),
        Build('ftz', ftz=True),
        Build('toward-0', toward_zero=True),
    ]
