# Implementation step M4: the emitter of `slrrc`

Written 2026-10-11. Second implementation step of `notes\compiler_spec.md`:
milestone M4, the trees. It builds on the front half of `notes\impl_m1-m3.md`
(scanner, parser, classes, pool, declarations) and adds the emitter, the float
literal conversions, the pack names and the two member lookups. Nothing under
`lib\`, `analysis\`, `spec\`, in the corpus `D:\SLRR-test`, in the game
installs or in the slrr-reverse repo was changed; no exe and no Ghidra was run.

## 0. Result

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I tests\run_tests.py m4
```

prints (20 s, exit code 0):

```
M4 trees         7568/7570 node for node (float setting per file; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources
baseline         0 lost
```

These are the two numbers the spec names for M4 (7,568 of 7,570 with the float
setting chosen per file, 7,396 with setting A alone), reached for the code
under test, with three more counts in the same line. M1 to M3 still print their
lines, and the two tests outside the corpus pass
(`python -I tests\run_tests.py m1 m2 m3 m4 extra extra4`, 2 minutes, exit 0):

```
M1 tokenise      3454/3454 sources, 852211 tokens
M2 parse         3454/3454 sources
M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
M4 trees         7568/7570 node for node (float setting per file; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources
extra            dialect 181/181 cases; deletions 3067/3068 rejected, lib 3067; mutations 9060 texts, 0 exceptions, 0 differences from lib
extra4           dialect 75/75 cases; trees 13/13 written out by hand; lookup 55/55; mutations 12560 texts, 0 exceptions, 0 differences from the spike
baseline         0 lost
```

The package imports nothing from `lib\`, `analysis\` or `spec\` and nothing
outside the standard library (`struct` in `floatlit.py` is the one standard
module it uses). `run_tests.py` checks that before every run, and `t_extra4.py`
runs the emitter in an interpreter of its own with only the package on the path.

No file is special-cased anywhere. The two trees that do not come out equal are
the two the spec predicts (section 5, finding 2).

## 1. What was built

### The package `compiler\slrrc\`

| File | Lines | State | Content |
| --- | ---: | --- | --- |
| `emit.py` | 1,220 | new | The emitter. `emit_class(scope, build, packs, float_mode) -> [Tree]`, one tree per tree owner of the rows. Class `Emitter` with one method per statement form and per family of expression; op bytes, kinds of expression, `SymNode`, `Operand`, `Tree`. |
| `floatlit.py` | 107 | new | `float_bits(mode, int_digits, frac_digits)`: the four conversions A to D in explicit IEEE operations. |
| `packs.py` | 232 | new | Interface `PackNames.canonical(written_path) -> file name or None`; `TablePackNames(table, files)`; `TABLE_940`, the 126 spellings of the shipped pools; `pack_path()`. |
| `classes.py` | 515 | +267 | `class_chain`, `find_field -> FieldHit(field, owner)`, `lookup_method -> MethodHit(method, score, tie)`, `find_method`, `has_method`, `type_score`, `distance`, `split_params`, `class_of_desc`; three caches in `UnitScope`. |
| `dump.py` | 437 | +109 | `dump_tree`, `dump_trees`, `operand_text`, `quoted`: one line per node, operands by content. |
| `__init__.py` | 197 | +48 | `emit_trees(source, name, provider, packs, options) -> FrontResult`: `front()` plus the emitter. |
| `decl.py` | 381 | +1 | `ClassBuild.trees` (empty until the emitter has run). |

Unchanged: `scanner.py`, `parser.py`, `pool.py`, `write.py`, `diag.py`. No bug
was found in the code of the first step; nothing there was rewritten.

The emitter does no file access and keeps no state between trees except the
caches of the scope (per compilation). Bad input never raises: an unknown name
is a diagnostic and the walk goes on, so that one compilation reports every
unknown name of a source; a source with an error has `ok == False`.

### The tests `compiler\tests\`

| File | Lines | Content |
| --- | ---: | --- |
| `t4_trees.py` | 394 | milestone M4 |
| `t_extra4.py` | 1,524 | the emitter outside the corpus (`run_tests.py extra4`) |
| `t4_controls.py` | 191 | control runs: one rule changed, trees lost (not a milestone, 100 s) |
| `gen_pack_table.py` | 56 | prints `TABLE_940` from the shipped pools |
| `harness.py` | +40 | `packs()`: the `PackNames` of the tests; `pack_files()`; the provider collects the pack spellings of the pools |
| `run_tests.py` | +8 | `extra4` |
| `baseline\` | | `m4.pass` 7,568 items `class file#blob#tree`; `m4_other.pass` 157 (154 trees of older builds, 3 of stale pairs); `m4_spike.pass` 3,454 sources; `extra4.pass` 143 |

## 2. How the tests decide

**M4** (`t4_trees.py`, 20 s). Every source is compiled by
`slrrc.emit_trees(text, name, CorpusProvider, packs, Options(strict=True))`.
The class file of the source under test never reaches the compiler: its classes
are declared from the source, the provider answers for the others.

A tree passes when it has as many nodes as the shipped tree and every node has
the same op byte, the same line and the same operand. An operand that is a
number (local index, count, int value, float bits, kind of expression, jump
offset) must be equal; an operand that names a pool entry is compared through
what it names (the text; the class name; class, member name and descriptor;
pack file name and local id). The order of the pool is M5.

- A diagnostic of any kind fails every tree of its source, so the headline also
  says that no shipped source holds a construct outside the proven subset, as
  the emitter sees it (section 4).
- **Oracle pairs** (2,651 files, 7,570 trees): compared with the float setting
  that `tests\baseline\m5_float.tsv` names for the file, A for a file it does
  not list (2,563 files A, 60 B, 22 C, 6 D), and a second time with A for all.
  7,568 and 7,396 trees. By owner: 2,331 field initialisers, 2,745
  constructors, 2,492 methods.
- **Older builds** (153 files): 154 of 154 trees, setting A.
- **Stale pairs** (15 files): 3 of 18 trees. A control: the comparison is not
  lenient.
- **The spike** (all 3,454 sources, the 635 without a class included): the trees
  equal the ones of `spec\spike_compiler.py` node for node under setting A,
  8,513 of 8,513 trees. This is the part of the spec's target for M4 and M5
  ("equality with the spike for every source") that exists before the pool
  passes do.
- The high-water mark of the emitter (`Tree.max_locals`) equals the locals word
  `decl.locals_word()` computes from the declarations, for every method tree of
  every source.
- `floatlit.float_bits` equals `spec\float_closed_forms.py` for the 15,954
  distinct float literal texts of the corpus under each of the four settings.
- `packs.TABLE_940` equals the set of pack spellings of all shipped pools.
- With `-v` the test lists the rules the oracle sources exercise fewer than ten
  times (spec section 5): `&=` 1, `^=` 1, `^` 1, `~` 1, `continue` 1, a `for`
  with condition and without update 1, `<<` 2, `do` 3, `?:` 5, `>>` 6,
  `default` 6, `/=` 7; and `true` / `false` 10, `this(..)` 13, `return;` 32,
  static methods with code 70.

**extra4** (`t_extra4.py`, 23 s):

- *dialect*, 75 small sources with the codes expected in default and in strict
  mode: the proven subset (no diagnostic), every construct of section 4, names
  and members that resolve nowhere, `break` and `continue` out of place.
- *trees*, 13 trees of 7 sources **written out by hand** from the rules of
  `statements.md` and `expressions.md`, as the dump of `slrrc\dump.py`: every
  statement form, every family of expression, the name and call forms, lines
  built with lookahead, jump offsets, locals, and the defaults of appendix B
  (`continue` in `while` / `do` / `for(;;)`, a group that falls through, two
  labels on a group, `default` first, an empty group, a compound assignment as
  a value, `new T(..);`, `%=`, `;`). All 13 agreed with the emitter on their
  first run.
- *lookup*, 43 calls of `classes.find_method` and 12 of `classes.find_field`
  against the score table of `expressions.md` 6.3, one case per row (identical
  type, int for float, float for int, int or float for String, null, subclass
  steps, interface, array for Object, the superclasses, a tie, an unknown type,
  an inheritance loop).
- *details*: the four float settings through `Options.float_mode` (0.00065
  gives `3A2A64C1`, `3A2A64C4`, `3A2A64C1`, `3A2A64C3`, the values of
  `spec\out\float_files.txt`), A as the default, `ValueError` for a fifth; pack
  names as written, from the table, from the table over a file list, from a
  file list alone; the classes the emitter looks at are in `consulted`; a class
  of the source wins over the provider; the package alone in an interpreter of
  its own.
- *mutations*: every 11th source, 20 lines moved or copied and 20 token edits
  each (fixed seed), compiled in default mode by the package and by the spike.
  12,560 texts, no exception; 3,305 texts both compile, with the same trees;
  for every accepted text the high-water mark equals the locals word. Where
  only one of the two compiles the text the reason is one of three: a type name
  that resolves nowhere (the package writes `L;` with `W_TYPE`, the spike
  raises) or the import of a single class (the spike has no rule), together
  149 texts; or a construct the package refuses on purpose, 216 texts.

**Controls** (`python -I tests\t4_controls.py`, 100 s, not part of a milestone
run). Each control is a textual change of one rule in `emit.py` or
`classes.py`, compiled in memory; the oracle pairs are emitted with it and
compared as M4 does. It shows what the 7,568 are worth, and which choices the
corpus does not decide:

| Rule changed | Trees lost of 7,568 |
| --- | ---: |
| the target of an assignment before its value | 3,763 |
| a field ref always names the declaring class | 3,575 |
| a `return` ends the scope of the locals it releases | 3,288 |
| a field ref always names the type of the receiver | 2,030 |
| an `if` without `else` costs no lookahead token | 1,456 |
| the array before the index | 573 |
| parameters numbered in source order | 323 |
| the implicit super call on the line of the `{` | 172 |
| the path header of a call on the line of the `)` | 99 |
| the lookup takes the first method that accepts the arguments | 78 |
| `break` without releases | 18 |
| the releases of a case group on the line of its last token | 13 |
| the outermost local of a name wins | 11 |
| a binary operator on the line of the last token of its right operand | 8 |
| `END(statement)` without the dual context | 3 |
| the type carrier of `new` on the line of the `)` | 1 |
| the else-branch does not inherit the dual context | **0** |
| of equal scores the method met last stays | **0** |
| a parenthesised literal or name counts as a typed argument | **0** |
| an assignment on the line of the last token of its value | **0** |
| the nodes of a declaration on the line of the declared name | **0** |

A regression is caught: with one of these changes applied in memory,
`run_tests.py m4` prints the lost items (`LOST m4.pass: ...`), counts them in
the baseline line and exits with 1.

## 3. Decisions inside the spec's frame

**The output** follows spec 2.5. `SymNode(op, line, kind, value)`: `op` is the
op byte of the class file; `kind` says what `value` is (`K_NONE`, `K_IMM`,
`K_UTF8`, `K_CLASS`, `K_REF`, `K_RID`); for the last four `value` indexes
`Tree.operands`, a list of `Operand(text, name, desc, number)` that holds the
pool content. `K_LABEL` exists only while a tree is being emitted: `finish()`
turns every label into the offset in nodes relative to the jump node.
`Tree.max_locals` is the high-water mark of the live locals. The M5 passes need
nothing else: they walk the nodes and read the types of the locals from the
`declare` / `type` pairs.

**The emitter** is one class, `Emitter`, that lives for one class of the source
and is reset per tree. Its state is what spec 2.5 lists: the live locals as two
parallel lists (name, descriptor), the stack of enclosing loops and switches
(`Flow`: break label, continue label, number of locals at the entry), the
labels. Every `put_*()` takes the index of the token whose line the node
carries; the docstring of `emit.py` has the table per kind of node.

**Names.** `walk(tokens, count)` resolves a dotted name identifier by
identifier into `Segment`s (local, field ref, class) and returns the type
reached. A FIELD node whose target is a name is part of that name; any other
target is computed and the member is fetched by name. The parser left this
undecided on purpose (`a.b.c` is `FIELD(FIELD(NAME a, b), c)`).

**Lookups** live in `classes.py`, as the spec's module table says. They go
through `scope.find()`, so every class the emitter looks at is recorded in
`consulted`. Per compilation the scope caches the superclass chain of a class,
a name index of its fields and its methods by name in scan order; the
`ClassInfo` of the provider is not touched.

**Diagnostics instead of exceptions.** After an error the emitter emits nothing
for the construct and goes on. Labels that were never placed (possible only
after an error) point behind the tree. A `RecursionError` is caught in
`emit_trees` and becomes `E_LIMIT`: a net for the Python reference only. The
tree height is bounded by `parser.MAX_DEPTH`, which costs the emitter at most
two interpreter frames a level, fewer than the parser needed for the same
source. When an error is reported the result carries no trees.

**Pack names.** `PackNames.canonical(path)` gets the pack in path form
(`cars\racers\duhen.rpk`: the written name with backslashes and `.rpk`) and
returns the spelling for the pool, or `None` when the pack has no file; the
emitter then writes `system.rpk` with local id 0. `TablePackNames(table,
files)`: no file, `None`; in the table, the spelling of the table; otherwise
the spelling of the file; and when the host gives no file list, the table and
then the name as written. The tests use `TABLE_940` over the `.rpk` files of
the corpus, which is the rule of the spike.

**The dump** (`dump.dump_tree`) is the format the C++ tool is to print for
`--dump trees`: `index L<line> mnemonic [operand]`; the kind of an `expr` /
`stmt` by name, a jump offset with its sign, float bits in hex, other numbers
unsigned decimal, texts in quotes with `\\ \" \n \t \r \xHH`.

## 4. Constructs outside the proven subset, as the emitter sees them

The parser flags what it can see (`impl_m1-m3.md` section 4). The emitter flags
the rest; each row is a case of `t_extra4.py`, and no shipped source triggers
any of them (M4 runs in strict mode). "Appendix B" marks the rows the spec
decides; the others were decided here from the "unknown" paragraphs of the two
notes and are marked **new**.

**Warning `W_UNVERIFIED`, error `E_UNSUPPORTED` in strict mode**

| Construct | Compiled as | Source of the decision |
| --- | --- | --- |
| `continue` in a `while`, in a `do`, in a `for` without update, inside a `switch` in a loop | jump to the condition (`while`), to the condition test (`do`), to the closing jump (`for`); the enclosing loop for a `switch`; releases down to the loop's mark | appendix B |
| compound assignment used as a value | the `expr` form of the same kind | appendix B |
| a call without receiver whose name is a local in scope | a call (the local is ignored) | appendix B |
| ... or a field of the class or a superclass | a call | **new**: `expressions.md` 9.1 counts both, no source has either |
| a method with a return type that does not end in `return`; a void method or constructor that ends in `return;` | implicit return by the kind of method, whatever the last statement is | appendix B |
| argument `true`, `false`, `(x)`, `!x`, `~x`, `?:`, `instanceof`, `new T[n]` in a call for which the receiver class has a method of that name and argument count | `true` / `false` typed (int), the others untyped: the bare name | appendix B, narrowed: where nothing could be resolved these arguments occur in shipped sources and the call ends in the name |
| `return e;` in a void method; `return;` in a method with a return type | the nodes of the statement as written | **new** (0 in all sources) |
| `this` in a static method, in a field initialiser | `local 0` | **new** |
| a parenthesised name as the receiver of a member or of a call, `(a).b` | a computed receiver: `getfield` / `call_on` by name | **new**: `expressions.md` 5.4 "unknown" |
| `length` of a computed value, `g().length` | `getfield "length"` | **new**: `expressions.md` 5.4 "unknown" |
| a local used in its own initialiser, `int k = k + 1;` | the new local (it exists before its initialiser is compiled) | **new**: no shipped initialiser names its variable |
| a name that is a variable and also a class name the source uses | the variable | **new**: `expressions.md` 5.2, the place of class names in the order is unknown. Only the class names of the source so far are looked at, so that the provider is not asked about every variable |
| two methods with different descriptors reach the same best score | the one met first in the scan order of the lookup | **new**: `expressions.md` 6.3, 0 ties in the corpus |
| a method body whose statements emit no node, `{ ; }`, `{ { } }` | not an empty tree: implicit return and releases | **new**: `statements.md` 2.3 "unknown" |

**Errors in both modes**

| Case | Code | Remark |
| --- | --- | --- |
| identifier that is no local, field or class; a class name used as a value; a class written with its package inside a body | `E_NAME` | spec 2.9 |
| member not found in a dotted name; a member of an array other than `length`; a member of an int or float | `E_MEMBER` | spec 2.9 |
| `break` outside a loop or switch, `continue` outside a loop | `E_SYNTAX` | **new**: the spec names no code; the parser uses the same one for a `case` outside a switch |
| `return e;` in a constructor | `E_UNSUPPORTED` | **new**: no default exists that keeps the value (the spike drops the expression) |
| method not found, or an argument of unknown type | no diagnostic | the bare name: this is the proven rule |

**Not flagged**, and therefore limits of what a strict pass says:

- A layout in which one of the 14 undetermined line rules decides (two
  candidate tokens on different lines, `statements.md` 8.1). The construct is
  proven, only the line of a node would be a guess; the controls with 0 lost
  trees above are two of them. `analysis\stmt\line_risk.py` counts such nodes
  for the 635 sources: 0.
- A type name that resolves in more than one place (own package and an import):
  `classes.resolve_type` takes the first, as in the first step.
- A call through a name whose callee is also a field of the receiver class.

## 5. Findings

1. **The number was reached on the first full run.** `t4_trees.py` was written
   first and failed for want of `slrrc.emit`; with the modules in place its
   first complete run printed 7568 / 7570, 7396, 154 / 154, 3 / 18 and 3454 /
   3454. What followed was hardening (section 4, the tests outside the corpus,
   the controls) and none of it moved a number.

2. **The two trees that differ** are the ones `expected_fail.tsv` lists, each in
   one node, and the test reports them as expected (`-v`), not as failures:

   - `Center_Differential.java` line 41,
     `Transmission t = the_car.getBlock().getTransmission();`
     tree 2 node 25: the package emits
     `method java.game.parts.bodypart.Chassis.getBlock:()Ljava.game.parts.enginepart.Block;`,
     the class file holds `...getBlock:()Ljava.game.parts.Part;`. The shipped
     `Chassis` declares `public Block getBlock()`.
   - `Garage.java` line 1482,
     `String serverMessage = NetworkEngine.httpRequestAsync("GET", "image-code.com", "server_message.php", "", this, "handleEvent");`
     tree 99 node 1234: the package emits `name "httpRequestAsync"`, the class
     file holds `method java.net.NetworkEngine.httpRequestAsync:(Ljava.lang.String; ... )I`.
     The shipped `NetworkEngine` has no such method.

   Both class files were compiled against another version of the class they
   call. Every other node of both trees is equal (42 and 4,114 nodes).

3. **The pack table alone does not decide a pack name.** One of the 126
   spellings, `sl\Scripts\game\CareerEvents\old_stdpack_231.rpk`, names a pack
   that has no file in the install, and it is the very pack whose two literals
   in `TimeAttackTest` compile to `system.rpk` id 0. So the question "does the
   pack have a file" has to be asked before the table, and the table cannot
   answer it. That is why `TablePackNames` takes the list of pack files and why
   the order is: no file, then table, then file spelling (the spec's line reads
   "table, then disk spelling, then as written"). Three table entries are
   spelled otherwise than the directory listing (`cars\traffic\CivilVan_2.rpk`,
   `CivilVan_3.rpk`, `maps\ROC_Track\smallmap.rpk`); 22 pack files are named by
   no shipped pool. Where the spelling comes from stays open.

4. **The spike keeps state between compilations.** `spike_compiler.own_view_lib`
   adds the classes a source declares to the symbol universe of its `Env` and
   leaves them there. For the sources of the corpus that changes nothing (their
   classes are in the universe already), so no number of the spec is affected;
   a mutated text with a new class name is seen by every text behind it. It
   showed as 10 mutated texts in which the spike could not resolve `WHL_FRONT`
   or `WHL_REAR`, static fields of the superclass `Spring`. Minimal example,
   one `Env`: `spring_SuperDuty_500_rear.java` compiles; then a text that
   declares `class Spring` in the package `java.game.parts.running_gear.springs`
   compiles; then the first source fails with `unknown name WHL_REAR`, because
   `Spring` now resolves to the class of its own package that no longer exists.
   `t_extra4.py` gives the spike a universe of its own per text; nothing in
   `spec\` was changed. The package has no such state.

5. **Two places where the spike and the package differ on purpose**, both
   outside the corpus: a field or parameter whose type name resolves nowhere is
   `L;` in the package (spec 2.9) and `L<name>;` in the spike, which also
   raises for such a type inside a body; and the import of a single class is
   resolved by the package (appendix B) and unknown to the spike.

6. **What the corpus does not decide**, measured by the controls: whether the
   else-branch of an `if` inherits the dual context (the three trees that need
   the dual context need it through the then-branch and through `END` itself);
   which of two methods with the same score is taken; whether a parenthesised
   literal or name is a typed argument; the line of an assignment and of a
   declaration between two candidate tokens. The emitter takes what
   `stmtgen.py` and the spike take, so that the two stay comparable.

7. **Counts that settle a choice.** 16 declarations of the pairs shadow a live
   local of the same name and 44 take the name of a field: the innermost local
   wins (11 trees lost otherwise, control above) and a local hides a field
   (`expressions.md` 5.1: 82 trees). One method with a
   return type has the body `{}` (`InventoryItem.java` line 29): an empty tree.
   In all 3,454 sources: 0 `return e;` in a void method or constructor, 0
   `return;` in a value method, 0 calls named like a local or a field in scope,
   0 initialisers that name their own variable, 0 `length` of a computed value,
   0 calls on an array or on an int through a name.

8. **Runs made once, not stored as tests.** 27,640 mutated texts (every 5th
   source) through the package and the spike: 7,154 compiled by both, all with
   the same trees, no exception. 29,640 damaged texts (truncated, a piece cut
   out, a byte changed, a piece inserted or moved; both modes; `str` and
   `bytes`) through `emit_trees`: no exception, `ok` exactly when no error was
   reported. Sources nested 100 to 250 deep in twelve ways under Python's
   default recursion limit: compiled or `E_LIMIT`, never an exception.

## 6. Deviations from the spec, with reasons

| Spec | Built | Reason |
| --- | --- | --- |
| headline `M4 trees 7568/7570 node for node`, `7396/7570` with setting A | the same two numbers, then older builds, stale pairs and the spike in the same line | one line per milestone; the three additions are the controls and the cross-check of section 4 of the spec |
| the target of M4 / M5 is "equality with the spike for every source: identical bytes" | at M4: identical trees (operands by content) for 3,454 of 3,454 sources | bytes need the pool passes of M5 |
| `PackNames::canonical(written_path, &file_name) -> bool` | `canonical(written_path) -> file name or None`; the path is `cars\racers\duhen.rpk` | the Python form of the same interface |
| pack names "default: table, then disk spelling, then as written" | no file: `None` (the emitter writes `system.rpk` id 0); then table; then file spelling; as written only when the host gives no file list | finding 3 |
| baseline `m4.pass` (`file#blob#tree`) | `m4.pass` as stated (oracle trees), plus `m4_other.pass` and `m4_spike.pass` | the other parts of the headline need lists of their own |
| appendix B: a call whose name is a local | also a field | section 4 |
| appendix B: argument forms `true`, `(x)`, `!x`, ... | flagged only where the receiver class has a method the call could bind to | section 4 |
| spec 2.9 names no code for `break` / `continue` out of place and none for `return e;` in a constructor | `E_SYNTAX`, `E_UNSUPPORTED` | section 4 |
| module list of 2.4 | plus `packs.py` (named in section 4 of the spec) | - |
| - | `tests\t_extra4.py`, `tests\t4_controls.py`, `tests\gen_pack_table.py` | the corpus says nothing about rejected input, about the defaults of appendix B, or about how much a passing tree proves |
| `find_method_ex` of `gen.py` returns (method, score, tie) | `classes.lookup_method` returns a `MethodHit`; `find_method` is the method alone | plain data instead of a tuple; the tie is reported as `W_UNVERIFIED` |

## 7. The two open points

Neither was guessed away.

**Float literals.** `floatlit.float_bits(mode, int_digits, frac_digits)`
implements the four conversions that occur; `Options.float_mode` selects one,
`'A'` is the default for a new compilation. The oracle test takes the setting
per file from `tests\baseline\m5_float.tsv` and prints the number for A alone
beside it: 172 trees of 88 files need B, C or D. Which conversion Build 940
uses, and what selects it, has to be read in the scanner of the exe (M9).

**Pack names.** The emitter asks a `PackNames`; it never decides a spelling
itself. `TABLE_940` holds what the shipped pools show and sits behind the
interface. A host that knows where the game takes the spelling from replaces
the implementation, not the compiler.

## 8. What remains

- **M5 files**: the resolve pass and the emit pass of the pool over the trees
  (`declarations.md` 6.5, 6.6; `poolmodel.resolve_tree`, `emit_node`), node
  encoding and file assembly in `write.py`, `compile_source` in `__init__.py`,
  `tests\t5_files.py` (`m5.pass`, `m5_older.pass`, `m5_float.tsv` and
  `expected_fail.tsv` are in the baseline). The trees are ready for it: the
  operands are content, and the type of every local is in the `declare` /
  `type` pairs.
- **M6**: `cli.py`, `tests\t6_source_only.py`. The driver will need a file list
  for `TablePackNames` (the `.rpk` files of the install).
- **M7**: the C++ port; `--impl cpp` answers "not built" (exit 2). `dump_trees`
  is the format for its tree comparison.
- Small things left as they are: the three limits of strict mode at the end of
  section 4; `resolve_type` does not report a type name that resolves in two
  places; duplicate field names in one class are not reported (0 in the 2,993
  shipped classes; the first one wins).

## 9. Notes for the C++ port

(Later, `impl_fix.md` section 5 replaces two statements of this section: how
the items of a node get into `Ast::lists`, and what the float conversions need
from the build; and the emitter no longer has a loop that counts below zero.)

- `Emitter` ports method by method. Child nodes are indices; `items` is a slice
  of `Ast::lists`. `BINARY_KIND`, `ASSIGN_KIND` and the mnemonic tables are
  switches or static arrays.
- `Tree` is two vectors (`SymNode`, `Operand`); `Operand` is three strings and
  a number. Keep `K_LABEL` inside the emitter: no tree leaves it with a label.
- `walk()` returns a small struct (`ok`, segments, type, class); `name_toks()`
  fills a vector of token indices that the caller can reuse.
- The scope caches (`chains`, `field_maps`, `method_maps`) are maps keyed by
  class name that live as long as one compilation; the scan order of
  `_method_map` (instance methods last to first, then static methods last to
  first) has to be kept, it decides a tie.
- `type_score` compares descriptors as strings; `NULL_TYPE` is a value no
  descriptor has.
- `floatlit`: the C forms are in the docstring; compile with `/fp:precise` and
  SSE2. `r32(x)` is a cast to `float`, `bits_of` a `memcpy`.
- `packs.lower` is ASCII lower case on purpose: pack names are compared without
  regard to case, and the result must not depend on a locale.
- The emitter recurses once per level of the tree for expressions and for
  statements, twice through a call. The tree height is at most
  `parser.MAX_DEPTH` (200).
- `tests\t_extra4.py` holds 13 trees written out by hand and 55 lookup cases
  that need no corpus: the first things to run against the C++ emitter.
