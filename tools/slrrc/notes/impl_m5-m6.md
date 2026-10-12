# Implementation step M5 and M6 (offline): whole class files and the command line of `slrrc`

Written 2026-10-11. Third implementation step of `notes\compiler_spec.md`:
milestone M5 (whole files, byte for byte) and the offline half of M6 (the 635
sources without a class file, through a cache directory). It builds on
`notes\impl_m1-m3.md` (front half) and `notes\impl_m4.md` (emitter) and adds
the two pool passes, the writer, a class file reader, a provider for a game
tree on disk and the command line. Nothing under `lib\`, `analysis\`, `spec\`,
in the corpus `D:\SLRR-test`, in the game installs or in the slrr-reverse repo
was changed; no exe and no Ghidra was run.

## 0. Result

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I tests\run_tests.py m5 m6
```

prints (36 s, exit code 0):

```
M5 files         2649/2651 byte-identical (float setting per file; 2561/2651 with setting A alone); older builds 153/153; stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454 sources
M6 source-only   635/635 compiled through the cache (635 classes, 771 trees, 32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline; strict reader 635/635; layout rules hold; stack simulation holds (21523 nodes); structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver checks 22/22
baseline         0 lost
```

These are the numbers the spec names for M5 (2,649 of 2,651 files with the
float setting chosen per file, 2,561 with setting A alone, 153 of 153 older
builds, 0 of 15 stale, CONS exact for 2,703 of 2,705 classes, the same bytes as
the spike for all 3,454 sources) and for the offline half of M6 (635 of 635,
hashes equal to the baseline, layout rules, stack simulation, 767 of 767,
522 of 522). All milestones and the three tests outside the corpus together
(`python -I tests\run_tests.py m1 m2 m3 m4 m5 m6 extra extra4 extra5`, 3
minutes, exit code 0):

```
M1 tokenise      3454/3454 sources, 852211 tokens
M2 parse         3454/3454 sources
M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
M4 trees         7568/7570 node for node (float setting per file; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources
M5 files         2649/2651 byte-identical (float setting per file; 2561/2651 with setting A alone); older builds 153/153; stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454 sources
M6 source-only   635/635 compiled through the cache (635 classes, 771 trees, 32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline; strict reader 635/635; layout rules hold; stack simulation holds (21523 nodes); structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver checks 22/22
extra            dialect 181/181 cases; deletions 3067/3068 rejected, lib 3067; mutations 9060 texts, 0 exceptions, 0 differences from lib
extra4           dialect 75/75 cases; trees 13/13 written out by hand; lookup 55/55; mutations 12560 texts, 0 exceptions, 0 differences from the spike
extra5           hand 3/3; outputs 512/512 runs well-formed; reader 2921/2921 class files equal to lib/tufa, 7378 damaged variants, 0 disagreements; details 0 failures; mutations 12560 texts, 0 exceptions, 0 differences from the spike
baseline         0 lost
```

The cache for the corpus:

```
python -I slrrc --root D:\SLRR-test --out out\class_cache --only-missing
compiled 635 of 635 sources (635 classes, 693847 bytes, float setting A); wrote 635 class files and manifest.json (635 entries) to C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler\out\class_cache
```

`out\class_cache\` holds 636 files: 635 class files (693,847 bytes) under
`cars\` (418) and `parts\` (217), and `manifest.json` (1,062,748 bytes). The
class files are byte for byte the ones of `spec\out\source_only_classes\`
(`diff -r` shows the manifest as the only difference) and their hashes are the
ones of `tests\baseline\m6.sha256`.

No file is special-cased anywhere. The package imports nothing from `lib\`,
`analysis\` or `spec\`.

## 1. What was built

### The package `compiler\slrrc\`

| File | Lines | State | Content |
| --- | ---: | --- | --- |
| `write.py` | 392 | +294 | The writer. `resolve_pass` and `resolve_tree` (pool step 4: the trees by row vector), `emit_pass` (step 5: the trees in tree order), `finish_pool`; `operand_index`, `tree_payload` (node encoding), `class_blob` (TUFA, CONS, [FILD], [MTHD], CLSS, TREE), `class_file`. |
| `__init__.py` | 320 | +123 | `compile_source(source, name, provider, packs, options) -> Result(ok, class_file, class_names, consulted, missed, diagnostics, ast, classes)`; `declared_names(source)`; the check that a text source holds bytes. |
| `classfile.py` | 381 | new | Strict reader of class files: `read(data, with_trees=False) -> ReadResult(ok, error, blobs)`, `class_info(blob) -> ClassInfo`. No exception for a damaged file. |
| `install.py` | 264 | new | A game tree on disk: `Install` (class files, sources, packs), `InstallProvider` (the `ClassProvider` of the command line), `install_packs` (its `PackNames`). Reads, never writes. |
| `cli.py` | 614 | new | The command line: selection, dry run, compile into a cache directory with a manifest, `--check`, `--compare`, `--dump`, `--write-install`. |
| `__main__.py` | 25 | new | Entry point for `python -I slrrc` and `python -m slrrc`. |
| `dump.py` | 500 | +63 | `dump_class`, `dump_classes`: the pool entry by entry with the five step lengths, CLSS, rows. |
| `classes.py` | 562 | +47 | `declaration_text(info)`: the canonical text a cache hashes; `UnitScope.missed`. |
| `decl.py` | 386 | +5 | `ClassBuild.pool_steps`: the length of the pool after each step. |
| `packs.py` | 236 | +4 | `TABLE_940_VERSION`. |

Unchanged: `scanner.py`, `parser.py`, `pool.py`, `emit.py`, `floatlit.py`,
`diag.py`. The float literals with their four selectable conversions were
built in the M4 step (`floatlit.py`); this step carries the setting through
`Options.float_mode` to the bytes and to the command line (`--float`).

The compiler core is every module but `install.py`, `cli.py` and
`__main__.py`. It reads no file, prints nothing, uses no reflection and has no
`global` statement, and the only standard module it imports is `struct`;
`tests\harness.py` checks all of that before every run.

### The tests `compiler\tests\`

| File | Lines | Content |
| --- | ---: | --- |
| `t5_files.py` | 418 | milestone M5 |
| `t6_source_only.py` | 623 | milestone M6, offline half |
| `t_extra5.py` | 626 | the writer and the reader outside the corpus (`run_tests.py extra5`) |
| `t5_controls.py` | 256 | control runs: one rule of the writer changed, files and pools lost (not a milestone, 90 s) |
| `harness.py` | +32 | the self-containment check also holds the core to "no I/O, no reflection" |
| `run_tests.py` | +9 | `extra5`, the M5 and M6 lines |
| `baseline\` | | `m5.pass` 2,649, `m5_older.pass` 153, `m6.pass` 635 and `m6.sha256` 635 are the seeds of the spike, unchanged: the pass lists of this code are identical to them. New: `m5_cons.pass` 2,703 (`class file#blob` with an exact CONS), `m5_spike.pass` 3,454 sources, `extra5.pass` 3 |

Also new: `compiler\README.md` and `compiler\out\class_cache\`.

## 2. How the tests decide

**M5** (`t5_files.py`, 25 s). Every source is compiled by
`slrrc.compile_source(text, name, CorpusProvider, packs, Options(strict=True,
build, float_mode))`. The class file of the source under test never reaches
the compiler.

- **Oracle pairs** (2,651 files): the bytes must equal the shipped file, under
  the float setting `tests\baseline\m5_float.tsv` names for the file (A for a
  file it does not list) and a second time under A for all: 2,649 and 2,561.
  The test also checks the table: a file listed under B, C or D differs under
  every earlier setting, and a file listed without a float literal has none
  (2,563 files A, 60 B, 22 C, 6 D).
- **CONS** per class: 2,703 of 2,705 pools byte for byte.
- **Older builds** (153 files): compiled with the build number of the shipped
  header, setting A: 153.
- **Stale pairs** (15 files): none equal under any of the four settings. A
  control: the comparison is not lenient.
- **The spike**: the bytes equal `spec\spike_compiler.py` for all 3,454
  sources.
- **Every output**, whatever it is compared with: the strict reader of the
  foundation (`lib\tufa.py`) reads it back, `validate()` finds nothing, it
  serialises to the same bytes; one blob per class with its name; the pool
  only grew over its five steps, its first three steps are the prefix of M3,
  every type carrier names an entry of step 2, and encoding the trees interned
  nothing (steps 4 and 5 covered every operand); a node has an operand exactly
  when the opcode table says so; a source without a float literal gives the
  same bytes under setting D.
- **Declarations**: for the 2,858 classes of the consistent pairs,
  `classes.declaration_text` of the class as declared from the source equals
  that of the class read from the shipped file with the package's own reader.
- A diagnostic of any kind fails the file (strict mode).

**M6** (`t6_source_only.py`, 13 s). The test starts the command line in an
interpreter of its own, as a host would
(`python -I slrrc --root D:\SLRR-test --out <temporary directory> --only-missing`),
and looks at what it wrote:

- the SHA-256 of each of the 635 files equals `tests\baseline\m6.sha256`;
- the strict reader, `validate()`, round trip: 635;
- the 30 MUST rules of `lib\check_layout.py`, run on the cache directory: hold
  for 635 blobs;
- the stack simulation of `lib\check_opcodes.py`: X1 21,523 of 21,523, X2 118,
  X3 784, X4 4,956, X5 2,019, X6 134, none failing;
- statement forms (`analysis\stmt\destruct.py`) and expression shapes
  (`analysis\expr\decomp.py`): 767 of 767 trees with nodes;
- siblings: 528 sources have a sibling of the same token shape that ships a
  class, 528 with the same opcode sequence; 522 with every token on the same
  line, 522 equal in opcode, line, local index, count, jump offset and kind of
  expression;
- the manifest: one entry per output; source path, size and hash, output size
  and hash, float setting, build, compiler and pack table version checked; the
  5,329 declaration hashes recomputed from the `CorpusProvider` of the tests,
  which reads the class files with `lib\tufa.py` and not with the package's
  reader; the 2,249 names recorded as not found are not found by it either;
- nothing else was written into the cache directory;
- **the game tree is the same before and after**: size and time stamp of its
  6,522 class files, sources and packs, and no file named like an output;
- the provider of the driver declares all 3,585 class names of the corpus as
  the provider of the tests does;
- 22 checks of the command line on a small tree made for the test: an `--out`
  inside the tree or around it is refused, a run without a destination is
  refused, a dry run lists and writes nothing, `--check` writes nothing, a
  construct no shipped source uses is a warning and an error with `--strict`,
  the cache layout, the manifest (class compiled against, origin, a name
  looked for in vain, packs), a pack without a file compiles to `system.rpk`
  id 0, a second run gives the same bytes, a run for one source keeps the
  other entries, a file that is no manifest is replaced with a warning,
  `python -m slrrc`, `--write-install` (beside the source, with its time
  stamp, never replacing a class file unless `--overwrite`), `--only-missing`,
  `--compare`, a source that does not compile (exit code 1, a diagnostic with
  line and code, no output for it, the others compiled), a source outside a
  `src` directory.

What M6 cannot say here: whether the host loads the files.

**extra5** (`t_extra5.py`, 29 s):

- *hand*: one source with two classes whose pool is **written out by hand**,
  entry by entry, from the five steps of `declarations.md` 12 (46 entries and
  7, with the step lengths and the rows), and the 231 bytes of its second class
  written out by hand in hexadecimal from `foundation.md` 3. Both agreed with
  the writer on their first run.
- *outputs*: the 256 synthetic sources of `t_extra.py` and `t_extra4.py` (every
  construct of appendix B among them) compiled to bytes in both modes, 512
  runs: a result that is ok passes the checks of M5 for every output (196
  runs), one that is not has no bytes.
- *reader*: `classfile.py` against `lib\tufa.py`. All 2,921 class files of the
  corpus read by both: the same pool, rows, CLSS record and nodes, and the
  same `ClassInfo`. 7,378 damaged variants of six class files (every
  truncation, 500 changed bytes each, inserted bytes, appended tails): both
  readers accept (1,446) or both reject, no exception.
- *details*: the build number of the header; bytes and text as input; the four
  float settings; the result of a source with an error; a character above
  U+00FF; a byte above 0x7F; option values that do not exist; class names;
  `declared_names`; `declaration_text` against a text written by hand.
- *mutations*: the 12,560 mutated texts of `t_extra4.py`, compiled in default
  mode by the package and by the spike, now to bytes: no exception, every
  output of the package well-formed, 3,304 texts both compile **with the same
  bytes**, 2 differ in the descriptor of a type name that resolves nowhere
  (`L;` against the spike's `L<name>;`), 149 only the package compiles, 216 the
  package refuses on purpose.

**Controls** (`python -I tests\t5_controls.py`, 90 s, not part of a milestone
run). Each control is a textual change of one rule in `write.py` or `pool.py`,
compiled in memory; the oracle pairs are compiled with it. The trees are the
same in every control, so only the pool order, the indices and the layout
differ:

| Rule changed | Files lost of 2,649 | Pools lost of 2,703 |
| --- | ---: | ---: |
| inside a nat the descriptor before the name | 2,608 | 2,641 |
| the strings before the refs (step 5 before step 4) | 2,611 | 2,642 |
| steps 4 and 5 in one pass per tree | 2,602 | 2,631 |
| FILD written for a class without a field | 2,460 | 0 |
| inside a ref the nat before the class | 1,445 | 1,460 |
| a rid entry holds the local id before the pack name | 594 | 606 |
| the class of a receiver is not interned before the lookup | 311 | 326 |
| step 4 in tree order instead of by row vector | 45 | 49 |
| `this` as a value interns nothing | 42 | 42 |
| step 5 by row vector, as step 4 | 41 | 46 |
| step 4: field initialisers before methods | 30 | 34 |
| step 4: instance methods before static methods | 18 | 18 |
| step 4: instance field initialisers before static ones | 2 | 2 |
| the class of a receiver interned only before a bare name | 1 | 1 |
| the class of an array receiver interned before the lookup too | **0** | **0** |
| step 5 interns type carriers too | **0** | **0** |

The counts agree with the witnesses of `declarations.md` 6.5 and 6.6 (which
count all 2,873 pair classes: 2,811, 2,806, 1,620, 327, 50, 42, 47, 35, 18, 2,
1). The two zeros are not choices: an array receiver only ever has the member
`length`, whose ref interns the array class anyway, and every type carrier is
in the pool since step 2 (the M5 test checks the second for every output).

A regression is caught: with the control "instance field initialisers first"
applied in memory, `run_tests.py m5` prints `2647/2651`, `LOST m5.pass:
sl/Scripts/game/City.class`, `LOST m5.pass: sl/Scripts/game/OptionsDialog.class`,
`baseline 2 lost`, and exits with 1.

## 3. The command line

```
python -I slrrc --root GAME --out CACHE [SOURCE ...]     compile into a cache directory
python -I slrrc --root GAME --out CACHE --only-missing   ... only what has no class file
python -I slrrc --root GAME --out CACHE --dry-run        list what would be done
python -I slrrc --root GAME --check [SOURCE ...]         compile, report, write nothing
python -I slrrc --root GAME --compare [SOURCE ...]       compile and compare with the class files of the tree
python -I slrrc --root GAME --dump trees SOURCE          print tokens, ast, trees or class
```

Options: `--float A|B|C|D` (default A; `auto` with `--compare`), `--strict`,
`--build N`, `--only-missing`, `-n` / `--dry-run`, `--write-install`,
`--overwrite`, `--exclude PREFIX`, `-v`, `-q`. Exit code 0; 1 when a source did
not compile or `--compare` found a difference; 2 for a command line that
cannot be carried out. Diagnostics go to stderr as
`path:line:column: error E_SYNTAX: text`.

**The tree is never written to** unless `--write-install` is given. An `--out`
directory inside the tree is refused, and so is one that contains the tree
(its layout mirrors the tree, so one of its paths could be a path of the
tree). `install.py` has no call that writes.

**Cache layout.** The class file of `X\src\Name.java` is `CACHE\X\Name.class`,
the path the class file would have in the install (the layout of
`spec\out\source_only_classes\` and of spec 2.10); a source with several
classes gives one file with one blob per class. `CACHE\manifest.json` has one
entry per output:

| Field | Content |
| --- | --- |
| `output`, `size`, `sha256` | the class file, relative to the cache directory |
| `source`, `source_size`, `source_sha256` | the source, relative to the root |
| `compiler`, `build`, `float`, `strict` | `slrrc 0.3`, the build number, the float setting, strict mode |
| `pack_table` | `940-shipped-pools-1`, the version of `packs.TABLE_940` |
| `classes` | the classes in the file |
| `compiled_against` | `{class name: SHA-256 of its declarations}` for every class found through the provider |
| `not_found` | every class name asked of the provider and not found |
| `packs` | `{written, pool}` per pack a resource id names (`pool` is `null` for a pack without a file) |
| `warnings` | number of warnings |

and a head with `format` (1), `generator`, `root`, `pack_table` (version,
number of entries, hash of the table) and `origins` (the file each class of a
`compiled_against` was read from). The file has sorted keys and no time stamp:
two runs over the same tree write the same bytes. A run into a directory that
has a manifest keeps the entries of other outputs; a source that no longer
compiles loses its entry.

The manifest of `out\class_cache\`: 635 entries, all float setting A, build
0x24F9D, no warning; 5,329 dependencies on 71 classes (7 to 16 per output), 67
of them read from class files and 4 declared from sources that have no class
file themselves (`Block_Vee_OHV`, `OHV_Camshaft`, `SOHC_Camshaft`,
`OHV_CylinderHead`); 2,249 names not found; 78 outputs name a pack (95 names,
17 of them spelled otherwise in the pool than in the source, none without a
file).

## 4. Decisions inside the spec's frame

**The two pool passes** are a transliteration of `declarations.md` 6.5 and 6.6
onto the emitter's trees. Step 4 reads the static type of a local from the
tree itself (`declare i` is followed by the carrier of its type) and the frame
from the method row (`this` unless static, then the parameters of the
descriptor in reverse); it needs no AST and no scope. The final index of an
operand is found by interning its content once more when the trees are
encoded, and the trees are encoded before CONS is written: an operand that
steps 4 and 5 had missed would then still get a valid index instead of a
dangling one. The tests assert that this never happens.

**`ClassBuild.pool_steps`** holds the length of the pool after each of the
five steps. `dump_class` prints it, the tests use it, and it is what
`poolwho.py` shows for a shipped class: `Trigger` gives `4 12 24 54 59`, the
ranges of the table in `declarations.md` 6.8.

**The reader** (`classfile.py`) exists because the driver's provider needs the
declarations of 2,921 class files and the package may not import `lib\tufa.py`.
It is as strict as that reader (the tests compare the two on 7,378 damaged
files) and skips the nodes of TREE unless asked. The spec's module table has
it as `sc_classfile.cpp` of the test tool.

**Which class answers for a name** in the driver is the rule of the tests'
`CorpusProvider`, re-implemented on the package's reader: every class file of
the tree is indexed by the name the class carries; a class file wins over a
source; of several files that define a name the first in index order stays,
except that a file named like the class wins over a blob inside another file;
a class only a source declares is declared from the first such source on
first use. The M6 test compares the two providers name by name (3,585 names, 0
differences), and `--compare --float auto` reproduces the M5 numbers through
the command line alone (section 5, finding 3).

**`declaration_text`** lists static fields before instance fields, which is
the order of a class file, and methods in the order of the `ClassInfo`. With
that the text is the same whether a class was declared from its source or read
from the class file compiled from it: 2,858 of 2,858 classes of the consistent
pairs (with the fields in `ClassInfo` order the text of 33 of the 2,705
oracle classes differed, in the order of its lines only). A field is
looked up by name, so the order between the two kinds carries no meaning; the
order of the methods does (the lookup scans them) and is kept.

**`Result.missed`.** A compilation depends on the classes it found and on the
absence of the ones it looked for in vain: a type name is tried in the package
of the source before the imports, so a class added later under such a name
changes the result. `UnitScope` records both answers of the provider; the
manifest stores the second as `not_found` (3.5 names per output on average).
The spec's manifest (2.10) has the consulted classes only.

**`--write-install`** follows spec 2.10: the class beside the source, with the
time stamp of the source and the build number of the options. It never
replaces a class file unless `--overwrite` is given and never deletes one.

**A text source holds bytes.** `compile_source` takes bytes, or a `str` with
one character per byte. A `str` with a character above U+00FF is `E_LEX` (see
finding 2).

## 5. Findings

1. **The numbers were reached on the first full run.** `t5_files.py` was
   written first and failed for want of `slrrc.compile_source`; with the two
   passes and the writer in place its first run on one source (`Trigger`) was
   byte-identical and its first run on the corpus printed 2649 / 2651, 2561,
   153 / 153, 0 / 15, 2703 / 2705 and 3454 / 3454. `t6_source_only.py` printed
   its line on its first run as well; the one failure there was a mistake of
   the test (two of its directories had the same name). What followed was
   hardening, and none of it moved a number. So there is no miss to report
   with a minimal example beyond the two expected ones (finding 3).

2. **A bug of the earlier steps, fixed.** A source handed over as `str` with a
   character above U+00FF (a euro sign in a string literal) passed the scanner
   and raised `UnicodeEncodeError` in the writer, where a utf8 entry is turned
   into bytes. The front half never encoded anything, so M1 to M4 could not see it.
   `__init__._parse` now reports `E_LEX` with line and column
   (`character U+20AC: a source is a sequence of bytes`); `front`,
   `emit_trees`, `declare_source` and `declared_names` share it. A case of
   `t_extra5.py`. The C++ compiler is given bytes and has no such case.

3. **The two files that differ are explained, not only expected.**
   `Center_Differential.class` and `Garage.class` hold a ref to a method as
   another version of the callee declared it (`impl_m4.md`, finding 2). The M5
   test now reads the missing method ref from the shipped file, gives the
   compiler a provider in which the callee declares exactly that
   (`Chassis.getBlock:()Ljava.game.parts.Part;`,
   `NetworkEngine.httpRequestAsync:(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.util.resource.GameRef;Ljava.lang.String;)I`),
   and compares again: **both files are byte-identical** (`-v` prints the
   lines). So every byte of all 2,651 files follows from the rules; what the
   install lacks is two declarations. The test still counts the two as not
   reproduced, because the install is what a compiler gets.

4. **The command line alone reproduces M5.**
   `python -I slrrc --root D:\SLRR-test --compare --float auto --strict` (7 s)
   prints `compared 2819 sources with their class files: 2802 identical (float
   setting A 2714, B 60, C 22, D 6), 17 different`: 2,649 + 153 identical; the
   17 are the 15 stale pairs and the two of finding 3. This run uses only the
   package: its own reader, provider, pack list and build numbers.

5. **`python -I -m slrrc` cannot work** from the compiler directory. Isolated
   mode (`-I`) sets `safe_path`, which keeps the current directory off
   `sys.path`, and it ignores `PYTHONPATH`; the interpreter answers `No module
   named slrrc`. Without installing the package into the interpreter there is
   no way around that. `python -I slrrc` (the package directory run as a
   script) works under `-I`, and `python -m slrrc` works without it; both are
   tested. `__main__.py` replaces the path entry of the package directory by
   its parent before it imports anything, so that no module of the package is
   importable as a top-level module.

6. **Another program writes into the corpus while the tests run.** Files under
   `D:\SLRR-test\save\career\` changed at 16:35 and the time stamp of the root
   directory at 16:40 on the day of this work, and a count of the files named
   like class files, sources, packs, `*.json` or `*.tmp*` gave 6,524 in one
   run of the M6 test (before and after its compile alike) and 6,522 in all
   others. None of it is from this work: the driver and the M6 test were run
   again afterwards, and the root and every file under it kept their time
   stamps. The tree check of M6 therefore looks at the class files, sources
   and packs and at names an output would have, not at every file.

7. **What the corpus does not decide** in this step: nothing new. The two
   controls with 0 lost files are no choices (section 2).

8. **Counts.** Pool entries of the 635 outputs by step are not compared with
   an original, but the first three steps equal the declaration model for all
   of them (M3) and the last two equal the spike (M5). For the consistent
   pairs: 2,858 classes, 0 whose source declares anything else than its class
   file.

## 6. Deviations from the spec and from the task, with reasons

| Asked | Built | Reason |
| --- | --- | --- |
| a command line `python -I -m slrrc`, run from the compiler directory | `python -I slrrc ...` and `python -m slrrc ...` | finding 5: `-I` and `-m` of a package in the current directory exclude each other |
| spec section 3: `python -I slrrc\cli.py compile-missing --install <root> --cache <dir>` | `python -I slrrc --root <root> --out <dir> --only-missing` | one command with options instead of sub-commands; `--only-missing` is the task's name for it |
| "an output directory laid out as a cache (relative path of the source directory, Name.class)" | `CACHE\X\Name.class` for `X\src\Name.java` | the path of the class file in the install (spec 2.10, `spec\out\source_only_classes\`, `m6.sha256`); the `src` component is not part of it |
| a manifest with source path, size and hash, float setting, pack table version, classes compiled against | those, and: output size and hash, compiler version, build, strict, the classes in the file, the pack names used, the names not found, the origin of each class | what else the output is a function of; `not_found` closes a gap of spec 2.10 (section 4) |
| spec 2.10: cache at `%LOCALAPPDATA%\slrr-reverse\script-cache\<install key>\`, switches `SLRR_SCRIPT_*` | the directory is `--out`; no environment variable is read | those are the host's; the offline tool writes where it is told |
| spec 2.4: `write.py` on `lib\tufa.py` | `write.py` carries its own encoder | the package may not import `lib\` |
| module list of 2.4 | plus `classfile.py`, `install.py`, `__main__.py` | reader for the provider (the spec's `sc_classfile.cpp`), the I/O side of the two interfaces, the entry point |
| spec 2.6: `Result { ok, class_file, class_names, consulted, diagnostics }` | plus `missed`, `ast`, `classes` | the cache (section 4); dumps and tests |
| spec 2.6: a wrong option | `ValueError` for a float setting or a build number that does not exist | a mistake of the caller, not bad input |
| headline `M5 files 2649/2651 byte-identical` (2,561 with A alone); older `153/153`; stale `0/15`; CONS 2,703 of 2,705 | the same numbers in one line, plus `equal to the spike 3454/3454 sources` | one line per milestone |
| headline `M6 source-only 635/635 compiled`, hashes, layout rules, stack simulation, 767 / 767, 522 / 522; host: `635/635 cache files loaded` | the offline part in one line, plus the manifest and the driver checks; no host line | the host half needs the host |
| baseline: `m5.pass`, `m5_float.tsv`, `m5_older.pass`, `m6.sha256` | those, plus `m5_cons.pass`, `m5_spike.pass`, `extra5.pass` | the other parts of the headlines need lists of their own |
| per-file float table for the oracle tests | in the tests only (`m5_float.tsv`); the command line has `--float X` and `--compare --float auto` | a table of shipped files is no option of a compiler |
| - | `--check`, `--compare`, `--dump`, `--exclude`, `--overwrite`; `tests\t_extra5.py`, `tests\t5_controls.py` | checking an install without writing; the formats the C++ tool will print; tests of what the corpus cannot show |
| previous steps: "do not rewrite them" | `decl.py` +5 lines (`pool_steps`), `classes.py` +47 (`declaration_text`, `missed`), `packs.py` +4, `harness.py` (purity check; the entry point may name its own package) | additions the writer, the cache and the tests need; nothing was rewritten |

## 7. The two open points

Neither was guessed away.

**Float literals.** `Options.float_mode` and `--float` select one of the four
conversions; A is the default for a new compilation, and the 635 class files
of the cache use A (575 of their sources hold a float literal, 1,817 literals).
The oracle test takes the setting per file from `tests\baseline\m5_float.tsv`
and prints the number for A alone beside it: 88 files need B, C or D. If the
game turns out to use another conversion for those 635, at most 3 units in the
last place of some literals change, the cache is regenerated with `--float`,
and `m6.sha256` is re-baselined on purpose.

**Pack names.** The writer never decides a spelling: it interns what the
emitter got from the `PackNames` it was given. The command line passes
`TablePackNames(TABLE_940, the pack files of the tree)`; the manifest records
the table version and, per output, each pack as written and as spelled.

## 8. What remains

- **M6, host half**: the host looks into a cache directory and loads the 635
  files (`635/635 cache files loaded`). `out\class_cache\` is ready for it.
- **M7**: the C++ port; `--impl cpp` answers "not built" (exit 2).
  `--dump tokens | ast | trees | class` print the formats its tool has to
  match.
- **M8, M9**: the compiler in the engine; the exe read for the float
  conversion and the pack names.
- Small things left as they are:
  - the manifest is JSON; a host without a JSON reader needs a small one (the
    file uses objects, arrays, strings, numbers, `true`, `false`, `null`);
  - a cache entry whose source no longer compiles loses its manifest entry,
    the old class file stays in the directory;
  - the provider reads every class file of the tree at start (about 2 s in
    Python); it has no index file of its own;
  - what `impl_m4.md` section 8 lists (three limits of strict mode, duplicate
    field names, a type name that resolves in two places).

## 9. Notes for the C++ port

(Later, `impl_fix.md` section 5 replaces two statements of this section: the
reader keeps its private exception in C++, and the lists of a `Result` are
copied into it at one place, not "filled by `UnitScope::find`".)

- `write.py` ports function by function. `resolve_tree` needs a vector of
  descriptors indexed by local (grown on `declare`), the method descriptor and
  a flag for `this`; `frame_types` uses '' for `this`, a value no descriptor
  has. Keep the two interning statements of `Pool::ref` and `Pool::nat` in
  their order (class before nat, name before descriptor): 1,445 and 2,608
  files depend on it.
- Encode TREE into a buffer before CONS is written (`class_blob`), then emit
  the chunks in file order.
- A node is 3 bytes, or 7 with an operand; whether it has one is the kind of
  the `SymNode`, which agrees with the opcode table for every node the emitter
  makes (tested). The line is 16 bits; the scanner rejects sources beyond
  65,535 lines.
- `classfile.py`: `_Cursor` is a pointer pair with bounds checks; the private
  exception becomes an error return. `OP_HAS_OPERAND` is the high nibble of
  `tree_op_flags.inc` (1, 2 or 4).
- `declaration_text` must be reproduced to the byte if the host is to validate
  entries written by this tool: lines closed by LF, static fields first, text
  encoded one byte per character before hashing.
- `Result.consulted` and `Result.missed` are filled by `UnitScope::find`; a
  host cache wants both.
- `install.py` and `cli.py` are not ported: in the engine the provider answers
  from the loaded classes and the class loader (spec 2.6), and the cache is the
  loader's.
- `tests\t_extra5.py` holds a pool and a class written out by hand that need
  no corpus: the first things to run against the C++ writer.
