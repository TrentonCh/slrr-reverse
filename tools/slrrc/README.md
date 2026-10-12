# slrrc: a compiler for the script sources of Street Legal Racing: Redline

`slrrc` turns the `.java` script sources of the game (Steam Build 940) into
`.class` files. The aim is output that is **byte-identical to what the game's
own compiler wrote**, so that a host engine can compile the scripts that ship
without a class file, and scripts a mod adds, and load the result exactly as it
loads the shipped classes.

This directory holds the reference implementation in Python (standard library
only), the tests that compare it with the shipped classes, and the analysis it
was built from. The C++ port for the host engine is the next step and is not
here yet.

State on 2026-10-11, measured by the tests of this directory on the corpus
`D:\SLRR-test` (a Build 940 install with 3,454 sources and 2,921 class files):

| What | Result |
| --- | --- |
| Shipped classes that can be compared (Build 940, consistent with their source) | **2,649 of 2,651 files byte-identical** |
| ... the two others | compiled against a version of another class that the install no longer has; byte-identical once that declaration is put back |
| The 136 of the 2,651 that the Build 940 exe itself wrote | **136 of 136 byte-identical with the one float conversion the exe contains**, no per-file choice |
| All 2,651 with that one conversion | 2,434 byte-identical; 215 of the others need one of three older forms of the conversion (the same routine in x87 arithmetic; which one is listed per file, not explained), 2 are the two above |
| Classes written by older builds of the game | 153 of 153 byte-identical |
| Sources without a class file | 635 of 635 compile; the files pass every check that needs no original |

Two things were state of the game's exe that no source holds: how a float
literal is converted, and how the pack of a resource id is spelled. Both were
read in the exe and are implemented as read (`notes\exe_reads.md`):

- **Float literals.** The exe has one conversion (scalar SSE single precision,
  setting `B`), and that is the default. 215 of the shipped class files hold
  bits this exe cannot produce: they follow the same routine in x87
  arithmetic, as earlier builds of the compiler had it (that they came from
  such builds is inferred). Three more settings (`A`, `C`, `D`) reproduce
  them, and which of the three a file needs is listed per file, because
  nothing in the install says it.
- **Pack names.** The pool spells a pack as it was first registered in the
  pack table of the session that compiled the class. The compiler replays that
  table (start of the game, the pack scans of `GameLogic`, then the sources)
  and gets every one of the 126 spellings of the shipped pools.

What is still open is in [What is open](#what-is-open).

## Directory

| Path | Content |
| --- | --- |
| `slrrc\` | the compiler: the package described below |
| `tests\` | milestone tests `m1` to `m6` and `m9`, four tests outside the milestones (`extra`, `extra4`, `extra5`, `review`), the baseline of passing items |
| `out\class_cache\` | the 635 class files the corpus has no class file for, with `manifest.json` |
| `notes\` | the rules, each with the script and the corpus count that proves it; `compiler_spec.md` is the design and the build plan, `impl_*.md` say what each implementation step built, `exe_reads.md` holds what was read in the exe (float literals, pack names) with the addresses |
| `lib\` | strict reader and writer of class files, corpus index, the first parser (used by the tests as oracles) |
| `analysis\`, `spec\` | the models the rules were proven with, and the composition spike that showed they add up to a compiler (oracles for the tests) |

The package imports nothing from `lib\`, `analysis\` or `spec\`; the tests check
that before every run.

## Running the compiler

Python 3.10 or later (developed with 3.14), standard library only, nothing to
install. Start it from this directory:

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I slrrc --root <game tree> --out <cache directory> [options] [SOURCE ...]
```

`python -m slrrc ...` is the same program. `python -I -m slrrc` does **not**
work: `-I` (isolated mode) keeps the current directory off the module path, so
Python does not find the package. `python -I slrrc` runs the package directory
itself and needs nothing on the path; from another directory give its full
path (`python -I C:\...\compiler\slrrc ...`).

`--root` is a directory laid out like an install of the game: class files
`X\Name.class`, their sources `X\src\Name.java`, packs `*.rpk`. It is only read.
Without a `SOURCE` every source of the tree is compiled.

| Command | What it does |
| --- | --- |
| `python -I slrrc --root G --out C --only-missing` | compile the sources that have no class file into the cache directory `C` |
| `python -I slrrc --root G --out C` | compile every source of the tree into `C` |
| `python -I slrrc --root G --out C X\src\Name.java` | compile one source (path absolute, or relative to the current directory or to the root) |
| `python -I slrrc --root G --out C --only-missing --dry-run` | list what would be compiled and written; do neither |
| `python -I slrrc --root G --check` | compile everything, report diagnostics, write nothing |
| `python -I slrrc --root G --compare --float auto` | compile every source that has a class file and compare the bytes; says which float setting reproduces each file (tried in the order B, A, C, D) |
| `python -I slrrc --root G --dump trees X\src\Name.java` | print the `tokens`, `ast`, `trees` or `class` (pool and rows) of one source |

| Option | Meaning |
| --- | --- |
| `--float A\|B\|C\|D` | the float literal conversion. The default `B` is the one the Build 940 exe contains; `A`, `C`, `D` reproduce shipped class files of earlier builds and are meant for `--compare`. `auto` with `--compare` |
| `--strict` | constructs no shipped source uses are errors instead of warnings |
| `--build N` | build number of the file header (default `0x24F9D`) |
| `--only-missing` | only sources with no class file beside them (or with one the reader does not accept) |
| `-n`, `--dry-run` | list, do nothing |
| `--write-install` | write `X\Name.class` beside `X\src\Name.java` **in the game tree**, with the time stamp of the source (what the game's own compiler does); never replaces a class file |
| `--overwrite` | with `--write-install`: replace class files that exist |
| `--exclude PREFIX` | skip directories whose name starts with `PREFIX` |
| `-v`, `-q` | one line per source; errors and the last line only |

Exit code 0; 1 when a source did not compile, could not be read or its class
file could not be written (or, with `--compare`, a file differs); 2 for a
command line that cannot be carried out.

**The game tree is never written to** unless `--write-install` is given: an
`--out` directory inside the tree is refused, and the tests compare the size
and time stamp of every class file, source and pack of the corpus before and
after a run.

Messages are text; a character the encoding of a redirected output lacks is
written as a backslash escape. A dump (`--dump`) redirected to a file or a pipe
is bytes: one byte per character of the source, LF line ends, the same on
every system, because it is the format the C++ port is compared with.

A class file the reader does not accept (damaged, empty) is reported and
defines nothing; the source beside it is then treated as a source without a
class file, so its classes are still known to the other sources.

### The cache directory

The class file of `X\src\Name.java` is `<cache>\X\Name.class`: the path the
class file would have in the install. It is an ordinary class file with one
blob per class of the source. Beside the files stands `manifest.json`, with one
entry per output:

| Field | Content |
| --- | --- |
| `output`, `size`, `sha256` | the class file, relative to the cache directory |
| `source`, `source_size`, `source_sha256` | the source, relative to the root |
| `compiler`, `build`, `float`, `strict` | how it was compiled |
| `pack_table` | version of the pack table replay (see "Pack names" below) |
| `classes` | the classes in the file |
| `compiled_against` | every class the compiler looked at outside the source, with the SHA-256 of its declarations |
| `not_found` | every class name the compiler asked for and did not find (a type name is looked for in the package of the source before the imports) |
| `packs` | every pack a resource id names: as written, and as spelled in the pool (`null`: the pack has no file, the resource id was compiled to `system.rpk` id 0) |
| `warnings` | number of warnings |

A compiled class is not a function of its source alone: it embeds the
descriptors of the methods it calls and the classes that declare the fields it
names. The entry lists what the output was made from, so a host can decide
whether it is still valid. The head of the manifest names the tree, the pack
table replay with its scan list and, under `origins`, the file each of those
classes was read from. The
declaration hash is the SHA-256 of `slrrc.classes.declaration_text(info)`, a
text that is the same whether a class was declared from its source or read
from its class file.
A run into a directory that has a manifest keeps the entries of other outputs;
two runs over the same tree write the same bytes (no time stamps).

An entry always describes the class file that is there, also after a run that
failed: the manifest is written once before anything is compiled (a directory
in which it cannot be written is refused before a class file is replaced), a
class file that cannot be written fails that one source and leaves the old
file with its old entry, and the manifest is written again when the run ends,
also when it ends with an exception. A `manifest.json` that is not a manifest
of this format is replaced, with a warning.

`out\class_cache\` is the result of

```
python -I slrrc --root D:\SLRR-test --out out\class_cache --only-missing
```

635 class files (693,847 bytes) and the manifest, compiled with float setting
B. (Until the exe was read they were compiled with setting A; 10 of the 635
hold a literal the two settings convert differently and changed, 625 are the
same bytes as before.) Whether the host loads them is not tested here: that
needs the host.

### Pack names

A resource id `cars.racers.duhen:0x0000002Ar` is stored in the class file as
a pack file name and a local id, and the file name is not always spelled as
the source spells it: the class file says `cars\racers\Duhen.rpk`. In the
game the spelling is state of the session: the resource engine keeps one
record per pack, finds a record without regard to letter case, and a record
keeps the name it was first registered with. Whoever names a pack first
decides: a script that opens it (`System.openLib`, `System.rpkScan`), the
external reference table of another pack, a class that runs, or the source
being compiled.

The command line has no session, so it replays the start of one
(`packs.session_940`): `system.rpk`, then the pack scans of the constructor of
`GameLogic` (`parts\engines\`, `parts\`, `cars\racers\`, `cars\fake_racers\`,
`maps\`, `sl\Scripts\game\CareerEvents\`, every directory of
`multibot\maps\`), then the sources of the run in index order. So:

| The pack | Its spelling in the class file |
| --- | --- |
| lies in one of the scanned directories | the directory as `GameLogic` writes it, the file name as the directory listing spells it |
| lies elsewhere | as the external reference table of a pack loaded before spells it, else as the first source of the run that names it writes it |
| has no file | none: the resource id is compiled to `system.rpk` id 0, with a warning `W_PACK` |

For the shipped sources the order of a run changes nothing (every source
spells a pack outside the scanned directories alike). A mod that writes such a
pack in another letter case than the shipped scripts gets its own spelling
when it is compiled alone; the game loads either, because it looks packs up
without regard to case.

## Using it as a library

Run from this directory (or with it on `sys.path`). The provider and the pack
names of a game tree on disk come from `slrrc\install.py`:

```python
import sys
import slrrc
from slrrc import install

sys.setrecursionlimit(20000)                 # see "Nesting" below
tree = install.Install(r'D:\SLRR-test')      # a game tree; it is only read
tree.scan()
provider = install.InstallProvider(tree)     # classes.ClassProvider: the classes of the tree
provider.load()                              # reads every class file once, about 2 s
pack_names = install.install_packs(tree)     # packs.PackNames: the pack table of a session,
#                                              replayed over the *.rpk files of the tree

with open(r'D:\SLRR-test\sl\Scripts\game\src\Trigger.java', 'rb') as f:
    source_bytes = f.read()
result = slrrc.compile_source(source_bytes, 'Trigger.java', provider, pack_names,
                              slrrc.Options(build=0x24F9D, float_mode='B', strict=False))
result.ok            # False: no bytes; result.diagnostics says why (line, column, code, text)
result.class_file    # the bytes: the blobs of all classes of the source, in order
result.class_names   # one full name per blob
result.consulted     # the classes it looked at through the provider
result.missed        # the class names it asked for and did not find
```

The compiler core does no file access and keeps no global state. What it needs
from its surroundings comes through two interfaces, each with a contract that
is written out in its docstring:

- `classes.ClassProvider.find(full_name) -> ClassInfo or None`: what another
  class declares. The classes of the source itself never come from the
  provider. The class must carry exactly the name asked for; it must hold its
  interfaces, the static flag of every field, its methods in source order, and
  descriptors as a class file holds them (a constructor has no return part
  there, `(I)`; `(I)V` is taken for the same); `find` may be re-entered and
  then answers a class that is being declared with an empty `ClassInfo`; it is
  asked once per name and compilation. A class that breaks what can be checked
  is reported (`W_CLASS`) and treated as not found.
- `packs.PackNames.canonical(path) -> file name or None`: how the pack of a
  resource id is spelled in the pool. `None` means "the install has no file
  for this pack", never "not known": the resource id is then compiled to
  `system.rpk` id 0, as one shipped class shows, with a warning (`W_PACK`).
  A second question, `external(path, k)`, is asked only for an id with a high
  word (the k-th external reference of the pack), which no shipped source
  has. The answers are state of a session, so the implementation a host needs
  is `packs.PackTable`, the pack table of the resource engine as it was read
  in the exe: the host creates one at start, registers `system.rpk`, calls
  `open_lib(name)` wherever a script calls `System.openLib`, registers the
  pack names of executed resource ids and of data files, and passes the table
  to every compilation. It answers for its files through `packs.PackFiles`
  (which pack files can be opened, and the external reference table at the
  head of each).

`slrrc\install.py` implements both for a game tree on disk (it is what the
command line uses; its pack table is a replay, see "Pack names" above),
`slrrc\classfile.py` is the strict reader it gets the
declarations from. `slrrc.declare_source()` and `slrrc.declared_names()` are
what a provider of its own needs for classes that exist as source only. Bad
input never raises: a source with an error gives `ok == False`, no bytes, and
diagnostics (`E_LEX`, `E_SYNTAX`, `E_UNSUPPORTED`, `E_TYPE`, `E_NAME`,
`E_MEMBER`, `E_LIMIT`; warnings `W_UNVERIFIED`, `W_TYPE`, `W_BYTE`, `W_PACK`,
`W_CLASS`).

**Nesting.** Statements and expressions may be nested 200 deep
(`parser.MAX_DEPTH`); beyond that a source is refused with `E_LIMIT`, reported
at the innermost construct. The Python reference needs about 4,000 interpreter
frames for that depth, more than Python's default of 1,000: the command line
and the tests raise the limit to 20,000, and a program that calls
`compile_source` itself has to do the same (`sys.setrecursionlimit`), or deeply
nested sources (from about 160 parentheses) get `E_LIMIT` earlier.

**Float literals.** The default `Options.float_mode` is `'B'`, the conversion
the Build 940 exe contains: the integer part in 32 bits, then per fraction
digit `v += d * s; s *= 0.1f` in single precision. It is not the correctly
rounded value: for 4,748 of the 15,954 distinct literal texts of the shipped
sources the bits differ from it, mostly by 1 or 2 units in the last place,
and that is what the game computes. Two forms behave unlike a decimal reader: `.5` (no digit before the
point) is **0.0**, with a warning, and only the first 1,024 fraction digits
are read. `floatlit.self_test()` returns `[]` when this build computes the
bits of the reference; the command line checks it at start. It matters for
the C++ port (fused multiply-add, x87 arithmetic, a library conversion), see
the docstring of `floatlit.py`.

| Module | Content |
| --- | --- |
| `scanner.py`, `parser.py` | tokens; AST with token indices |
| `classes.py` | `ClassInfo`, `ClassProvider`, type names, descriptors, field lookup, the scored method lookup |
| `pool.py` | the interning constant pool |
| `decl.py` | pool steps 1 to 3 (seed, types, names); the rows of FILD, MTHD, CLSS |
| `emit.py` | the emitter: statements, expressions, names, calls, locals, jumps, lines |
| `floatlit.py` | the float literal conversion of the exe (B) and the three of earlier builds (A, C, D) |
| `packs.py` | `PackNames`, `resolve`, the pack table of the resource engine (`PackTable`), its replay (`session_940`) |
| `write.py` | pool steps 4 and 5 (resolve by row vector, emit in tree order), node encoding, chunks, the file |
| `dump.py`, `diag.py` | text forms of tokens, AST, trees and classes; diagnostics |
| `classfile.py`, `install.py`, `cli.py` | class file reader; a game tree as provider; the command line |

The modules above the last row are written to be ported to C++ line by line:
plain data structures, no reflection, one function per rule.

## Running the tests

The tests need Python 3.10 or later, the corpus (a Build 940 install, read
only) and its index `notes\pairs.json`. The corpus is looked for at
`D:\SLRR-test`; the environment variable `SLRR_CORPUS` names another place.
The index belongs to one corpus: after pointing the tests at another copy,
build it again with

```
python -I lib\oracle.py build                    # scans the corpus, writes notes\pairs.json and notes\corpus.md
```

(the baseline in `tests\baseline\` holds the results for the install it was
made from; another install passes or fails item by item against it).

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I tests\run_tests.py                     # milestones m1 to m6 and m9, about 2 minutes
python -I tests\run_tests.py m5 m6               # whole files, and the 635 through the command line
python -I tests\run_tests.py m9                  # the two rules read in the exe, about 15 s
python -I tests\run_tests.py extra extra4 extra5 # the checks outside the corpus, 2 to 3 minutes
python -I tests\run_tests.py review              # one check per defect a review found, about 1 minute
python -I tests\run_tests.py m5 --only Trigger -v   # one source, with details
```

Each milestone prints one headline; the last line counts the items of the
stored baseline (`tests\baseline\`) that no longer pass. Exit code 0 when every
headline shows what its milestone asks for and nothing was lost. What a full
run prints:

```
M1 tokenise      3454/3454 sources, 852211 tokens
M2 parse         3454/3454 sources
M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
M4 trees         7568/7570 node for node (float setting per file; 7251/7570 with setting B, the conversion of the exe, alone; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources; pack table replay 126/126 spellings of the shipped pools, 9058/9058 rid nodes, 2/2 for the pack without a file
M5 files         2649/2651 byte-identical (float setting per file; 2434/2651 with setting B, the conversion of the exe, alone; 2561/2651 with setting A alone); written by the Build 940 exe 136/136 under B; older builds 153/153; stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454 sources
M6 source-only   635/635 compiled through the cache (635 classes, 771 trees, 32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline (float setting B; under setting A 10 files differ, 33 literals); strict reader 635/635; layout rules hold; stack simulation holds (21523 nodes); structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver checks 23/23
M9 exe rules     floats: 136/136 class files the exe wrote byte-identical under B (90 under A), B equals the exact model on 15954/15954 literal texts; packs: 126/126 spellings of the shipped pools, 9058/9058 rid nodes, 9952/9952 literals the same in any order of compilation (119 and 123 spellings without the scans); checks 25/25
baseline         0 lost
```

| Test | What it compares |
| --- | --- |
| `m1`, `m2` | tokens and tree of every source against the first scanner and parser |
| `m3` | pool prefix, CLSS, FILD, MTHD against the shipped classes, from the source alone |
| `m4` | every tree against the shipped class, node for node; the pack names of the replayed pack table against the shipped pools |
| `m5` | every class file against the shipped one, byte for byte, with the float setting listed per file, with setting B alone and with setting A alone; the 136 class files the exe itself wrote under B; every source against the composition spike |
| `m6` | the 635 sources without a class file, compiled by the command line into a temporary cache: hashes against the baseline, what setting A would give, the strict reader, the 30 layout rules, a stack simulation, statement and expression structure, siblings that ship a class, the manifest, the game tree untouched, the command line on a small tree |
| `m9` | the two rules read in the exe, each on the corpus: the float conversion (the 136 files the exe wrote, the exact model on every literal text, the quoted instruction bytes at their addresses in the exe file) and the pack table (scan list against `GameLogic.java`, 126 spellings, every resource id node, any order of compilation, controls without the scans, the interface against the functions of the exe) |
| `extra`, `extra4`, `extra5` | synthetic sources with the diagnostics expected, trees and a class file written out by hand, the lookups case by case, the class file reader on damaged files, about 34,000 mutated texts compared with the oracles |
| `review` | one check, at least, for each of the 20 findings of a review of the reference (`notes\impl_fix.md`): what a result hands out, the contract of the two interfaces, where the parser and the reader stop, bounds that wrap in 32 bits, the float self test against an exact model under ten wrong builds, the two exits of the float routine, diagnostics (among them a resource id with a low word of 0 or with a high word), the command line when a run fails part-way, six rules no shipped source decides, and every source rewritten with one token per line against the composition spike |
| `t4_controls.py`, `t5_controls.py` | one rule changed at a time: how many trees or files are lost (not part of a run) |

A regression is an item of the baseline that no longer passes: the run names it
(`LOST m5.pass: ...`) and exits with 1. `--update` stores new passes and refuses
when anything was lost. For the 635 there is no original, so the baseline is
the hash of each output (`tests\baseline\m6.sha256`); a wanted change of an
output is re-baselined by hand, as the 10 files of the float default were
(`tests\baseline\m6_float_a.sha256` keeps what setting A gives for them, and
`m6` checks it). `tests\baseline\m5_exe940.tsv` lists the 136 class files the
Build 940 exe wrote, `tests\baseline\m5_float.tsv` the float setting each
shipped file needs, `tests\pack_oracle.py` the 126 pack spellings of the
shipped pools.

## What is proven

Proven means: a script checks the rule on the whole corpus, and the count is in
the notes. Everything below is exercised by the byte-identical files.

- **The class file format**: all 2,921 class files of the install parse with
  the strict reader and serialise back byte for byte (`notes\foundation.md`).
- **The language**: the grammar of all 3,454 sources; type names; descriptors;
  the rows and flags of fields and methods (`notes\grammar.md`,
  `notes\declarations.md`).
- **The emitted code**: 17 statement forms, every expression form, names and
  paths, calls with the game's scored method lookup, locals and scopes, jump
  offsets, and the source line of every node: 7,568 of 7,570 trees node for
  node (`notes\statements.md`, `notes\expressions.md`).
- **The order of the constant pool**: five steps (class header, type
  descriptors in parse order, member names, refs in a pass by row vector,
  strings in a pass in tree order): 2,703 of 2,705 pools byte for byte.
- **Whole files**: 2,649 of 2,651. The two that differ (`Center_Differential`,
  `Garage`) hold a ref to a method as another version of the callee declared
  it; with that one declaration given to the compiler both come out
  byte-identical (`tests\t5_files.py -v`).
- **The float literal conversion of the exe** (read at 0x527530, setting B):
  the 136 class files the exe itself wrote are byte-identical under it without
  a per-file choice; setting A, the default until then, reproduces 90 of them.
- **Where a pack name comes from** (the pack table of the resource engine,
  read at 0x518940 and 0x5189E0): the replayed table reproduces 126 of 126
  spellings of the shipped pools and 9,058 of 9,058 resource id nodes, in any
  order of compilation; without the pack scans of `GameLogic` it does not.
- **The comparison is not lenient**: none of the 15 pairs whose source was
  edited after the compile comes out equal, and every rule of the writer that
  the controls change loses files.

## What is open

1. **Which earlier form of the float routine each of 215 shipped files was
   compiled with.** The exe has one conversion, B, and with B alone 2,434 of
   the 2,651 comparable files are byte-identical. 215 others hold a literal
   that only the same routine in x87 arithmetic converts to the shipped bits:
   setting A (x87 under 24-bit precision control), C or D (x87 at 53 or 64
   bits, values stored to float or kept in registers). The arithmetic is
   exact; that those files came from earlier builds of the compiler is
   inferred (the builds are not at hand), and which of the three a file needs
   follows from nothing in the install. It is listed per file
   (`tests\baseline\m5_float.tsv`: 2,649 of 2,651 with it). This does not
   concern new compilations: they use B.
2. **Which session compiled the shipped classes.** The spelling of a pack is
   the spelling of its first registration in the session. The replay "start of
   the game, the scans of `GameLogic`, then the sources" gives every shipped
   spelling, but it is the simplest session that does, not a known one. A host
   with a real session has to feed its own pack table at the same events as
   the exe (`notes\exe_reads.md` 2.5); the present pack registry of the host
   does not keep the first spelling and cannot serve as it is. Two cases of
   the id were read in the exe and never seen in a class file (a low word of
   0; a high word that selects an external reference of the pack); they are
   compiled as read, with a warning `W_UNVERIFIED`.
3. **Constructs no shipped source uses** (`notes\compiler_spec.md` appendix B:
   a case group that falls through, `continue` in a `while`, `%=`, a compound
   assignment used as a value, ...). They compile by the plain extension of the
   proven rules with a warning `W_UNVERIFIED`, or are refused with `--strict`.
   No claim of byte identity is made for them. The shipped sources use none.
4. **Which class answers for a name** when several files define it, and how the
   game finds classes outside its class path. The command line indexes every
   class file of the tree; no shipped source depends on the choice.
5. **Read in the exe and not applied** (`notes\exe_reads.md` 4): a decimal
   int with a leading zero is the exe's octal rule and an error there, here
   it is read as decimal with a warning; `0x10L` is accepted by the exe and
   refused here; a resource id is three tokens in the exe (white space may
   stand inside it, the number may be decimal), one token here. No shipped
   source has any of these forms.
6. **Not done**: the host loading the 635 class files (needs the host), the C++
   port (milestone M7), the pack table in the host.

## Where to read more

| File | Content |
| --- | --- |
| `notes\compiler_spec.md` | what is settled, the design, milestones M0 to M9, risks |
| `notes\exe_reads.md` | M9: what the Build 940 exe does with a float literal and with the pack of a resource id, the addresses, the rules, the numbers that prove them, what was read and not applied |
| `notes\impl_m1-m3.md`, `impl_m4.md`, `impl_m5-m6.md` | what each implementation step built, how its tests decide, deviations from the plan, findings |
| `notes\impl_fix.md` | the 20 findings of the review of the reference: how each was reproduced, what was changed, the test that holds it, and what the C++ port has to take from it |
| `notes\foundation.md` | the class file format |
| `notes\grammar.md`, `declarations.md`, `expressions.md`, `statements.md`, `opcodes.md` | the rules with their corpus numbers |
