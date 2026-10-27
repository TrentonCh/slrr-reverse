// diag.cpp - diagnostics of the compiler.  (Reference: slrrc/diag.py.)
#include "diag.hpp"

#include <utility>

namespace slrrc {

std::string Diagnostic::format(std::string_view file_name) const {
    // '%s:%d:%d: %s %s: %s'
    std::string out(file_name);
    out += ':';
    out += dec(line);
    out += ':';
    out += dec(column);
    out += ": ";
    out += error ? "error" : "warning";
    out += ' ';
    out += code;
    out += ": ";
    out += text;
    return out;
}

std::string Diagnostic::exe_wording(std::string_view file_name) const {
    // 'JVM::compileSource: Syntax error in "%s" at line %d'
    std::string out("JVM::compileSource: Syntax error in \"");
    out += file_name;
    out += "\" at line ";
    out += dec(line);
    return out;
}

Diagnostics::Diagnostics(bool strict_mode) : errors(0), strict(strict_mode) {}

void Diagnostics::error(const char* code, uint32_t line, uint32_t column, std::string text) {
    items.push_back(Diagnostic{true, line, column, code, std::move(text)});
    errors += 1;
}

void Diagnostics::warning(const char* code, uint32_t line, uint32_t column, std::string text) {
    items.push_back(Diagnostic{false, line, column, code, std::move(text)});
}

bool Diagnostics::unverified(uint32_t line, uint32_t column, std::string_view text) {
    std::string full(text);
    full += " (no shipped source uses it)";
    if (strict) {
        error(E_UNSUPPORTED, line, column, std::move(full));
        return true;
    }
    warning(W_UNVERIFIED, line, column, std::move(full));
    return false;
}

bool Diagnostics::unresolved_type(uint32_t line, uint32_t column, std::string_view name) {
    if (strict) {
        std::string text("unknown type ");
        text += name;
        error(E_TYPE, line, column, std::move(text));
        return true;
    }
    std::string text("unknown type ");
    text += name;
    text += ", compiled to \"L;\"";
    warning(W_TYPE, line, column, std::move(text));
    return false;
}

// ---------------------------------------------------------------------------
// numbers in messages and dumps
// ---------------------------------------------------------------------------

std::string dec(uint32_t value) {
    char buf[10];               // 4294967295 has 10 digits
    unsigned n = 0;
    do {
        buf[n] = static_cast<char>('0' + value % 10);
        n += 1;
        value /= 10;
    } while (value != 0);
    std::string out;
    out.reserve(n);
    while (n > 0) {
        n -= 1;
        out += buf[n];
    }
    return out;
}

std::string hex(uint32_t value, unsigned min_width, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[8];
    unsigned n = 0;
    do {
        buf[n] = digits[value & 0xF];
        n += 1;
        value >>= 4;
    } while (value != 0);
    std::string out;
    while (min_width > n) {
        out += '0';
        min_width -= 1;
    }
    while (n > 0) {
        n -= 1;
        out += buf[n];
    }
    return out;
}

}  // namespace slrrc
