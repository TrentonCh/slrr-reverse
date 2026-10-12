"""floatlit.py - the bit pattern of a float literal.

    bits = float_bits(mode, int_digits, frac_digits)
    bad = self_test()               # [] when this build computes the right bits

The scanner hands over the digits before and behind the point
(scanner.float_digits); this module turns them into the 32 bits of the
`float` node.

WHICH CONVERSION THE GAME USES IS AN OPEN QUESTION (notes/compiler_spec.md
1.4 item 1).  Four conversions occur in the shipped classes, one per class
file, and nothing in a source says which; the scanner of the exe has to be
read to settle it.  All four are implemented and selectable:

    A   s = 1;     per digit:  s = s / 10;      v = v + d * s      single precision
    B   s = 0.1f;  per digit:  v = v + d * s;   s = s * 0.1f       single precision
    C   as A, but d * s and the sum are formed in double before the store to float
    D   s = 0.1f;  per digit:  v = v + d * s;   s = s / 10         double, one rounding at the end

v starts as the integer part, which is accumulated in an unsigned 32-bit
integer and wraps.  Of the 2,514 oracle files with a float literal A
reproduces 2,426, and A then B then C then D cover 2,426, 2,486, 2,508, 2,514;
the results differ by at most 3 units in the last place.  A is the default
for a new compilation (the largest group, and the only one the older builds
show); the oracle tests take the setting per file from
tests/baseline/m5_float.tsv.

The arithmetic is written with explicit IEEE operations so that it does not
depend on the floating point state of the host.  r32(x) rounds a double to
single precision; a single precision operation is the double operation
followed by r32, which is exact for one sum, product or quotient of two
singles (53 >= 2 * 24 + 2).  tests/floatmodel.py is the same in exact
arithmetic (fractions, every rounding written out); the two agree on every
float literal of the corpus under each setting.

C++ (sc_float.cpp).  The four settings differ in WHERE a result is rounded,
so the C code must leave no rounding to the compiler.  Every product, sum and
quotient is stored in a named variable of the type the form names; nothing
that is rounded in between stands inside one expression:

    #pragma STDC FP_CONTRACT OFF            /* MSVC: #pragma fp_contract(off) */
    uint32_t ip;                            /* the integer part, modulo 2**32 */

    /* A */  float v = (float)ip;  float s = 1.0f;
             per digit d:   s = s / 10.0f;
                            float p = (float)d * s;
                            v = v + p;
    /* B */  float v = (float)ip;  float s = 0.1f;
             per digit d:   float p = (float)d * s;
                            v = v + p;
                            s = s * 0.1f;
    /* C */  float v = (float)ip;  float s = 1.0f;
             per digit d:   double q = (double)s / 10.0;
                            s = (float)q;
                            double p = (double)d * (double)s;
                            double t = (double)v + p;
                            v = (float)t;
    /* D */  double v = (double)ip;  double s = (double)0.1f;
             per digit d:   double p = (double)d * s;
                            v = v + p;
                            s = s / 10.0;
             result (float)v

What makes other bits out of these forms, and for how many of 17,741 literal
texts (tests/floatmodel.py: wrong_builds; the 15,954 texts of the corpus and
1,787 made ones):

    v + d * s contracted to a fused multiply-add      A 75, B 157
        (GCC and Clang do this at -O2 on a target with FMA unless
        -ffp-contract=off is given; the pragma above says the same)
    x87 arithmetic (a 32-bit build without SSE2)      A 75, B 157: the sum
        is rounded to single only when it is stored, which is setting C
    x87 under 24-bit precision control                C 75, D 5,230: both
        collapse into single precision.  Direct3D 9 sets this on the thread
        that creates a device without D3DCREATE_FPU_PRESERVE, and the
        engine creates its device without it
    the integer part converted from a signed int      every literal from
        2147483648.0 on; the wrap at 2**32 lost: every literal from
        4294967296.0 on (1000000000000000.0 stands 8 times in 2 shipped
        sources)
    0.1 where the form says 0.1f                      B 417
    a rounding mode other than to-nearest, or         most literals; the
    flush-to-zero                                     literals below 1.2e-38

So: compile sc_float.cpp for SSE2 (MSVC: /arch:SSE2 for a 32-bit build,
/fp:precise, never /fp:fast; GCC and Clang: -msse2 -mfpmath=sse
-ffp-contract=off, never -ffast-math), and run self_test() once at start-up.
SELF_TEST holds 15 literals with their bits under each setting; every
deviation of the list above changes at least one of them in every setting it
touches.  A host whose self test fails must not write a class file: its
float literals would differ from the reference by a unit in the last place
and nothing else would show it.
"""
import struct

MODES = ('A', 'B', 'C', 'D')
DEFAULT_MODE = 'A'

_PACK_F = struct.Struct('<f')
_PACK_I = struct.Struct('<I')


def r32(x):
    """The double x rounded to single precision (as a double)."""
    return _PACK_F.unpack(_PACK_F.pack(x))[0]


def bits_of(x):
    """The bit pattern of the single nearest to the double x."""
    return _PACK_I.unpack(_PACK_F.pack(x))[0]


F01 = r32(0.1)              # the float constant 0.1f


def int_part(digits):
    """The digits before the point as an unsigned 32-bit number (wraps)."""
    v = 0
    for ch in digits:
        v = (v * 10 + (ord(ch) - 48)) & 0xFFFFFFFF
    return v


def float_bits(mode, int_digits, frac_digits):
    """Bit pattern of the literal  int_digits "." frac_digits  under the
    conversion `mode` ('A', 'B', 'C' or 'D').  Either part may be empty."""
    ip = int_part(int_digits)
    if mode == 'B':
        v = r32(float(ip))
        s = F01
        for ch in frac_digits:
            d = ord(ch) - 48
            v = r32(v + r32(d * s))
            s = r32(s * F01)
        return bits_of(v)
    if mode == 'C':
        v = r32(float(ip))
        s = 1.0
        for ch in frac_digits:
            d = ord(ch) - 48
            s = r32(s / 10.0)
            v = r32(v + d * s)
        return bits_of(v)
    if mode == 'D':
        v = float(ip)
        s = F01
        for ch in frac_digits:
            d = ord(ch) - 48
            v = v + d * s
            s = s / 10.0
        return bits_of(v)
    # A, the default
    v = r32(float(ip))
    s = 1.0
    for ch in frac_digits:
        d = ord(ch) - 48
        s = r32(s / 10.0)
        v = r32(v + r32(d * s))
    return bits_of(v)


# ---------------------------------------------------------------------------
# the self test
# ---------------------------------------------------------------------------
#
# (integer digits, fraction digits, bits under A, B, C, D).  The bits are those
# of tests/floatmodel.py (exact arithmetic), not of the code above.  What each
# row is for:
#
#   0.0, 1.0, 0.1, 0.5         the plain cases; 0.1 and 0.5 show a rounding
#                              mode other than to-nearest
#   1.9, 0.527                 A and B differ from C and D: a fused
#                              multiply-add or x87 evaluation turns A and B
#                              into C; 24-bit precision control turns C and D
#                              into A
#   0.69                       B alone differs: 0.1 in place of 0.1f
#   0.958                      D alone differs
#   0.00065                    the literal of the corpus on which the four
#                              differ most (AirFuelDeliverySystem)
#   3000000000.0               above 2**31: the integer part is unsigned
#   4294967297.5               above 2**32: the integer part wraps (to 1)
#   1000000000000000.0         both, and a literal of 2 shipped sources
#   0.000...003 (3e-39)        a subnormal result: gradual underflow
#   16777217.0                 2**24 + 1: the integer part is rounded, to even
#   123456.789                 integer and fraction together

SELF_TEST = (
    ('0', '0', 0x00000000, 0x00000000, 0x00000000, 0x00000000),
    ('1', '0', 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000),
    ('0', '1', 0x3DCCCCCD, 0x3DCCCCCD, 0x3DCCCCCD, 0x3DCCCCCD),
    ('0', '5', 0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000),
    ('1', '9', 0x3FF33334, 0x3FF33334, 0x3FF33333, 0x3FF33333),
    ('0', '69', 0x3F30A3D7, 0x3F30A3D8, 0x3F30A3D7, 0x3F30A3D7),
    ('0', '527', 0x3F06E978, 0x3F06E979, 0x3F06E979, 0x3F06E979),
    ('0', '958', 0x3F753F7E, 0x3F753F7E, 0x3F753F7E, 0x3F753F7D),
    ('0', '00065', 0x3A2A64C1, 0x3A2A64C4, 0x3A2A64C1, 0x3A2A64C3),
    ('3000000000', '0', 0x4F32D05E, 0x4F32D05E, 0x4F32D05E, 0x4F32D05E),
    ('4294967297', '5', 0x3FC00000, 0x3FC00000, 0x3FC00000, 0x3FC00000),
    ('1000000000000000', '0', 0x4F24C680, 0x4F24C680, 0x4F24C680, 0x4F24C680),
    ('0', '000000000000000000000000000000000000003',
     0x0020AAC8, 0x0020AACB, 0x0020AAC8, 0x0020AAC8),
    ('16777217', '0', 0x4B800000, 0x4B800000, 0x4B800000, 0x4B800000),
    ('123456', '789', 0x47F12065, 0x47F12065, 0x47F12065, 0x47F12065),
)


def self_test():
    """Does this build compute the bits of SELF_TEST?  Returns the rows it
    does not, as (mode, literal text, bits computed, bits expected); [] when
    all is well.  A host runs it once before it compiles anything (the
    module text says what it guards against)."""
    bad = []
    for row in SELF_TEST:
        k = 0
        for mode in MODES:
            got = float_bits(mode, row[0], row[1])
            if got != row[2 + k]:
                bad.append((mode, row[0] + '.' + row[1], got, row[2 + k]))
            k += 1
    return bad
