// decl.hpp - the declarations of a class: the first three steps of its pool
// and the rows of FILD, MTHD and CLSS.  (Reference: slrrc/decl.py.)
//
// Everything here follows from the source alone (notes/declarations.md 3, 4,
// 6.2 to 6.4 and 12; byte-identical for all 2,873 classes of the pairs):
//
//     1 seed    the class, its superclass, its interfaces: utf8 + class entry each
//     2 types   the type descriptors, in the order the game's parser met them
//               (type_walk)
//     3 names   per member in source order: field -> its name; method -> its
//               descriptor, then its name ("<init>" for a constructor)
//
// The rows only name entries of these steps.  Steps 4 and 5 (member refs,
// strings) need the finished trees and belong to the writer.
//
// A node of the tree is its index in Ast::nodes, as in parser.hpp.  This
// header names the tree by declaration only and does not include parser.hpp
// (classes.hpp says why): the rows are also what the reader of class files
// (classfile.hpp) hands to a host.
#ifndef SLRRC_DECL_HPP
#define SLRRC_DECL_HPP

#include <cstdint>
#include <vector>

#include "classes.hpp"
#include "emit.hpp"
#include "pool.hpp"

namespace slrrc {

namespace parser {
struct Ast;
struct Items;
}  // namespace parser

namespace decl {

inline constexpr uint32_t NONE = 0xFFFFFFFF;

// One row of FILD (16 bytes).
struct FieldRow {
    uint32_t flags;         // OR of the modifier bits
    uint32_t name;          // utf8
    uint32_t type;          // utf8 of the descriptor
    uint32_t tree;          // initialiser tree, NONE without one
};

// One row of MTHD (20 bytes).
struct MethodRow {
    uint32_t flags;
    uint32_t name;          // utf8
    uint32_t desc;          // utf8 of the descriptor
    uint32_t tree;          // NONE for an abstract method
    uint32_t locals;        // number of local slots, 0 for an empty tree
};

// The member a tree belongs to.  The trees of a class are numbered in source
// order over the fields with an initialiser and the methods that are not
// abstract; owners[t] is the owner of tree t.
struct TreeOwner {
    uint32_t node;          // DECLARATOR of the field, or the METHOD_DECL
    uint32_t decl;          // the FIELD_DECL of a declarator, or the METHOD_DECL
    bool is_field;
    bool is_static;
};

// FILD, MTHD and CLSS of a class, with pool indices.
struct Rows {
    std::vector<FieldRow> static_fields;        // source order
    std::vector<FieldRow> instance_fields;
    std::vector<MethodRow> static_methods;      // source order
    std::vector<MethodRow> instance_methods;
    uint32_t interface_flag = 0;                // CLSS: 1 for an interface (the reference's
    //                                             `interface`, a macro of the Windows headers)
    uint32_t name = NONE;                       // CLSS: class entry of the class
    uint32_t super = NONE;                      // CLSS: class entry of the superclass, or NONE
    std::vector<uint32_t> interfaces;           // CLSS: class entries
    std::vector<TreeOwner> owners;              // one per tree
};

// One class of a source after the declaration steps.
struct ClassBuild {
    uint32_t node = NONE;                       // the CLASS node
    const classes::ClassInfo* info = nullptr;   // an element of UnitScope::own
    bool first_in_file = false;
    pool::Pool pool;                            // after steps 1 to 3 (the writer adds steps 4 and 5)
    Rows rows;
    std::vector<emit::Tree> trees;              // one per tree owner, once the emitter has run
    std::vector<uint32_t> pool_steps;           // length of the pool after each step taken so far
};

// ---------------------------------------------------------------------------
// step 1: seed
// ---------------------------------------------------------------------------

// The class, then its superclass, then its interfaces in implements order.
// java.lang.Object and an interface have no superclass pair.
void seed(pool::Pool& pool, const classes::ClassInfo& info);

// ---------------------------------------------------------------------------
// step 2: types
// ---------------------------------------------------------------------------

// Intern the type descriptors of a class body in the order the game's parser
// reduces the constructs that carry them (notes/declarations.md 6.3):
//
//     field             its type, at the declaration
//     parameter         its type, left to right (also of a bodiless method)
//     method            its return type, after the parameters, before the body
//     constructor       nothing for a return type
//     new T(args)       "LT;" before the arguments
//     new T[n]          the array type "[LT;" / "[I" / "[F", before n
//     (int)x (float)x   "I" / "F", before x; a cast to a class: nothing
//     x instanceof T    "LT;" after x
//     T a = e, b = f;   the type of a local after its initialisers
//     end of the body   "LI;" per interface, then "LC;" of the class itself
//                       when it is not the first class of its source
//
// A descriptor that is in the pool adds nothing, so only first uses show.
//
// Five of these places are a choice no shipped class can tell from its
// alternative (notes/declarations.md 11); the choice made here is the one of
// the model that reproduced the 2,873 pools: a field type before its
// initialiser; a local type after the last declarator (not after each); and
// "new T[n]" before n, "(int)x" before x, "x instanceof T" after x.
void type_walk(pool::Pool& pool, classes::UnitScope& scope, uint32_t class_node,
               const classes::ClassInfo& info, bool first_in_file);

// A local declaration: the types inside its initialisers first, then its own
// type.
void local_types(pool::Pool& pool, classes::UnitScope& scope, uint32_t s);

void stmt_types(pool::Pool& pool, classes::UnitScope& scope, uint32_t s);

void expr_types(pool::Pool& pool, classes::UnitScope& scope, uint32_t e);

// ---------------------------------------------------------------------------
// step 3: names, and the rows
// ---------------------------------------------------------------------------

// Does a method own a tree with nodes?  A body written ";" gives an empty
// tree, and so does "{}" on a method; a constructor written "{}" still holds
// the implicit super call (notes/declarations.md 4.5).
bool has_code(const parser::Ast& ast, uint32_t m);

// Largest number of declared locals alive at one time inside a statement.  A
// declaration lives to the end of the enclosing block, one in a for header to
// the end of the for statement, one in a switch group to the next label.
uint32_t max_live(const parser::Ast& ast, uint32_t s);

uint32_t max_live_list(const parser::Ast& ast, parser::Items stmts);

// The fifth word of a method row: this (unless static) + parameters + the
// largest number of declared locals alive at one time; 0 when the tree is
// empty.
uint32_t locals_word(const parser::Ast& ast, uint32_t m);

// Step 3 and the rows.  The members are walked once in source order, fields
// and methods mixed: the names are interned, the rows go to the static or the
// instance vector (a stable split), and one counter numbers the trees: a
// field with an initialiser and every method that is not abstract takes the
// next number, also when its tree is empty.
Rows names_and_rows(pool::Pool& pool, const parser::Ast& ast, uint32_t class_node,
                    const classes::ClassInfo& info);

// Steps 1 to 3 for one class of a source: its pool so far and its rows.
ClassBuild declare_class(classes::UnitScope& scope, uint32_t class_node,
                         const classes::ClassInfo& info, bool first_in_file);

}  // namespace decl
}  // namespace slrrc

#endif
