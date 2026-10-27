// floatlit.cpp - the bit pattern of a float literal.  (Reference:
// slrrc/floatlit.py; the functions stand in the order of that file and carry
// its names.)
//
// THE ARITHMETIC IS WRITTEN SO THAT THE COMPILER HAS NOTHING TO DECIDE.  The
// four settings differ in where a result is rounded.  Every product, sum and
// quotient below is one operation on two operands of one type, stored in a
// named variable of the type the form names (the reference: r32() around
// every operation that the form rounds to single).  Nothing that is rounded
// in between stands inside one expression, so no evaluation in a wider format
// and no fused multiply-add can stand in for two roundings.
//
// The build has to do the rest (CMakeLists.txt): arithmetic in the declared
// type (SSE2, not values kept in x87 registers), no contraction, no fast-math.
// The pragmas below say the same to the compilers that listen, so that a host
// which compiles this file with its own flags gets the right code where that
// can be forced; self_test() finds out whatever the flags were.
//
// WHERE THE C++ DIFFERS IN FORM FROM THE REFERENCE
//
//   - A setting is a Mode, not a letter; ORIGIN is origin().
//   - r32(x) is the store into a float variable, bits_of(x) a copy of the
//     four bytes.  The reference computes in double and rounds; a single
//     precision operation gives the same result, because the double result of
//     one sum, product or quotient of two singles rounds to the single the
//     operation gives directly (53 >= 2 * 24 + 2).
//   - (float)ip, the unsigned integer part with one rounding, is written
//     through double: every 32-bit number is a double, and the store to float
//     is the one rounding.  No helper of the compiler for "unsigned to float"
//     has to be trusted.
#include "floatlit.hpp"

#include <cstring>
#include <utility>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
// precise: every operation in the type of its operands, a store rounds; also
// when the file is compiled with /fp:fast
#pragma float_control(precise, on)
#pragma fp_contract(off)
#endif
// (GCC has no pragma for it: -ffp-contract=off, CMakeLists.txt)

namespace slrrc {
namespace floatlit {

bool is_mode(Mode mode) {
    return mode == Mode::A || mode == Mode::B || mode == Mode::C || mode == Mode::D;
}

char mode_letter(Mode mode) {
    return static_cast<char>(mode);
}

bool mode_from_letter(char letter, Mode& mode) {
    for (Mode m : MODES) {
        if (static_cast<char>(m) == letter) {
            mode = m;
            return true;
        }
    }
    return false;
}

const char* origin(Mode mode) {
    switch (mode) {
    case Mode::A: return "x87 code under 24-bit precision control (shipped files of earlier builds)";
    case Mode::B: return "SSE single precision: the scanner of the Build 940 exe";
    case Mode::C:
        return "x87 code at 53 or 64 bits, values stored to float (shipped files of earlier builds)";
    case Mode::D:
        return "x87 code at 53 or 64 bits, values in registers (shipped files of earlier builds)";
    }
    return "";
}

namespace {

// The bit pattern of a single.
uint32_t bits_of(float x) {
    uint32_t bits = 0;
    std::memcpy(&bits, &x, sizeof bits);
    return bits;
}

// (float)ip for an unsigned integer part: one rounding.
float single_of(uint32_t ip) {
    const double exact = static_cast<double>(ip);       // every 32-bit number is a double
    const float rounded = static_cast<float>(exact);
    return rounded;
}

}  // namespace

uint32_t int_part(std::string_view digits) {
    uint32_t v = 0;
    for (char ch : digits) {
        v = v * 10 + static_cast<uint32_t>(ch - '0');   // modulo 2**32
    }
    return v;
}

uint32_t float_bits(Mode mode, std::string_view int_digits, std::string_view frac_digits) {
    if (int_digits.empty()) {
        return 0x00000000;
    }
    const uint32_t ip = int_part(int_digits);
    if (frac_digits.size() > MAX_FRACTION_DIGITS) {
        frac_digits = frac_digits.substr(0, MAX_FRACTION_DIGITS);
    }
    if (mode == Mode::B) {
        // the exe: t = d * s (MULSS); s = s * 0.1f (MULSS); v = v + t (ADDSS)
        float v = single_of(ip);
        float s = 0.1f;
        for (char ch : frac_digits) {
            const float d = static_cast<float>(ch - '0');
            const float t = d * s;
            s = s * 0.1f;
            v = v + t;
        }
        return bits_of(v);
    }
    if (mode == Mode::C) {
        float v = single_of(ip);
        float s = 1.0f;
        for (char ch : frac_digits) {
            const double d = static_cast<double>(ch - '0');
            const double q = static_cast<double>(s) / 10.0;
            s = static_cast<float>(q);
            const double p = d * static_cast<double>(s);
            const double t = static_cast<double>(v) + p;
            v = static_cast<float>(t);
        }
        return bits_of(v);
    }
    if (mode == Mode::D) {
        double v = static_cast<double>(ip);
        double s = static_cast<double>(0.1f);
        for (char ch : frac_digits) {
            const double d = static_cast<double>(ch - '0');
            const double p = d * s;
            v = v + p;
            s = s / 10.0;
        }
        const float rounded = static_cast<float>(v);
        return bits_of(rounded);
    }
    // A
    float v = single_of(ip);
    float s = 1.0f;
    for (char ch : frac_digits) {
        const float d = static_cast<float>(ch - '0');
        s = s / 10.0f;
        const float p = d * s;
        v = v + p;
    }
    return bits_of(v);
}

// ---------------------------------------------------------------------------
// the self test
// ---------------------------------------------------------------------------

std::vector<BadRow> self_test() {
    std::vector<BadRow> bad;
    for (const SelfTestRow& row : SELF_TEST) {
        size_t k = 0;
        for (Mode mode : MODES) {
            const uint32_t got = float_bits(mode, row.int_digits, row.frac_digits);
            if (got != row.bits[k]) {
                std::string literal(row.int_digits);
                literal += '.';
                literal += row.frac_digits;
                bad.push_back(BadRow{mode, std::move(literal), got, row.bits[k]});
            }
            k += 1;
        }
    }
    for (const EdgeTestRow& row : EDGE_TEST) {
        for (Mode mode : MODES) {
            const uint32_t got = float_bits(mode, row.int_digits, row.frac_digits);
            if (got != row.bits) {
                std::string literal(row.int_digits);
                literal += '.';
                literal += row.frac_digits;
                bad.push_back(BadRow{mode, std::move(literal), got, row.bits});
            }
        }
    }
    return bad;
}

}  // namespace floatlit
}  // namespace slrrc
