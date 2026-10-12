"""packs.py - the pack of a resource id: the name and the id the pool gets.

A resource id literal  cars.racers.duhen:0x0000002Ar  becomes the pool entry
{utf8 pack file name, local id}.  The file name is the written name with the
dots turned into backslashes and ".rpk" appended, but not always in the
letter case of the source: the pool of a shipped class holds
"cars\\racers\\Duhen.rpk".

WHERE THE GAME TAKES THE NAME FROM was read in the Build 940 exe
(notes/exe_reads.md 2).  The compiler does not choose the spelling.  It
builds the written path (pack_path), asks the resource engine to resolve
(path, id) to a global id, and writes into the pool the name of the engine's
record of that pack and the low 16 bits of the global id.  The engine keeps
one record per pack.  A lookup by name is find-or-add: the first record whose
name is equal without regard to the case of A to Z (nothing else is folded,
"/" and "\\" are different characters), else a new record that keeps the name
as given, cut to 63 characters.  So the pool spelling of a pack is the
spelling of ITS FIRST REGISTRATION IN THAT SESSION, by whichever of these
came first:

    the start of the game     "system.rpk", record 0
    System.openLib(name)      the string of the script, unchanged.  This is
                              where System.rpkScan(dir) puts its names: it is
                              a script method that calls openLib(dir + name)
                              for every *.rpk the directory listing has, so
                              the directory is spelled as the script spells
                              it and the file as the listing does
    a pack that is loaded     every name of its external reference table, as
                              the pack file spells it
    a resource id node the    the pool spelling of that class file
      VM executes
    a data file opened        the pack names stored in it
      for reading
    the compiler              the written path

A pack whose file cannot be opened resolves to global id 0, and global id 0
is record 0 with local id 0: "system.rpk" id 0.  The same answer comes for an
id whose low 16 bits are 0.  A high word k that is not 0 selects the k-th
external reference of the pack: the pool then names that other pack.  (Both
id cases were read, not seen: every one of the 9,952 literals of the shipped
sources has a low word that is not 0 and a high word 0.)

THE INTERFACE.  The compiler asks its host through PackNames; resolve() is
the rule on top of its two questions.  The answer is state of a session, not
a function of the install, so a host answers from the table of the packs it
has registered:

    table = PackTable(files)                  # files: a PackFiles of the host
    table.open_lib('cars\\racers\\Duhen.rpk') # what System.openLib does
    resolve(table, 'cars\\racers\\duhen.rpk', 0x2A)
                                              # ('cars\\racers\\Duhen.rpk', 0x2A, R_OK)

PackTable is that table: a host creates one at start, feeds it at the events
of the list above, in the order they happen, and hands it to every
compilation.  It must not register its own internal opens: a name with "/"
would be another record than the same name with "\\".

WITHOUT A SESSION (the command line, the tests) the table is replayed:
session_940() registers "system.rpk" and then runs the rpkScan calls of the
constructor of GameLogic (sl\\Scripts\\game\\src\\GameLogic.java, lines 345 to
360) over a listing of the pack files.  Over the Build 940 install this
reproduces every one of the 126 pack spellings of the shipped pools and every
one of the 9,058 resource id nodes of the comparable classes
(tests/t4_trees.py, tests/t9_exe.py); without the scans 7 or 3 packs come out
in the lower case of their sources, depending on the order of compilation.
Which session really compiled the shipped classes is not known; only the
table it left can be read off the pools.

Nothing here reads a directory or a file: the host passes the listing and
answers for the files through PackFiles (install.py does both for a game
tree on disk).

C++ (script_compiler.hpp):
    struct PackNames {
      virtual bool canonical(std::string_view written_path, std::string* file_name) = 0;
      virtual PackWhy external(std::string_view written_path, uint32_t k,
                               std::string* file_name) { return PackWhy::Unknown; }
    };
false is the None of the reference.  PackTable is a std::vector of records
and an unordered_map from the lower-case name to the first record.
"""

SYSTEM_PACK = 'system.rpk'      # record 0 of the engine's table
NO_FILE_PACK = SYSTEM_PACK      # what a pack without a file compiles to,
NO_FILE_ID = 0                  # with this local id
PACK_EXT = '.rpk'
NAME_MAX = 63                   # characters of a name a record keeps
NONE = 0xFFFFFFFF               # "no record"

# why resolve() answered as it did
R_OK = 0                # the name of the pack's record, the low word of the id
R_NO_FILE = 1           # the file of the pack cannot be opened: system.rpk id 0
R_ZERO = 2              # the low word of the id is 0: system.rpk id 0
R_EXTERNAL = 3          # high word k: the k-th external reference of the pack
R_NO_EXTERNAL = 4       # high word k, and the pack has no k-th reference: system.rpk id 0
R_FAULT = 5             # ... its k-th reference is a pack without a file: the
#                         compiler of the game reads through a null record here
R_UNKNOWN = 6           # high word k, and the PackNames cannot say

# who registered a record first (PackRecord.origin)
O_BOOT = 0              # the start of the game
O_OPEN = 1              # System.openLib, and so System.rpkScan
O_EXTERNAL = 2          # the external reference table of a pack that was loaded
O_LITERAL = 3           # the compiler: a resource id literal
O_NODE = 4              # a resource id node the VM executed
O_DATA = 5              # a data file opened for reading
ORIGIN_NAMES = ('boot', 'openLib', 'external table', 'literal', 'node', 'data file')


class PackNames(object):
    """The pack of a resource id, as the session knows it.

    THE CONTRACT.  canonical() has two answers and they are not alike:

      a name    the spelling for the pool: the name the pack was first
                registered with in this session.  A host that cannot tell
                whether the pack exists answers with a name too: the
                spelling it knows, or written_path itself.
      None      the INSTALL HAS NO FILE for this pack.  The compiler then
                writes system.rpk id 0 in place of the resource id, the one
                form a shipped class shows for such a pack, and warns
                (W_PACK) at the literal: the class will name another
                resource than its source does.

    So None is a statement about the files of the install, and has to be
    answered from them (the pack file is opened, or looked up in a listing of
    the *.rpk files), not from the packs the host happens to have opened: a
    pack that is not open yet, asked for while a script compiles, would turn
    every resource id of that pack into system.rpk:0x00000000.  The name, on
    the other hand, comes from the session: it is the first spelling under
    which the pack was registered, and asking registers the written path when
    nothing did before (PackTable).  The answer for a path does not change
    while a compilation runs; the compiler asks once per pack
    (CachedPackNames).

    external() is asked only for a literal whose id has a high word, which no
    shipped source has."""

    def canonical(self, written_path):
        """written_path: the pack as the source names it, in path form
        (pack_path).  Returns the spelling for the pool, or None when the
        install has no file for the pack (never for "not known")."""
        return written_path

    def external(self, written_path, k):
        """The k-th external reference of the pack (k from 1): the pair
        (R_EXTERNAL, name of that other pack as the session spells it), or
        (R_NO_EXTERNAL, ''), (R_FAULT, name), (R_UNKNOWN, '') with the
        meanings of the R_ constants.  Asked only after canonical() gave a
        name for the pack.  A PackNames that knows no external reference
        tables answers R_UNKNOWN, and the literal is then an error."""
        return (R_UNKNOWN, '')


def resolve(names, written_path, number):
    """What the pool gets for the literal  written_path : number  -
    (pack file name, local id, why).  The rule of the exe (resolve
    0x5189E0, the id mapping 0x51E860, the pool entry 0x54B170) on top of
    the two questions of a PackNames:

        the file of the pack cannot be opened         system.rpk, 0
        the low 16 bits of the id are 0               system.rpk, 0
        the high 16 bits are 0                        the pack's name, the low word
        the high word k names an external reference   that pack's name, the low word
        ... and names none                            system.rpk, 0"""
    name = names.canonical(written_path)
    if name is None:
        return (NO_FILE_PACK, NO_FILE_ID, R_NO_FILE)
    lo = number & 0xFFFF
    hi = (number >> 16) & 0xFFFF
    if lo == 0:
        return (NO_FILE_PACK, NO_FILE_ID, R_ZERO)
    if hi == 0:
        return (name, lo, R_OK)
    answer = names.external(written_path, hi)
    if answer[0] == R_EXTERNAL:
        return (answer[1], lo, R_EXTERNAL)
    return (NO_FILE_PACK, NO_FILE_ID, answer[0])


class CachedPackNames(PackNames):
    """A PackNames that asks another one once per written path: what one
    compilation puts between the emitter and the PackNames of its host."""

    def __init__(self, inner):
        self.inner = inner
        self.answers = {}           # written path -> the answer (a name or None)
        self.externals = {}         # (written path, k) -> the answer of external()

    def canonical(self, written_path):
        if written_path in self.answers:
            return self.answers[written_path]
        name = self.inner.canonical(written_path)
        self.answers[written_path] = name
        return name

    def external(self, written_path, k):
        key = (written_path, k)
        if key in self.externals:
            return self.externals[key]
        answer = self.inner.external(written_path, k)
        self.externals[key] = answer
        return answer


def pack_path(written):
    """'cars.racers.Axis' -> 'cars\\racers\\Axis.rpk': the path the parser of
    the game builds from a written pack name (0x55C1D0: the parts joined
    with a backslash, ".rpk" appended), in the letter case of the source."""
    return written.replace('.', '\\') + PACK_EXT


def lower(text):
    """ASCII lower case: A to Z folded, nothing else (the _stricmp of the
    exe; pack names are compared this way)."""
    out = []
    for ch in text:
        if 'A' <= ch <= 'Z':
            out.append(chr(ord(ch) + 32))
        else:
            out.append(ch)
    return ''.join(out)


def file_key(name):
    """How a pack name is matched with a file: "/" is read as "\\" and the
    letter case does not count (the open of the exe, 0x565200, ends in
    CreateFileA).  Not how records are compared: see PackTable.find."""
    return lower(name.replace('/', '\\'))


# ---------------------------------------------------------------------------
# the files, as the host answers for them
# ---------------------------------------------------------------------------

class PackFiles(object):
    """The pack files of the host, as the resource engine sees them."""

    def externals(self, name):
        """name: a pack name as a record carries it, a path relative to the
        install.  Returns None when that file cannot be opened; else the
        names of its external reference table in file order ([] when it has
        none): header "RPAK", u32 0x200, u32 n1, u32 n2, then n1 + n2 records
        of 0x40 bytes {u16, u16, name, u32}."""
        return None


class ListedPackFiles(PackFiles):
    """The PackFiles of a host that has a list of its pack files and no
    more.  files: relative paths in the spelling of the directory listing.
    tables: optionally {path as listed: [external reference names]}; a file
    without an entry has no external references."""

    def __init__(self, files, tables=None):
        self.tables = {}
        for p in files:
            self.tables[file_key(p)] = []
        if tables is not None:
            for p in tables:
                self.tables[file_key(p)] = list(tables[p])

    def externals(self, name):
        return self.tables.get(file_key(name))


# ---------------------------------------------------------------------------
# the pack table of the resource engine
# ---------------------------------------------------------------------------

class PackRecord(object):
    """One pack of the table."""
    __slots__ = ('name', 'origin', 'loaded', 'ext')

    def __init__(self, name, origin):
        self.name = name            # as first registered; never rewritten
        self.origin = origin        # an O_ constant: who registered it
        self.loaded = False         # its file was opened and its table read
        self.ext = []               # once loaded: record numbers, [own, reference 1, ...]


class PackTable(PackNames):
    """The table of packs of one session (the exe: records of 0x70 bytes in
    the resource engine at *0x5DB008).  Record 0 is "system.rpk".

    What the host does with it:
        open_lib(name)                  System.openLib(name)
        find_or_add(name, O_NODE)       a resource id node is executed (then load)
        find_or_add(name, O_DATA)       a data file names a pack
    and the compiler asks canonical() and external() through resolve().

    Not modelled: the special record 0xFFFF (it would take 65,535 packs), and
    a pack file that does not start with "RPAK" (the exe reads it unchecked;
    here the PackFiles decides what its table is)."""

    def __init__(self, files):
        self.files = files          # a PackFiles
        self.records = []           # PackRecord, in the order of registration
        self.index = {}             # lower-case name -> the first record that has it
        self.find_or_add(SYSTEM_PACK, O_BOOT)

    def find(self, name):
        """The number of the first record whose name equals `name` without
        regard to the case of A to Z; NONE when there is none."""
        return self.index.get(lower(name), NONE)

    def find_or_add(self, name, origin):
        """0x518940: the record of that name, or a new one that keeps the
        name as given, cut to NAME_MAX characters.  (A longer name is never
        found again: the comparison is with the whole name.)"""
        i = self.find(name)
        if i != NONE:
            return i
        i = len(self.records)
        kept = name
        if len(kept) > NAME_MAX:
            kept = kept[:NAME_MAX]
        self.records.append(PackRecord(kept, origin))
        key = lower(kept)
        if key not in self.index:
            self.index[key] = i
        return i

    def load(self, i):
        """0x51E310: open the pack of record i on first use and register the
        names of its external reference table (they are not loaded).  False
        when the file cannot be opened; the record stays and is tried again
        the next time."""
        rec = self.records[i]
        if not rec.loaded:
            names = self.files.externals(rec.name)
            if names is None:
                return False
            rec.loaded = True
            rec.ext = [i]
            for n in names:
                rec.ext.append(self.find_or_add(n, O_EXTERNAL))
        return True

    def open_lib(self, name):
        """System.openLib(name) (0x4E0470): find-or-add with the string as
        the script spells it, then load.  Returns the record number."""
        i = self.find_or_add(name, O_OPEN)
        self.load(i)
        return i

    def name_of(self, lookup):
        """The name of the record a lookup of `lookup` finds; None when no
        record has that name.  Registers nothing."""
        i = self.find(lookup)
        if i == NONE:
            return None
        return self.records[i].name

    # -- the questions of the compiler ----------------------------------------

    def canonical(self, written_path):
        i = self.find_or_add(written_path, O_LITERAL)
        if not self.load(i):
            return None
        return self.records[i].name

    def external(self, written_path, k):
        i = self.find_or_add(written_path, O_LITERAL)
        if not self.load(i):
            return (R_NO_FILE, '')
        rec = self.records[i]
        if not (k < len(self.records) and k < len(rec.ext)):
            return (R_NO_EXTERNAL, '')
        j = rec.ext[k]
        if not self.load(j):
            return (R_FAULT, self.records[j].name)
        return (R_EXTERNAL, self.records[j].name)

    # -- the same as the exe has it, for the tests ----------------------------

    def global_id(self, path, number):
        """0x5189E0 with 0x51E860: the global id of (path, number); 0 for a
        pack without a file, a low word 0, a high word that names nothing."""
        i = self.find_or_add(path, O_LITERAL)
        if not self.load(i):
            return 0
        lo = number & 0xFFFF
        hi = (number >> 16) & 0xFFFF
        if lo == 0 or not hi < len(self.records):
            return 0
        if hi == 0:
            return (i << 16) | lo
        ext = self.records[i].ext
        if hi < len(ext):
            return (ext[hi] << 16) | lo
        return 0

    def pool_entry(self, gid):
        """0x54B170: (name, local id) of the pool entry for a global id; None
        where the exe reads through a null record (the record of the id is a
        pack without a file).  Record 0 is taken to be there."""
        j = gid >> 16
        if j != 0 and not self.load(j):
            return None
        return (self.records[j].name, gid & 0xFFFF)


# ---------------------------------------------------------------------------
# a session without the game: boot and the scans of GameLogic
# ---------------------------------------------------------------------------

# System.rpkScan(dir) calls of the constructor of GameLogic, in source order
# (sl\Scripts\game\src\GameLogic.java lines 345 to 350), as the script spells
# the directories ...
SCANS_940 = (
    'parts\\engines\\',
    'parts\\',
    'cars\\racers\\',
    'cars\\fake_racers\\',
    'maps\\',
    'sl\\Scripts\\game\\CareerEvents\\',
)
# ... and the loop behind them (lines 352 to 360): rpkScan(path + D + "\\")
# for every directory D of path, as the listing spells D.
SUB_SCANS_940 = (
    'multibot\\maps\\',
)
# Names this replay in a cache manifest: an output with a resource id depends
# on it.  Change it with every change of the two lists or of the table.
SESSION_940_VERSION = '940-pack-table-1'


def _ends_with_ext(name):
    n = len(name)
    return n >= len(PACK_EXT) and lower(name[n - len(PACK_EXT):]) == PACK_EXT


def listed_packs(listing, directory):
    """The *.rpk files that lie directly in `directory`, as the listing
    spells them, in the order of their lower-case names.  listing: the pack
    files of the host as relative paths in the spelling of the directory
    listing; directory: 'parts\\engines\\' (its letter case does not count,
    as for FindFirstFile)."""
    d = file_key(directory)
    out = []
    for path in listing:
        p = path.replace('/', '\\')
        if len(p) > len(d) and lower(p[:len(d)]) == d:
            rest = p[len(d):]
            if '\\' not in rest and _ends_with_ext(rest):
                out.append(rest)
    return sorted(out, key=lower)


def listed_directories(listing, directory):
    """The directories directly below `directory` that hold a pack file
    somewhere, as the listing spells them, in the order of their lower-case
    names.  (A directory without a pack file registers nothing when it is
    scanned, so it need not be listed.)"""
    d = file_key(directory)
    seen = {}
    out = []
    for path in listing:
        p = path.replace('/', '\\')
        if len(p) > len(d) and lower(p[:len(d)]) == d:
            rest = p[len(d):]
            cut = 0                     # the length of the first component
            while cut < len(rest) and rest[cut] != '\\':
                cut += 1
            if cut > 0 and cut < len(rest):
                sub = rest[:cut]
                if lower(sub) not in seen:
                    seen[lower(sub)] = True
                    out.append(sub)
    return sorted(out, key=lower)


def rpk_scan(table, listing, directory):
    """System.rpkScan(directory), a script method (System.class): for every
    name FindFile lists for directory + "*.rpk", System.openLib(directory +
    name).  The directory is spelled as the caller spells it, the file as the
    listing does.  Returns the number of files."""
    names = listed_packs(listing, directory)
    for name in names:
        table.open_lib(directory + name)
    return len(names)


def session_940(files, listing, scans=True):
    """The pack table as it stands when the scripts of a Build 940 session
    are compiled: "system.rpk", then the scans of the constructor of
    GameLogic.  files: a PackFiles; listing: the pack files as relative
    paths in the spelling of the directory listing.  scans=False leaves the
    scans out (a control of the tests: the spellings then depend on the
    order in which the sources are compiled)."""
    table = PackTable(files)
    if scans:
        for d in SCANS_940:
            rpk_scan(table, listing, d)
        for base in SUB_SCANS_940:
            for sub in listed_directories(listing, base):
                rpk_scan(table, listing, base + sub + '\\')
    return table
