// scanner.cpp - the tokens of one source text.  (Reference: slrrc/scanner.py;
// the functions stand in the order of that file and carry its names, a
// leading underscore dropped.)
#include "scanner.hpp"

namespace slrrc {
namespace scanner {

namespace {

// ---------------------------------------------------------------------------
// tables
// ---------------------------------------------------------------------------

struct Word {
    std::string_view text;
    uint8_t kind;
};

// in the order of their kinds: KEYWORDS[k - KW_PACKAGE] is the word of kind k
constexpr Word KEYWORDS[] = {
    {"package", KW_PACKAGE}, {"import", KW_IMPORT}, {"class", KW_CLASS},
    {"interface", KW_INTERFACE}, {"extends", KW_EXTENDS}, {"implements", KW_IMPLEMENTS},
    {"public", KW_PUBLIC}, {"private", KW_PRIVATE}, {"protected", KW_PROTECTED},
    {"static", KW_STATIC}, {"final", KW_FINAL}, {"abstract", KW_ABSTRACT},
    {"native", KW_NATIVE}, {"synchronized", KW_SYNCHRONIZED}, {"new", KW_NEW},
    {"return", KW_RETURN}, {"if", KW_IF}, {"else", KW_ELSE}, {"for", KW_FOR},
    {"while", KW_WHILE}, {"do", KW_DO}, {"switch", KW_SWITCH}, {"case", KW_CASE},
    {"default", KW_DEFAULT}, {"break", KW_BREAK}, {"continue", KW_CONTINUE},
    {"this", KW_THIS}, {"super", KW_SUPER}, {"null", KW_NULL}, {"true", KW_TRUE},
    {"false", KW_FALSE}, {"instanceof", KW_INSTANCEOF}, {"void", KW_VOID},
    {"int", KW_INT}, {"float", KW_FLOAT},
};

// in the order of their kinds: OPERATORS[k - P_SEMI] is the operator of kind k
constexpr Word OPERATORS[] = {
    {";", P_SEMI}, {",", P_COMMA}, {".", P_DOT}, {"(", P_LPAREN}, {")", P_RPAREN},
    {"{", P_LBRACE}, {"}", P_RBRACE}, {"[", P_LBRACKET}, {"]", P_RBRACKET},
    {"=", P_ASSIGN}, {"+=", P_ADD_ASSIGN}, {"-=", P_SUB_ASSIGN}, {"*=", P_MUL_ASSIGN},
    {"/=", P_DIV_ASSIGN}, {"%=", P_MOD_ASSIGN}, {"&=", P_AND_ASSIGN}, {"|=", P_OR_ASSIGN},
    {"^=", P_XOR_ASSIGN}, {"<<=", P_SHL_ASSIGN}, {">>=", P_SHR_ASSIGN},
    {">>>=", P_USHR_ASSIGN}, {"?", P_QUESTION}, {":", P_COLON}, {"||", P_LOR},
    {"&&", P_LAND}, {"|", P_OR}, {"^", P_XOR}, {"&", P_AND}, {"==", P_EQ}, {"!=", P_NE},
    {"<", P_LT}, {">", P_GT}, {"<=", P_LE}, {">=", P_GE}, {"<<", P_SHL}, {">>", P_SHR},
    {">>>", P_USHR}, {"+", P_ADD}, {"-", P_SUB}, {"*", P_MUL}, {"/", P_DIV}, {"%", P_MOD},
    {"!", P_NOT}, {"~", P_BITNOT}, {"++", P_INC}, {"--", P_DEC},
};

constexpr uint32_t KEYWORD_COUNT = sizeof(KEYWORDS) / sizeof(KEYWORDS[0]);
constexpr uint32_t OPERATOR_COUNT = sizeof(OPERATORS) / sizeof(OPERATORS[0]);

constexpr bool in_kind_order(const Word* words, uint32_t count, uint32_t first) {
    for (uint32_t i = 0; i < count; ++i) {
        if (words[i].kind != first + i) {
            return false;
        }
    }
    return true;
}

static_assert(KEYWORD_COUNT == 35 && in_kind_order(KEYWORDS, KEYWORD_COUNT, KW_PACKAGE),
              "KEYWORDS is indexed by kind");
static_assert(OPERATOR_COUNT == 46 && in_kind_order(OPERATORS, OPERATOR_COUNT, P_SEMI),
              "OPERATORS is indexed by kind");
static_assert(KW_FLOAT == KW_PACKAGE + KEYWORD_COUNT - 1 && P_DEC == P_SEMI + OPERATOR_COUNT - 1 &&
              P_DEC < KIND_COUNT, "the kinds are contiguous");

// The character sets of the reference (_WHITE, _DIGITS, _HEX, _ID_START,
// _ID_CHAR, _FLOAT_SUFFIX) as one 256-entry table of bits.
constexpr uint8_t C_WHITE = 0x01;           // ' ' TAB CR FF VT; LF is handled apart: it counts lines
constexpr uint8_t C_DIGIT = 0x02;           // 0-9
constexpr uint8_t C_HEX = 0x04;             // 0-9 A-F a-f
constexpr uint8_t C_ID_START = 0x08;        // A-Z a-z _
constexpr uint8_t C_ID_CHAR = 0x10;         // A-Z a-z _ 0-9
constexpr uint8_t C_FLOAT_SUFFIX = 0x20;    // f F d D

struct CharTable {
    uint8_t bits[256];
};

constexpr CharTable make_char_table() {
    CharTable t = {};
    const char white[] = {' ', '\t', '\r', '\f', '\v'};
    for (char ch : white) {
        t.bits[static_cast<unsigned char>(ch)] |= C_WHITE;
    }
    for (unsigned ch = '0'; ch <= '9'; ++ch) {
        t.bits[ch] |= C_DIGIT | C_HEX | C_ID_CHAR;
    }
    for (unsigned ch = 'A'; ch <= 'Z'; ++ch) {
        t.bits[ch] |= C_ID_START | C_ID_CHAR;
    }
    for (unsigned ch = 'a'; ch <= 'z'; ++ch) {
        t.bits[ch] |= C_ID_START | C_ID_CHAR;
    }
    t.bits[static_cast<unsigned char>('_')] |= C_ID_START | C_ID_CHAR;
    for (unsigned ch = 'A'; ch <= 'F'; ++ch) {
        t.bits[ch] |= C_HEX;
    }
    for (unsigned ch = 'a'; ch <= 'f'; ++ch) {
        t.bits[ch] |= C_HEX;
    }
    const char suffix[] = {'f', 'F', 'd', 'D'};
    for (char ch : suffix) {
        t.bits[static_cast<unsigned char>(ch)] |= C_FLOAT_SUFFIX;
    }
    return t;
}

constexpr CharTable CHARS = make_char_table();

inline bool in_set(char ch, uint8_t set) {
    return (CHARS.bits[static_cast<unsigned char>(ch)] & set) != 0;
}

inline bool is_ascii(std::string_view text) {
    for (char ch : text) {
        if (static_cast<unsigned char>(ch) >= 0x80) {
            return false;
        }
    }
    return true;
}

// Number of LF characters in source[pos:end].
inline uint32_t count_lines(std::string_view source, size_t pos, size_t end) {
    uint32_t count = 0;
    for (size_t k = pos; k < end; ++k) {
        if (source[k] == '\n') {
            count += 1;
        }
    }
    return count;
}

// text.lstrip('0')
inline std::string_view lstrip_zeros(std::string_view text) {
    size_t k = 0;
    while (k < text.size() && text[k] == '0') {
        k += 1;
    }
    return text.substr(k);
}

inline bool starts_with(std::string_view text, std::string_view head) {
    return text.size() >= head.size() && text.substr(0, head.size()) == head;
}

inline Token token(uint8_t kind, size_t pos, size_t length, uint32_t line) {
    return Token{kind, static_cast<uint32_t>(pos), static_cast<uint32_t>(length), line};
}

inline uint32_t column_of(std::string_view source, size_t pos) {
    return column_at(source, static_cast<uint32_t>(pos));
}

// Offset of the first occurrence of `what` at or behind pos; the length of
// the source when there is none.
size_t find(std::string_view source, std::string_view what, size_t pos) {
    const size_t k = source.find(what, pos);
    if (k == std::string_view::npos) {
        return source.size();
    }
    return k;
}

std::string show(std::string_view text);
void comment(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags,
             std::vector<Comment>* comments);
size_t dotted_end(std::string_view source, size_t end, size_t n);
size_t rid_tail(std::string_view source, size_t k, size_t n);
size_t string_end(std::string_view source, size_t pos, size_t n);
struct Number {
    uint8_t kind;
    size_t end;
};
Number number(std::string_view source, size_t pos, size_t n);
void check_int(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags);
void check_float(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags);
void check_string(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags);
void check_rid(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags);

}  // namespace

uint8_t keyword(std::string_view text) {
    if (text.size() < 2 || text.size() > 12) {     // "do" .. "synchronized"
        return T_ID;
    }
    for (const Word& w : KEYWORDS) {
        if (w.text == text) {
            return w.kind;
        }
    }
    return T_ID;
}

uint8_t operator_kind(std::string_view text) {
    for (const Word& w : OPERATORS) {
        if (w.text == text) {
            return w.kind;
        }
    }
    return T_EOF;
}

const char* kind_name(uint8_t kind) {
    switch (kind) {
    case T_EOF: return "eof";
    case T_ID: return "id";
    case T_INT: return "int";
    case T_FLOAT: return "float";
    case T_STR: return "str";
    case T_RID: return "rid";
    default: break;
    }
    if (kind >= KW_PACKAGE && kind <= KW_FLOAT) {
        return "kw";
    }
    if (kind >= P_SEMI && kind <= P_DEC) {
        return "op";
    }
    return "?";
}

const char* fixed_text(uint8_t kind) {
    if (kind >= KW_PACKAGE && kind <= KW_FLOAT) {
        return KEYWORDS[kind - KW_PACKAGE].text.data();
    }
    if (kind >= P_SEMI && kind <= P_DEC) {
        return OPERATORS[kind - P_SEMI].text.data();
    }
    return "";
}

uint32_t column_at(std::string_view source, uint32_t pos) {
    uint32_t k = pos;
    while (k > 0 && source[k - 1] != '\n') {
        k -= 1;
    }
    return pos - k + 1;
}

// ---------------------------------------------------------------------------
// the scanner
// ---------------------------------------------------------------------------

std::vector<Token> scan(std::string_view source, Diagnostics& diags, std::vector<Comment>* comments) {
    std::vector<Token> toks;
    const size_t n = source.size();
    size_t pos = 0;
    uint32_t line = 1;
    size_t chain_end = 0;       // end of the dotted name looked at last
    const uint32_t errors_before = diags.errors;
    if (n > MAX_SOURCE) {
        // not in the reference: its offsets have no width
        diags.error(E_LIMIT, 1, 1, "source of more than " + dec(MAX_SOURCE) +
                                   " bytes (an offset is 32 bits wide)");
        toks.push_back(token(T_EOF, 0, 0, 1));
        return toks;
    }
    if (starts_with(source, "\xEF\xBB\xBF")) {
        diags.warning(W_BYTE, 1, 1, "UTF-8 byte order mark skipped");
        pos = 3;
    }
    while (pos < n && diags.errors == errors_before) {
        const char ch = source[pos];
        if (ch == '\n') {
            line += 1;
            pos += 1;
            continue;
        }
        if (in_set(ch, C_WHITE)) {
            pos += 1;
            continue;
        }
        if (in_set(ch, C_ID_START)) {
            size_t end = pos + 1;
            while (end < n && in_set(source[end], C_ID_CHAR)) {
                end += 1;
            }
            size_t rid_end = 0;
            if (end > chain_end) {
                // A resource id is a dotted name with ":0x..r" behind it.  A
                // dotted name without that tail holds no resource id, so the
                // identifiers inside it are not looked at again.
                chain_end = dotted_end(source, end, n);
                rid_end = rid_tail(source, chain_end, n);
            }
            if (rid_end > 0) {
                check_rid(source, pos, rid_end, line, diags);
                toks.push_back(token(T_RID, pos, rid_end - pos, line));
                pos = rid_end;
                continue;
            }
            const uint8_t kind = keyword(source.substr(pos, end - pos));
            toks.push_back(token(kind, pos, end - pos, line));
            pos = end;
            continue;
        }
        if (in_set(ch, C_DIGIT) || (ch == '.' && pos + 1 < n && in_set(source[pos + 1], C_DIGIT))) {
            const Number num = number(source, pos, n);
            const uint8_t kind = num.kind;
            size_t end = num.end;
            if (kind == T_INT) {
                check_int(source, pos, end, line, diags);
            } else if (kind == T_FLOAT) {
                check_float(source, pos, end, line, diags);
            } else if (in_set(ch, C_DIGIT)) {
                end = pos;
                while (end < n && end < pos + 40 &&
                       (in_set(source[end], C_ID_CHAR) || source[end] == '.')) {
                    end += 1;
                }
                diags.error(E_LEX, line, column_of(source, pos),
                            "malformed number " + show(source.substr(pos, end - pos)));
                break;
            }
            if (kind != T_EOF) {
                toks.push_back(token(kind, pos, end - pos, line));
                pos = end;
                continue;
            }
            // a '.' that starts no number is the operator
        }
        if (ch == '/' && pos + 1 < n) {
            const char nx = source[pos + 1];
            if (nx == '/') {
                const size_t end = find(source, "\n", pos);
                comment(source, pos, end, line, diags, comments);
                pos = end;
                continue;
            }
            if (nx == '*') {
                size_t end = find(source, "*/", pos + 2);
                if (end == n) {
                    diags.error(E_LEX, line, column_of(source, pos), "block comment is not closed");
                    break;
                }
                end += 2;
                comment(source, pos, end, line, diags, comments);
                line += count_lines(source, pos, end);
                pos = end;
                continue;
            }
        }
        if (ch == '"') {
            const size_t end = string_end(source, pos, n);
            if (end == 0) {
                diags.error(E_LEX, line, column_of(source, pos),
                            "string literal is not closed on its line");
                break;
            }
            check_string(source, pos, end, line, diags);
            toks.push_back(token(T_STR, pos, end - pos, line));
            pos = end;
            continue;
        }
        if (ch == '\'') {
            diags.error(E_UNSUPPORTED, line, column_of(source, pos),
                        "character literal (the dialect has none)");
            break;
        }
        // operators, longest match first
        uint8_t kind = T_EOF;
        size_t length = n - pos >= 4 ? 4 : n - pos;
        while (length > 0) {
            kind = operator_kind(source.substr(pos, length));
            if (kind != T_EOF) {
                break;
            }
            length -= 1;
        }
        if (kind == T_EOF) {
            diags.error(E_LEX, line, column_of(source, pos),
                        "character " + show(source.substr(pos, 1)) + " is not part of any token");
            break;
        }
        toks.push_back(token(kind, pos, length, line));
        pos += length;
    }
    if (diags.errors == errors_before && !toks.empty() && toks[toks.size() - 1].line > MAX_LINE) {
        // lines only grow, so the last token tells; what follows it (white
        // space, comments) stamps no node
        const Token& t = toks[toks.size() - 1];
        diags.error(E_LIMIT, t.line, column_at(source, t.pos),
                    "token behind line " + dec(MAX_LINE) +
                    " (the line of a node is 16 bits wide)");
    }
    toks.push_back(token(T_EOF, pos < n ? pos : n, 0, line));
    return toks;
}

namespace {

// A piece of source for a message: quoted, non-printing bytes escaped.
std::string show(std::string_view text) {
    std::string out("\"");
    for (char ch : text) {
        const unsigned o = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (32 <= o && o < 127) {
            out += ch;
        } else {
            out += "\\x";
            out += hex(o, 2, false);
        }
    }
    out += '"';
    return out;
}

void comment(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags,
             std::vector<Comment>* comments) {
    if (comments != nullptr) {
        comments->push_back(Comment{static_cast<uint32_t>(pos), static_cast<uint32_t>(end - pos)});
    }
    if (!is_ascii(source.substr(pos, end - pos))) {
        diags.warning(W_BYTE, line, column_of(source, pos), "byte above 0x7F in a comment");
    }
}

// end is the offset behind an identifier; returns the offset behind the
// { "." Identifier }  that follows it without white space.
size_t dotted_end(std::string_view source, size_t end, size_t n) {
    size_t k = end;
    while (k + 1 < n && source[k] == '.' && in_set(source[k + 1], C_ID_START)) {
        k += 2;
        while (k < n && in_set(source[k], C_ID_CHAR)) {
            k += 1;
        }
    }
    return k;
}

// A resource id literal is  Identifier { "." Identifier } ":" "0" ("x" |
// "X") HexDigit { HexDigit } "r"  with no identifier character behind it.
// k is the offset behind the dotted name; returns the offset behind the
// literal, or 0 when no such tail follows.
size_t rid_tail(std::string_view source, size_t k, size_t n) {
    if (!(k + 2 < n && source[k] == ':' && source[k + 1] == '0' &&
          (source[k + 2] == 'x' || source[k + 2] == 'X'))) {
        return 0;
    }
    size_t h = k + 3;
    while (h < n && in_set(source[h], C_HEX)) {
        h += 1;
    }
    if (h == k + 3 || h >= n || source[h] != 'r') {
        return 0;
    }
    if (h + 1 < n && in_set(source[h + 1], C_ID_CHAR)) {
        return 0;
    }
    return h + 1;
}

// Offset behind the closing quote of the string literal that opens at pos,
// or 0 when the line or the file ends first.  A backslash takes the next
// character with it.
size_t string_end(std::string_view source, size_t pos, size_t n) {
    size_t j = pos + 1;
    while (j < n) {
        const char ch = source[j];
        if (ch == '"') {
            return j + 1;
        }
        if (ch == '\n') {
            return 0;
        }
        if (ch == '\\') {
            if (j + 1 >= n || source[j + 1] == '\n') {
                return 0;
            }
            j += 2;
        } else {
            j += 1;
        }
    }
    return 0;
}

// Offset behind  ("e" | "E") ["+" | "-"] Digit { Digit }  at j, or j.
size_t exponent_end(std::string_view source, size_t j, size_t n) {
    if (j < n && (source[j] == 'e' || source[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (source[k] == '+' || source[k] == '-')) {
            k += 1;
        }
        const size_t d = k;
        while (k < n && in_set(source[k], C_DIGIT)) {
            k += 1;
        }
        if (k > d) {
            return k;
        }
    }
    return j;
}

// Nothing that could continue a number stands at j.
bool float_tail_ok(std::string_view source, size_t j, size_t n) {
    return !(j < n && (in_set(source[j], C_ID_CHAR) || source[j] == '.'));
}

// (kind, end) of the number at pos: T_FLOAT, T_INT, or (T_EOF, pos) when no
// rule matches.  The alternatives are tried in this order, each with its
// longest match:
//     digits "." [digits] [exponent] [suffix]  |  "." digits [exponent] [suffix]
//     digits exponent [suffix]
//     digits suffix
//     "0" ("x" | "X") hex digits
//     digits
// A float must not be followed by a letter, digit, "_" or "."; an int not
// by a letter, digit or "_".  The corpus uses  digits "." digits ["f"],
// digits "f"  and both int forms only.
Number number(std::string_view source, size_t pos, size_t n) {
    size_t i = pos;
    while (i < n && in_set(source[i], C_DIGIT)) {
        i += 1;
    }
    size_t j = 0;                       // offset behind "." digits; 0: no point
    if (i > pos) {
        if (i < n && source[i] == '.') {
            j = i + 1;
            while (j < n && in_set(source[j], C_DIGIT)) {
                j += 1;
            }
        }
    } else {
        j = pos + 1;                    // source[pos] is ".", a digit follows
        while (j < n && in_set(source[j], C_DIGIT)) {
            j += 1;
        }
    }
    if (j > 0) {
        j = exponent_end(source, j, n);
        if (j < n && in_set(source[j], C_FLOAT_SUFFIX)) {
            j += 1;
        }
        if (float_tail_ok(source, j, n)) {
            return Number{T_FLOAT, j};
        }
    }
    if (i == pos) {
        return Number{T_EOF, pos};
    }
    j = exponent_end(source, i, n);
    if (j > i) {
        if (j < n && in_set(source[j], C_FLOAT_SUFFIX)) {
            j += 1;
        }
        if (float_tail_ok(source, j, n)) {
            return Number{T_FLOAT, j};
        }
    }
    if (i < n && in_set(source[i], C_FLOAT_SUFFIX) && float_tail_ok(source, i + 1, n)) {
        return Number{T_FLOAT, i + 1};
    }
    if (source[pos] == '0' && pos + 1 < n && (source[pos + 1] == 'x' || source[pos + 1] == 'X')) {
        j = pos + 2;
        while (j < n && in_set(source[j], C_HEX)) {
            j += 1;
        }
        if (j > pos + 2 && !(j < n && in_set(source[j], C_ID_CHAR))) {
            return Number{T_INT, j};
        }
    }
    if (!(i < n && in_set(source[i], C_ID_CHAR))) {
        return Number{T_INT, i};
    }
    return Number{T_EOF, pos};
}

// ---------------------------------------------------------------------------
// literal forms no shipped source uses
// ---------------------------------------------------------------------------
//
// What the scanner of the Build 940 exe does with them was read in its flex
// tables and its literal actions (notes/exe_reads.md 1.5); none was run:
//
//   int above 32 bits          decimal and hex wrap modulo 2**32: as here
//   1.5F, 5.                   float rules with [fF] and without fraction
//                              digits: as here
//   .5                         matched as a float, converted to 0.0: as here
//                              (floatlit::float_bits)
//   1.5d, 1.5e3                suffix d is reported as "NOT SUPPORTED DATA
//                              TYPE"; there is no exponent rule (1.5e3 is the
//                              float 1.5 and the identifier e3): errors here
//   012, 08                    NOT AS HERE.  0[0-7]+ is the octal rule of the
//                              exe: it reports "octal values not supported"
//                              and leaves the value unassigned; 08 is two
//                              tokens.  Here both are read as decimal, with
//                              the warning below (an error in strict mode)
//   0x10L                      accepted by the exe as the int 0x10; a
//                              malformed number here
// They stay what they were, W_UNVERIFIED or an error: no class file shows any.

void check_int(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags) {
    const std::string_view text = source.substr(pos, end - pos);
    if (starts_with(text, "0x") || starts_with(text, "0X")) {
        const std::string_view digits = lstrip_zeros(text.substr(2));
        if (digits.size() > 8) {
            diags.unverified(line, column_of(source, pos),
                             "int literal above 32 bits, taken modulo 2**32");
        }
        return;
    }
    if (text.size() > 1 && text[0] == '0') {
        diags.unverified(line, column_of(source, pos),
                         "decimal int literal with a leading zero, read as decimal (the "
                         "scanner of the game reports \"octal values not supported\")");
    }
    const std::string_view digits = lstrip_zeros(text);
    if (digits.size() > 10 || (digits.size() == 10 && digits > std::string_view("2147483647"))) {
        diags.unverified(line, column_of(source, pos),
                         "decimal int literal above 2147483647, taken modulo 2**32");
    }
}

void check_float(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags) {
    const std::string_view text = source.substr(pos, end - pos);
    const uint32_t col = column_of(source, pos);
    const char last = text[text.size() - 1];
    if (last == 'd' || last == 'D') {
        diags.error(E_UNSUPPORTED, line, col, std::string("float literal with suffix ") + last);
        return;
    }
    const std::string_view body = (last == 'f' || last == 'F') ? text.substr(0, text.size() - 1) : text;
    if (body.find('e') != std::string_view::npos || body.find('E') != std::string_view::npos) {
        diags.error(E_UNSUPPORTED, line, col, "float literal with an exponent");
        return;
    }
    if (last == 'F') {
        diags.unverified(line, col, "float literal with suffix F");
    }
    if (body[0] == '.') {
        diags.unverified(line, col, "float literal without digits before the point: it is "
                                    "0.0, as the scanner of the game converts it");
    } else if (body[body.size() - 1] == '.') {
        diags.unverified(line, col, "float literal without digits after the point");
    }
}

void check_string(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags) {
    const std::string_view text = source.substr(pos, end - pos);
    if (!is_ascii(text)) {
        diags.warning(W_BYTE, line, column_of(source, pos), "byte above 0x7F in a string literal");
    }
    // A control character other than TAB written raw into a literal (51
    // shipped literals hold a TAB, none any other): a NUL would end the utf8
    // entry early for a reader that goes by the terminator.
    for (char ch : text) {
        const unsigned o = static_cast<unsigned char>(ch);
        if ((o < 0x20 && ch != '\t') || o == 0x7F) {
            diags.unverified(line, column_of(source, pos),
                             "control character 0x" + hex(o, 2, true) + " in a string literal");
            break;
        }
    }
    size_t i = 1;
    const size_t last = text.size() - 1;
    while (i < last) {
        if (text[i] == '\\' && i + 1 < last) {
            i += 1;
            const char e = text[i];
            if (!(e == 'n' || e == 't' || e == '"' || e == '\\')) {
                diags.unverified(line, column_of(source, pos),
                                 std::string("escape \\") + e + " in a string literal");
            }
        }
        i += 1;
    }
}

void check_rid(std::string_view source, size_t pos, size_t end, uint32_t line, Diagnostics& diags) {
    const std::string_view text = source.substr(pos, end - pos);
    // text[text.rfind(':') + 3:len(text) - 1].lstrip('0'); a resource id has its ":"
    const size_t colon = text.rfind(':');
    const std::string_view digits = lstrip_zeros(text.substr(colon + 3, text.size() - 1 - (colon + 3)));
    if (digits.size() > 8) {
        diags.unverified(line, column_of(source, pos),
                         "resource id above 32 bits, taken modulo 2**32");
    }
}

// ---------------------------------------------------------------------------
// literal values
// ---------------------------------------------------------------------------

// _HEX_VALUE[ch]: the value of a digit of either base.  The texts handed to
// the functions below are tokens, so every character is one.
inline uint32_t hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<uint32_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<uint32_t>(ch - 'a') + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<uint32_t>(ch - 'A') + 10;
    }
    return 0;
}

}  // namespace

uint32_t int_value(std::string_view text) {
    uint32_t v = 0;
    if (starts_with(text, "0x") || starts_with(text, "0X")) {
        for (char ch : text.substr(2)) {
            v = (v << 4) | hex_value(ch);
        }
        return v;
    }
    for (char ch : text) {
        v = v * 10 + hex_value(ch);
    }
    return v;
}

std::string string_value(std::string_view text) {
    std::string out;
    size_t i = 1;
    const size_t last = text.size() > 0 ? text.size() - 1 : 0;
    while (i < last) {
        const char ch = text[i];
        if (ch == '\\' && i + 1 < last) {
            i += 1;
            const char e = text[i];
            if (e == 'n') {
                out += '\n';
            } else if (e == 't') {
                out += '\t';
            } else if (e == 'r') {
                out += '\r';
            } else if (e == '0') {
                out += '\0';
            } else {
                out += e;
            }
        } else {
            out += ch;
        }
        i += 1;
    }
    return out;
}

RidParts rid_parts(std::string_view text) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return RidParts{text, 0};       // not a resource id token
    }
    uint32_t v = 0;
    size_t k = colon + 3;               // behind ":0x"
    while (k + 1 < text.size()) {       // up to the "r"
        v = (v << 4) | hex_value(text[k]);
        k += 1;
    }
    return RidParts{text.substr(0, colon), v};
}

FloatDigits float_digits(std::string_view text) {
    size_t n = text.size();
    if (n > 0 && (text[n - 1] == 'f' || text[n - 1] == 'F')) {
        n -= 1;
    }
    size_t point = 0;
    while (point < n && text[point] != '.') {
        point += 1;
    }
    if (point == n) {
        return FloatDigits{text.substr(0, n), std::string_view()};
    }
    return FloatDigits{text.substr(0, point), text.substr(point + 1, n - (point + 1))};
}

}  // namespace scanner
}  // namespace slrrc
