// packs.hpp - the pack of a resource id: the name and the id the pool gets.
// (Reference: slrrc/packs.py.)
//
// A resource id literal  cars.racers.duhen:0x0000002Ar  becomes the pool entry
// {utf8 pack file name, local id}.  The file name is the written name with the
// dots turned into backslashes and ".rpk" appended, but not always in the
// letter case of the source: the pool of a shipped class holds
// "cars\racers\Duhen.rpk".
//
// WHERE THE GAME TAKES THE NAME FROM was read in the Build 940 exe
// (ref/notes/exe_reads.md 2).  The compiler does not choose the spelling.  It
// builds the written path (pack_path), asks the resource engine to resolve
// (path, id) to a global id, and writes into the pool the name of the engine's
// record of that pack and the low 16 bits of the global id.  The engine keeps
// one record per pack.  A lookup by name is find-or-add: the first record whose
// name is equal without regard to the case of A to Z (nothing else is folded,
// "/" and "\" are different characters), else a new record that keeps the name
// as given, cut to 63 characters.  So the pool spelling of a pack is the
// spelling of ITS FIRST REGISTRATION IN THAT SESSION, by whichever of these
// came first:
//
//     the start of the game     "system.rpk", record 0
//     System.openLib(name)      the string of the script, unchanged.  This is
//                               where System.rpkScan(dir) puts its names: it is
//                               a script method that calls openLib(dir + name)
//                               for every *.rpk the directory listing has, so
//                               the directory is spelled as the script spells
//                               it and the file as the listing does
//     a pack that is loaded     every name of its external reference table, as
//                               the pack file spells it
//     a resource id node the    the pool spelling of that class file
//       VM executes
//     a data file opened        the pack names stored in it
//       for reading
//     the compiler              the written path
//
// A pack whose file cannot be opened resolves to global id 0, and global id 0
// is record 0 with local id 0: "system.rpk" id 0.  The same answer comes for an
// id whose low 16 bits are 0.  A high word k that is not 0 selects the k-th
// external reference of the pack: the pool then names that other pack.  (Both
// id cases were read, not seen: every one of the 9,952 literals of the shipped
// sources has a low word that is not 0 and a high word 0.)
//
// THE INTERFACE.  The compiler asks its host through PackNames; resolve() is
// the rule on top of its two questions.  The answer is state of a session, not
// a function of the install, so a host answers from the table of the packs it
// has registered:
//
//     PackTable table(files);                     files: a PackFiles of the host
//     table.open_lib("cars\\racers\\Duhen.rpk");  what System.openLib does
//     resolve(table, "cars\\racers\\duhen.rpk", 0x2A)
//                                                 {"cars\\racers\\Duhen.rpk", 0x2A, PackWhy::Ok}
//
// PackTable is that table: a host creates one at start, feeds it at the events
// of the list above, in the order they happen, and hands it to every
// compilation.  It must not register its own internal opens: a name with "/"
// would be another record than the same name with "\".
//
// WITHOUT A SESSION (the command line, the tests) the table is replayed:
// session_940() registers "system.rpk" and then runs the rpkScan calls of the
// constructor of GameLogic (sl\Scripts\game\src\GameLogic.java, lines 345 to
// 360) over a listing of the pack files.  Over the Build 940 install this
// reproduces every one of the 126 pack spellings of the shipped pools and every
// one of the 9,058 resource id nodes of the comparable classes; without the
// scans 7 or 3 packs come out in the lower case of their sources, depending on
// the order of compilation.
//
// Nothing here reads a directory or a file: the host passes the listing and
// answers for the files through PackFiles (tool/install.hpp does both for a
// game tree on disk).
//
// NAMES THAT DIFFER FROM THE REFERENCE.  The constants R_OK ... R_UNKNOWN are
// the values of PackWhy (R_OK is a macro of <unistd.h>), and NAME_MAX is
// RECORD_NAME_MAX (a macro of <limits.h> on POSIX systems).  A name that the
// reference answers as "a text or None" is a bool and a text here.
//
// This header can stand behind <windows.h>.
#ifndef SLRRC_PACKS_HPP
#define SLRRC_PACKS_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slrrc {
namespace packs {

inline constexpr char SYSTEM_PACK[] = "system.rpk";     // record 0 of the engine's table
inline constexpr char NO_FILE_PACK[] = "system.rpk";    // what a pack without a file compiles to,
inline constexpr uint32_t NO_FILE_ID = 0;               // with this local id
inline constexpr char PACK_EXT[] = ".rpk";
inline constexpr size_t RECORD_NAME_MAX = 63;           // characters of a name a record keeps
inline constexpr uint32_t NONE = 0xFFFFFFFF;            // "no record"

// why resolve() answered as it did (the reference: R_OK ... R_UNKNOWN)
enum class PackWhy : uint8_t {
    Ok = 0,             // the name of the pack's record, the low word of the id
    NoFile = 1,         // the file of the pack cannot be opened: system.rpk id 0
    Zero = 2,           // the low word of the id is 0: system.rpk id 0
    External = 3,       // high word k: the k-th external reference of the pack
    NoExternal = 4,     // high word k, and the pack has no k-th reference: system.rpk id 0
    Fault = 5,          // ... its k-th reference is a pack without a file: the
    //                     compiler of the game reads through a null record here
    Unknown = 6         // high word k, and the PackNames cannot say
};

// who registered a record first (PackRecord::origin)
inline constexpr uint32_t O_BOOT = 0;           // the start of the game
inline constexpr uint32_t O_OPEN = 1;           // System.openLib, and so System.rpkScan
inline constexpr uint32_t O_EXTERNAL = 2;       // the external reference table of a pack that was loaded
inline constexpr uint32_t O_LITERAL = 3;        // the compiler: a resource id literal
inline constexpr uint32_t O_NODE = 4;           // a resource id node the VM executed
inline constexpr uint32_t O_DATA = 5;           // a data file opened for reading

// ORIGIN_NAMES[origin]: "boot", "openLib", "external table", "literal",
// "node", "data file"; "?" for any other number.
const char* origin_name(uint32_t origin);

// The pack of a resource id, as the session knows it.
//
// THE CONTRACT.  canonical() has two answers and they are not alike:
//
//   true, a name   the spelling for the pool: the name the pack was first
//                  registered with in this session.  A host that cannot tell
//                  whether the pack exists answers with a name too: the
//                  spelling it knows, or written_path itself.
//   false          the INSTALL HAS NO FILE for this pack.  The compiler then
//                  writes system.rpk id 0 in place of the resource id, the one
//                  form a shipped class shows for such a pack, and warns
//                  (W_PACK) at the literal: the class will name another
//                  resource than its source does.
//
// So false is a statement about the files of the install, and has to be
// answered from them (the pack file is opened, or looked up in a listing of
// the *.rpk files), not from the packs the host happens to have opened: a
// pack that is not open yet, asked for while a script compiles, would turn
// every resource id of that pack into system.rpk:0x00000000.  The name, on the
// other hand, comes from the session: it is the first spelling under which the
// pack was registered, and asking registers the written path when nothing did
// before (PackTable).  The answer for a path does not change while a
// compilation runs; the compiler asks once per pack (CachedPackNames).
//
// external() is asked only for a literal whose id has a high word, which no
// shipped source has.
//
// The struct itself answers every pack as it is written and knows no external
// references: the PackNames of a host that has no session.
struct PackNames {
    // written_path: the pack as the source names it, in path form (pack_path).
    // Returns true and the spelling for the pool in file_name, or false when
    // the install has no file for the pack (never for "not known").
    virtual bool canonical(std::string_view written_path, std::string& file_name);

    // The k-th external reference of the pack (k from 1): External and the
    // name of that other pack as the session spells it; or NoExternal (no
    // name), Fault (the name), Unknown (no name) with the meanings of PackWhy.
    // Asked only after canonical() gave a name for the pack.  A PackNames that
    // knows no external reference tables answers Unknown, and the literal is
    // then an error.
    virtual PackWhy external(std::string_view written_path, uint32_t k, std::string& file_name);

    virtual ~PackNames() = default;
};

// What the pool gets for a literal: the pack file name, the local id, and why.
struct Answer {
    std::string name;
    uint32_t id;
    PackWhy why;
};

// What the pool gets for the literal  written_path : number.  The rule of the
// exe (resolve 0x5189E0, the id mapping 0x51E860, the pool entry 0x54B170) on
// top of the two questions of a PackNames:
//
//     the file of the pack cannot be opened         system.rpk, 0
//     the low 16 bits of the id are 0               system.rpk, 0
//     the high 16 bits are 0                        the pack's name, the low word
//     the high word k names an external reference   that pack's name, the low word
//     ... and names none                            system.rpk, 0
Answer resolve(PackNames& names, std::string_view written_path, uint32_t number);

// A PackNames that asks another one once per written path: what one
// compilation puts between the emitter and the PackNames of its host.
class CachedPackNames : public PackNames {
public:
    explicit CachedPackNames(PackNames& inner_names);

    bool canonical(std::string_view written_path, std::string& file_name) override;
    PackWhy external(std::string_view written_path, uint32_t k, std::string& file_name) override;

private:
    struct Name {
        bool found;
        std::string name;
    };
    struct External {
        PackWhy why;
        std::string name;
    };

    PackNames& inner;
    std::map<std::string, Name, std::less<>> answers;               // written path -> the answer
    std::map<std::pair<std::string, uint32_t>, External> externals; // (written path, k) -> the answer
};

// "cars.racers.Axis" -> "cars\racers\Axis.rpk": the path the parser of the
// game builds from a written pack name (0x55C1D0: the parts joined with a
// backslash, ".rpk" appended), in the letter case of the source.
std::string pack_path(std::string_view written);

// ASCII lower case: A to Z folded, nothing else (the _stricmp of the exe; pack
// names are compared this way).
std::string lower(std::string_view text);

// How a pack name is matched with a file: "/" is read as "\" and the letter
// case does not count (the open of the exe, 0x565200, ends in CreateFileA).
// Not how records are compared: see PackTable::find.
std::string file_key(std::string_view name);

// ---------------------------------------------------------------------------
// the files, as the host answers for them
// ---------------------------------------------------------------------------

// The pack files of the host, as the resource engine sees them.
struct PackFiles {
    // name: a pack name as a record carries it, a path relative to the
    // install.  Returns false when that file cannot be opened; else true and,
    // in names, the names of its external reference table in file order (none
    // when it has no table): header "RPAK", u32 0x200, u32 n1, u32 n2, then
    // n1 + n2 records of 0x40 bytes {u16, u16, name, u32}.
    virtual bool externals(std::string_view name, std::vector<std::string>& names);

    virtual ~PackFiles() = default;
};

// The external reference table of one listed file: the path as listed, and
// the names.
typedef std::pair<std::string, std::vector<std::string>> ListedTable;

// The PackFiles of a host that has a list of its pack files and no more.
// files: relative paths in the spelling of the directory listing.  tables:
// optionally, per path as listed, its external reference names; a file
// without an entry has no external references (and an entry makes a file).
class ListedPackFiles : public PackFiles {
public:
    explicit ListedPackFiles(const std::vector<std::string>& files,
                             const std::vector<ListedTable>& tables = {});

    bool externals(std::string_view name, std::vector<std::string>& names) override;

private:
    std::map<std::string, std::vector<std::string>, std::less<>> tables_;   // file_key -> the names
};

// ---------------------------------------------------------------------------
// the pack table of the resource engine
// ---------------------------------------------------------------------------

// One pack of the table.
struct PackRecord {
    std::string name;               // as first registered; never rewritten
    uint32_t origin;                // an O_ constant: who registered it
    bool loaded = false;            // its file was opened and its table read
    std::vector<uint32_t> ext;      // once loaded: record numbers, [own, reference 1, ...]
};

// The table of packs of one session (the exe: records of 0x70 bytes in the
// resource engine at *0x5DB008).  Record 0 is "system.rpk".
//
// What the host does with it:
//     open_lib(name)                  System.openLib(name)
//     find_or_add(name, O_NODE)       a resource id node is executed (then load)
//     find_or_add(name, O_DATA)       a data file names a pack
// and the compiler asks canonical() and external() through resolve().
//
// Not modelled: the special record 0xFFFF (it would take 65,535 packs; the
// record numbers of a global id are 16 bits), and a pack file that does not
// start with "RPAK" (the exe reads it unchecked; here the PackFiles decides
// what its table is).
//
// The table refers to the PackFiles it was made with, which has to outlive it.
// It can be moved (session_940 returns one) and is not copied.
class PackTable : public PackNames {
public:
    explicit PackTable(PackFiles& pack_files);
    PackTable(PackTable&&) = default;
    PackTable(const PackTable&) = delete;
    PackTable& operator=(const PackTable&) = delete;

    PackFiles& files;
    std::vector<PackRecord> records;                        // in the order of registration
    std::map<std::string, uint32_t, std::less<>> index;     // lower-case name -> the first record
    //                                                         that has it

    // The number of the first record whose name equals `name` without regard
    // to the case of A to Z; NONE when there is none.
    uint32_t find(std::string_view name) const;

    // 0x518940: the record of that name, or a new one that keeps the name as
    // given, cut to RECORD_NAME_MAX characters.  (A longer name is never found
    // again: the comparison is with the whole name.)
    uint32_t find_or_add(std::string_view name, uint32_t origin);

    // 0x51E310: open the pack of record i on first use and register the names
    // of its external reference table (they are not loaded).  False when the
    // file cannot be opened; the record stays and is tried again the next
    // time.  i is the number of a record.
    bool load(uint32_t i);

    // System.openLib(name) (0x4E0470): find-or-add with the string as the
    // script spells it, then load.  Returns the record number.
    uint32_t open_lib(std::string_view name);

    // The name of the record a lookup of `lookup` finds; nullptr when no
    // record has that name.  Registers nothing.
    const std::string* name_of(std::string_view lookup) const;

    // -- the questions of the compiler ---------------------------------------

    bool canonical(std::string_view written_path, std::string& file_name) override;

    // (Besides the answers of PackNames::external: NoFile when the pack itself
    // cannot be opened.)
    PackWhy external(std::string_view written_path, uint32_t k, std::string& file_name) override;

    // -- the same as the exe has it, for the tests ---------------------------

    // 0x5189E0 with 0x51E860: the global id of (path, number); 0 for a pack
    // without a file, a low word 0, a high word that names nothing.
    uint32_t global_id(std::string_view path, uint32_t number);

    // 0x54B170: the name and the local id of the pool entry for a global id;
    // false where the exe reads through a null record (the record of the id
    // is a pack without a file).  Record 0 is taken to be there.  (False also
    // for an id whose record does not exist: global_id() makes no such id, and
    // the reference does not expect one.)
    bool pool_entry(uint32_t gid, std::string& name, uint32_t& id);
};

// ---------------------------------------------------------------------------
// a session without the game: boot and the scans of GameLogic
// ---------------------------------------------------------------------------

// System.rpkScan(dir) calls of the constructor of GameLogic, in source order
// (sl\Scripts\game\src\GameLogic.java lines 345 to 350), as the script spells
// the directories ...
inline constexpr const char* SCANS_940[] = {
    "parts\\engines\\",
    "parts\\",
    "cars\\racers\\",
    "cars\\fake_racers\\",
    "maps\\",
    "sl\\Scripts\\game\\CareerEvents\\",
};
// ... and the loop behind them (lines 352 to 360): rpkScan(path + D + "\\")
// for every directory D of path, as the listing spells D.
inline constexpr const char* SUB_SCANS_940[] = {
    "multibot\\maps\\",
};
// Names this replay in a cache manifest: an output with a resource id depends
// on it.  Change it with every change of the two lists or of the table.
inline constexpr char SESSION_940_VERSION[] = "940-pack-table-1";

// The *.rpk files that lie directly in `directory`, as the listing spells
// them, in the order of their lower-case names.  listing: the pack files of
// the host as relative paths in the spelling of the directory listing;
// directory: "parts\\engines\\" (its letter case does not count, as for
// FindFirstFile).
std::vector<std::string> listed_packs(const std::vector<std::string>& listing,
                                      std::string_view directory);

// The directories directly below `directory` that hold a pack file somewhere,
// as the listing spells them, in the order of their lower-case names.  (A
// directory without a pack file registers nothing when it is scanned, so it
// need not be listed.)
std::vector<std::string> listed_directories(const std::vector<std::string>& listing,
                                            std::string_view directory);

// System.rpkScan(directory), a script method (System.class): for every name
// FindFile lists for directory + "*.rpk", System.openLib(directory + name).
// The directory is spelled as the caller spells it, the file as the listing
// does.  Returns the number of files.
uint32_t rpk_scan(PackTable& table, const std::vector<std::string>& listing,
                  std::string_view directory);

// The pack table as it stands when the scripts of a Build 940 session are
// compiled: "system.rpk", then the scans of the constructor of GameLogic.
// files: a PackFiles (it has to outlive the table); listing: the pack files as
// relative paths in the spelling of the directory listing.  scans false leaves
// the scans out (a control of the tests: the spellings then depend on the
// order in which the sources are compiled).
PackTable session_940(PackFiles& files, const std::vector<std::string>& listing, bool scans = true);

}  // namespace packs
}  // namespace slrrc

#endif
