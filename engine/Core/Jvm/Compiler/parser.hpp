// parser.hpp - recursive descent parser: tokens -> AST.  (Reference:
// slrrc/parser.py.)
//
//     ast = parse(source, toks, diags)      ast.root is NONE after an error
//
// The grammar is the one of notes/grammar.md section 3 (all 3,454 shipped
// sources); binary operators by precedence climbing, the ten levels of Java.
//
// THE AST
//
// One Node type for every construct:
//
//     kind                node kind, one of the constants below
//     (index)             its position in ast.nodes (creation order)
//     tok0, tok1          index of its first and last token
//     op_tok, name_tok    index of an operator / a name token, or NONE
//     a, b, c             child nodes (indices into ast.nodes) or NONE
//                         (meaning per kind)
//     list, count         its items: the slice ast.lists[list .. list + count)
//                         of node indices (meaning per kind); ast.items(node)
//     flags               modifier bits and other small facts (per kind)
//
// Names are not copied into the tree: the name of a node is the text of its
// name token (ast.text(n.name_tok)), a literal is the text of its token, an
// operator is the kind of its token.  The token indices are kept because the
// compiler stamps every tree node with the line its parser had reached, which is
// a function of token positions (notes/statements.md 8).
//
// The children per kind ("-" = unused: NONE or no items):
//
//     kind            a               b               c               items
//     UNIT            package QNAME   -               -               IMPORT and CLASS nodes
//     QNAME           -               -               -               -
//     IMPORT          QNAME           -               -               -
//     CLASS           extends TYPE    LIST of TYPE    -               members
//                                     (implements)
//     FIELD_DECL      TYPE            -               -               DECLARATORs
//     DECLARATOR      initialiser     -               -               -
//     METHOD_DECL     return TYPE     body BLOCK      -               PARAMs
//     PARAM           TYPE            -               -               -
//     TYPE            -               -               -               -
//     LIST            -               -               -               the nodes
//     BLOCK           -               -               -               statements
//     LOCAL_VAR       TYPE            -               -               DECLARATORs
//     EXPR_STMT       expression      -               -               -
//     IF              condition       then            else            -
//     WHILE           condition       body            -               -
//     DO              body            condition       -               -
//     FOR             condition       body            init            update expressions
//     SWITCH          subject         -               -               SWITCH_GROUPs
//     SWITCH_GROUP    LIST of labels  -               -               statements
//     CASE_LABEL      value           -               -               -
//     DEFAULT_LABEL, BREAK, CONTINUE, EMPTY, LITERAL, NAME, THIS, SUPER: none
//     RETURN          value           -               -               -
//     FIELD           target          -               -               -
//     INDEX           target          index           -               -
//     CALL            target          -               -               arguments
//     CTOR_CALL       -               -               -               arguments
//     NEW             TYPE            -               -               arguments
//     NEW_ARRAY       element TYPE    size            -               -
//     UNARY, POSTFIX  operand         -               -               -
//     BINARY          left            right           -               -
//     ASSIGN          target          value           -               -
//     COND            condition       then            else            -
//     CAST            TYPE            operand         -               -
//     INSTANCEOF      operand         TYPE            -               -
//     PAREN           expression      -               -               -
//
//     METHOD_DECL: a is NONE for a constructor, b is NONE for a body written ";".
//     FOR: c is a LOCAL_VAR, a LIST of expressions, or NONE; a may be NONE.
//     IF: c may be NONE.  RETURN, DECLARATOR, CALL, UNIT, CLASS: a may be NONE.
//
// The tokens and flags per kind:
//
//     kind            tok0 .. tok1                name_tok        op_tok      flags
//     UNIT            the whole source            -               -           -
//     QNAME           id { . id }                 the last id     -           -
//     IMPORT          import .. ;                 -               the ":"     F_STAR, F_COLON
//     CLASS           first modifier .. }         the class name  -           modifiers, F_INTERFACE
//     FIELD_DECL      first modifier .. ;         -               -           modifiers
//     DECLARATOR      name .. end of initialiser  the name        the "="     dims
//     METHOD_DECL     first modifier .. } or ;    the method name -           modifiers, F_CTOR
//     PARAM           [final] type name           the name        -           modifiers, dims
//     TYPE            name { [ ] }                last id of the  -           dims
//                                                 name
//     LIST            first .. last item          -               -           -
//     BLOCK           { .. }                      -               -           -
//     LOCAL_VAR       [final] type .. [;]         -               -           modifiers
//     EXPR_STMT       expression ;                -               -           -
//     IF, WHILE, FOR  keyword .. end of the body  -               -           -
//     DO              do .. ;                     -               -           -
//     SWITCH          switch .. }                 -               -           -
//     SWITCH_GROUP    first label .. last token   -               -           -
//                     before the next label
//     CASE_LABEL      case .. :                   -               -           -
//     DEFAULT_LABEL   default :                   -               -           -
//     BREAK, CONTINUE, RETURN   keyword .. ;      -               -           -
//     EMPTY           ;                           -               -           -
//     LITERAL         the literal token           -               -           -
//     NAME            the identifier              = tok0          -           -
//     THIS, SUPER     the keyword                 -               -           -
//     FIELD           target . name               the name        -           -
//     INDEX           target [ index ]            -               -           -
//     CALL            [ target . ] name ( .. )    the name        -           -
//     CTOR_CALL       this ( .. ) / super ( .. )  -               -           -
//     NEW             new T ( .. )                -               -           -
//     NEW_ARRAY       new T [ n ]                 -               -           -
//     UNARY           op operand                  -               the op      -
//     POSTFIX         operand op                  -               the op      -
//     BINARY, ASSIGN  left op right               -               the op      -
//     COND            c ? x : y                   -               the "?"     -
//     CAST            ( T ) operand               -               -           -
//     INSTANCEOF      x instanceof T              -               the keyword -
//     PAREN           ( e )                       -               -           -
//
//     The start of an expression node is the first token of its leftmost
//     operand: a.b[i].f(x) + 1 and all its left parts start at "a".
//
// A LOCAL_VAR that is a statement ends at its ";", one in a for header at its
// last declarator.  A name chain a.b.c is FIELD(FIELD(NAME a, b), c): the
// parser does not decide what a name is.  Parentheses are kept (PAREN).
//
// Constructs outside the proven subset (notes/compiler_spec.md appendix B):
// diags.unverified() where a default is stated (an error in strict mode),
// E_UNSUPPORTED where none is, E_SYNTAX for everything outside the grammar.
//
// HOW THE PARSER STOPS AT AN ERROR.  It stops at the first error, and it does
// so by unwinding: fail() records the diagnostic and throws the private Stop,
// which parse() catches; nothing else throws it and nothing else catches it.
// It never leaves parse().  Every function of the parser is written on that
// assumption (parser.cpp says which calls never return).
//
// NODES ARE NUMBERED IN THE ORDER THEY ARE MADE (node()): the AST dump prints
// that number, and two implementations are compared through it.  A node made
// for a construct that starts with an operand or a type is made AFTER that
// part (BINARY, ASSIGN, COND and INSTANCEOF after their left operand; FIELD,
// INDEX, POSTFIX and a CALL with a receiver after their target; FIELD_DECL, a
// METHOD_DECL with a return type, PARAM, LOCAL_VAR, NEW and NEW_ARRAY after
// their TYPE), every other node BEFORE its children (UNIT, IMPORT, CLASS, a
// constructor, DECLARATOR, BLOCK, EXPR_STMT, IF, WHILE, DO, FOR, SWITCH,
// SWITCH_GROUP and its LIST, CASE_LABEL, RETURN, PAREN, UNARY, CAST, a CALL
// without receiver, CTOR_CALL, LIST).
//
// EVERY NODE IS CLOSED EXACTLY ONCE (done()), after its last child and its last
// item, and before any token behind it is read.  The items of a node are
// collected in a vector of the function that parses the construct and copied
// into Ast::lists by done(), where they become the slice (list, count) of the
// node; because every node is closed once and after its items, the slices do
// not interleave.
//
// The names of this header are those of parser.py, and one of them is a macro
// of the Windows headers: THIS (the COM part of <windows.h> defines it; with
// it in front, the list of node kinds does not compile).  Include this header
// in no translation unit that includes <windows.h>, or define
// WIN32_LEAN_AND_MEAN there.  diag.hpp and scanner.hpp have no such name, and
// the public interface of the compiler does not include this header.
#ifndef SLRRC_PARSER_HPP
#define SLRRC_PARSER_HPP

#ifdef THIS
#error "parser.hpp: THIS is a macro here (the COM headers of <windows.h> define it); see the head of this file"
#endif

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "diag.hpp"
#include "scanner.hpp"

namespace slrrc {
namespace parser {

inline constexpr uint32_t NONE = 0xFFFFFFFF;        // "no token" in op_tok / name_tok, "no node"

// ---------------------------------------------------------------------------
// node kinds
// ---------------------------------------------------------------------------

enum NodeKind : uint16_t {
    UNIT = 1,
    QNAME = 2,
    IMPORT = 3,
    CLASS = 4,
    FIELD_DECL = 5,
    DECLARATOR = 6,
    METHOD_DECL = 7,
    PARAM = 8,
    TYPE = 9,
    LIST = 10,
    BLOCK = 11,
    LOCAL_VAR = 12,
    EXPR_STMT = 13,
    IF = 14,
    WHILE = 15,
    DO = 16,
    FOR = 17,
    SWITCH = 18,
    SWITCH_GROUP = 19,
    CASE_LABEL = 20,
    DEFAULT_LABEL = 21,
    BREAK = 22,
    CONTINUE = 23,
    RETURN = 24,
    EMPTY = 25,
    LITERAL = 26,
    NAME = 27,
    THIS = 28,
    SUPER = 29,
    FIELD = 30,
    INDEX = 31,
    CALL = 32,
    CTOR_CALL = 33,
    NEW = 34,
    NEW_ARRAY = 35,
    UNARY = 36,
    POSTFIX = 37,
    BINARY = 38,
    ASSIGN = 39,
    COND = 40,
    CAST = 41,
    INSTANCEOF = 42,
    PAREN = 43
};

// KIND_NAMES[kind]: "Unit", "QName", ... (the names the AST dump prints).
const char* kind_name(uint16_t kind);

// ---------------------------------------------------------------------------
// flags
// ---------------------------------------------------------------------------

// The modifier bits are the bits of the flags word of a FILD / MTHD row
// (notes/declarations.md 4.2), so flags & ACC_MASK is that word.
inline constexpr uint32_t ACC_PUBLIC = 0x01;
inline constexpr uint32_t ACC_PROTECTED = 0x02;
inline constexpr uint32_t ACC_PRIVATE = 0x04;
inline constexpr uint32_t ACC_STATIC = 0x08;
inline constexpr uint32_t ACC_ABSTRACT = 0x10;
inline constexpr uint32_t ACC_FINAL = 0x20;
inline constexpr uint32_t ACC_NATIVE = 0x40;
inline constexpr uint32_t ACC_SYNCHRONIZED = 0x80;
inline constexpr uint32_t ACC_MASK = 0xFF;

inline constexpr uint32_t F_INTERFACE = 0x0100;     // CLASS: declared with the keyword interface
inline constexpr uint32_t F_CTOR = 0x0200;          // METHOD_DECL: a constructor (no return type)
inline constexpr uint32_t F_STAR = 0x0400;          // IMPORT: ends in ".*"
inline constexpr uint32_t F_COLON = 0x0800;         // IMPORT: "word:" in front of the name (tok0 + 1 is the word)
inline constexpr uint32_t DIMS_SHIFT = 16;          // TYPE, DECLARATOR, PARAM: number of "[" "]" pairs = flags >> 16

// MODIFIER_BITS.get(kind, 0): the modifier bit of a keyword token, 0 for any
// other token.
uint32_t modifier_bit(uint8_t token_kind);

// BINARY_PREC.get(kind, 0): binary operators: token kind -> precedence (higher
// binds tighter); all group to the left.  instanceof sits with the relational
// operators.  0 for a token that is no binary operator.
uint32_t binary_prec(uint8_t token_kind);

bool is_assign_op(uint8_t token_kind);          // kind in ASSIGN_OPS
bool is_prefix_op(uint8_t token_kind);          // kind in PREFIX_OPS
bool is_literal_token(uint8_t token_kind);      // kind in LITERAL_TOKENS

// The limit on nesting; beyond it the source is refused with E_LIMIT.  It
// bounds two things: the recursion of the parser (one level per statement,
// per expression and per prefix operator or cast; the deepest shipped source,
// Garage.java, reaches 51) and the height of the tree (Garage.java: 56), which
// bounds the recursion of every pass behind the parser.  The C++ compiler has
// no second limit: its answer is the one of the reference under a raised
// recursion limit (which is what the reference's command line and tests run
// under).  parser.cpp says how much stack the limit needs: 140 KB in a 32-bit
// release build for the deepest input it allows.
inline constexpr uint32_t MAX_DEPTH = 200;

struct Node {
    uint16_t kind;
    uint32_t tok0;
    uint32_t tok1;
    uint32_t op_tok;
    uint32_t name_tok;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t list;          // items: ast.lists[list .. list + count)
    uint32_t count;
    uint32_t flags;

    // TYPE, DECLARATOR, PARAM: the number of "[" "]" pairs.
    uint32_t dims() const { return flags >> DIMS_SHIFT; }
};

// The items of a node: a view into Ast::lists (valid while the Ast lives and
// is not changed).
struct Items {
    const uint32_t* first;
    uint32_t count;

    const uint32_t* begin() const { return first; }
    const uint32_t* end() const { return first + count; }
    uint32_t size() const { return count; }
    bool empty() const { return count == 0; }
    uint32_t operator[](uint32_t k) const { return first[k]; }
};

// The tree of one source with the tokens it refers to.
//
// `source` is a view: the text the Ast was parsed from has to outlive it.
struct Ast {
    std::string_view source;
    std::vector<scanner::Token> toks;
    std::vector<Node> nodes;        // every node, in creation order
    std::vector<uint32_t> lists;    // the items of all nodes, one slice per node
    uint32_t root = NONE;           // the UNIT node; NONE when the parse failed

    const Node& node(uint32_t index) const { return nodes[index]; }

    Items items(const Node& n) const {
        return Items{n.count != 0 ? lists.data() + n.list : nullptr, n.count};
    }
    Items items(uint32_t index) const { return items(nodes[index]); }

    // Source text of token number tok.
    std::string_view text(uint32_t tok) const {
        const scanner::Token& t = toks[tok];
        return source.substr(t.pos, t.length);
    }

    uint32_t line(uint32_t tok) const { return toks[tok].line; }

    // Text of the dotted name that spans the tokens tok0 .. tok1
    // (identifiers with a "." between them): "java.util.Vector".
    std::string dotted(uint32_t tok0, uint32_t tok1) const;

    // The name a TYPE node writes: int, float, void, or the class name.
    std::string type_name(const Node& type_node) const;
};

// The child nodes of a node in source order.
std::vector<uint32_t> children(const Ast& ast, uint32_t n);

// True for a TYPE node that names int, float or void.
bool is_primitive(const Ast& ast, const Node& type_node);

// Parse the tokens of one source (the vector scan() returned for it: closed
// by T_EOF).  Returns the Ast; ast.root is NONE when the source does not fit
// the grammar (the diagnostics say why).  Never throws for bad input.
Ast parse(std::string_view source, std::vector<scanner::Token> toks, Diagnostics& diags);

}  // namespace parser
}  // namespace slrrc

#endif
