// install.hpp - a game tree on disk, as the compiler's host.  (Reference:
// slrrc/install.py.)
//
// The compiler core reads no file.  What it needs from its surroundings comes
// through two interfaces, and this module implements both for a directory tree
// laid out like an install of the game:
//
//     Install             the tree: its class files, its sources, its packs
//     InstallProvider     classes::ClassProvider: what the classes of the tree
//                         declare
//     DiskPackFiles       packs::PackFiles: which pack files can be opened, and
//                         the external reference table at the head of each
//     InstallPacks        packs::PackNames: the pack table of a session of the
//                         game, replayed over the pack files of the tree (the
//                         reference: install_packs())
//
// PACK NAMES.  How a class file spells the pack of a resource id is state of
// the session that compiles it: the first spelling under which the pack was
// registered in the pack table of the resource engine (packs.hpp).  A command
// line has no session, so InstallPacks replays the start of one: the game
// registers system.rpk, the constructor of GameLogic scans six directories and
// every directory of multibot\maps for packs (packs::session_940), and then
// the sources are compiled, in the order of the run.  A pack in a scanned
// directory is therefore spelled as the directory listing spells its file, any
// other pack as the first source of the run that names it writes it (or as the
// external reference table of a pack loaded earlier does), and a pack without
// a file compiles to system.rpk id 0.  For the shipped sources the order of
// the run changes nothing: every source spells a pack outside the scanned
// directories in the same way.
//
// LAYOUT OF A TREE.  A class file is X/Name.class; its source, when there is
// one, is X/src/Name.java (the directory is spelled src, Src or SRC).  Packs are
// *.rpk files anywhere.  A .java file that does not sit in such a directory has
// no place for its class file and is ignored (Install::stray_sources).
//
// WHICH CLASS ANSWERS FOR A NAME.  The compiler asks for a class by its full
// name (java.game.parts.Part); where the game's own compiler finds such a class
// is not known for the classes outside its class path (notes/compiler_spec.md
// 1.4 item 4).  The provider therefore indexes every class file of the tree by
// the name the class carries inside the file:
//
//   - a class file wins over a source: the classes of a source are known from
//     the source only when no class file defines them (so a compilation sees
//     its neighbours as they were compiled, not as their sources read today);
//   - a name that several class files define keeps the first file in index
//     order (directory, then file name, without regard to case), except that a
//     file named like the class wins over a class that sits inside a file of
//     another name;
//   - a class that only a source declares is declared from that source on first
//     use, without compiling a body; of several sources the first in index
//     order.  While that goes on the name answers with an empty class, so that
//     two sources that name each other cannot recurse (rule 3 of the contract
//     of classes::ClassProvider);
//   - a class file the reader does not accept (damaged, empty, not a class
//     file) defines nothing: the source beside it is treated as a source
//     without a class file, so its classes are still known.  The file is
//     reported (InstallProvider::unreadable) and left alone.
//
// Reading is all this module does to a tree: it never writes, creates, renames
// or deletes anything in it.
//
// PATHS are UTF-8 strings here (std::filesystem converts them at the system
// calls).  Names are ordered as the reference orders them, by code point,
// which is the byte order of UTF-8.
#ifndef SLRRC_TOOL_INSTALL_HPP
#define SLRRC_TOOL_INSTALL_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "classes.hpp"
#include "diag.hpp"
#include "packs.hpp"

namespace slrrc {
namespace install {

inline constexpr char SOURCE_DIR[] = "src";         // compared without regard to case
inline constexpr char SOURCE_EXT[] = ".java";
inline constexpr char CLASS_EXT[] = ".class";
inline constexpr char PACK_EXT[] = ".rpk";
inline constexpr char PACK_MAGIC[] = "RPAK";        // a pack file: "RPAK", u32 0x200, u32 n1, u32 n2,
inline constexpr size_t PACK_HEAD = 16;             // then n1 + n2 records of PACK_RECORD bytes
inline constexpr size_t PACK_RECORD = 0x40;         // {u16, u16, name, u32}: an external reference
inline constexpr size_t PACK_NAME_AT = 4;

// ASCII lower case: paths are compared without regard to case, and the result
// must not depend on a locale.
std::string lower(std::string_view text);

// os.path.abspath: the absolute path, "." and ".." resolved, native
// separators, none at the end.
std::string abspath(std::string_view path);

// os.path.join(dir, path): a path that is absolute stays as it is.
std::string join_path(std::string_view dir, std::string_view path);

// Does path lie in the tree below root (or is it the root)?  Links are
// resolved first.
bool is_inside(std::string_view path, std::string_view root);

bool is_dir(std::string_view path);
bool is_file(std::string_view path);

// The bytes of a file; false and the reason when it cannot be read.
bool read_file(std::string_view path, std::string& data, std::string& why);

// One source of a tree.
struct SourceFile {
    std::string path;           // absolute path of the file
    std::string rel;            // relative to the root, with "/"
    std::string class_dir;      // where its class file belongs: relative, "/"; "" = the root
    std::string name;           // file name without ".java"
    bool has_class = false;     // class_rel is set (Python: class_rel is not None)
    std::string class_rel;      // the class file beside it, relative
    bool class_unreadable = false;  // ... and the reader does not accept that file
    //                                 (known once InstallProvider::load() has run)
    bool external = false;      // a file outside the tree, named on the command line
    bool has_declared = false;  // declared is set (Python: declared is not None)
    std::vector<std::string> declared;  // full names of its classes, once they were asked for

    // Has this source no usable class file beside it: none at all, or one
    // the reader rejects?  (What --only-missing selects.)
    bool lacks_class() const { return !has_class || class_unreadable; }

    // Where the class file of this source belongs, relative to the root of
    // the tree or of a cache directory.
    std::string output_rel() const;
};

// The files of a game tree that matter to the compiler.
class Install {
public:
    explicit Install(std::string_view root_dir, const std::vector<std::string>& exclude_prefixes = {});

    std::string root;
    std::vector<std::string> exclude;
    std::vector<std::string> class_files;       // relative paths with "/", in index order
    std::vector<SourceFile> sources;            // in index order
    std::vector<std::string> pack_files;        // relative paths with "\", as the directory lists them
    std::vector<std::string> stray_sources;     // .java files outside a src directory (ignored)

    bool skipped(std::string_view dir_name) const;

    // Walk the tree once and fill the lists.
    void scan();

    std::string path(std::string_view rel) const;

    bool read(std::string_view rel, std::string& data, std::string& why) const;

    // A SourceFile for a file that is not a source of the tree: its class
    // file has no directory of its own.
    SourceFile external_source(std::string_view path) const;

    // The SourceFile of the tree for a path (absolute, or relative to the
    // current directory or to the root), nullptr when it is not one.
    const SourceFile* find_source(std::string_view path) const;
};

bool read_source(const SourceFile& sf, std::string& data, std::string& why);

// A source that could not be declared for another one, with what it reported.
struct FailedSource {
    std::string rel;
    std::vector<Diagnostic> diagnostics;
};

// A file the provider could not use, and why.
struct Unreadable {
    std::string rel;
    std::string why;
};

// What the classes of a tree declare (the rule is in the head of this file).
//
// load() reads every class file once and the type declarations of the sources
// without a class file the reader accepts.  It marks those sources in the
// Install (SourceFile::class_unreadable, declared), which therefore has to
// outlive the provider and must not change behind it.
//
// A pointer find() returned stays valid as long as the provider lives: a class
// that is known is never replaced.
class InstallProvider : public classes::ClassProvider {
public:
    InstallProvider(Install& tree, bool strict_mode = false);

    Install& install;
    bool strict;
    // full name -> ClassInfo (the reference's `classes`, which is the name of
    // a namespace here)
    std::map<std::string, classes::ClassInfo, std::less<>> known;
    std::map<std::string, std::string, std::less<>> origin;     // full name -> the file it was taken
    //                                                             from (relative)
    std::map<std::string, uint32_t> builds;     // class file -> the build number of its header
    std::vector<Unreadable> unreadable;         // (class file, what does not fit)
    std::vector<Unreadable> unreadable_sources; // (source, why it could not be read)
    std::map<std::string, size_t, std::less<>> pending;     // full name -> the source that declares
    //                                                         it (its place in install.sources)
    std::map<std::string, classes::ClassInfo, std::less<>> in_progress;    // full name -> stub, while
    //                                                                         its source is declared
    std::vector<FailedSource> failed;           // sources that did not declare
    uint32_t class_count = 0;                   // classes read from class files, duplicates included

    void load();

    // The contract of classes::ClassProvider: the class of exactly this name;
    // while a source is being declared, a stub for every name it declares
    // (find() is re-entered through declare()).
    const classes::ClassInfo* find(std::string_view fqn) override;
};

// The names of the external reference table of a pack file.  head: its first
// PACK_HEAD bytes; records: the bytes behind them (as many as the caller could
// read).  Nothing for a file that does not start with "RPAK".
std::vector<std::string> external_names(std::string_view head, std::string_view records);

// The pack files of a tree on disk.  listing: its *.rpk files as relative
// paths in the spelling of the directory listing (Install::pack_files).  A
// pack "can be opened" when the listing has it; its table is read from the
// head of the file, once.
class DiskPackFiles : public packs::PackFiles {
public:
    DiskPackFiles(std::string_view root_dir, const std::vector<std::string>& listing);

    bool externals(std::string_view name, std::vector<std::string>& names) override;

private:
    std::string root_;
    std::map<std::string, std::string, std::less<>> listed_;    // packs::file_key -> the path as listed
    std::map<std::string, std::vector<std::string>, std::less<>> tables_;  // packs::file_key -> the
    //                                                                        external reference names
};

// The PackNames of a tree (the reference: install_packs()): the pack table of
// a session of the game as it stands when the scripts are compiled, replayed
// over the pack files of the tree (packs::session_940: system.rpk, then the
// pack scans of the constructor of GameLogic).  The table has state: one
// object is the table of one run, and the sources of the run are compiled
// against it in turn.
class InstallPacks {
public:
    explicit InstallPacks(const Install& install);
    InstallPacks(const InstallPacks&) = delete;
    InstallPacks& operator=(const InstallPacks&) = delete;

    DiskPackFiles files;            // declared before the table, which refers to it
    packs::PackTable table;
};

}  // namespace install
}  // namespace slrrc

#endif
