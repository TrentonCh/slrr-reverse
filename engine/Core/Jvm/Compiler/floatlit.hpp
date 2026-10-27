// floatlit.hpp - the bit pattern of a float literal.  (Reference:
// slrrc/floatlit.py.)
//
//     bits = float_bits(mode, int_digits, frac_digits)
//     bad = self_test()               empty when this build computes the right bits
//
// The scanner hands over the digits before and behind the point
// (scanner::float_digits); this module turns them into the 32 bits of the
// `float` node.
//
// THE CONVERSION OF BUILD 940 IS SETTING B.  It was read in the exe
// (ref/notes/exe_reads.md 1): the scanner converts a float token with the
// engine's own sscanf("%f"), whose digit loop (0x527530) is scalar SSE single
// precision,
//
//     uint32 ip = 0;   per leading digit:  ip = ip * 10 + digit       (wraps)
//     float  v  = (float)ip;                                 unsigned, one rounding
//     float  s  = 0.1f;                                      0x3DCCCCCD
//     per fraction digit d, at most 1,024 of them:
//         float t = (float)d * s;      MULSS
//         s = s * 0.1f;                MULSS
//         v = v + t;                   ADDSS
//
// with nothing wider than a single at any point.  B is therefore the default,
// and what a host has to use for every class it compiles.
//
// Two things the routine does that a decimal reader would not (both read, not
// run; no shipped source has either form):
//
//   - it stops at a first character that is no digit and stores nothing, and
//     the caller's result was preset to 0.0: the literal ".5" compiles to 0.0;
//   - only the first 1,024 fraction digits enter the sum
//     (MAX_FRACTION_DIGITS).  No bit depends on that: the scale has long
//     underflowed to zero by then.
//
// A, C AND D ARE HISTORY.  215 of the 2,649 shipped class files the compiler
// reproduces hold a literal that B converts to other bits; the same routine
// in x87 arithmetic gives them (the 2005 build of the game has it in that
// form).  Written as the single precision or double precision forms that give
// the same bits on every float literal of the corpus:
//
//     A   s = 1;     per digit:  s = s / 10;      v = v + d * s      single precision
//                    = the x87 code under 24-bit precision control
//     C   as A, but d * s and the sum are formed in double before the store to float
//                    = x87 code at 53 or 64 bits that stores v and s to float
//                      after every statement
//     D   s = 0.1f;  per digit:  v = v + d * s;   s = s / 10         double, one rounding at the end
//                    = the x87 code at 53 or 64 bits with v and s in registers
//
// The three stay selectable for one purpose: to reproduce those shipped files
// in the oracle tests.  Never use them for a new compilation.
//
// The integer part is accumulated in an unsigned 32-bit integer and wraps, in
// all four.  The two exits of the routine (no digit before the point, 1,024
// fraction digits) are applied to all four.
//
// WHAT THE BUILD HAS TO GUARANTEE.  The four settings differ in WHERE a result
// is rounded, so the code leaves no rounding to the compiler: every product,
// sum and quotient is stored in a named variable of the type the form names,
// and nothing that is rounded in between stands inside one expression
// (floatlit.cpp).  What makes other bits out of these forms all the same
// (counts over 17,741 literal texts, ref/tests/floatmodel.py):
//
//     v + d * s contracted to a fused multiply-add          A 75, B 157
//     x87 arithmetic with values kept in registers           A 75, B 157
//     x87 under 24-bit precision control                     C 75, D 5,230
//       (Direct3D 9 sets this on the thread that creates a device without
//       D3DCREATE_FPU_PRESERVE)
//     the integer part converted from a signed int           every literal from 2147483648.0 on
//     0.1 where the form says 0.1f                           B 417
//     a rounding mode other than to-nearest, flush-to-zero   most literals; the literals below 1.2e-38
//     a library conversion (strtof, from_chars)              B 4,748 of the 15,954 texts of the
//                                                            corpus; and ".5", which is 0.0
//
// So: compile floatlit.cpp for SSE2 with the standard semantics (MSVC:
// /arch:SSE2 for a 32-bit build, /fp:precise, never /fp:fast; GCC and Clang:
// -msse2 -mfpmath=sse -ffp-contract=off, never -ffast-math); CMakeLists.txt
// sets these flags and says why.  And run self_test() once at start-up, on
// the thread that will compile: it also sees the state of the floating point
// unit (rounding mode, flush-to-zero) at that moment.  SELF_TEST holds 15
// literals with their bits under each setting; every deviation of the list
// above changes at least one of them in every setting it touches.  EDGE_TEST
// holds the forms the routine does not read as a decimal number.  A host
// whose self test fails must not write a class file: its float literals would
// differ from the reference by a unit in the last place and nothing else
// would show it.
//
// This header can stand behind <windows.h>.
#ifndef SLRRC_FLOATLIT_HPP
#define SLRRC_FLOATLIT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slrrc {
namespace floatlit {

// The conversion of a float literal.  (The reference: the letter itself.)
enum class Mode : char { A = 'A', B = 'B', C = 'C', D = 'D' };

inline constexpr Mode MODES[] = {Mode::A, Mode::B, Mode::C, Mode::D};
inline constexpr size_t MODE_COUNT = 4;
inline constexpr Mode EXE_MODE = Mode::B;           // the conversion the Build 940 exe contains
inline constexpr Mode DEFAULT_MODE = EXE_MODE;      // ... and so the one for every new compilation
inline constexpr size_t MAX_FRACTION_DIGITS = 1024; // fraction digits that enter the sum

// mode in MODES: is the value one of the four?  (An enum can be made of any
// number; a host that takes the setting from outside checks it here, or reads
// it with mode_from_letter.)
bool is_mode(Mode mode);

// The letter of a setting, 'A' to 'D'.
char mode_letter(Mode mode);

// The setting a letter names; false for any other character (upper case
// only, as the reference has it).
bool mode_from_letter(char letter, Mode& mode);

// ORIGIN[mode]: what each setting is, in one line (the command line and the
// tests print it).  "" for a value that is no setting.
const char* origin(Mode mode);

// The digits before the point as an unsigned 32-bit number (wraps).
uint32_t int_part(std::string_view digits);

// Bit pattern of the literal  int_digits "." frac_digits  under the
// conversion `mode`.
//
// A literal without a digit before the point is 0.0 whatever follows (the
// routine of the exe stops at a first character that is no digit); a literal
// without a digit behind it is its integer part.  Fraction digits beyond
// MAX_FRACTION_DIGITS are not looked at.  The digits are '0' to '9' (what the
// scanner hands over).  A mode that is none of the four is taken as A, as the
// reference's last branch does; the driver refuses such a mode before it gets
// here (slrrc.cpp).
uint32_t float_bits(Mode mode, std::string_view int_digits, std::string_view frac_digits);

// ---------------------------------------------------------------------------
// the self test
// ---------------------------------------------------------------------------
//
// (integer digits, fraction digits, bits under A, B, C, D).  The bits are those
// of ref/tests/floatmodel.py (exact arithmetic), not of the code of this
// module.  What each row is for:
//
//   0.0, 1.0, 0.1, 0.5         the plain cases; 0.1 and 0.5 show a rounding
//                              mode other than to-nearest
//   1.9, 0.527                 A and B differ from C and D: a fused
//                              multiply-add or x87 evaluation turns A and B
//                              into C; 24-bit precision control turns C and D
//                              into A
//   0.69                       B alone differs: 0.1 in place of 0.1f
//   0.958                      D alone differs
//   0.00065                    the literal of the corpus on which the four
//                              differ most (AirFuelDeliverySystem)
//   3000000000.0               above 2**31: the integer part is unsigned
//   4294967297.5               above 2**32: the integer part wraps (to 1)
//   1000000000000000.0         both, and a literal of 2 shipped sources
//   0.000...003 (3e-39)        a subnormal result: gradual underflow
//   16777217.0                 2**24 + 1: the integer part is rounded, to even
//   123456.789                 integer and fraction together

struct SelfTestRow {
    const char* int_digits;
    const char* frac_digits;
    uint32_t bits[MODE_COUNT];          // under A, B, C, D
};

inline constexpr SelfTestRow SELF_TEST[] = {
    {"0", "0", {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {"1", "0", {0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000}},
    {"0", "1", {0x3DCCCCCD, 0x3DCCCCCD, 0x3DCCCCCD, 0x3DCCCCCD}},
    {"0", "5", {0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000}},
    {"1", "9", {0x3FF33334, 0x3FF33334, 0x3FF33333, 0x3FF33333}},
    {"0", "69", {0x3F30A3D7, 0x3F30A3D8, 0x3F30A3D7, 0x3F30A3D7}},
    {"0", "527", {0x3F06E978, 0x3F06E979, 0x3F06E979, 0x3F06E979}},
    {"0", "958", {0x3F753F7E, 0x3F753F7E, 0x3F753F7E, 0x3F753F7D}},
    {"0", "00065", {0x3A2A64C1, 0x3A2A64C4, 0x3A2A64C1, 0x3A2A64C3}},
    {"3000000000", "0", {0x4F32D05E, 0x4F32D05E, 0x4F32D05E, 0x4F32D05E}},
    {"4294967297", "5", {0x3FC00000, 0x3FC00000, 0x3FC00000, 0x3FC00000}},
    {"1000000000000000", "0", {0x4F24C680, 0x4F24C680, 0x4F24C680, 0x4F24C680}},
    {"0", "000000000000000000000000000000000000003",
     {0x0020AAC8, 0x0020AACB, 0x0020AAC8, 0x0020AAC8}},
    {"16777217", "0", {0x4B800000, 0x4B800000, 0x4B800000, 0x4B800000}},
    {"123456", "789", {0x47F12065, 0x47F12065, 0x47F12065, 0x47F12065}},
};

// (integer digits, fraction digits, bits under every setting): the literal
// forms the routine of the exe does not read as a decimal reader would.  A
// port that converts with a library function fails here whatever its
// arithmetic.
//
//   .5, .25, .0                no digit before the point: 0.0
//   5.                         no digit behind it: the integer part
struct EdgeTestRow {
    const char* int_digits;
    const char* frac_digits;
    uint32_t bits;
};

inline constexpr EdgeTestRow EDGE_TEST[] = {
    {"", "5", 0x00000000},
    {"", "25", 0x00000000},
    {"", "0", 0x00000000},
    {"5", "", 0x40A00000},
};

// A row this build does not compute: the setting, the literal text, the bits
// computed and the bits expected.
struct BadRow {
    Mode mode;
    std::string literal;
    uint32_t got;
    uint32_t expected;
};

// Does this build compute the bits of SELF_TEST and EDGE_TEST?  Returns the
// rows it does not; nothing when all is well.  A host runs it once before it
// compiles anything (the head of this file says what it guards against).
std::vector<BadRow> self_test();

}  // namespace floatlit
}  // namespace slrrc

#endif
