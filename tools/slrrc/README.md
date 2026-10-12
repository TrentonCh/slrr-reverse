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
| Classes written by older builds of the game | 153 of 153 byte-identical |
| Sources without a class file | 635 of 635 compile; the files pass every check that needs no original |

Two things stand between this and "byte-identical, full stop". Both are state
of the game's exe that no source holds; see [What is open](#what-is-open).

## Directory

| Path | Content |
| --- | --- |
| `slrrc\` | the compiler: the package described below |
| `tests\` | milestone tests `m1` to `m6`, four tests outside the milestones (`extra`, `extra4`, `extra5`, `review`), the baseline of passing items |
| `out\class_cache\` | the 635 class files the corpus has no class file for, with `manifest.json` |
| `notes\` | the rules, each with the script and the corpus count that proves it; `compiler_spec.md` is the design and the build plan, `impl_*.md` say what each implementation step built |
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
| `python -I slrrc --root G --compare --float auto` | compile every source that has a class file and compare the bytes; says which float setting reproduces each file |
| `python -I slrrc --root G --dump trees X\src\Name.java` | print the `tokens`, `ast`, `trees` or `class` (pool and rows) of one source |

| Option | Meaning |
| --- | --- |
| `--float A\|B\|C\|D` | the float literal conversion (default `A`); `auto` with `--compare` |
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
| `pack_table` | version of the pack name table (see below) |
| `classes` | the classes in the file |
| `compiled_against` | every class the compiler looked at outside the source, with the SHA-256 of its declarations |
| `not_found` | every class name the compiler asked for and did not find (a type name is looked for in the package of the source before the imports) |
| `packs` | every pack a resource id names: as written, and as spelled in the pool (`null`: the pack has no file) |
| `warnings` | number of warnings |

A compiled class is not a function of its source alone: it embeds the
descriptors of the methods it calls and the classes that declare the fields it
names. The entry lists what the output was made from, so a host can decide
whether it is still valid. The head of the manifest names the tree, the pack
table and, under `origins`, the file each of those classes was read from. The
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

635 class files (693,847 bytes) and the manifest. Whether the host loads them
is not tested here: that needs the host.

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
pack_names = install.install_packs(tree)     # packs.PackNames: the *.rpk files of the tree

with open(r'D:\SLRR-test\sl\Scripts\game\src\Trigger.java', 'rb') as f:
    source_bytes = f.read()
result = slrrc.compile_source(source_bytes, 'Trigger.java', provider, pack_names,
                              slrrc.Options(build=0x24F9D, float_mode='A', strict=False))
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

`slrrc\install.py` implements both for a game tree on disk (it is what the
command line uses), `slrrc\classfile.py` is the strict reader it gets the
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

**Float arithmetic.** `floatlit.self_test()` returns `[]` when the float
literal conversions of this build give the bits of the reference; the command
line checks it at start. It matters for the C++ port (fused multiply-add, x87
arithmetic), see the docstring of `floatlit.py`.

| Module | Content |
| --- | --- |
| `scanner.py`, `parser.py` | tokens; AST with token indices |
| `classes.py` | `ClassInfo`, `ClassProvider`, type names, descriptors, field lookup, the scored method lookup |
| `pool.py` | the interning constant pool |
| `decl.py` | pool steps 1 to 3 (seed, types, names); the rows of FILD, MTHD, CLSS |
| `emit.py` | the emitter: statements, expressions, names, calls, locals, jumps, lines |
| `floatlit.py` | the four float literal conversions |
| `packs.py` | `PackNames`, the table of the shipped pools |
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
python -I tests\run_tests.py                     # milestones m1 to m6, about 80 s
python -I tests\run_tests.py m5 m6               # whole files, and the 635 through the command line
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
M4 trees         7568/7570 node for node (float setting per file; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources
M5 files         2649/2651 byte-identical (float setting per file; 2561/2651 with setting A alone); older builds 153/153; stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454 sources
M6 source-only   635/635 compiled through the cache (635 classes, 771 trees, 32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline; strict reader 635/635; layout rules hold; stack simulation holds (21523 nodes); structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver checks 22/22
baseline         0 lost
```

| Test | What it compares |
| --- | --- |
| `m1`, `m2` | tokens and tree of every source against the first scanner and parser |
| `m3` | pool prefix, CLSS, FILD, MTHD against the shipped classes, from the source alone |
| `m4` | every tree against the shipped class, node for node |
| `m5` | every class file against the shipped one, byte for byte; every source against the composition spike |
| `m6` | the 635 sources without a class file, compiled by the command line into a temporary cache: hashes against the baseline, the strict reader, the 30 layout rules, a stack simulation, statement and expression structure, siblings that ship a class, the manifest, the game tree untouched, the command line on a small tree |
| `extra`, `extra4`, `extra5` | synthetic sources with the diagnostics expected, trees and a class file written out by hand, the lookups case by case, the class file reader on damaged files, about 34,000 mutated texts compared with the oracles |
| `review` | one check, at least, for each of the 20 findings of a review of the reference (`notes\impl_fix.md`): what a result hands out, the contract of the two interfaces, where the parser and the reader stop, bounds that wrap in 32 bits, the float self test against an exact model under nine wrong builds, diagnostics, the command line when a run fails part-way, six rules no shipped source decides, and every source rewritten with one token per line against the composition spike |
| `t4_controls.py`, `t5_controls.py` | one rule changed at a time: how many trees or files are lost (not part of a run) |

A regression is an item of the baseline that no longer passes: the run names it
(`LOST m5.pass: ...`) and exits with 1. `--update` stores new passes and refuses
when anything was lost. For the 635 there is no original, so the baseline is
the hash of each output (`tests\baseline\m6.sha256`).

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
- **The comparison is not lenient**: none of the 15 pairs whose source was
  edited after the compile comes out equal, and every rule of the writer that
  the controls change loses files.

## What is open

1. **The float literal conversion.** Four conversions occur in the shipped
   classes, one per class file: 2,561 of the 2,651 files are reproduced by
   setting A alone, 60 more need B, 22 C and 6 D
   (`tests\baseline\m5_float.tsv`). The results differ by at most 3 units in
   the last place. Which conversion Build 940 contains, and what selects it
   (probably the precision state of the x87 unit of the compiling thread), can
   only be read in the exe. Until then all four are selectable, A is the
   default for new compilations, and the 635 class files of the cache use A.
2. **The spelling of pack names.** A resource id is stored with the file name
   of its pack, in a letter case that is neither always the source's nor always
   the directory's. The compiler asks its host (`PackNames`); the command line
   answers from the 126 spellings found in the shipped pools
   (`packs.TABLE_940`), then the spelling of the file. Where the game takes the
   spelling from is not known.
3. **Constructs no shipped source uses** (`notes\compiler_spec.md` appendix B:
   a case group that falls through, `continue` in a `while`, `%=`, a compound
   assignment used as a value, ...). They compile by the plain extension of the
   proven rules with a warning `W_UNVERIFIED`, or are refused with `--strict`.
   No claim of byte identity is made for them. The shipped sources use none.
4. **Which class answers for a name** when several files define it, and how the
   game finds classes outside its class path. The command line indexes every
   class file of the tree; no shipped source depends on the choice.
5. **Not done**: the host loading the 635 class files (needs the host), the C++
   port (milestone M7), reading the exe for points 1 and 2 (M9).

## Where to read more

| File | Content |
| --- | --- |
| `notes\compiler_spec.md` | what is settled, the design, milestones M0 to M9, risks |
| `notes\impl_m1-m3.md`, `impl_m4.md`, `impl_m5-m6.md` | what each implementation step built, how its tests decide, deviations from the plan, findings |
| `notes\impl_fix.md` | the 20 findings of the review of the reference: how each was reproduced, what was changed, the test that holds it, and what the C++ port has to take from it |
| `notes\foundation.md` | the class file format |
| `notes\grammar.md`, `declarations.md`, `expressions.md`, `statements.md`, `opcodes.md` | the rules with their corpus numbers |
