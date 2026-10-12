# The review of the reference compiler: 20 findings, reproduced and fixed

Written 2026-10-11. The reference compiler `slrrc` (milestones M1 to M6,
`notes\impl_m1-m3.md`, `impl_m4.md`, `impl_m5-m6.md`) was reviewed with one
question: what would a C++ port, a host engine and a person at the command
line trip over? The review returned 20 findings, none about a shipped class
that compiles wrongly. This note says, per finding, how it was reproduced, what
was changed, and which check now holds it; then what the port has to take from
it. Nothing under `lib\`, `analysis\`, `spec\`, in the corpus `D:\SLRR-test`,
in the game installs or in the slrr-reverse repo was changed; no exe and no
Ghidra was run.

## 0. Result

All 20 findings were reproduced; none had to be set aside. 19 are fixed in the
code, each with at least one check; one (2, the parser's exception) is settled
by a decision that is now written into the code and enforced by a check,
without rewriting the parser. Where a fix differs from what the review
proposed, section 3 says how and why. No byte of any output changed.

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I tests\run_tests.py m1 m2 m3 m4 m5 m6
```

prints (80 to 90 s, exit code 0):

```
M1 tokenise      3454/3454 sources, 852211 tokens
M2 parse         3454/3454 sources
M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
M4 trees         7568/7570 node for node (float setting per file; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources
M5 files         2649/2651 byte-identical (float setting per file; 2561/2651 with setting A alone); older builds 153/153; stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454 sources
M6 source-only   635/635 compiled through the cache (635 classes, 771 trees, 32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline; strict reader 635/635; layout rules hold; stack simulation holds (21523 nodes); structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver checks 22/22
baseline         0 lost
```

These are the lines of before, to the character. The four suites outside the
milestones (`python -I tests\run_tests.py extra extra4 extra5 review`, 2 min
25 s, exit code 0; all ten suites in one run just under 4 minutes):

```
extra            dialect 181/181 cases; deletions 3067/3068 rejected, lib 3067; mutations 9060 texts, 0 exceptions, 0 differences from lib
extra4           dialect 75/75 cases; trees 13/13 written out by hand; lookup 55/55; mutations 12560 texts, 0 exceptions, 0 differences from the spike
extra5           hand 3/3; outputs 512/512 runs well-formed; reader 2921/2921 class files equal to lib/tufa, 7378 damaged variants, 0 disagreements; details 0 failures; mutations 12560 texts, 0 exceptions, 0 differences from the spike
review           results 9/9; provider 43/43; parser 38/38; reader 13/13; numbers 18/18; floats 17/17; diagnostics 37/37; driver 30/30; gaps 21/21, 6/6 mutants caught, one token per line 3454/3454 sources equal to the spike; readme 3/3
baseline         0 lost
```

`review` is new (`tests\t_review.py`, 229 named checks in 10 groups, 28 s).

The cache was regenerated:

```
python -I slrrc --root D:\SLRR-test --out out\class_cache --only-missing
compiled 635 of 635 sources (635 classes, 693847 bytes, float setting A); wrote 635 class files and manifest.json (635 entries) to C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler\out\class_cache
```

The 635 class files are byte for byte the ones of before (and of
`spec\out\source_only_classes\`; their hashes are `tests\baseline\m6.sha256`).
In `manifest.json` one field differs in every entry, `compiler`: `slrrc 0.3`
became `slrrc 0.4`. Everything else is the same: 5,329 dependencies on 71
classes with unchanged declaration hashes, 2,249 names not found, no warning.
Two more runs wrote the same manifest bytes. `--compare --float auto --strict`
over the tree still gives 2,802 identical (A 2,714, B 60, C 22, D 6) and 17
different.

## 1. The findings

"Before" is the package as the review saw it (a copy was taken before the
first change); the reproductions are scripts run against that copy and again
against the fixed package. Check names are those of `tests\t_review.py`
(`python -I tests\t_review.py -v` lists all of them).

| # | Finding | Reproduced | What was changed | Held by (group of `review`) |
| ---: | --- | --- | --- | --- |
| 1 | `Result.consulted`, `missed`, `diagnostics` are complete only because Python shares one list between `front()`, the scope and the result | Before: the result list **is** the list of the scope. For `Axis_200S` it holds 5 names when `front()` returns and 15 after the emitter; it grows after `front()` for 3,449 of 3,454 sources (26,089 names), `missed` for 265; a source with an unknown identifier has 0 diagnostics when `front()` returns | `__init__.py`: one function, `_finish()`, copies the three lists into a result, once, at the end of the stage that returns it; `front()`, `emit_trees()`, `compile_source()` and `declare_source()` are built on `_front()` / `_emit()`, which append to one `Diagnostics` and one `UnitScope`. No result shares a list with the scope | results: 9 checks. The emitter is run by hand on a `FrontResult` and its lists do not grow; the full result holds what the emitter consulted, missed and reported |
| 2 | The parser unwinds with a private exception; about 130 call sites assume the call does not return | With `fail()` turned into "record and return": `UnboundLocalError` (no class keyword), `AttributeError` on `None` twice (unclosed block, statement before the first case label). 56 call sites never return, 47 return on success only, 27 return in default mode only; `classfile.py` has 23 raise sites of the same kind | **Decision, not a rewrite** (section 3): the port keeps the exception. Written into `parser.py` ("How the parser stops at an error") and `classfile.py`, and into the docstring of every such function. The reference stays as it is | parser: per module of the core, the exact set of (function, exception raised) and (function, exception caught); 12 error paths that end in one diagnostic and no tree |
| 3 | A pack for which `PackNames.canonical()` answers `None` compiles to `system.rpk` id 0 without a diagnostic, in both modes | `int r = some.pack:0x0000002Ar;` with a `PackNames` that answers `None`: ok, 221 bytes, no diagnostic, default and strict | `emit.py`: warning `W_PACK` with line and column at every such literal, in both modes (a warning, because the shipped `TimeAttackTest` must compile in strict mode). `packs.py`: the contract of `PackNames`: `None` means "the install has no file", never "not known", and is answered from the files, not from the packs that are open | diagnostics: 6 checks, among them `TimeAttackTest` in strict mode: byte-identical, with exactly its two warnings. `tests\harness.py` lists those two as the only diagnostics a shipped source may give |
| 4 | What a host must put into a `ClassInfo`, and how `find()` must behave, is not written at the interface | A provider that answers `p.base` with the class `p.Base`: ok, no diagnostic, `p.base` in the class file. Over all 3,454 sources with a memory-only provider: unchanged 0 sources with other bytes; without interfaces 3 (first `GameLogic.java`); fields without the static flag 2,574; methods without the static flag 0; methods reversed 0. `engine/include/jvm.hpp`: `JvmClass` has no interface list | `classes.py`: the contract as the docstring of `ClassProvider`, four rules (the name; what a `ClassInfo` holds; re-entry; one answer), with these numbers. `UnitScope.find()` checks what can be checked (`check_class_info`): another name, an empty name, a descriptor that is none give `W_CLASS` and the class counts as not found. `__init__.py` names the three entry points of the C++ header: `compile`, `declare`, `declared_names` | provider (43 checks, shared with 18 and 19). The exact name; the interfaces and the static flag each decide a ref; a provider written to rule 3 declares two sources that name each other (re-entered to depth 2, a stub served); every one of the 3,585 classes of the corpus keeps the contract |
| 5 | Two bounds checks of the reader are right only with unbounded integers: `p + n > end` wraps in 32 bits | `classfile.py` lines 107 and 262-263 before. Model: p 0x40, n 0xFFFFFFF0, end 0x200: `p + n > end` is true exactly and false in 32 bits | Every check compares the length with the bytes left: `n > end - p`, `csize > size - begin`, also the five with a constant. The rule is in the docstring ("Bounds") | reader: 13 checks. No comparison of `classfile.py` has a sum on either side; 8 length and count fields each set to 9 values near 2**32, among them the ones a 32-bit sum lets through: all rejected, by `lib\tufa.py` too |
| 6 | The four float conversions are right in Python, but the C forms of the docstring give other bits under a fused multiply-add, x87 arithmetic, another rounding mode | `tests\floatmodel.py` (exact fractions, every rounding written out) against `float_bits`: 0 differences in 70,964 cases (17,741 literal texts, the 15,954 of the corpus among them, 4 settings). The model under wrong builds: a fused multiply-add changes 75 literals under A and 157 under B; x87 evaluation as many; 24-bit precision control 75 under C and 5,230 under D. `engine/CMakeLists.txt` sets no floating point flag; the device is created without the FPU-preserve bit | `floatlit.py`: the C forms rewritten so that every product, sum and quotient is stored in a named variable; the pragma, the compiler flags and the reasons; `SELF_TEST`, 15 literals with their bits under each setting, and `self_test()`. The command line runs it at start and compiles nothing when it fails | floats: 17 checks. `SELF_TEST` equals the exact model; each of 9 wrong builds changes a row in every setting it touches (and in no other); `float_bits` equals the model on 363 literals |
| 7 | A run that fails part-way leaves class files without a matching manifest, and a temporary file | Three sources compiled, then changed, `m\B.class` held open: exit 2, `A.class` new with the old hash in the manifest, `C.java` never compiled, `m\B.class.tmp8064` left. A directory named `manifest.json`: three class files and no manifest | `cli.py`: the manifest is written once before anything is compiled (a directory where that fails is refused first); a class file that cannot be written, or a source that cannot be read, fails that one source and the run goes on (exit code 1, old file and old entry stay); the manifest is written again in a `finally`; `write_atomic` removes its temporary file | driver: 8 checks, with the file held open, with a directory in its place, with an unwritable manifest, with a source that is gone, with an interrupt behind the first source. After each: every entry describes the file that is there |
| 8 | A `manifest.json` that is valid JSON of another shape ends in a traceback | 8 of 18 shapes: `AttributeError` (`[]`, `null`, `12`, `"x"`), `KeyError` / `TypeError` after a class file was written, `RecursionError` for 100,000 nested `[` | `cli.py`: `manifest_fault()` takes a document over only when it is a manifest as this module writes it (format, `entries` with the 16 fields of an entry and their types, `origins`); everything else is replaced with a warning | driver: 2 checks. 20 shapes, none ends in a traceback, each is replaced with a warning; a manifest of this format is taken over |
| 9 | Text to a redirected stdout uses the code page with strict errors: `UnicodeEncodeError` for input the compiler accepts | `--dump tokens > file` for a source with the byte 0x80: exit 1, 0 bytes. A source named with U+0416 and `-v`: exit 1 after three class files, no manifest. An `--out` with U+0416: exit 1 behind the run | `cli.py`: stdout and stderr write a character their encoding lacks as a backslash escape; a dump to a file or a pipe is bytes, one per character of the source, LF line ends, for all four dump forms | driver: 6 checks. The four dumps through a pipe equal the dump functions byte for byte; U+0416 in a source name with `-v`, `--check -v`, `--dry-run`, and in `--out` |
| 10 | Six single-rule changes pass all nine suites | Each of the six applied to a copy of the package, all nine suites run: every headline unchanged, exit code 0, six times | `tests\t_review.py`: the six rules pinned by cases written out by hand (three trees with one token per line, a call with `true`, the five pool orders no shipped class decides, the scores 100 and 101); the six changes applied in memory, each must fail a case; and every source of the corpus rewritten with one token per line against the spike | gaps: 21 checks, 6 of 6 mutants caught, 3,454 of 3,454 sources. With a mutant in place the rewritten sources alone show 54, 3,446 and 509 sources with other bytes for the three line rules |
| 11 | Strict mode compiles a class that is its own superclass, an instance field in a static method, a raw NUL in a string | All three: `checked 3 of 3`, no diagnostic, default and strict. Also two classes of one source that extend each other, and a ring through a class of the provider | `classes.py`: the superclasses of every class of the source are walked once after the declarations; a class that is its own superclass is `E_TYPE` in both modes. `emit.py`: an instance field named in static code is reported through `unverified`. `scanner.py`: a control character other than TAB in a string literal likewise | diagnostics: 26 checks (rings of one, three, and through the provider, with and without a body that looks a member up; five static cases flagged, five not; NUL, 0x01, DEL, a lone CR flagged, TAB and a comment not) |
| 12 | An unreadable class file beside a source hides the class: the file is ignored and the source is not used | `Base.java` and `Sub.java`: 2 of 2. With an empty `Base.class`: `E_TYPE: unknown superclass Base`, exit 1; `--only-missing` does not compile `Base` either | `install.py`: a source whose class file the reader rejects (or that cannot be opened) is treated as a source without a class file. `--only-missing` selects it too (`SourceFile.lacks_class`) | driver: 8 checks, with an empty, a truncated and a text file, with `--only-missing`, with a class file that disappears |
| 13 | `RecursionError` can leave `compile_source()` when the caller is deep in the stack | Limit 1000, 150 additions: `RecursionError` from a caller 840 frames deep; 185 additions from 820 | `__init__.py`: every stage is under a guard that reports `E_LIMIT` | diagnostics: five sources from callers 560 to 975 frames deep in steps of 5: `ok` or `E_LIMIT`, never an exception. (A caller with fewer than about 8 frames left still gets `RecursionError` from the call itself; no callee can change that) |
| 14 | Loops that count down to zero and -1 sentinels need signed integers; the data structures are `uint32_t` | 59 places in the core: `while k >= 0`, `return -1`, `x < 0`, `= -1`, negative subscripts | Every countdown is `k = n; while k > down_to: k -= 1; ...`; "nothing" is `NONE` (an index), `NO_FIT` (a score) or 0 (an offset behind something); searches answer the length. No negative number is left in the core, subscripts included | numbers: 18 checks. Per module: no negative number and no comparison with zero that is always true for an unsigned number, two listed places apart (`scanner._find`, the range check of `Options.build`) |
| 15 | `Node.items` grows while other nodes are made, and `done()` is called more than once on a node; node numbers depend on the order of creation | Instrumented parser: the label list of a switch group is closed once per label, a local declaration statement twice; items are appended after the first `done()` | `parser.py`: every node is closed exactly once, after its last child and item (the label list when the first statement of the group arrives; a local declaration by its caller). The docstring says in which order nodes are made and how the port fills `Ast::lists` | parser: every node of 178 synthetic and 384 shipped sources is closed once, after its children and items, and the flat list a port would build holds its items as one slice; the dump of two trees (14 and 40 nodes) written out by hand |
| 16 | The answer for deeply nested input depends on the recursion limit, and the command line runs with another limit than the tests; the height limit is reported at line 1, column 1 | Command line, 180 nested parentheses: `E_LIMIT` at 3:174 ("nested too deep"); under the limit of the tests the same source compiles. 196 parentheses under the raised limit: `E_LIMIT` at 1:1 | `cli.py` raises the recursion limit to the 20,000 of the tests. `parser.py` reports the height limit at the first token of the deepest node below the node that exceeded it | parser: 7 checks (196, 197, 199 parentheses at the innermost operand; an operator chain at its first operand). driver: 6 checks (150, 180, 190 compile through the command line; 196 and 205 give the diagnostic of the library) |
| 17 | One diagnostic depends on what an earlier method happened to resolve | `void f(int Util) { Util = 1; }` and `void g(int a) { a = Util.abs(a); }` in strict mode: compiles in one order, `E_UNSUPPORTED` in the other | `emit.py`: "a variable and the name of a class" is decided from the source alone: a class the source declares, or a name it writes as a type anywhere (`UnitScope.type_names`). Narrower than before in one case, see section 3 | diagnostics: four pairs of methods in both orders, the same diagnostics each time |
| 18 | Descriptors that come from the provider are used unchecked: one case raises through `compile_source` | A field with the descriptor `""` passed as an argument: `IndexError`. `(L`, `([` pass only because Python clamps slices | Fixed with 4: a class with a descriptor that is none is not taken. `split_params`, `class_of_desc` and `type_score` no longer rely on clamping or on negative subscripts | provider: 19 kinds of damaged class are refused with `W_CLASS` and no exception, 6 kinds are accepted; `desc_kind` on 33 texts |
| 19 | `UnitScope.find()` does not remember the answer of the provider | 72,748 calls of `provider.find` for 57,353 distinct (name, compilation) pairs, 25,013 of them for names that do not exist; `Garage.java` 600 calls for 349 names, one name 61 times; 9,952 calls of `canonical` for 126 packs | `classes.py`: the answer for a name, found or not, is kept for the compilation. `packs.CachedPackNames` does the same for pack names. Rule 4 of the contract says that a negative answer must be cheap. Now: 57,369 calls for 57,369 pairs; `Garage.java` 349 for 349; 1,311 calls of `canonical` | provider: 4 checks, on the synthetic sources, on `Garage.java` and on every 25th shipped source |
| 20 | The README lacks `SLRR_CORPUS`, how `notes\pairs.json` is made, how to build a provider, the recursion limit, the Python version | Checked as written | `README.md`: those five, the two contracts in brief, the new diagnostics, what the command line does when a run fails. "About 2 minutes" for the extra suites is now "2 to 3 minutes" | readme: 3 checks; the library example of the README is run and must give the shipped `Trigger.class` |

## 2. How it is known that the new checks notice

A check that passes proves little unless it would fail without the fix. So
each fix was taken out again, one at a time, in a copy of the final package,
and `review` was run against the copy: **25 reverts, each loses at least one
check of the baseline** (between 1 and 34; the run names them, `LOST
review.pass: ...`, and exits with 1), none passes. The reverts: the result lists shared
again; the lists copied when the front half ends; the pack warning; the check
of a `ClassInfo`; the kept answers of the provider and of the pack names; the
sum in the reader's bounds check; setting A with an unrounded product; the
temporary file; the manifest written first; the shape of a manifest; strict
stdout; text dumps; the ring check, eager and at all; the static check; the
control characters; the hidden class; the stack guard; one signed loop;
`done()` twice; the unit token for the height limit; the recursion limit of the
command line; the cache-dependent diagnostic; a second raise site in the
parser.

For finding 10 the suite does this itself on every run: the six mutants of the
review are applied in memory and each must fail a case ("6/6 mutants caught").

Expectations written out by hand before the code was run, and right on the
first run of the suite: the two tree dumps of the parser group (node numbers,
token ranges, children and items of 54 nodes), the four trees and five pool
orders of the gaps group, the scores, the tree of the numbers group. What did
fail on the first run were five checks that were wrong themselves, not the
compiler: a search text, two thresholds, a lint that also took the bounds of a
slice for a sum, and two raise sites in `dump.py` (the unparser of the M2 test,
for a node kind it has no case for) that the list of expected exceptions
lacked.

The controls of the earlier steps were updated for the changed text and run
again: `t4_controls.py` and `t5_controls.py` lose the trees and files they lost
before (323 for the parameter order, 3,763 for the assignment order, 2,608 /
2,611 / 2,602 files for the three pool orders, and so on; the zeros are the
same zeros).

## 3. Decisions, and where a fix is narrower than proposed

**Finding 2: the port keeps an exception.** The review offered two ways: one
C++ exception type thrown in `fail()` and caught in `parse()`, or a `failed`
flag tested behind every call, with the Python rewritten first to show the
checks. The first was taken. Reasons: the engine is built with exceptions (its
pack reader, `engine/Core/Platform/rpak.cpp`, uses `try` / `catch`, and its
CMake file sets no flag against them); the exception never crosses the
interface of the compiler; and the flag would add a test behind about 130 call
sites and behind every call of every parsing function, each one a place to
forget. What changed is that the assumption is no longer silent: the module
text says which functions never return and what relies on it, every such
function says so in its docstring, and a check fails when a `raise` or an
`except` appears anywhere else in the core. `classfile.py` follows the same
rule (`impl_m5-m6.md` had said "the private exception becomes an error
return"; that is withdrawn).

**Finding 11: the ring is looked for at the declarations.** The superclasses
of every class of the source are walked once when the declarations are done.
That asks the provider for the whole chain also where no body would have: the
list of consulted classes gets longer for 5 of the 3,454 sources (16 names in
all), for none of the 635. A cache entry of those five depends on a few classes
more than its bytes do, which costs a recompilation at most. Looking only when
a member is looked up would have left a ring unreported in a class whose bodies
name no member. The test of the front half that pinned the list of consulted
classes was changed with it (`t_extra.py`: `java.lang.Object`, the superclass
of the superclass, is now on it).

**Finding 17: the check is decided from the source, and sees less in one
case.** A variable is "also the name of a class" when the source declares that
class or writes the name as a type anywhere (in a declaration, a `new`, a cast,
`instanceof`, `extends`). A class that the source uses only as the head of a
name, `Util.abs(a)`, is no longer seen: finding it without depending on the
order of the methods would mean asking the provider about every variable, and
every such question is a name the cache would then depend on. So the pair of
the review now compiles without a diagnostic in both orders. This joins the
limits of strict mode listed in `impl_m4.md` section 4.

**Finding 4: a constructor may come with a return part.** The review asked for
the contract to say that a constructor descriptor has none (`(I)`, as the pool
holds it; the engine's loader rewrites it to `(I)V`). Refusing `(I)V` was
tried: a provider that hands it over then fails 3,336 of 3,454 sources. The
compiler never reads the return part of a constructor, so it is accepted
instead, and `declaration_text()` writes a constructor without it: with such a
provider all 3,454 sources give the same bytes and the same declaration
hashes. Every other descriptor has to be the text of the pool.

**Finding 6: the flags are named, not set.** There is no CMake file of the
port yet; `floatlit.py` names the flags and the pragma, and `self_test()` is
what makes a wrong build visible whatever the flags are.

**Finding 13.** A caller with fewer than about 8 frames left under the
recursion limit still gets `RecursionError`: the call itself needs them.

**New diagnostics.** `W_PACK` (finding 3) and `W_CLASS` (findings 4, 18) are
warnings in both modes. `W_CLASS` and the `E_LIMIT` of the stack guard have no
place in the source and carry line 1, column 1; what follows a `W_CLASS` (an
unknown type or superclass) points at the use.

**Version.** `slrrc.VERSION` is `0.4`: some sources that compiled now do not (a
class that is its own superclass) or compile with other diagnostics, and the
version is part of every cache entry. No output byte changed.

## 4. What changed for a caller

- `FrontResult`, `Result` and `DeclareResult` hold lists of their own.
  `consulted` can hold the superclasses above the classes the source names
  (section 3).
- A `ClassProvider` is asked once per name and compilation, a `PackNames` once
  per written pack name; both contracts are in the docstrings.
- Two new warning codes; `E_TYPE` also for a class that is its own superclass.
- `floatlit.SELF_TEST`, `floatlit.self_test()`; `packs.CachedPackNames`;
  `classes.check_class_info`, `desc_kind`, `type_desc_end`, `params_part`,
  `NO_FIT` (was -1); `emit.Emitter.lookup_local` answers `emit.NONE` (was -1).
- Command line: exit code 1 also for a source that could not be read or
  written; the summary line gains `, N NOT READ OR NOT WRITTEN` in that case
  and is otherwise unchanged; `--only-missing` also takes a source whose class
  file the reader rejects; a dump to a pipe is bytes with LF line ends; the
  recursion limit is raised; the float self test runs first.

## 5. For the C++ port

This replaces four statements of the earlier notes (`impl_m1-m3.md` section 7:
"or an error flag checked in the loops"; `impl_m4.md` section 9: "`items` is a
slice of `Ast::lists`" and "compile with `/fp:precise` and SSE2";
`impl_m5-m6.md` section 9: "the private exception becomes an error return" and
"`Result.consulted` and `Result.missed` are filled by `UnitScope::find`").

- **Results.** Copy or move `consulted`, `missed` and `diagnostics` into the
  result at one place, when the stage that returns it has ended
  (`__init__._finish`). A copy made when the front half ends lacks what the
  emitter adds: classes for 491 of 3,454 sources, and every diagnostic of the
  emitter.
- **Errors inside the parser and the reader.** One exception type each, thrown
  where `fail()` and `_Bad` are raised, caught in `parse()` and `read()`. Do
  not turn `fail()` into a function that returns.
- **The provider.** Take `ClassInfo` from a strict reader or from the source,
  not from `JvmClass` as it is: it has no interface list (3 sources compile to
  other bytes without one). The compiler checks the name and the descriptors
  and reports `W_CLASS`. The header needs `declare()` and `declared_names()`
  beside `compile()`; a provider that declares sources answers a class that is
  being declared with an empty `ClassInfo` (`install.InstallProvider.find` is
  the model, about 30 lines).
- **Pack names.** `canonical()` answers "no file" only from a listing of the
  pack files, never because a pack is not open.
- **The reader.** `n > end - pos`, never `pos + n > end`.
- **Numbers.** Everything is unsigned; the reference has no loop that counts
  below zero and no -1. The two places that compare with zero are named in
  `tests\t_review.py` (`SIGNED`).
- **Floats.** The C forms of `floatlit.py` with every intermediate result in a
  named variable, the contraction pragma, SSE2. Run the 15 rows of `SELF_TEST`
  at start-up and refuse to write class files when one differs.
- **The tree.** Number the nodes in the order of the reference's `node()`
  calls (the docstring of `parser.py` lists which nodes are made after their
  first part); close every node once; collect the items of a node in a vector
  of the parsing function and copy them into `Ast::lists` in `done()`. The two
  dumps written out by hand in `tests\t_review.py` (`AST_SMALL`, `AST_SWITCH`)
  need no corpus: the first things to run against the C++ parser, with the
  trees of the gaps group for the emitter.
- **Nesting.** One limit, `MAX_DEPTH`, reported at the deepest node
  (`Parser.deepest`, a vector parallel to `heights`). The answer of the port is
  the answer of the reference under a raised recursion limit, which is what the
  command line gives.
- **Dumps.** Bytes, LF line ends; a token dump holds the bytes of the source.

## 6. Files

| File | State | Content |
| --- | --- | --- |
| `slrrc\__init__.py` | changed | `_front`, `_emit`, `_finish`; the stack guards; the entry points of the C++ header; version 0.4 |
| `slrrc\classes.py` | changed | the contract of `ClassProvider`; `check_class_info` and the descriptor functions; `UnitScope.find` asks once; the ring check; `type_names`; unsigned loops and `NO_FIT` |
| `slrrc\parser.py` | changed | how the parser stops; nodes closed once; the order of the nodes; `deepest` |
| `slrrc\emit.py` | changed | `W_PACK`; instance field in static code; the class-name check; unsigned loops and `NONE` |
| `slrrc\scanner.py` | changed | control characters in a string; `_find`, `column_at`, `float_digits` without -1 |
| `slrrc\classfile.py` | changed | bounds by subtraction; the two rules in the docstring |
| `slrrc\floatlit.py` | changed | the C forms, what breaks them, `SELF_TEST`, `self_test()` |
| `slrrc\packs.py` | changed | the contract of `PackNames`; `CachedPackNames` |
| `slrrc\diag.py`, `write.py` | changed | `W_PACK`, `W_CLASS`; two loops |
| `slrrc\install.py` | changed | a rejected class file does not hide its source; `lacks_class` |
| `slrrc\cli.py` | changed | the manifest (shape, written first and last), a failing source, output encoding, dumps as bytes, the recursion limit, the float self test |
| `slrrc\decl.py`, `pool.py`, `dump.py`, `__main__.py` | unchanged | |
| `tests\t_review.py` | new | the suite `review`: 229 checks, the six mutants, one token per line |
| `tests\floatmodel.py` | new | the float conversions in exact arithmetic, and nine wrong builds |
| `tests\harness.py` | changed | `EXPECTED_DIAGNOSTICS`, `surprises()`: the two warnings of `TimeAttackTest` |
| `tests\t4_trees.py`, `t5_files.py`, `t4_controls.py`, `t5_controls.py` | changed | use `surprises()`; the control texts follow the code |
| `tests\t_extra.py` | changed | one expectation: the consulted classes of the front half (section 3) |
| `tests\run_tests.py` | changed | `review` |
| `tests\baseline\review.pass`, `review_lines.pass` | new | the 229 checks; the 3,454 sources that pass with one token per line. No other baseline file changed |
| `README.md` | changed | finding 20 |
| `notes\impl_m1-m3.md`, `impl_m4.md`, `impl_m5-m6.md` | one sentence added | at the head of "Notes for the C++ port": which statements section 5 of this note replaces |
| `out\class_cache\` | regenerated | 635 class files (the same bytes), `manifest.json` (compiler 0.4) |

## 7. What stays open

The two open points are as they were: the float conversion is selectable with
A as the default and the per-file table only in the tests; pack spelling comes
only through `PackNames`. Both need the exe.

Also unchanged: the host half of M6, the C++ port (M7; `--impl cpp` answers
"not built"), and the small items of `impl_m5-m6.md` section 8. Two things the
review touched and this step did not settle: a cache entry whose source no
longer compiles loses its manifest entry while the old class file stays in the
cache directory, and strict mode does not flag a variable named like a class
that the source uses only as the head of a name.
