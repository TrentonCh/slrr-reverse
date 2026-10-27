// slrrc.hpp - the public interface of the compiler for the script dialect of
// Street Legal Racing: Redline (Build 940).  (Reference: slrrc/__init__.py.)
//
// The output is meant to be byte-identical to the class files the game's own
// compiler wrote; ref/notes/compiler_spec.md is the plan.  The three entry
// points a host needs:
//
//     Result compile(source, file_name, ClassProvider&, PackNames&, const Options&);
//     DeclareResult declare(source, file_name, ClassProvider&, const Options&);
//     std::vector<std::string> declared_names(source);
//
// compile() is compile_source() of the reference.  declare() is
// declare_source() and declared_names() is the function of that name: a
// provider that has to answer for a class that exists as source only needs
// both (classes::ClassProvider, rule 3).
//
// WHAT IS BUILT.  The port is made module by module.  Here so far: declare()
// and declared_names(), and the two things a compilation needs besides its
// source and the classes: Options::float_mode (floatlit.hpp) and the PackNames
// of the host (packs.hpp).  compile() and Result come with the writer
// (notes/port_3-emitter.md); until then a compilation goes as far as its
// trees, through emit_trees() of compilation.hpp.
//
// Two things no source holds, and both were read in the Build 940 exe
// (ref/notes/exe_reads.md).  The float literal conversion: the exe has one,
// setting B of floatlit, and that is the default of Options::float_mode; A, C
// and D reproduce shipped class files that earlier builds of the compiler
// wrote.  The pack names: the pool spells a pack as it was first registered in
// the pack table of the session, so the PackNames a host passes in is that
// table (packs::PackTable), and a compilation is not a function of its source
// and the install alone.
//
// BEFORE THE FIRST COMPILATION a host calls floatlit::self_test() once, on the
// thread that compiles, and writes no class file when it reports a row: the
// bits of a float literal depend on how this library was built and on the
// state of the floating point unit (floatlit.hpp).
//
// The core does no file access and keeps no global state: the source comes in
// as bytes, other classes through a ClassProvider, pack names through a
// PackNames.  Bad input never throws: the result carries diagnostics with
// line, column and code, and ok is false.  (When memory runs out the standard
// library throws std::bad_alloc; nothing in the core catches that.)
//
// This header can stand behind <windows.h>: it includes neither parser.hpp
// (the node kind THIS is a macro there) nor a header that does.
#ifndef SLRRC_SLRRC_HPP
#define SLRRC_SLRRC_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "classes.hpp"
#include "diag.hpp"
#include "floatlit.hpp"
#include "packs.hpp"

namespace slrrc {

inline constexpr char VERSION[] = "0.5";            // part of every cache entry: a change of the
//                                                     output of any source needs a new one
inline constexpr uint32_t BUILD_940 = 0x24F9D;

// The float literal conversion: FloatMode::B (the default) is the one the
// Build 940 exe contains and the one for every new compilation; A, C and D
// exist to reproduce shipped class files of earlier builds (floatlit.hpp,
// ref/notes/exe_reads.md 1).
typedef floatlit::Mode FloatMode;

// build: the build number written into the file header.
// float_mode: the float literal conversion.  A value that is none of the four
// (an enum can be made of any number) is a mistake of the caller: the
// compilation then ends with one error, E_UNSUPPORTED at line 1, column 1,
// where the reference raises ValueError.
// strict: constructs no shipped source uses are errors instead of warnings.
struct Options {
    uint32_t build = BUILD_940;
    FloatMode float_mode = floatlit::DEFAULT_MODE;
    bool strict = false;
};

// ok, diagnostics, and infos: one ClassInfo per class of the source;
// consulted: full names of the classes found through the provider, in the
// order of their first use; missed: full names asked of the provider and not
// found.
struct DeclareResult {
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::vector<classes::ClassInfo> infos;
    std::vector<std::string> consulted;
    std::vector<std::string> missed;
};

// The full names of the classes a source declares, in source order, from its
// package line and its type declarations alone (no class is looked up);
// nothing for a source that does not parse.  For a host that has to know
// which source to ask for a class.
std::vector<std::string> declared_names(std::string_view source);

// What the classes of a source declare, without compiling a body: for a
// provider that has to answer for a class that exists as source only.
//
// The provider passed in is asked for the superclass and for the types of
// the members, so a provider that calls this from its own find() is
// re-entered; classes::ClassProvider (rule 3) says what it has to answer
// meanwhile.
//
// source: the bytes of the file.  file_name: for messages.
// What compile() returns (the reference's Result).  ok: no error, and
// class_file holds the output: the blobs of all classes of the source in
// source order (empty when not ok).  class_names: the full name of each
// class, one per blob.  consulted / missed: as in DeclareResult, for the
// whole compilation (the emitter included).
struct Result {
    bool ok = false;
    std::vector<uint8_t> class_file;
    std::vector<std::string> class_names;
    std::vector<std::string> consulted;
    std::vector<std::string> missed;
    std::vector<Diagnostic> diagnostics;
};

// Compile one source file to the bytes of its class file (the reference's
// compile_source): never throws for bad input, writes nothing anywhere.
Result compile(std::string_view source, std::string_view file_name,
               classes::ClassProvider& provider, packs::PackNames& packs,
               const Options& options = Options());

DeclareResult declare(std::string_view source, std::string_view file_name,
                      classes::ClassProvider& provider, const Options& options = Options());

}  // namespace slrrc

#endif
