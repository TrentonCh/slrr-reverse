// diag.hpp - diagnostics of the compiler.  (Reference: slrrc/diag.py.)
//
// The compiler never throws for bad input.  Every module reports into one
// Diagnostics list; an entry carries a line, a column and a code.  A source
// that produced an error gives no output (compiler_spec.md 2.9).
//
// Codes
//     E_LEX          character no rule accepts, unterminated string or comment
//     E_SYNTAX       token sequence outside the grammar
//     E_UNSUPPORTED  construct no shipped source uses and that has no stated
//                    default (or any such construct in strict mode)
//     E_TYPE         type name that resolves nowhere (strict mode; always for a
//                    superclass or an interface); a class that is its own
//                    superclass
//     E_NAME         identifier that is no local, field or class
//     E_MEMBER       member not found in a dotted name
//     E_LIMIT        a limit of the class format or of the compiler
//     W_UNVERIFIED   construct no shipped source uses, compiled by the plain
//                    extension of the proven rules (appendix B)
//     W_TYPE         type name that resolves nowhere, compiled to "L;"
//     W_BYTE         byte above 0x7F in a string or comment, byte order mark
//     W_PACK         resource id whose pack has no file, compiled to
//                    "system.rpk" id 0 (a warning in both modes: one shipped
//                    class shows this form, so strict mode must compile it)
//     W_CLASS        a class the provider handed over that breaks the contract
//                    of classes::ClassProvider (another name than the one asked
//                    for, a descriptor that is none); the class is treated as
//                    not found.  It has no place in the source: line 1, column 1
//
// A code is a pointer to one of the arrays below; two codes are the same code
// when the pointers are equal (the arrays are inline variables: one object in
// the whole program).
//
// The module also holds the two number-to-text helpers the messages and the
// dumps need (Python: the "%" operator).
#ifndef SLRRC_DIAG_HPP
#define SLRRC_DIAG_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slrrc {

inline constexpr char E_LEX[] = "E_LEX";
inline constexpr char E_SYNTAX[] = "E_SYNTAX";
inline constexpr char E_UNSUPPORTED[] = "E_UNSUPPORTED";
inline constexpr char E_TYPE[] = "E_TYPE";
inline constexpr char E_NAME[] = "E_NAME";
inline constexpr char E_MEMBER[] = "E_MEMBER";
inline constexpr char E_LIMIT[] = "E_LIMIT";
inline constexpr char W_UNVERIFIED[] = "W_UNVERIFIED";
inline constexpr char W_TYPE[] = "W_TYPE";
inline constexpr char W_BYTE[] = "W_BYTE";
inline constexpr char W_PACK[] = "W_PACK";
inline constexpr char W_CLASS[] = "W_CLASS";

struct Diagnostic {
    bool error;             // true: no output is produced
    uint32_t line;          // 1-based, lines are counted by LF
    uint32_t column;        // 1-based, a TAB counts as one
    const char* code;
    std::string text;

    // "file:line:column: error CODE: text"
    std::string format(std::string_view file_name) const;

    // The line the game's own compiler logs for a source that does not
    // parse (docs/ENGINE.md 4.2); the host prints it for E_SYNTAX.
    std::string exe_wording(std::string_view file_name) const;
};

// The diagnostics of one compilation, in the order they were found.
//
// strict selects what a construct of appendix B does: an error
// (E_UNSUPPORTED) in strict mode, a warning (W_UNVERIFIED) otherwise.
//
// One Diagnostics object lives as long as one compilation and every stage
// appends to it.  A result gets `items` once, at the end of the stage that
// returns it (Python: __init__._finish); nothing outside keeps a reference to
// the vector that grows.
class Diagnostics {
public:
    explicit Diagnostics(bool strict_mode = false);

    std::vector<Diagnostic> items;
    uint32_t errors;
    bool strict;

    void error(const char* code, uint32_t line, uint32_t column, std::string text);
    void warning(const char* code, uint32_t line, uint32_t column, std::string text);

    // A construct no shipped source uses but whose compilation is the plain
    // extension of the proven rules.  Returns true when it was reported as
    // an error (strict mode).
    bool unverified(uint32_t line, uint32_t column, std::string_view text);

    // A type name that resolves nowhere.  Returns true when it was reported
    // as an error (strict mode).
    bool unresolved_type(uint32_t line, uint32_t column, std::string_view name);
};

// ---------------------------------------------------------------------------
// numbers in messages and dumps
// ---------------------------------------------------------------------------

// "%d" of an unsigned number.
std::string dec(uint32_t value);

// "%X" / "%x" of an unsigned number, at least min_width digits ("%08X" is
// hex(value, 8, true)).
std::string hex(uint32_t value, unsigned min_width, bool upper);

}  // namespace slrrc

#endif
