// scanner.hpp - the tokens of one source text.  (Reference: slrrc/scanner.py.)
//
//     toks = scan(source, diags)             the last token is T_EOF
//     toks = scan(source, diags, &comments)  comments receives (pos, length)
//
// The source is the bytes of the file, CR LF kept; nothing is decoded
// (compiler_spec.md, "Encoding").  Lines are counted by LF, CR is white space.
// The rules are those of notes/grammar.md section 2, which hold for all
// 852,211 tokens of the shipped sources.
//
// A Token is (kind, pos, length, line); its text is source.substr(pos,
// length).  Every keyword and every operator has a kind of its own, so the
// parser never compares text.
//
// The scanner stops at the first error (E_LEX, or E_UNSUPPORTED for a literal
// form the dialect has no rule for) and returns the tokens read so far, closed
// by T_EOF; the caller looks at diags.errors.
//
// Literal values are functions of the token text:
//     int_value(text)      unsigned 32-bit value (wraps modulo 2**32)
//     string_value(text)   text after escape processing
//     rid_parts(text)      (pack name as written, local id)
//     float_digits(text)   (integer digits, fraction digits)
//
// NUMBERS.  Offsets, lengths and lines are unsigned.  "Not found" is never a
// negative number: a search answers the length of the text (find), and a
// function that returns an offset behind something answers 0 when there is
// nothing (rid_tail, string_end; no token ends at offset 0).  A token holds
// 32-bit offsets, so a source of more than MAX_SOURCE bytes is refused
// (E_LIMIT); the reference has no such bound because its offsets have no
// width.
#ifndef SLRRC_SCANNER_HPP
#define SLRRC_SCANNER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "diag.hpp"

namespace slrrc {
namespace scanner {

// ---------------------------------------------------------------------------
// token kinds
// ---------------------------------------------------------------------------

enum Kind : uint8_t {
    T_EOF = 0,
    T_ID = 1,
    T_INT = 2,
    T_FLOAT = 3,
    T_STR = 4,
    T_RID = 5,

    // keywords (all 35 words the scanner reserves; notes/grammar.md 2.3)
    KW_PACKAGE = 10,
    KW_IMPORT = 11,
    KW_CLASS = 12,
    KW_INTERFACE = 13,
    KW_EXTENDS = 14,
    KW_IMPLEMENTS = 15,
    KW_PUBLIC = 16,
    KW_PRIVATE = 17,
    KW_PROTECTED = 18,
    KW_STATIC = 19,
    KW_FINAL = 20,
    KW_ABSTRACT = 21,
    KW_NATIVE = 22,
    KW_SYNCHRONIZED = 23,
    KW_NEW = 24,
    KW_RETURN = 25,
    KW_IF = 26,
    KW_ELSE = 27,
    KW_FOR = 28,
    KW_WHILE = 29,
    KW_DO = 30,
    KW_SWITCH = 31,
    KW_CASE = 32,
    KW_DEFAULT = 33,
    KW_BREAK = 34,
    KW_CONTINUE = 35,
    KW_THIS = 36,
    KW_SUPER = 37,
    KW_NULL = 38,
    KW_TRUE = 39,
    KW_FALSE = 40,
    KW_INSTANCEOF = 41,
    KW_VOID = 42,
    KW_INT = 43,
    KW_FLOAT = 44,

    // operators and punctuation (notes/grammar.md 2.8)
    P_SEMI = 50,            // ;
    P_COMMA = 51,           // ,
    P_DOT = 52,             // .
    P_LPAREN = 53,          // (
    P_RPAREN = 54,          // )
    P_LBRACE = 55,          // {
    P_RBRACE = 56,          // }
    P_LBRACKET = 57,        // [
    P_RBRACKET = 58,        // ]
    P_ASSIGN = 59,          // =
    P_ADD_ASSIGN = 60,      // +=
    P_SUB_ASSIGN = 61,      // -=
    P_MUL_ASSIGN = 62,      // *=
    P_DIV_ASSIGN = 63,      // /=
    P_MOD_ASSIGN = 64,      // %=
    P_AND_ASSIGN = 65,      // &=
    P_OR_ASSIGN = 66,       // |=
    P_XOR_ASSIGN = 67,      // ^=
    P_SHL_ASSIGN = 68,      // <<=
    P_SHR_ASSIGN = 69,      // >>=
    P_USHR_ASSIGN = 70,     // >>>=
    P_QUESTION = 71,        // ?
    P_COLON = 72,           // :
    P_LOR = 73,             // ||
    P_LAND = 74,            // &&
    P_OR = 75,              // |
    P_XOR = 76,             // ^
    P_AND = 77,             // &
    P_EQ = 78,              // ==
    P_NE = 79,              // !=
    P_LT = 80,              // <
    P_GT = 81,              // >
    P_LE = 82,              // <=
    P_GE = 83,              // >=
    P_SHL = 84,             // <<
    P_SHR = 85,             // >>
    P_USHR = 86,            // >>>
    P_ADD = 87,             // +
    P_SUB = 88,             // -
    P_MUL = 89,             // *
    P_DIV = 90,             // /
    P_MOD = 91,             // %
    P_NOT = 92,             // !
    P_BITNOT = 93,          // ~
    P_INC = 94,             // ++
    P_DEC = 95              // --
};

inline constexpr uint32_t KIND_COUNT = 96;      // every kind is below it

// KEYWORDS.get(text, T_ID): the kind of the keyword, T_ID for any other word.
uint8_t keyword(std::string_view text);

// OPERATORS.get(text, T_EOF): the kind of the operator that is exactly this
// text, T_EOF when there is none.
uint8_t operator_kind(std::string_view text);

// KIND_NAMES[kind]: kind -> name, for dumps and messages ("eof", "id", "int",
// "float", "str", "rid", "kw", "op").
const char* kind_name(uint8_t kind);

// FIXED_TEXT[kind]: the one text a keyword or operator token has ("" for a
// kind that has none).
const char* fixed_text(uint8_t kind);

inline constexpr uint32_t MAX_LINE = 0xFFFF;            // the line of a tree node is a u16
inline constexpr uint32_t MAX_SOURCE = 0x7FFFFFFF;      // bytes; a token holds 32-bit offsets

struct Token {
    uint8_t kind;
    uint32_t pos;           // offset of the first character
    uint32_t length;
    uint32_t line;          // 1-based line of the first character
};

struct Comment {
    uint32_t pos;
    uint32_t length;
};

// 1-based column of the character at pos (a TAB counts as one).
uint32_t column_at(std::string_view source, uint32_t pos);

// Tokens of the source, closed by a T_EOF token.
std::vector<Token> scan(std::string_view source, Diagnostics& diags,
                        std::vector<Comment>* comments = nullptr);

// ---------------------------------------------------------------------------
// literal values
// ---------------------------------------------------------------------------

// Value of an int token as an unsigned 32-bit number: the literal modulo
// 2**32, decimal or hexadecimal (25,940 of 25,940 literals of the pairs).
uint32_t int_value(std::string_view text);

// Text of a str token after escape processing.  Proven: \n is LF, \t is
// TAB, \" and \\ stand for the character (12,753 of 12,753 literals).
// Unproven (W_UNVERIFIED at scan time): \r is CR, \0 is NUL, any other
// escaped character stands for itself.
std::string string_value(std::string_view text);

// ("cars.racers.Axis", 0xFFB4) for "cars.racers.Axis:0x0000FFB4r": the pack
// name as written and the local id (modulo 2**32).  The name is a view into
// the text passed in.
struct RidParts {
    std::string_view pack;
    uint32_t id;
};
RidParts rid_parts(std::string_view text);

// ("12", "50") for "12.50f": the digits before and behind the point, without
// the suffix.  Either part may be empty.  Views into the text passed in.
struct FloatDigits {
    std::string_view int_digits;
    std::string_view frac_digits;
};
FloatDigits float_digits(std::string_view text);

}  // namespace scanner
}  // namespace slrrc

#endif
