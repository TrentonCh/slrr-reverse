// emit.hpp - the emitter: the AST of a method or of a field initialiser -> the
// node list of its tree.  (Reference: slrrc/emit.py.)
//
//     trees = emit_class(scope, build, packs, float_mode)     one Tree per tree owner
//
// The rules are those of ref/notes/statements.md (statements, locals, jumps,
// lines) and ref/notes/expressions.md (expressions, names, calls); together
// they reproduce 7,568 of the 7,570 trees of the oracle pairs node for node,
// the other two being trees that were compiled against another version of a
// class they call.
//
// THE OUTPUT
//
// A Tree is a list of SymNode(op, line, kind, value) and a table of operands.
// op is the op byte of the class file, line the source line the node carries.
// An operand that is a number (local index, count, int value, float bits, kind
// of expression, jump offset) is in the node; an operand that names a pool
// entry is kept BY CONTENT in tree.operands (a text; a class name; a class, a
// member name and a descriptor; a pack file name and a local id) and the node
// holds its index there.  Pool indices do not exist yet: the pool is filled by
// two passes over the finished trees of the whole class (the writer).
//
// WHAT THE EMITTER KEEPS WHILE IT WALKS A BODY
//
//     locals      one list for the frame: `this` (not in a static method), the
//                 parameters in REVERSE order, then every declared local.  A
//                 declaration takes the next index; the end of its scope (block,
//                 for statement, case group, body) releases it.  break, continue
//                 and return emit releases without closing the scope.
//     flow        the enclosing loops and switches: where break and continue
//                 jump to, and how many locals were live at the entry
//     labels      positions that jumps name; a jump holds a label until the
//                 tree is finished and then the offset in nodes, relative to
//                 the jump node itself
//
// THE ORDER OF THE NODES is the order of execution, which is not the order of
// the source: the value of an assignment before its target, the index before
// the array, the arguments before a computed receiver, the update of a for
// behind its body, the case values of a switch before its groups.
//
// THE LINE OF A NODE is the line of the token the game's parser had read when
// it built the node (statements.md 8): the last token of a construct that
// needs no lookahead (a literal, a name, the ")" of a call, the "]" of an
// index), the token BEHIND a construct that ends in an operand (operators,
// casts, assignments, ?:, instanceof, new T[n]).  The jumps of if, while and
// for carry the line of end_tok(): the lookahead rule of an LALR(1) parser for
// the grammar of the Java specification.  Every put_*() of the emitter takes
// the index of that token.  Per kind of node:
//
//     expr literal and its carrier                the literal
//     local, field (a simple name)                the identifier
//     path n of a name                            its last identifier
//     segments of a path                          the identifier of the segment
//     getfield                                    the member name
//     index                                       the "]"
//     push_int, call, call_on, call_super,
//       expr call / stmt call                     the ")" of the call
//     path n of a call, method / name             the callee name
//     expr new, its push_int, new_init            the ")"
//     type carrier of new                         the "(" behind the class name
//     expr newarray and its type carrier          the token behind the "]"
//     - ! ~ ++x --x                               the token behind the operand
//     x++ x--                                     the operator
//     binary operators; and_test / or_test,
//       their jump, expr land / lor               the token behind the right operand
//     assignment                                  the token behind the value
//     ?:                                          the token behind the third operand
//     cast and its carrier                        the token behind the operand
//     instanceof and its type carrier             the token behind the type
//     this(..) / super(..): push_int,
//       this_init / super_init, stmt ctor         the ";"
//     the implicit super call                     the first token behind the "{"
//     declare, type, init, store                  the token behind the declared name
//     release at the end of a block               its "}"
//     jump_false and jump of if, while, for;
//       release at the end of a for               end_tok(statement)
//     jump_true of do                             its ";"
//     case, pop, jump of a switch                 the "}" of the switch
//     release at the end of a case group          the token behind the group
//     release and jump of break / continue        its ";"
//     value / push_void / ret, release,
//       ret_value of a return                     its ";"
//     implicit return, releases at the end
//       of a body                                 the "}" of the body
//     end of a field initialiser                  the token behind the declarator
//
// For 14 of these kinds two candidate tokens stand on one line in every shipped
// source (statements.md 8.1, "not ruled out"); the token taken here is the one
// the grammar suggests.
//
// NAMES (expressions.md 5).  The first identifier of a name is a local, else a
// field of the class or a superclass, else a class.  a.b.c with such a head is
// one `path n` with n segments; every further segment is a field ref.  A field
// ref names the declaring class for a static field and the static type of the
// receiver for an instance field (the class being compiled for a simple name).
// A member of anything else (this, an element, a call, a cast, a parenthesis)
// is fetched by name: getfield.
//
// CALLS (expressions.md 6).  f(..) and a.b.f(..) end in a method ref when every
// argument is a literal, a local, a field written as a simple name or `this`,
// and the scored lookup (classes::find_method) on the receiver class finds a
// method; the ref names the receiver class and carries the found descriptor.
// In every other case the call ends in the bare name, behind a `path 1` header
// when it has no receiver.  The compiler knows a type only for those four
// forms; it never derives the type of an operator, a call, an element or a new.
//
// LITERALS.  An int is its value modulo 2**32, a string its text after escape
// processing, true and false a carrier of their own.  Two literals depend on
// state that no source holds, and neither is decided here.  The bits of a float
// come from floatlit::float_bits() under the conversion the caller selects: B
// is what the scanner of the Build 940 exe computes, A, C and D reproduce
// shipped files of earlier builds.  The pack and the id of a resource id come
// from packs::resolve() over the PackNames the caller passes in: the pack table
// of the session, as in the exe, which resolves every resource id through the
// resource engine when it writes a class (0x54B310).  The emitter asks when it
// reaches the literal, tree by tree in tree order.  A pack for which the
// PackNames answers "no file" is compiled to system.rpk id 0, the form one
// shipped class shows, and reported (W_PACK) at every such literal, in both
// modes: the resource is not the one the source names.  An id with a low word
// of 0 or with a high word (an external reference of the pack) follows the rule
// read in the exe and is reported as unverified: no shipped source has one.
//
// DIAGNOSTICS.  An identifier that is no local, field or class is E_NAME, a
// member that is not found in a dotted name is E_MEMBER; the tree of a source
// with an error is never used.  Constructs no shipped source uses are compiled
// by the plain extension of the rules and reported through diags.unverified()
// (ref/notes/compiler_spec.md appendix B): a warning, or an error in strict
// mode.  Every diagnostic is a function of the source and of the answers of the
// two interfaces; none depends on what an earlier method happened to look up.
// The emitter never throws for bad input.
//
// NUMBERS.  Local indices, counts, token indices and node positions are
// unsigned.  No loop counts below zero, and "no local" is NONE, not a negative
// number.  The one place that needs a signed difference is the jump offset,
// which finish() forms modulo 2**32.
//
// THIS HEADER DOES NOT INCLUDE parser.hpp (classes.hpp says why): a Tree is
// part of a ClassBuild, which the reader of class files shares with a host.
// The tree of the source is named by declaration only; a node of it is its
// index in Ast::nodes.
#ifndef SLRRC_EMIT_HPP
#define SLRRC_EMIT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "classes.hpp"
#include "floatlit.hpp"
#include "packs.hpp"

namespace slrrc {

namespace parser {
struct Ast;
}  // namespace parser

namespace decl {
struct ClassBuild;
}  // namespace decl

namespace emit {

// ---------------------------------------------------------------------------
// op bytes (ref/notes/opcodes.md; the 16-bit opcode of the VM is in the comment)
// ---------------------------------------------------------------------------

inline constexpr uint8_t OP_LOCAL = 0x01;           // 0x1001  local i
inline constexpr uint8_t OP_DECLARE = 0x02;         // 0x1002  declare i; a type carrier follows
inline constexpr uint8_t OP_RELEASE = 0x03;         // 0x1003  release i
inline constexpr uint8_t OP_AND_TEST = 0x04;        // 0x0004  test of &&; a jump follows
inline constexpr uint8_t OP_OR_TEST = 0x05;         // 0x0005  test of ||; a jump follows
inline constexpr uint8_t OP_TYPE = 0x06;            // 0x1006  carrier: utf8 type descriptor
inline constexpr uint8_t OP_EXPR = 0x07;            // 0x1007  expression of kind imm, value used
inline constexpr uint8_t OP_STMT = 0x08;            // 0x1008  expression of kind imm, as a statement
inline constexpr uint8_t OP_STR = 0x09;             // 0x4009  carrier: string literal (utf8)
inline constexpr uint8_t OP_FLOAT = 0x0A;           // 0x200a  carrier: float literal (bits)
inline constexpr uint8_t OP_INT = 0x0B;             // 0x100b  carrier: int literal
inline constexpr uint8_t OP_NULL = 0x0C;            // 0x000c  carrier: null
inline constexpr uint8_t OP_BOOL = 0x0D;            // 0x100d  carrier: true (1) / false (0)
inline constexpr uint8_t OP_RID = 0x0E;             // 0x100e  carrier: resource id (rid entry)
inline constexpr uint8_t OP_END = 0x10;             // 0x0010  end of a field initialiser
inline constexpr uint8_t OP_PATH = 0x11;            // 0x1011  name path of imm segments
inline constexpr uint8_t OP_NAME = 0x12;            // 0x4012  segment: method by name (utf8)
inline constexpr uint8_t OP_JUMP = 0x14;            // 0x1014
inline constexpr uint8_t OP_JUMP_FALSE = 0x15;      // 0x1015
inline constexpr uint8_t OP_JUMP_TRUE = 0x16;       // 0x1016
inline constexpr uint8_t OP_CASE = 0x17;            // 0x1017  compare with the switch subject, jump when equal
inline constexpr uint8_t OP_POP = 0x18;             // 0x0018  drop the switch subject
inline constexpr uint8_t OP_CLASS = 0x19;           // 0x1019  segment / carrier: class entry
inline constexpr uint8_t OP_METHOD = 0x1A;          // 0x101a  segment: method ref
inline constexpr uint8_t OP_FIELD = 0x1B;           // 0x101b  field ref (a node of its own, or a segment)
inline constexpr uint8_t OP_GETFIELD = 0x1C;        // 0x401c  member of a computed value, by name (utf8)
inline constexpr uint8_t OP_INDEX = 0x20;           // 0x0020  array element
inline constexpr uint8_t OP_NEW_INIT = 0x21;        // 0x0021  constructor call of new
inline constexpr uint8_t OP_THIS_INIT = 0x22;       // 0x0022  this(..)
inline constexpr uint8_t OP_SUPER_INIT = 0x23;      // 0x0023  super(..)
inline constexpr uint8_t OP_CALL = 0x24;            // 0x0024  call; a method ref or a path follows
inline constexpr uint8_t OP_CALL_ON = 0x25;         // 0x4025  call on a computed receiver, by name (utf8)
inline constexpr uint8_t OP_CALL_SUPER = 0x26;      // 0x4026  super.f(..), by name (utf8)
inline constexpr uint8_t OP_PUSH_INT = 0x27;        // 0x1027  argument count
inline constexpr uint8_t OP_VALUE = 0x28;           // 0x0028  detach the return value from its variable
inline constexpr uint8_t OP_PUSH_VOID = 0x29;       // 0x0029  the value of return;
inline constexpr uint8_t OP_RET = 0x2A;             // 0x002a  return from a constructor
inline constexpr uint8_t OP_RET_VALUE = 0x2B;       // 0x002b  return
inline constexpr uint8_t OP_INIT = 0x2C;            // 0x002c  marker: the declaration has an initialiser
inline constexpr uint8_t OP_STORE = 0x2D;           // 0x002d  store the initialiser

// OP_NAMES.get(op): the mnemonic of an op byte ("local", "declare", ...), for
// dumps; nullptr for a byte that is none.
const char* op_name(uint8_t op);

// kinds of expression: the immediate of OP_EXPR / OP_STMT (expressions.md 4)
inline constexpr uint32_t X_COND = 0x00;
inline constexpr uint32_t X_LOR = 0x01;
inline constexpr uint32_t X_LAND = 0x02;
inline constexpr uint32_t X_NEG = 0x16;
inline constexpr uint32_t X_LNOT = 0x15;
inline constexpr uint32_t X_BNOT = 0x14;
inline constexpr uint32_t X_POSTINC = 0x17;
inline constexpr uint32_t X_POSTDEC = 0x18;
inline constexpr uint32_t X_PREINC = 0x19;
inline constexpr uint32_t X_PREDEC = 0x1A;
inline constexpr uint32_t X_CALL = 0x1B;
inline constexpr uint32_t X_LITERAL = 0x1C;
inline constexpr uint32_t X_NEW = 0x1E;
inline constexpr uint32_t X_NEWARRAY = 0x1F;
inline constexpr uint32_t X_INSTANCEOF = 0x20;
inline constexpr uint32_t X_CAST = 0x21;
inline constexpr uint32_t X_CTOR = 0x22;

inline constexpr uint32_t NONE = 0xFFFFFFFF;

// BINARY_KIND[kind of the operator token]: | 0x03, ^ 0x04, & 0x05, != 0x06,
// == 0x07, >= 0x08, <= 0x09, > 0x0A, < 0x0B, >> 0x0D, << 0x0E, - 0x0F, + 0x10,
// % 0x11, / 0x12, * 0x13.  NONE for a token that is not in the table (the
// parser lets no other binary operator through).
uint32_t binary_kind(uint8_t token_kind);

// ASSIGN_KIND[kind of the operator token]: = 0x23, *= 0x24, /= 0x25, %= 0x26,
// += 0x27, -= 0x28, &= 0x2C, ^= 0x2D, |= 0x2E.  NONE for any other token.
uint32_t assign_kind(uint8_t token_kind);

// X_NAMES.get(kind): kind -> name, for dumps; nullptr for a kind that has
// none.  0x0C, 0x1D and 0x29 to 0x2B are never emitted (by their place in the
// table: >>>, -, <<=, >>=, >>>=).
const char* x_name(uint32_t kind);

// ---------------------------------------------------------------------------
// the output
// ---------------------------------------------------------------------------

// what the value of a node is
inline constexpr uint8_t K_NONE = 0;        // the node has no operand (3 bytes in the file)
inline constexpr uint8_t K_IMM = 1;         // a number
inline constexpr uint8_t K_LABEL = 2;       // a label; only while the tree is being emitted
inline constexpr uint8_t K_UTF8 = 3;        // index into tree.operands: a text
inline constexpr uint8_t K_CLASS = 4;       // index into tree.operands: a class
inline constexpr uint8_t K_REF = 5;         // index into tree.operands: a field or a method of a class
inline constexpr uint8_t K_RID = 6;         // index into tree.operands: a resource

struct SymNode {
    uint8_t op;
    uint16_t line;          // (the scanner refuses a source of more than 65,535 lines)
    uint8_t kind;
    uint32_t value;
};

// What a node names in the pool, by content.
//
//     K_UTF8      text
//     K_CLASS     text = the class name
//     K_REF       text = the class name, name, desc
//     K_RID       text = the pack file name, number = the local id
struct Operand {
    std::string text;
    std::string name;
    std::string desc;
    uint32_t number;
};

struct Tree {
    std::vector<SymNode> nodes;
    std::vector<Operand> operands;
    uint32_t max_locals = 0;        // the most locals alive at one time; 0 for an empty tree
};

// ---------------------------------------------------------------------------
// the tree of the source, as the emitter reads it
// ---------------------------------------------------------------------------

// Is the expression a name: an identifier, or a name "." identifier?
bool is_name(const parser::Ast& ast, uint32_t e);

// The identifier tokens of a name, first to last.
std::vector<uint32_t> name_toks(const parser::Ast& ast, uint32_t e);

// The expression inside its parentheses.
uint32_t strip_parens(const parser::Ast& ast, uint32_t e);

// Is the statement a written this(..) or super(..)?
bool is_ctor_call(const parser::Ast& ast, uint32_t s);

// The trees of one class, in tree order: one Tree per tree owner of build.rows
// (a field with an initialiser, a method that is not abstract).
//
// scope: the scope of the compilation the class belongs to.  packs: the pack
// names of the host (the driver puts a CachedPackNames in between).
// float_mode: the conversion of float literals.
std::vector<Tree> emit_class(classes::UnitScope& scope, const decl::ClassBuild& build,
                             packs::PackNames& packs, floatlit::Mode float_mode);

}  // namespace emit
}  // namespace slrrc

#endif
