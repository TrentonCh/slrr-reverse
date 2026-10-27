// compilation.hpp - the stages of one compilation, for the tool and the tests.
// (Reference: slrrc/__init__.py: FrontResult, front(), emit_trees(); the
// functions are in slrrc.cpp.)
//
// The compiler is compile(): source in, class file bytes out (slrrc.hpp).  Its
// stages are public here as they are in the reference, so that a module of the
// port is compared with the same module of the reference:
//
//     front()        takes a source to the point where every class has its
//                    declarations, its rows and the first three steps of its
//                    pool
//     emit_trees()   adds the trees, node for node, with operands that name
//                    pool content
//
// Unlike slrrc.hpp this header shows the tree and so includes parser.hpp: it
// cannot stand behind <windows.h>.
//
// WHAT A RESULT HOLDS IS COPIED INTO IT.  While a compilation runs, one
// Diagnostics object and one UnitScope collect the diagnostics, the classes
// found through the provider (consulted) and the names asked for in vain
// (missed); every stage appends to them, the emitter most of all (for 3,449 of
// the 3,454 shipped sources it consults classes the declarations did not).  A
// result gets its three lists from finish() (slrrc.cpp), once, at the end of
// the stage that hands the result out: that is the one place where they are
// copied or moved, and no result shares a list with the scope.
#ifndef SLRRC_COMPILATION_HPP
#define SLRRC_COMPILATION_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "classes.hpp"
#include "decl.hpp"
#include "diag.hpp"
#include "packs.hpp"
#include "parser.hpp"
#include "slrrc.hpp"

namespace slrrc {

// What front() and emit_trees() fill.
//
// name: the file name given.  ok: no error.  diagnostics: in the order they
// were found.  has_ast: the scanner reported no error and the parser ran
// (the reference: ast is not None); ast.root is NONE when the source did not
// parse.  scope: the UnitScope (null before the declarations).  classes: one
// ClassBuild per class of the source, in source order; behind emit_trees()
// each carries its trees, and there is none when the emitter reported an
// error.  consulted: full names of the classes found through the provider.
// missed: full names asked of the provider and not found.
//
// diagnostics, consulted and missed are lists of this result alone, filled
// when the stage that returns it has ended.
//
// The object also holds what the reference keeps alive by reference: `diags`,
// the Diagnostics the stages append to (the scope refers to it).  The scope
// refers to `ast` and to the provider, and the tree to the text of the
// source: a FrontResult is neither copied nor moved, and the source text and
// the provider have to outlive it.
struct FrontResult {
    FrontResult() = default;
    FrontResult(const FrontResult&) = delete;
    FrontResult& operator=(const FrontResult&) = delete;

    std::string name;
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    bool has_ast = false;
    parser::Ast ast;
    Diagnostics diags;                              // declared before the scope, which refers to it
    std::unique_ptr<classes::UnitScope> scope;
    std::vector<decl::ClassBuild> classes;
    std::vector<std::string> consulted;
    std::vector<std::string> missed;
};

// The front half of a compilation: scan, parse, declare the classes of the
// source, and for each class the pool steps 1 to 3 and its rows.
//
// source: the bytes of the file.  name: the file name, for messages.
// provider: the classes the compiler can see besides the ones of the source.
// res: a FrontResult nothing has been done with.
void front(std::string_view source, std::string_view name, classes::ClassProvider& provider,
           const Options& options, FrontResult& res);

// front(), then the emitter: every ClassBuild of the result carries its trees
// (ClassBuild::trees, one emit::Tree per tree owner of its rows, in tree
// order).  The operands of the nodes are symbolic: they name pool content, not
// pool indices.
//
// pack_names: the pack names of the host; the compilation asks it once per
// pack (packs::CachedPackNames).  options.float_mode selects the float literal
// conversion (a value that is none of the four: slrrc.hpp, Options).
// res: a FrontResult nothing has been done with.
void emit_trees(std::string_view source, std::string_view name, classes::ClassProvider& provider,
                packs::PackNames& pack_names, const Options& options, FrontResult& res);

}  // namespace slrrc

#endif
