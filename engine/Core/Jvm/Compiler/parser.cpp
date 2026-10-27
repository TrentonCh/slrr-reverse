// parser.cpp - recursive descent parser: tokens -> AST.  (Reference:
// slrrc/parser.py; the functions of Parser stand in the order of that file
// and carry its names.)
//
// HOW THE PARSER STOPS AT AN ERROR.  It stops at the first error, and it does
// so by unwinding: fail() records the diagnostic and throws the private Stop,
// which parse() catches; nothing else throws it and nothing else catches it.
// Every function of the parser is written on that assumption:
//
//     never return        fail, error, unsupported, too_deep
//     return only when    expect, ident (the token is there), enter (the
//     all is well         nesting is within the limit), done (the tree is not
//                         too high), unverified (default mode; in strict mode
//                         the construct is an error and the call does not
//                         return)
//
// The code behind such a call relies on it: type_decl() reads is_interface
// after error(), block() loops until "}" and leaves an unclosed block through
// error(), switch_statement() uses `group` after error().  Do not turn fail()
// into a function that returns (notes/impl_fix.md, finding 2).
//
// WHERE THE C++ DIFFERS IN FORM FROM THE REFERENCE
//
//   - A node is its index in ast.nodes.  N(n) is a reference into that
//     vector and dies with the next node(): no reference is held across a
//     call that can make a node, and "n.a = self.expression()" is written
//     with a local, "x = expression(); N(n).a = x;", so that the order does
//     not depend on how the compiler evaluates an assignment.
//   - The items of a node are collected in an ItemList of the function that
//     parses the construct (the reference appends to n.items) and handed to
//     done(), which copies them into ast.lists.  Where the reference passes
//     the owner node to a helper that appends to it (method_rest, declarators,
//     arguments, expression_list, local_decl), the helper gets the list.
//   - The local variable `interface` of type_decl() is is_interface (a macro
//     of the Windows headers).
//   - A message with a "%s" in it is passed in two parts, error(text, more),
//     and put together in the function that reports: see SLRRC_NOINLINE.
//
// STACK.  The recursion is bounded by MAX_DEPTH levels of nesting.  A level
// costs at most 16 frames of this file (expression, conditional, ten times
// binary, unary, postfix, primary and arguments).  Measured for the deepest
// input the limit allows (200 levels, each one an operand of all ten levels
// of precedence that is a call), with the unwinding from the innermost level
// included: 140 KB in a Win32 release build, 241 KB in a Win32 debug build,
// 367 KB and 839 KB in the x64 builds (notes/port_1-front.md).  A host calls
// the compiler with that much stack left.
#include "parser.hpp"

#include <utility>

// What is kept out of line, so that the frames of the recursion stay small
// whatever the optimiser decides: the functions that report (they build the
// text of a message; inlined, their strings would be part of every frame of
// the descent), node() and close() (a Node on the stack), and unary() and
// class_type(), which binary() calls: binary() stands ten times in a level of
// nesting, once per level of precedence, and has to hold nothing but its own
// few numbers.
#if defined(_MSC_VER)
#define SLRRC_NOINLINE __declspec(noinline)
#else
#define SLRRC_NOINLINE __attribute__((noinline))
#endif

namespace slrrc {
namespace parser {

using namespace scanner;

// ---------------------------------------------------------------------------
// tables
// ---------------------------------------------------------------------------

const char* kind_name(uint16_t kind) {
    switch (kind) {
    case UNIT: return "Unit";
    case QNAME: return "QName";
    case IMPORT: return "Import";
    case CLASS: return "Class";
    case FIELD_DECL: return "FieldDecl";
    case DECLARATOR: return "Declarator";
    case METHOD_DECL: return "MethodDecl";
    case PARAM: return "Param";
    case TYPE: return "Type";
    case LIST: return "List";
    case BLOCK: return "Block";
    case LOCAL_VAR: return "LocalVar";
    case EXPR_STMT: return "ExprStmt";
    case IF: return "If";
    case WHILE: return "While";
    case DO: return "Do";
    case FOR: return "For";
    case SWITCH: return "Switch";
    case SWITCH_GROUP: return "SwitchGroup";
    case CASE_LABEL: return "CaseLabel";
    case DEFAULT_LABEL: return "DefaultLabel";
    case BREAK: return "Break";
    case CONTINUE: return "Continue";
    case RETURN: return "Return";
    case EMPTY: return "Empty";
    case LITERAL: return "Literal";
    case NAME: return "Name";
    case THIS: return "This";
    case SUPER: return "Super";
    case FIELD: return "Field";
    case INDEX: return "Index";
    case CALL: return "Call";
    case CTOR_CALL: return "CtorCall";
    case NEW: return "New";
    case NEW_ARRAY: return "NewArray";
    case UNARY: return "Unary";
    case POSTFIX: return "Postfix";
    case BINARY: return "Binary";
    case ASSIGN: return "Assign";
    case COND: return "Cond";
    case CAST: return "Cast";
    case INSTANCEOF: return "InstanceOf";
    case PAREN: return "Paren";
    default: return "?";
    }
}

uint32_t modifier_bit(uint8_t token_kind) {
    switch (token_kind) {
    case KW_PUBLIC: return ACC_PUBLIC;
    case KW_PROTECTED: return ACC_PROTECTED;
    case KW_PRIVATE: return ACC_PRIVATE;
    case KW_STATIC: return ACC_STATIC;
    case KW_ABSTRACT: return ACC_ABSTRACT;
    case KW_FINAL: return ACC_FINAL;
    case KW_NATIVE: return ACC_NATIVE;
    case KW_SYNCHRONIZED: return ACC_SYNCHRONIZED;
    default: return 0;
    }
}

uint32_t binary_prec(uint8_t token_kind) {
    switch (token_kind) {
    case P_LOR: return 1;
    case P_LAND: return 2;
    case P_OR: return 3;
    case P_XOR: return 4;
    case P_AND: return 5;
    case P_EQ: case P_NE: return 6;
    case P_LT: case P_GT: case P_LE: case P_GE: case KW_INSTANCEOF: return 7;
    case P_SHL: case P_SHR: case P_USHR: return 8;
    case P_ADD: case P_SUB: return 9;
    case P_MUL: case P_DIV: case P_MOD: return 10;
    default: return 0;
    }
}

bool is_assign_op(uint8_t token_kind) {
    switch (token_kind) {
    case P_ASSIGN: case P_MUL_ASSIGN: case P_DIV_ASSIGN: case P_MOD_ASSIGN: case P_ADD_ASSIGN:
    case P_SUB_ASSIGN: case P_SHL_ASSIGN: case P_SHR_ASSIGN: case P_USHR_ASSIGN: case P_AND_ASSIGN:
    case P_XOR_ASSIGN: case P_OR_ASSIGN:
        return true;
    default:
        return false;
    }
}

bool is_prefix_op(uint8_t token_kind) {
    switch (token_kind) {
    case P_ADD: case P_SUB: case P_NOT: case P_BITNOT: case P_INC: case P_DEC:
        return true;
    default:
        return false;
    }
}

bool is_literal_token(uint8_t token_kind) {
    switch (token_kind) {
    case T_INT: case T_FLOAT: case T_STR: case T_RID: case KW_NULL: case KW_TRUE: case KW_FALSE:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// the tree
// ---------------------------------------------------------------------------

std::string Ast::dotted(uint32_t tok0, uint32_t tok1) const {
    if (tok0 == tok1) {
        return std::string(text(tok0));
    }
    std::string out;
    uint32_t k = tok0;
    while (k <= tok1) {
        if (k > tok0) {
            out += '.';
        }
        out += text(k);
        k += 2;
    }
    return out;
}

std::string Ast::type_name(const Node& type_node) const {
    return dotted(type_node.tok0, type_node.name_tok);
}

std::vector<uint32_t> children(const Ast& ast, uint32_t n) {
    const Node& nd = ast.nodes[n];
    const uint16_t k = nd.kind;
    std::vector<uint32_t> out;
    if (k == METHOD_DECL) {
        if (nd.a != NONE) {
            out.push_back(nd.a);
        }
        for (uint32_t x : ast.items(nd)) {
            out.push_back(x);
        }
        if (nd.b != NONE) {
            out.push_back(nd.b);
        }
        return out;
    }
    if (k == FOR) {
        if (nd.c != NONE) {
            out.push_back(nd.c);
        }
        if (nd.a != NONE) {
            out.push_back(nd.a);
        }
        for (uint32_t x : ast.items(nd)) {
            out.push_back(x);
        }
        out.push_back(nd.b);
        return out;
    }
    if (nd.a != NONE) {
        out.push_back(nd.a);
    }
    if (nd.b != NONE) {
        out.push_back(nd.b);
    }
    if (nd.c != NONE) {
        out.push_back(nd.c);
    }
    for (uint32_t x : ast.items(nd)) {
        out.push_back(x);
    }
    return out;
}

bool is_primitive(const Ast& ast, const Node& type_node) {
    const uint8_t k = ast.toks[type_node.tok0].kind;
    return k == KW_INT || k == KW_FLOAT || k == KW_VOID;
}

// ---------------------------------------------------------------------------
// the parser
// ---------------------------------------------------------------------------

namespace {

// Unwinds the descent after the first error.  Thrown in Parser::fail() and
// in the strict branch of Parser::unverified(), caught in parse(), and
// nowhere else (the head of this file says what depends on that).
struct Stop {};

// The items of a node while its construct is parsed.
typedef std::vector<uint32_t> ItemList;

class Parser {
public:
    Parser(std::string_view source_text, std::vector<Token> tokens, Diagnostics& diagnostics)
        : source(source_text), diags(diagnostics), i(0), depth(0), cond_depth(0), conds(0) {
        ast.source = source_text;
        ast.toks = std::move(tokens);
    }

    Ast ast;

private:
    std::string_view source;
    Diagnostics& diags;
    uint32_t i;                     // index of the current token
    uint32_t depth;                 // recursion of the parser, see enter()
    std::vector<uint32_t> heights;  // per node: height of the tree below it, see done()
    std::vector<uint32_t> deepest;  // per node: first token of the deepest node below it
    uint32_t cond_depth;            // > 0 inside the second or third operand of ?:
    uint32_t conds;                 // number of COND nodes made so far

    Node& N(uint32_t n) { return ast.nodes[n]; }
    const Token& tok(uint32_t t) const { return ast.toks[t]; }

    // -- tokens --------------------------------------------------------------

    uint8_t kind() const { return ast.toks[i].kind; }

    // Kind of the token k places ahead (the end marker repeats).
    uint8_t peek_kind(uint32_t k) const {
        size_t j = static_cast<size_t>(i) + k;
        if (j >= ast.toks.size()) {
            j = ast.toks.size() - 1;
        }
        return ast.toks[j].kind;
    }

    bool at(uint8_t kind_wanted) const { return ast.toks[i].kind == kind_wanted; }

    bool accept(uint8_t kind_wanted) {
        if (ast.toks[i].kind == kind_wanted) {
            i += 1;
            return true;
        }
        return false;
    }

    // A token for a message.
    std::string show(uint32_t t) const {
        if (tok(t).kind == T_EOF) {
            return "end of file";
        }
        std::string out("\"");
        out += ast.text(t);
        out += '"';
        return out;
    }

    // Record the error and stop the parse.  DOES NOT RETURN.
    [[noreturn]] SLRRC_NOINLINE void fail(const char* code, uint32_t t, std::string text) {
        diags.error(code, tok(t).line, column_at(source, tok(t).pos), std::move(text));
        throw Stop();
    }

    // The token sequence is outside the grammar.  Does not return.
    // error(text, more) is error(text + more): the "%s" of the reference.
    [[noreturn]] SLRRC_NOINLINE void error(std::string_view text) { error(text, i); }

    [[noreturn]] SLRRC_NOINLINE void error(std::string_view text, std::string_view more) {
        std::string full(text);
        full += more;
        error(full, i);
    }

    [[noreturn]] SLRRC_NOINLINE void error(std::string_view text, uint32_t t) {
        // '%s (at %s)'
        std::string full(text);
        full += " (at ";
        full += show(t);
        full += ')';
        fail(E_SYNTAX, t, std::move(full));
    }

    // A construct the compiler has no rule for, in either mode.  Does not
    // return.  unsupported(t, text, more) is unsupported(t, text + more).
    [[noreturn]] SLRRC_NOINLINE void unsupported(uint32_t t, std::string_view text) {
        std::string full(text);
        full += " (no shipped source uses it)";
        fail(E_UNSUPPORTED, t, std::move(full));
    }

    [[noreturn]] SLRRC_NOINLINE void unsupported(uint32_t t, std::string_view text,
                                                 std::string_view more) {
        std::string full(text);
        full += more;
        unsupported(t, full);
    }

    // A construct of appendix B: a warning, after which the parse goes on;
    // in strict mode an error, and the call does not return.
    // unverified(t, text, more) is unverified(t, text + more).
    SLRRC_NOINLINE void unverified(uint32_t t, std::string_view text) {
        if (diags.unverified(tok(t).line, column_at(source, tok(t).pos), text)) {
            throw Stop();
        }
    }

    SLRRC_NOINLINE void unverified(uint32_t t, std::string_view text, std::string_view more) {
        std::string full(text);
        full += more;
        unverified(t, full);
    }

    // Consume the token of that kind; returns only when it was there.
    void expect(uint8_t kind_wanted, const char* where) {
        if (ast.toks[i].kind != kind_wanted) {
            expected(kind_wanted, where);
        }
        i += 1;
    }

    [[noreturn]] SLRRC_NOINLINE void expected(uint8_t kind_wanted, const char* where) {
        // 'expected "%s" in %s'
        std::string text("expected \"");
        text += fixed_text(kind_wanted);
        text += "\" in ";
        text += where;
        error(text);
    }

    // Consume an identifier and return its token index; returns only when
    // it was there.
    uint32_t ident(const char* where) {
        if (ast.toks[i].kind != T_ID) {
            error("expected an identifier in ", where);
        }
        i += 1;
        return i - 1;
    }

    // -- nodes ---------------------------------------------------------------

    // Make a node.  Its number is the number of nodes made before it.
    SLRRC_NOINLINE uint32_t node(uint16_t node_kind, uint32_t t) {
        const uint32_t n = static_cast<uint32_t>(ast.nodes.size());
        ast.nodes.push_back(Node{node_kind, t, t, NONE, NONE, NONE, NONE, NONE, 0, 0, 0});
        heights.push_back(1);
        deepest.push_back(t);
        return n;
    }

    // Close a node: its last token is the one consumed last, its items are
    // the ones collected for it.  Called exactly once per node, after its
    // last child and its last item.
    //
    // The height of the tree below the node is known here; a tree higher
    // than MAX_DEPTH is refused (a chain a + b + c + ... is parsed in a
    // loop but is as deep as it is long).  The error is reported where the
    // nesting is: at the first token of the deepest node below this one
    // (of several equally deep ones the first of a, b, c, items).
    uint32_t done(uint32_t n, const ItemList& items) {
        return close(n, items.data(), items.size());
    }

    // ... a node without items.
    uint32_t done(uint32_t n) { return close(n, nullptr, 0); }

    SLRRC_NOINLINE uint32_t close(uint32_t n, const uint32_t* items, size_t count) {
        Node& nd = ast.nodes[n];
        nd.tok1 = i > 0 ? i - 1 : 0;
        uint32_t h = 0;
        uint32_t deep = nd.tok0;
        if (nd.a != NONE && heights[nd.a] > h) {
            h = heights[nd.a];
            deep = deepest[nd.a];
        }
        if (nd.b != NONE && heights[nd.b] > h) {
            h = heights[nd.b];
            deep = deepest[nd.b];
        }
        if (nd.c != NONE && heights[nd.c] > h) {
            h = heights[nd.c];
            deep = deepest[nd.c];
        }
        for (size_t k = 0; k < count; ++k) {
            const uint32_t x = items[k];
            if (heights[x] > h) {
                h = heights[x];
                deep = deepest[x];
            }
        }
        // the items become one slice of ast.lists
        nd.list = static_cast<uint32_t>(ast.lists.size());
        nd.count = static_cast<uint32_t>(count);
        ast.lists.insert(ast.lists.end(), items, items + count);
        heights[n] = h + 1;
        deepest[n] = deep;
        if (h + 1 > MAX_DEPTH) {
            too_deep(deep);
        }
        return n;
    }

    // One level of recursion of the parser itself; returns only while the
    // nesting is within MAX_DEPTH.
    void enter() {
        depth += 1;
        if (depth > MAX_DEPTH) {
            too_deep(i);
        }
    }

    void leave() { depth -= 1; }

    // Does not return.
    [[noreturn]] SLRRC_NOINLINE void too_deep(uint32_t t) {
        fail(E_LIMIT, t, "statements or expressions nested deeper than " + dec(MAX_DEPTH) +
                         ", or an operator chain longer than that");
    }

    // -- names and types -----------------------------------------------------

    // QualifiedName = Identifier { "." Identifier }
    uint32_t qname(const char* where) {
        const uint32_t n = node(QNAME, i);
        N(n).name_tok = ident(where);
        while (at(P_DOT) && peek_kind(1) == T_ID) {
            i += 1;
            N(n).name_tok = ident(where);
        }
        return done(n);
    }

    // Count "[" "]" pairs.
    uint32_t dims() {
        uint32_t d = 0;
        while (at(P_LBRACKET) && peek_kind(1) == P_RBRACKET) {
            i += 2;
            d += 1;
        }
        return d;
    }

    // The name part of a type: int, float, void or a class name.  Returns
    // the TYPE node, not yet closed.
    uint32_t type_name(const char* where) {
        const uint32_t t = i;
        const uint8_t k = kind();
        const uint32_t n = node(TYPE, t);
        if (k == KW_INT || k == KW_FLOAT || k == KW_VOID) {
            i += 1;
            N(n).name_tok = t;
        } else if (k == T_ID) {
            N(n).name_tok = ident(where);
            if (at(P_DOT) && peek_kind(1) == T_ID) {
                unverified(t, "class name written with its package");
                while (at(P_DOT) && peek_kind(1) == T_ID) {
                    i += 1;
                    N(n).name_tok = ident(where);
                }
            }
        } else {
            error("expected a type in ", where);
        }
        return n;
    }

    // Type = ( "int" | "float" | "void" | Identifier ) [ "[" "]" ]
    uint32_t type(const char* where) {
        const uint32_t n = type_name(where);
        const uint32_t d = dims();
        if (d > 1) {
            unsupported(N(n).tok0, "array type with more than one dimension");
        }
        if (d && tok(N(n).tok0).kind == KW_VOID) {
            unsupported(N(n).tok0, "array of void");
        }
        N(n).flags = d << DIMS_SHIFT;
        return done(n);
    }

    // A class named behind extends, implements or instanceof.
    SLRRC_NOINLINE uint32_t class_type(const char* where) {
        if (!at(T_ID)) {
            error("expected a class name in ", where);
        }
        return done(type_name(where));
    }

    // { Modifier }: the OR of the modifier bits.
    uint32_t modifiers() {
        uint32_t mods = 0;
        for (;;) {
            const uint32_t bit = modifier_bit(kind());
            if (bit == 0) {
                return mods;
            }
            if (mods & bit) {
                unverified(i, "the same modifier twice");
            }
            if (bit == ACC_PROTECTED || bit == ACC_SYNCHRONIZED) {
                unverified(i, "modifier ", ast.text(i));
            }
            mods |= bit;
            i += 1;
        }
    }

    // -- compilation unit ----------------------------------------------------

public:
    // Unit = "package" QualifiedName ";" { Import } { TypeDecl }
    uint32_t unit() {
        const uint32_t u = node(UNIT, i);
        ItemList items;
        if (at(KW_PACKAGE)) {
            i += 1;
            const uint32_t q = qname("the package declaration");
            N(u).a = q;
            expect(P_SEMI, "the package declaration");
        } else {
            unverified(i, "source without a package declaration");
        }
        uint32_t types = 0;
        while (!at(T_EOF)) {
            if (at(KW_IMPORT)) {
                if (types) {
                    unverified(i, "import after a type declaration");
                }
                const uint32_t imp = import_decl();
                items.push_back(imp);
            } else if (at(P_SEMI)) {
                unverified(i, "\";\" between top-level declarations");
                i += 1;
            } else if (at(KW_PACKAGE)) {
                error("package declaration that is not at the start of the source");
            } else {
                const uint32_t ty = type_decl();
                items.push_back(ty);
                types += 1;
            }
        }
        return done(u, items);
    }

private:
    // Import = "import" [ Identifier ":" ] QualifiedName "." "*" ";"
    uint32_t import_decl() {
        const uint32_t n = node(IMPORT, i);
        i += 1;
        if (at(T_ID) && peek_kind(1) == P_COLON && peek_kind(2) == T_ID) {
            // import parts:part.root.BodyPart.*;   (2 sources; it names nothing)
            N(n).flags |= F_COLON;
            N(n).op_tok = i + 1;
            i += 2;
        }
        const uint32_t q = node(QNAME, i);
        N(q).name_tok = ident("an import");
        while (at(P_DOT)) {
            if (peek_kind(1) == P_MUL) {
                N(n).flags |= F_STAR;
                break;
            }
            i += 1;
            N(q).name_tok = ident("an import");
        }
        done(q);
        N(n).a = q;
        if (N(n).flags & F_STAR) {
            i += 2;
        } else {
            unverified(N(n).tok0, "import of a single class");
        }
        expect(P_SEMI, "an import");
        return done(n);
    }

    // TypeDecl = { Modifier } "class" Identifier [ "extends" Identifier ]
    //              [ "implements" Identifier { "," Identifier } ] "{" { Member } "}"
    //            | { Modifier } "interface" Identifier "{" "}"
    uint32_t type_decl() {
        const uint32_t start = i;
        const uint32_t mods = modifiers();
        bool is_interface = false;
        if (at(KW_CLASS)) {
            is_interface = false;
        } else if (at(KW_INTERFACE)) {
            is_interface = true;
        } else {
            error("expected class or interface");
        }
        if (mods & ~(ACC_PUBLIC | ACC_ABSTRACT)) {
            // the class file has no place for class modifiers
            unverified(start, "modifier other than public and abstract on a class");
        }
        i += 1;
        const uint32_t n = node(CLASS, start);
        ItemList members;
        N(n).flags = mods | (is_interface ? F_INTERFACE : 0);
        N(n).name_tok = ident("a type declaration");
        if (at(KW_EXTENDS)) {
            if (is_interface) {
                unsupported(i, "interface with extends");
            }
            i += 1;
            const uint32_t super = class_type("extends");
            N(n).a = super;
        }
        if (at(KW_IMPLEMENTS)) {
            if (is_interface) {
                unsupported(i, "interface with implements");
            }
            i += 1;
            const uint32_t lst = node(LIST, i);
            ItemList names;
            names.push_back(class_type("implements"));
            while (accept(P_COMMA)) {
                names.push_back(class_type("implements"));
            }
            N(n).b = done(lst, names);
        }
        expect(P_LBRACE, "a class body");
        while (!at(P_RBRACE)) {
            if (at(T_EOF)) {
                error("class body is not closed");
            }
            if (at(P_SEMI)) {
                unverified(i, "\";\" between class members");
                i += 1;
                continue;
            }
            if (is_interface && members.empty()) {
                unverified(i, "interface with members");
            }
            const uint32_t m = member(n);
            members.push_back(m);
        }
        i += 1;
        return done(n, members);
    }

    // Member          = FieldDecl | MethodDecl | ConstructorDecl
    // FieldDecl       = { Modifier } Type Declarator { "," Declarator } ";"
    // MethodDecl      = { Modifier } Type Identifier Params ( Block | ";" )
    // ConstructorDecl = { Modifier } Identifier Params ( Block | ";" )
    // A member that starts (after its modifiers) with  Identifier "("  is a
    // constructor; every other member starts with a type.
    uint32_t member(uint32_t cls) {
        const uint32_t start = i;
        const uint32_t mods = modifiers();
        if (at(P_LBRACE)) {
            unsupported(i, "initialiser block in a class");
        }
        if (at(T_ID) && peek_kind(1) == P_LPAREN) {
            if (ast.text(i) != ast.text(N(cls).name_tok)) {
                unverified(i, "constructor whose name is not the class name");
            }
            if (mods & ~ACC_PUBLIC) {
                unverified(start, "modifier other than public on a constructor");
            }
            const uint32_t n = node(METHOD_DECL, start);
            ItemList params;
            N(n).flags = mods | F_CTOR;
            N(n).name_tok = i;
            i += 1;
            method_rest(n, params);
            return done(n, params);
        }
        const uint8_t k = kind();
        if (!(k == T_ID || k == KW_INT || k == KW_FLOAT || k == KW_VOID)) {
            error("expected a member declaration");
        }
        const uint32_t ty = type("a member declaration");
        const uint32_t name = ident("a member declaration");
        if (at(P_LPAREN)) {
            const uint32_t n = node(METHOD_DECL, start);
            ItemList params;
            N(n).flags = mods;
            N(n).a = ty;
            N(n).name_tok = name;
            method_rest(n, params);
            return done(n, params);
        }
        if (tok(N(ty).tok0).kind == KW_VOID) {
            unsupported(N(ty).tok0, "field of type void");
        }
        const uint32_t n = node(FIELD_DECL, start);
        ItemList decls;
        N(n).flags = mods;
        N(n).a = ty;
        i -= 1;                         // back to the name of the first declarator
        declarators(decls, ty, "a field declaration");
        expect(P_SEMI, "a field declaration");
        return done(n, decls);
    }

    // "(" [ Param { "," Param } ] ")" ( Block | ";" ); the parameters into
    // `params`, the items of n.
    void method_rest(uint32_t n, ItemList& params) {
        expect(P_LPAREN, "a parameter list");
        if (!at(P_RPAREN)) {
            for (;;) {
                const uint32_t p = param();
                params.push_back(p);
                if (!accept(P_COMMA)) {
                    break;
                }
            }
        }
        expect(P_RPAREN, "a parameter list");
        if (at(P_LBRACKET)) {
            unsupported(i, "brackets after a parameter list");
        }
        const uint32_t flags = N(n).flags;
        if (flags & ACC_ABSTRACT) {
            unverified(N(n).tok0, "abstract method");
        }
        if (flags & ACC_FINAL) {
            unverified(N(n).tok0, "final method");
        }
        if (at(P_LBRACE)) {
            if (flags & ACC_NATIVE) {
                unsupported(i, "native method with a body");
            }
            if (flags & ACC_ABSTRACT) {
                unsupported(i, "abstract method with a body");
            }
            const uint32_t body = block((flags & F_CTOR) != 0);
            N(n).b = body;
        } else {
            expect(P_SEMI, "a method body");
        }
    }

    // Modifiers of a parameter or a local variable: final at most.
    uint32_t local_modifiers(const char* what) {
        const uint32_t start = i;
        const uint32_t mods = modifiers();
        if (mods & ~ACC_FINAL) {
            unsupported(start, "modifier other than final on ", what);
        }
        if (mods) {
            unverified(start, "final on ", what);
        }
        return mods;
    }

    // Param = Type Identifier
    uint32_t param() {
        const uint32_t start = i;
        const uint32_t mods = local_modifiers("a parameter");
        const uint32_t ty = type("a parameter");
        if (tok(N(ty).tok0).kind == KW_VOID) {
            unsupported(N(ty).tok0, "parameter of type void");
        }
        const uint32_t n = node(PARAM, start);
        N(n).a = ty;
        N(n).name_tok = ident("a parameter");
        N(n).flags = mods | (name_dims(ty) << DIMS_SHIFT);
        return done(n);
    }

    // Brackets behind a declared name: int a[]
    uint32_t name_dims(uint32_t ty) {
        const uint32_t t = i;
        const uint32_t d = dims();
        if (d) {
            if (d + N(ty).dims() > 1) {
                unsupported(t, "array type with more than one dimension");
            }
            unverified(t, "brackets after a declared name");
        }
        return d;
    }

    // Declarator { "," Declarator };  Declarator = Identifier [ "=" Expression ]
    // The declarators into `items`, the items of the declaration.
    void declarators(ItemList& items, uint32_t ty, const char* where) {
        for (;;) {
            const uint32_t d = node(DECLARATOR, i);
            N(d).name_tok = ident(where);
            N(d).flags = name_dims(ty) << DIMS_SHIFT;
            if (at(P_ASSIGN)) {
                N(d).op_tok = i;
                i += 1;
                if (at(P_LBRACE)) {
                    unsupported(i, "array initialiser");
                }
                const uint32_t init = expression();
                N(d).a = init;
            }
            items.push_back(done(d));
            if (!accept(P_COMMA)) {
                break;
            }
        }
    }

    // -- statements ----------------------------------------------------------

    // Block = "{" { Statement } "}".  In the body of a constructor the
    // first statement may be this(..) or super(..).
    uint32_t block(bool ctor_body = false) {
        const uint32_t n = node(BLOCK, i);
        ItemList items;
        expect(P_LBRACE, "a block");
        while (!at(P_RBRACE)) {
            if (at(T_EOF)) {
                error("block is not closed");
            }
            const uint32_t s = statement(ctor_body && items.empty());
            items.push_back(s);
        }
        i += 1;
        return done(n, items);
    }

    // True when the tokens ahead read  Type Identifier  (a local variable
    // declaration) and not an expression.
    bool at_local_decl() const {
        const uint8_t k = kind();
        if (k == KW_INT || k == KW_FLOAT) {
            return true;
        }
        if (modifier_bit(k) != 0) {
            return true;
        }
        if (k != T_ID) {
            return false;
        }
        // The end marker stops both loops: a "." or a "[" is never the last
        // token, and the token behind it is tested before it is passed.
        const std::vector<Token>& toks = ast.toks;
        size_t j = static_cast<size_t>(i) + 1;
        while (toks[j].kind == P_DOT && toks[j + 1].kind == T_ID) {
            j += 2;
        }
        while (toks[j].kind == P_LBRACKET && toks[j + 1].kind == P_RBRACKET) {
            j += 2;
        }
        return toks[j].kind == T_ID;
    }

    // LocalDecl = Type Declarator { "," Declarator }
    // Returns the LOCAL_VAR node, not yet closed, and its declarators in
    // `items`: a declaration that is a statement ends at its ";", one in a
    // for header at its last declarator, and the caller closes it there.
    uint32_t local_decl(ItemList& items) {
        const uint32_t start = i;
        const uint32_t mods = local_modifiers("a local variable");
        const uint32_t ty = type("a local variable declaration");
        if (tok(N(ty).tok0).kind == KW_VOID) {
            unsupported(N(ty).tok0, "local variable of type void");
        }
        const uint32_t n = node(LOCAL_VAR, start);
        N(n).flags = mods;
        N(n).a = ty;
        declarators(items, ty, "a local variable declaration");
        return n;
    }

    // The statement that is the body of if / else / a loop.
    uint32_t substatement(const char* where) {
        if (at_local_decl()) {
            unsupported(i, "declaration as the body of ", where);
        }
        return statement(false);
    }

    uint32_t statement(bool ctor_first) {
        enter();
        const uint32_t n = statement_1(ctor_first);
        leave();
        return n;
    }

    uint32_t statement_1(bool ctor_first) {
        const uint32_t t = i;
        const uint8_t k = kind();
        if (k == P_LBRACE) {
            return block();
        }
        if (k == P_SEMI) {
            unverified(t, "empty statement");
            i += 1;
            return done(node(EMPTY, t));
        }
        if (k == KW_IF) {
            return if_statement();
        }
        if (k == KW_WHILE) {
            i += 1;
            const uint32_t n = node(WHILE, t);
            expect(P_LPAREN, "while");
            const uint32_t cond = expression();
            N(n).a = cond;
            expect(P_RPAREN, "while");
            const uint32_t body = substatement("while");
            N(n).b = body;
            return done(n);
        }
        if (k == KW_DO) {
            i += 1;
            const uint32_t n = node(DO, t);
            const uint32_t body = substatement("do");
            N(n).a = body;
            expect(KW_WHILE, "do");
            expect(P_LPAREN, "do");
            const uint32_t cond = expression();
            N(n).b = cond;
            expect(P_RPAREN, "do");
            expect(P_SEMI, "do");
            return done(n);
        }
        if (k == KW_FOR) {
            return for_statement();
        }
        if (k == KW_SWITCH) {
            return switch_statement();
        }
        if (k == KW_RETURN) {
            i += 1;
            const uint32_t n = node(RETURN, t);
            if (!at(P_SEMI)) {
                const uint32_t value = expression();
                N(n).a = value;
            }
            expect(P_SEMI, "return");
            return done(n);
        }
        if (k == KW_BREAK || k == KW_CONTINUE) {
            i += 1;
            const uint32_t n = node(k == KW_BREAK ? BREAK : CONTINUE, t);
            if (at(T_ID)) {
                unsupported(i, "break or continue with a label");
            }
            expect(P_SEMI, k == KW_BREAK ? "break" : "continue");
            return done(n);
        }
        if (k == KW_CASE || k == KW_DEFAULT) {
            error("case label outside a switch body");
        }
        if (k == KW_ELSE) {
            error("else without if");
        }
        if (k == T_ID && peek_kind(1) == P_COLON) {
            unsupported(t, "labelled statement");
        }
        if (at_local_decl()) {
            ItemList decls;
            const uint32_t n = local_decl(decls);
            expect(P_SEMI, "a local variable declaration");
            return done(n, decls);
        }
        const uint32_t n = node(EXPR_STMT, t);
        if ((k == KW_THIS || k == KW_SUPER) && peek_kind(1) == P_LPAREN) {
            // this(..) / super(..): the explicit constructor call
            if (!ctor_first) {
                unsupported(t, "this(..) or super(..) elsewhere than as the "
                               "first statement of a constructor");
            }
            const uint32_t c = node(CTOR_CALL, t);
            ItemList args;
            i += 1;
            arguments(args);
            N(n).a = done(c, args);
        } else {
            const uint32_t e = expression();
            N(n).a = e;
            statement_expression(e);
        }
        expect(P_SEMI, "an expression statement");
        return done(n);
    }

    // StatementExpr = Assignment | ( "++" | "--" ) Unary
    //               | Postfix ( "++" | "--" ) | MethodCall
    void statement_expression(uint32_t e) {
        const uint16_t k = N(e).kind;
        if (k == ASSIGN || k == CALL || k == POSTFIX) {
            return;
        }
        if (k == UNARY) {
            const uint8_t op = tok(N(e).op_tok).kind;
            if (op == P_INC || op == P_DEC) {
                return;
            }
        }
        if (k == NEW) {
            unverified(N(e).tok0, "new T(..) as a statement");
            return;
        }
        error("not a statement: only an assignment, a call, ++ or -- can stand alone", N(e).tok0);
    }

    // "if" "(" Expression ")" Statement [ "else" Statement ]
    // An else belongs to the nearest if that has none yet.
    uint32_t if_statement() {
        const uint32_t n = node(IF, i);
        i += 1;
        expect(P_LPAREN, "if");
        const uint32_t cond = expression();
        N(n).a = cond;
        expect(P_RPAREN, "if");
        const uint32_t then = substatement("if");
        N(n).b = then;
        if (accept(KW_ELSE)) {
            const uint32_t other = substatement("else");
            N(n).c = other;
        }
        return done(n);
    }

    // "for" "(" [ LocalDecl | StatementExpr ] ";" [ Expression ] ";"
    // [ StatementExpr ] ")" Statement
    uint32_t for_statement() {
        const uint32_t n = node(FOR, i);
        ItemList updates;
        i += 1;
        expect(P_LPAREN, "for");
        if (!at(P_SEMI)) {
            if (at_local_decl()) {
                ItemList decls;
                const uint32_t decl = local_decl(decls);
                N(n).c = done(decl, decls);
                if (decls.size() > 1) {
                    // all 444 declarations in a for header declare one variable
                    unverified(N(decl).tok0, "several variables declared in a for header");
                }
            } else {
                const uint32_t lst = node(LIST, i);
                ItemList inits;
                expression_list(inits);
                N(n).c = done(lst, inits);
            }
        }
        expect(P_SEMI, "for");
        if (!at(P_SEMI)) {
            const uint32_t cond = expression();
            N(n).a = cond;
        }
        expect(P_SEMI, "for");
        if (!at(P_RPAREN)) {
            expression_list(updates);
        }
        expect(P_RPAREN, "for");
        const uint32_t body = substatement("for");
        N(n).b = body;
        return done(n, updates);
    }

    // The statement expressions of a for header, into `items` (the items of
    // their owner; the list is empty when this is called).
    void expression_list(ItemList& items) {
        const uint32_t first = i;
        for (;;) {
            const uint32_t e = expression();
            statement_expression(e);
            items.push_back(e);
            if (!accept(P_COMMA)) {
                break;
            }
        }
        if (items.size() > 1) {
            unverified(first, "several expressions in a for header");
        }
    }

    // "switch" "(" Expression ")" "{" { SwitchGroup } "}"
    // SwitchGroup = ( "case" Expression ":" | "default" ":" ) { Statement }
    // A group is the statements from one label to the next; it is a scope.
    uint32_t switch_statement() {
        const uint32_t n = node(SWITCH, i);
        ItemList groups;
        i += 1;
        expect(P_LPAREN, "switch");
        const uint32_t subject = expression();
        N(n).a = subject;
        expect(P_RPAREN, "switch");
        expect(P_LBRACE, "switch");
        bool seen_default = false;
        uint32_t group = NONE;
        ItemList labels;                // the items of group.a, the LIST of its labels
        ItemList stmts;                 // the items of group
        while (!at(P_RBRACE)) {
            const uint32_t t = i;
            const uint8_t k = kind();
            if (k == KW_CASE || k == KW_DEFAULT) {
                if (group == NONE || !stmts.empty()) {
                    if (group != NONE) {
                        close_group(group, labels, stmts, false);
                    }
                    group = node(SWITCH_GROUP, t);
                    const uint32_t lst = node(LIST, t);
                    N(group).a = lst;
                    labels.clear();
                    stmts.clear();
                    groups.push_back(group);
                } else {
                    unverified(t, "several labels on one switch group");
                }
                i += 1;
                uint32_t lab = NONE;
                if (k == KW_CASE) {
                    lab = node(CASE_LABEL, t);
                    const uint32_t value = expression();
                    N(lab).a = value;
                    expect(P_COLON, "a case label");
                    if (seen_default) {
                        unverified(t, "case label after the default label");
                    }
                } else {
                    lab = node(DEFAULT_LABEL, t);
                    expect(P_COLON, "the default label");
                    if (seen_default) {
                        unsupported(t, "second default label");
                    }
                    seen_default = true;
                }
                labels.push_back(done(lab));
            } else if (k == T_EOF) {
                error("switch body is not closed");
            } else {
                if (group == NONE) {
                    error("statement before the first case label");
                }
                if (stmts.empty()) {
                    // the labels of the group are complete: their list ends
                    // at the ":" read last
                    done(N(group).a, labels);
                }
                const uint32_t s = statement(false);
                stmts.push_back(s);
            }
        }
        if (group != NONE) {
            close_group(group, labels, stmts, true);
        } else {
            unverified(N(n).tok0, "switch without a case label");
        }
        i += 1;
        return done(n, groups);
    }

    void close_group(uint32_t group, const ItemList& labels, const ItemList& stmts, bool last) {
        if (stmts.empty()) {
            done(N(group).a, labels);   // labels and no statement behind them
        }
        done(group, stmts);
        if (stmts.empty()) {
            unverified(N(group).tok0, "switch group without statements");
        } else if (!last) {
            const uint16_t k = N(stmts[stmts.size() - 1]).kind;
            if (!(k == BREAK || k == RETURN || k == CONTINUE)) {
                unverified(N(group).tok0, "switch group that falls through");
            }
        }
    }

    // -- expressions ---------------------------------------------------------

    // Expression = Conditional | Assignment
    // Assignment = Target AssignOp Expression        (groups to the right)
    uint32_t expression() {
        enter();
        const uint32_t start = i;
        uint32_t left = conditional();
        const uint32_t t = i;
        const uint8_t k = kind();
        if (is_assign_op(k)) {
            i += 1;
            const uint16_t lk = N(left).kind;
            if (!(lk == NAME || lk == FIELD || lk == INDEX)) {
                unsupported(t, "assignment to something that is not a name, "
                               "a field or an array element");
            }
            if (k == P_SHL_ASSIGN || k == P_SHR_ASSIGN || k == P_USHR_ASSIGN) {
                unsupported(t, "operator ", fixed_text(k));
            }
            if (k == P_MOD_ASSIGN) {
                unverified(t, "operator %=");
            }
            const uint32_t n = node(ASSIGN, start);
            N(n).op_tok = t;
            N(n).a = left;
            const uint32_t value = expression();
            N(n).b = value;
            left = done(n);
        }
        leave();
        return left;
    }

    // Conditional = Binary [ "?" Expression ":" Conditional ]
    uint32_t conditional() {
        const uint32_t start = i;
        const uint32_t conds_before = conds;
        const uint32_t c = binary(1);
        if (!at(P_QUESTION)) {
            return c;
        }
        if (cond_depth || conds != conds_before) {
            unverified(i, "?: inside ?:");
        }
        conds += 1;
        const uint32_t n = node(COND, start);
        N(n).op_tok = i;
        N(n).a = c;
        i += 1;
        cond_depth += 1;
        enter();                        // a ? b : c ? d : ... recurses here
        const uint32_t x = expression();
        N(n).b = x;
        expect(P_COLON, "a conditional expression");
        const uint32_t y = conditional();
        N(n).c = y;
        leave();
        cond_depth -= 1;
        return done(n);
    }

    // Binary = Unary { BinaryOp Unary | "instanceof" Identifier }
    // by the levels of binary_prec, every level grouping to the left.
    uint32_t binary(uint32_t min_prec) {
        const uint32_t start = i;
        uint32_t left = unary();
        for (;;) {
            const uint32_t t = i;
            const uint8_t k = kind();
            const uint32_t prec = binary_prec(k);
            if (prec == 0 || prec < min_prec) {
                return left;
            }
            i += 1;
            uint32_t n = NONE;
            if (k == KW_INSTANCEOF) {
                n = node(INSTANCEOF, start);
                N(n).op_tok = t;
                N(n).a = left;
                if (at(KW_INT) || at(KW_FLOAT) || at(KW_VOID)) {
                    unsupported(i, "instanceof with a primitive type");
                }
                const uint32_t ty = class_type("instanceof");
                N(n).b = ty;
                if (at(P_LBRACKET)) {
                    unsupported(i, "instanceof with an array type");
                }
            } else {
                if (k == P_USHR) {
                    unsupported(t, "operator >>>");
                }
                n = node(BINARY, start);
                N(n).op_tok = t;
                N(n).a = left;
                const uint32_t right = binary(prec + 1);
                N(n).b = right;
            }
            left = done(n);
        }
    }

    // At "(": does a cast follow?  True for  "(" ( "int" | "float" ) ")"
    // and for  "(" Name ")"  followed by a token that can start an operand
    // other than + and -.
    bool at_cast() const {
        // (the end marker stops every loop, as in at_local_decl)
        const std::vector<Token>& toks = ast.toks;
        size_t j = static_cast<size_t>(i) + 1;
        uint8_t k = toks[j].kind;
        if (k == KW_INT || k == KW_FLOAT) {
            j += 1;
            while (toks[j].kind == P_LBRACKET && toks[j + 1].kind == P_RBRACKET) {
                j += 2;
            }
            return toks[j].kind == P_RPAREN;
        }
        if (k != T_ID) {
            return false;
        }
        j += 1;
        while (toks[j].kind == P_DOT && toks[j + 1].kind == T_ID) {
            j += 2;
        }
        while (toks[j].kind == P_LBRACKET && toks[j + 1].kind == P_RBRACKET) {
            j += 2;
        }
        if (toks[j].kind != P_RPAREN) {
            return false;
        }
        k = toks[j + 1].kind;
        return k == T_ID || k == T_INT || k == T_FLOAT || k == T_STR || k == T_RID ||
               k == KW_THIS || k == KW_SUPER || k == KW_NULL || k == KW_TRUE ||
               k == KW_FALSE || k == KW_NEW ||
               k == P_LPAREN || k == P_NOT || k == P_BITNOT;
    }

    // Unary = ( "+" | "-" | "!" | "~" | "++" | "--" ) Unary
    //       | "(" ( "int" | "float" ) ")" Unary
    //       | "(" Identifier ")" UnaryNotPlusMinus
    //       | Postfix [ "++" | "--" ]
    SLRRC_NOINLINE uint32_t unary() {
        const uint32_t t = i;
        const uint8_t k = kind();
        if (is_prefix_op(k)) {
            enter();
            i += 1;
            const uint32_t n = node(UNARY, t);
            N(n).op_tok = t;
            const uint32_t operand = unary();
            N(n).a = operand;
            if (k == P_INC || k == P_DEC) {
                incdec_target(t, operand);
            }
            leave();
            return done(n);
        }
        if (k == P_LPAREN && at_cast()) {
            enter();
            i += 1;
            const uint32_t n = node(CAST, t);
            const uint32_t ty = type("a cast");
            N(n).a = ty;
            if (N(ty).dims()) {
                unsupported(t, "cast to an array type");
            }
            expect(P_RPAREN, "a cast");
            const uint32_t operand = unary();
            N(n).b = operand;
            leave();
            return done(n);
        }
        return postfix();
    }

    void incdec_target(uint32_t t, uint32_t e) {
        const uint16_t k = N(e).kind;
        if (!(k == NAME || k == FIELD || k == INDEX)) {
            unsupported(t, "++ or -- applied to something that is not a name, "
                           "a field or an array element");
        }
    }

    // Postfix = Primary { "." Identifier [ Arguments ] | "[" Expression "]" }
    // and the postfix operators ++ and --.
    uint32_t postfix() {
        const uint32_t start = i;
        uint32_t e = primary();
        for (;;) {
            const uint32_t t = i;
            const uint8_t k = kind();
            if (k == P_DOT) {
                i += 1;
                const uint32_t name = ident("a member access");
                if (at(P_LPAREN)) {
                    const uint32_t n = node(CALL, start);
                    ItemList args;
                    N(n).a = e;
                    N(n).name_tok = name;
                    arguments(args);
                    e = done(n, args);
                } else {
                    if (N(e).kind == SUPER) {
                        unsupported(t, "super.field");
                    }
                    const uint32_t n = node(FIELD, start);
                    N(n).a = e;
                    N(n).name_tok = name;
                    e = done(n);
                }
            } else if (k == P_LBRACKET) {
                i += 1;
                const uint32_t n = node(INDEX, start);
                N(n).a = e;
                const uint32_t index = expression();
                N(n).b = index;
                expect(P_RBRACKET, "an array access");
                e = done(n);
            } else if (k == P_INC || k == P_DEC) {
                i += 1;
                incdec_target(t, e);
                const uint32_t n = node(POSTFIX, start);
                N(n).op_tok = t;
                N(n).a = e;
                e = done(n);
            } else {
                return e;
            }
        }
    }

    // Arguments = "(" [ Expression { "," Expression } ] ")", into `items`
    // (the items of their owner).
    void arguments(ItemList& items) {
        expect(P_LPAREN, "an argument list");
        if (!at(P_RPAREN)) {
            for (;;) {
                const uint32_t e = expression();
                items.push_back(e);
                if (!accept(P_COMMA)) {
                    break;
                }
            }
        }
        expect(P_RPAREN, "an argument list");
    }

    // Primary = Literal | Identifier [ Arguments ] | "this"
    //         | "super" "." Identifier Arguments | "(" Expression ")"
    //         | "new" Identifier Arguments
    //         | "new" ( "int" | "float" | Identifier ) "[" Expression "]"
    uint32_t primary() {
        const uint32_t t = i;
        const uint8_t k = kind();
        if (k == T_ID) {
            i += 1;
            if (at(P_LPAREN)) {
                const uint32_t n = node(CALL, t);
                ItemList args;
                N(n).name_tok = t;
                arguments(args);
                return done(n, args);
            }
            const uint32_t n = node(NAME, t);
            N(n).name_tok = t;
            return done(n);
        }
        if (is_literal_token(k)) {
            i += 1;
            return done(node(LITERAL, t));
        }
        if (k == KW_THIS || k == KW_SUPER) {
            i += 1;
            if (at(P_LPAREN)) {
                unsupported(t, "this(..) or super(..) elsewhere than as the "
                               "first statement of a constructor");
            }
            if (k == KW_SUPER && !at(P_DOT)) {
                error("super must be followed by \".\" or \"(\"");
            }
            return done(node(k == KW_THIS ? THIS : SUPER, t));
        }
        if (k == KW_NEW) {
            return new_expression();
        }
        if (k == P_LPAREN) {
            i += 1;
            const uint32_t n = node(PAREN, t);
            const uint32_t e = expression();
            N(n).a = e;
            expect(P_RPAREN, "a parenthesised expression");
            return done(n);
        }
        error("expected an expression");
    }

    uint32_t new_expression() {
        const uint32_t t = i;
        i += 1;
        const uint8_t k = kind();
        if (!(k == KW_INT || k == KW_FLOAT || k == T_ID)) {
            error("expected a type after new");
        }
        const uint32_t ty = done(type_name("new"));
        if (at(P_LPAREN)) {
            if (k != T_ID) {
                error("expected \"[\" after the primitive type of new");
            }
            const uint32_t n = node(NEW, t);
            ItemList args;
            N(n).a = ty;
            arguments(args);
            return done(n, args);
        }
        if (at(P_LBRACKET)) {
            const uint32_t n = node(NEW_ARRAY, t);
            N(n).a = ty;
            if (peek_kind(1) == P_RBRACKET) {
                unsupported(i, "array creation without a size");
            }
            i += 1;
            const uint32_t size = expression();
            N(n).b = size;
            expect(P_RBRACKET, "an array creation");
            if (at(P_LBRACKET)) {
                unsupported(i, "array creation with more than one dimension");
            }
            if (at(P_DOT)) {
                unsupported(i, "member access directly on new T[n]");
            }
            if (at(P_LBRACE)) {
                unsupported(i, "array initialiser");
            }
            return done(n);
        }
        error("expected \"(\" or \"[\" after the type of new");
    }
};

}  // namespace

Ast parse(std::string_view source, std::vector<Token> toks, Diagnostics& diags) {
    Parser p(source, std::move(toks), diags);
    try {
        const uint32_t root = p.unit();
        p.ast.root = root;
    } catch (const Stop&) {
        // the diagnostics say why; the tree has no root
    }
    return std::move(p.ast);
}

}  // namespace parser
}  // namespace slrrc
