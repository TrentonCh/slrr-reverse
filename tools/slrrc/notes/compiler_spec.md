# The compiler: what is settled, the design, and the build plan

Written 2026-10-11. This is the plan for a compiler that turns the script
sources of Street Legal Racing: Redline (Build 940) into `.class` files
byte-identical to the ones the game's own compiler wrote, and that is then
linked into the C++ host (`slrr-reverse`).

It rests on five notes in this directory, which I read in full and whose
tools I re-ran: `foundation.md`, `grammar.md`, `declarations.md`,
`expressions.md`, `statements.md` (plus `opcodes.md`, `corpus.md`). Nothing in
the slrr-reverse repo, in the game installs or in Ghidra was touched; no exe
was run. Everything new is under `compiler\spec\` and in this file.

Status words as in the other notes: **proven** = a script checks the rule on
the whole corpus and the count is quoted; **observed** = seen in examples
only; **unknown** = said outright. A number in this document is printed by a
script named next to it; `python -I spec\run_spec.py` regenerates all of them
(about 2 minutes) and fails when one differs.

## 0. Result in brief

- **The five proof pipelines reproduce.** `lib\run_all.py`,
  `lib\run_language.py`, `analysis\decl\run_decl.py`, `analysis\expr\run_all.py`
  and `analysis\stmt\run_all.py` all exit 0, and the five notes they generate
  (`corpus.md`, `opcodes.md`, `grammar.md`, `expressions.md`, `statements.md`)
  come out byte-identical to the ones on disk. I found no bug in the
  foundation and changed nothing in `lib\` or `analysis\`.
- **The four analyses compose into a compiler.** A composition spike
  (`spec\spike_compiler.py`, 574 lines of glue around the existing models)
  compiles a source to class file bytes without reading the class file of that
  source. Over the corpus (`spec\spike_check.py`):

  | Set | Result |
  | --- | --- |
  | Oracle pairs (Build 940, consistent) | **2,649 of 2,651 files byte-identical** (99.92%); 7,568 of 7,570 trees node for node |
  | The same with one fixed float conversion (setting A) | 2,561 of 2,651 files (96.6%); 7,396 of 7,570 trees |
  | Pairs written by older builds, with their build number | 153 of 153 files byte-identical |
  | Stale pairs (control) | 0 of 15 |
  | Sources without a class file | 635 of 635 compile (635 classes, 771 trees, 32,644 nodes, 693,847 bytes) |

  The two oracle files that differ are the two already known to be
  unreproducible (compiled against another version of a class they call).
  So no emission rule is missing; what stands between today and byte identity
  is one piece of state of the exe, the float conversion (section 1.4).
- **The 635 class files exist.** `spec\out\source_only_classes\` holds them in
  the layout a cache directory would have. There is no shipped class to
  compare them with, so they are checked in four other ways
  (`spec\spike_source_only.py`): the strict reader and the 30 layout rules of
  the foundation accept 635 of 635; every tree splits into the proven
  statement forms and expression shapes (767 of 767 trees with nodes); the
  foundation's stack simulation finds no underflow, no unbalanced join and an
  empty stack after every statement (21,523 node executions); and 522 of them
  have a sibling in their directory whose source has the same tokens on the
  same lines and whose class ships: opcode, line, local index, count, jump
  offset and expression kind of every node equal the sibling's for 522 of 522.
  Whether the host loads them is not tested: that needs the host to run.
- **Design decision.** Reference compiler in Python (assembled from what
  exists), final compiler hand-written in C++17 with no generated code and no
  dependency; one parse into an AST, then passes. The exe's pool order is
  reproduced by five interning steps, not by emitting during the parse:
  the exe itself does not emit in parse order (section 2.3).

Three statements in earlier documents need correcting:

1. The task description says the exe's compiler "emits nodes and pool entries
   as rules reduce, which is why pool order follows parse order". Only the
   type descriptors follow parse order. Member names are interned at the end
   of the class body, member refs in a pass over the code by row vector, and
   strings in a second pass in tree order (`declarations.md` 6; merging the
   two passes breaks 2,806 of 2,873 classes). Nodes are created at reduction
   time (their line shows it) but placed in execution order.
2. `docs/ENGINE.md` (M3) sets the goal "byte-equal to the 2,663 paired classes
   at build 0x24F9D". The reachable goal is 2,649: 12 pairs are stale and 2
   were compiled against classes that have changed since.
3. `expressions.md` compares floats "within 2 ulp". The conversions that occur
   differ from each other by up to 3 units in the last place (one literal,
   `0.00065` in `AirFuelDeliverySystem`; `spec\out\float_files.txt`).

## 1. What is settled and what is not

### 1.1 What was re-run

| Pipeline | Time | Result |
| --- | ---: | --- |
| `python -I lib\run_all.py` | 93 s | all steps pass: 2,921 of 2,921 class files round-trip, 30 layout rules hold, opcode rules hold with the 3 known exceptions in 2 class files |
| `python -I lib\run_language.py` | 170 s | 8 of 8 lexer / parser rules for 3,454 sources; 94,263 of 94,263 declaration comparisons; 7,727 of 7,742 tree shapes |
| `python -I analysis\decl\run_decl.py` | 110 s | CONS, CLSS, FILD, MTHD byte-identical for 2,873 of 2,873 pair classes; 2,819 of 2,819 files rebuilt around the shipped TREE |
| `python -I analysis\expr\run_all.py` | 300 s | 7,722 of 7,724 trees of the consistent pairs; 635 of 635 class-less sources without an unresolved name |
| `python -I analysis\stmt\run_all.py` | 66 s | 7,570 of 7,570 oracle trees with every line (590,101 nodes); 13,331 of 13,331 jumps |

The five generated notes (`corpus.md`, `opcodes.md`, `grammar.md`,
`expressions.md`, `statements.md`) have the same SHA-256 after the runs as
before, and so have the two written by hand (`foundation.md`,
`declarations.md`); five `.txt` outputs differ in a run time only.

### 1.2 The composition spike

The analyses were made in parallel and each left something to the others: the
statement model copies names and literal operands from the class under test,
the expression model leaves out 47,094 statement-level nodes and compares
through the pool, the pool model takes the shipped trees as its oracle. The
question a plan has to answer first is whether anything falls between them.

`spec\spike_compiler.py` puts them together:

| Part | Taken from | How |
| --- | --- | --- |
| scanner, parser | `lib\lexer.py`, `lib\parser.py` | unchanged; the models read the tree through their adapters `libast.py` and `libbridge.py` |
| statements, locals, jumps, lines | `analysis\stmt\stmtgen.py` | unchanged, run in its "free" mode (no class); a subclass replaces the oracle window |
| names, calls, method lookup | the rules of `analysis\expr\gen.py` | re-stated in the subclass (`head`, `walk`, `name`, `arg_type`, `callee`, `call_path`; about 130 lines); `gen.Model.find_method_ex` is called as it is |
| own classes | `lib\symbols.py` | the classes of the source enter the class database as the source declares them, never from their class file |
| other classes | `analysis\expr\classdb.py` | class files, and sources for the classes without one |
| pool, CLSS, FILD, MTHD | `analysis\decl\poolmodel.py` | unchanged; it is handed the spike's own trees (in a scratch pool order) instead of the shipped ones and returns the pool in the compiler's order |
| bytes | `lib\tufa.py` | unchanged |

Two tables stand in for state of the exe that no source holds: the spelling of
pack names (section 1.4, item 2) and the float conversion (item 1).

`spec\spike_check.py` (18 s for the whole corpus):

```
oracle       pairs 2651   byte-identical 2649 (float setting A alone: 2561)   by setting: A 2561, B 60, C 22, D 6
             trees 7570   node for node  7568 (float setting A alone: 7396)
older        pairs  153   byte-identical  153 (float setting A alone:  153)
stale        pairs   15   byte-identical    0
source only  sources 635   compiled and re-read by the strict reader 635 (635 classes, 771 trees, 32644 nodes, 693847 bytes)
```

- The numbers are the ones `spec\expected_totals.py` predicts from the
  separate proofs (2,649 and 2,561 files, 7,568 and 7,396 trees). Nothing was
  lost in the composition.
- With the pack names taken from the file names of the install instead of the
  table: 2,644 files; with the names as the source writes them: 2,581
  (`spec\out\spike_check_disk.txt`, `spike_check_written.txt`).
- The same bytes come out for 3,454 of 3,454 sources when the three analysis
  parsers are used in place of `lib\parser.py`
  (`compile_source(parsers='analysis')`; `spec\out\spike_parsers.txt`).
- The spike is evidence and an oracle generator, not the product: it runs
  three models with their statistics and options, raises on the first unknown
  name, and has seen no construct outside the corpus.

### 1.3 Status by rule area

The proof column quotes the analysis that established the rule; every row
marked proven is also exercised by the byte-identical files of 1.2. Rule ids
refer to the note named in the last column.

| Area | Rule | Status | Proof | Note |
| --- | --- | --- | --- | --- |
| **Lexing** | tokens of every source; comments; keywords | proven | 3,454 of 3,454 sources, 852,211 tokens; same stream from two scanners | `grammar.md` 2, 10.1 |
| | int literal: value modulo 2^32, hex with all 32 bits | proven | 25,940 of 25,940 | `grammar.md` 2.4 |
| | string literal: text after `\" \\ \n \t` | proven | 12,753 of 12,753 | `grammar.md` 2.6 |
| | resource id: one token, local id = hex value | proven | 9,055 of 9,055 | `grammar.md` 2.7 |
| | float literal: bit pattern | **open** | four conversions occur; chosen per compilation (1.4 item 1) | `foundation.md` 7.1, `spec\float_models.py` |
| | other escapes, character literals, exponents | unknown | 0 uses | `grammar.md` 12 |
| **Parsing** | grammar of `grammar.md` 3; precedence, casts, dangling else | proven | 3,454 of 3,454 parse; 7,727 of 7,742 tree shapes (the 15 others: 14 stale, 1 pack without file); wrong grammars lose 1 to 1,890 trees | `grammar.md` 10 |
| | precedence of `\|` `^` `&` against most levels; `(Name) - x` | undetermined | never adjacent without parentheses | `grammar.md` 6.1, 6.3 |
| | constructs of `grammar.md` 3.4 (fall-through, labels, `>>>` ...) | unknown | 0 uses | appendix B |
| **Type names** | own file, own package, imports, `java.lang` | proven (result), order undetermined | 16,345 names, 0 unknown, 0 ambiguous | `grammar.md` 7.2 |
| **Declarations** | blobs, CLSS, FILD, MTHD rows, flags, descriptors, tree numbers | proven | 2,873 of 2,873 CLSS; 238 of 238 FILD; 2,869 of 2,869 MTHD, byte for byte from the source alone | `declarations.md` 3, 4 |
| | locals word = high-water mark of live locals | proven | 5,411 of 5,411 rows | `declarations.md` 4.5 |
| | nothing implicit (no default constructor, no static initialiser) | proven | 2,873 of 2,873 classes | `grammar.md` M9 |
| **Pool order** | five steps: seed, types, names, resolve, emit | proven | 2,873 of 2,873 pools given the trees; 2,993 of 2,993 blobs for the source-free rules T1 to T8; 2,537 of 2,537 unreferenced entries explained | `declarations.md` 6 |
| | 5 orders: field type before or after its initialiser; local type after each declarator or after the last; `new T[n]`, `(int)x`, `x instanceof T` before or after the types inside | undetermined | 0 classes change under the alternative | `declarations.md` 9, 11 |
| **Names** | local before field; innermost local wins | proven | 82 trees lost otherwise | `expressions.md` 5.1 |
| | place of class names in the lookup | undetermined | no name is both | `expressions.md` 5.2 |
| | field ref: receiver type for instance fields, declaring class for statics | proven | 49,745 + 28,487 refs, 0 exceptions (all blobs) | `expressions.md` 5.3 |
| | `a.b.c` is one `path`; computed receiver uses `getfield` by name | proven | 91,542 name paths; R3, R4 without exception | `expressions.md` 5.4 |
| **Calls** | method ref only when every argument is a literal, local, simple-name field or `this`, and the lookup finds a method | proven | 24,381 call sites, 2 exceptions (the stale callees); R5 18,468 / 2, R6 8,065 / 0 | `expressions.md` 6.2 |
| | lookup = the VM's scored `getMethod`; ref names the receiver class | proven by outcome | 16,850 resolved calls; 0 ties, so scan order is unconstrained | `expressions.md` 6.3 |
| | `call_on`, `call_super`, `new_init`, `this_init` / `super_init` | proven | 1,087 / 758 / 8,242 / 2,834 | `expressions.md` 6.1 |
| **Expressions** | literals L1 to L6 | proven | 25,963 / 35,415 / 12,759 / 9,058 / 1,343 / 10 | `expressions.md` 3 |
| | operators O1 to O14, `&&` / `\|\|` with test and jump | proven | 10,332 unary, 10,939 binary, 981 short-circuit, 41,890 assignments | `expressions.md` 3, 4 |
| | `new`, arrays, casts, `instanceof` | proven | 8,242 / 1,814 / 211 / 222 | `expressions.md` 3 |
| | order: value before target, index before array, arguments before a computed receiver | proven | 7,722 of 7,724 trees | `expressions.md` 2 |
| | operators never used (`>>>`, `%=`, `<<=`, ...), compound assignment as a value | unknown | 0 uses | `expressions.md` 10 |
| **Statements** | 17 forms of `statements.md` 4 | proven | 7,570 of 7,570 oracle trees, 590,101 nodes | `statements.md` 4 |
| | implicit `super()`, implicit return, double release run | proven | 211 / 4,427 / 3,044 + 2,155 + 908 tails | `statements.md` 2 |
| | scopes: block, `for`, case group, body; reverse parameter numbering | proven | 24,847 releases; 9,197 parameter locals | `statements.md` 3 |
| | thin: `do` 3, `continue` 1, `default` 6, `?:` 5, `return;` in a constructor 1 | proven on few | see section 5 | `statements.md` 10.3 |
| | fall-through, several labels, comma lists, empty statement | unknown | 0 uses | appendix B |
| **Jumps** | signed node offset relative to the jump node | proven | 13,331 of 13,331; 11,401 of 11,401 offsets from source | `statements.md` 6 |
| **Lines** | line of the token the scanner had reached at reduction; `END(statement)` with the LALR(1) lookahead of the JLS grammar | proven | 590,101 of 590,101 nodes | `statements.md` 8 |
| | 14 of the 31 kinds of node, where two candidate tokens always stand on one line | undetermined | 0 nodes affected in the pairs and in the 635 | `statements.md` 8.1 |
| **File layout** | chunk order, no padding, one blob per class | proven | 2,921 of 2,921 files round-trip | `foundation.md` 3 |
| **Pack names** | pool spelling of the pack of a resource id | observed rule, source unknown | one spelling per pack in all pools; table reproduces 9,058 of 9,058 nodes | 1.4 item 2 |
| **Whole files** | all of the above together | proven by the spike | 2,649 of 2,651 | 1.2 |

### 1.4 The unknowns, most blocking first

"Touches" = sources that hold at least one construct the question is about
(`spec\blockers.py`); "blocks" = files that are not byte-identical because of
it (`spec\spike_check.py`, `spec\expected_totals.py`). The groups are the
2,651 oracle pairs, all 2,819 pairs, and the 635 sources without a class.

| # | Unknown | Oracle pairs | All pairs | Source only | Effect |
| ---: | --- | --- | --- | --- | --- |
| 1 | Float literal conversion: which of four conversions the exe uses, and what selects it | touches 2,514; **blocks 88** under the best single setting, 0 when chosen per file | touches 2,682 | touches 575 (1,817 literals) | byte identity only; the values differ by at most 3 units in the last place |
| 2 | Spelling of pack names in rid entries | touches 596; blocks 68 as written, **5** as on disk, 0 with the table | touches 599 | touches 78 (889 literals; 17 sources write another letter case, none names a missing pack) | byte identity; function only if the host compares pack names case-sensitively |
| 3 | The class versions the compiler sees: output depends on the declarations of other classes at compile time | **blocks 2** (`Center_Differential`, `Garage`); 5 name a dialog class that ships in two versions, 18 a class name defined by several files (none blocked) | same | 0 | not reproducible from the install; in general a compiled class is not a function of its source alone |
| 4 | How the exe finds classes outside the classpath | touches 128 (plus 70 that name a class of their own off-classpath directory) | 129 | 1 | not a byte question (the corpus database answers it); the host needs an index of every `scripts` directory or the loaded classes |
| 5 | Classes without a class file that other sources name | 1 | 1 | 3 (4 classes, depth at most 2) | compile order: the host must be able to declare a class from its source before compiling its user |
| 6 | Later classes of a multi-class source: how they are visible to earlier ones, and why each interns its own `LC;` | touches 23 (77 classes); blocks 0 | 23 | 0 | reproduced; mechanism unknown, so a first compile of a new multi-class source is a guess |
| 7 | Constructs no source uses (appendix B) | 0 | 0 | 0 | mods only |
| 8 | Undetermined choices: 5 pool-order rules, 14 line rules, class against variable, package search order, method scan order, typed argument forms | 0 | 0 | 0 | new code only; every choice gives the same bytes for every shipped source |
| 9 | What the exe does with an unknown identifier, member or type (one hint: type `L;`) | 0 | 0 | 0 | error handling of the host is a design choice (2.9) |
| 10 | `import parts:part.root.BodyPart.*;` | touches 2; blocks 0 | 2 | 0 | ignored, reproduced |
| 11 | Abstract and interface methods in a source; flags 0x2, 0x80 | 0 | 0 | 0 | only in class files without source |

Not unknowns of the compiler but worth the same table: 15 stale pairs (12 of
build 0x24F9D), excluded from the oracle; 156 pairs of older builds, 153
consistent, all 153 reproduced.

**1. Floats.** `spec\float_models.py` simulates 234 candidate conversions in
exact arithmetic (program, x87 precision control 24 / 53 / 64 bits, float or
double variables, float or double constant 0.1) against the 35,084 float
nodes of the oracle pairs. Four settings together reproduce every node of
every file, one setting per file, and each of the four is the only one that
reproduces some file:

| Setting | Conversion | Files it reproduces (of 2,514) | Files only it reproduces |
| --- | --- | ---: | ---: |
| A | `s = 1; per digit: s /= 10; v += d * s`, everything in single precision | 2,426 | 23 |
| B | `s = 0.1f; per digit: v += d * s; s *= 0.1f`, single precision | 2,298 | 42 |
| C | as A, but `d * s` and the sum formed in double before the store to float | 2,417 | 12 |
| D | `s = 0.1f; per digit: v += d * s; s /= 10`, double throughout, one rounding at the end | 1,579 | 6 |

A then B then C then D covers 2,426, 2,486, 2,508, 2,514 files. All 153
older-build files follow A. The integer part wraps modulo 2^32 in all four.
The reading that fits (inferred, not read in the exe): the result depends on
the precision state of the x87 unit of the compiling thread (A against C is
the same code under 24-bit and 53-bit precision; Direct3D switches a thread
to 24 bits when a device is created without the FPU-preserve flag), and on
top of that on at least two code variants (dividing the scale, multiplying
it). No single piece of code under three precision states reproduces more
than 2,464 files, two reproduce 2,511.
The files that need B or D sit in `sl\Scripts`, `parts\scripts`,
`multibot\maps` and `parts\engines`, which suggests other compile sessions or
another build of the exe with the same build number. The scanner's float
action in the exe has to be read to say which variant Build 940 contains;
nothing in the corpus can. Until then the compiler implements A as its
conversion (plain single-precision arithmetic, the largest group, the only
one the older builds show) and keeps B, C, D selectable for the oracle test.

**2. Pack names.** `spec\rid_packs.py`: of 9,058 rid nodes the pool spells the
pack as the source does in 8,037, as the file on disk in 9,046. The 12 others
are `maps\ROC_Track\smallmap.rpk` (6) and `cars\traffic\CivilVan_2.rpk`,
`CivilVan_3.rpk` (4), where the pool has the source's spelling and the disk
another, and the 2 literals of `TimeAttackTest` whose pack has no file and
which compile to `system.rpk` id 0. Across all 2,993 blobs every one of the
126 packs has exactly one spelling (7,242 rid entries). That is what an
interning pack table gives: the first spelling the resource system met in
that session, from the directory listing or from a script. The compiler
therefore asks its host for the canonical name (interface `PackNames`, 2.6);
the test harness uses the 126-entry table, a pack without a file becomes
`system.rpk` id 0 (observed once, Build 940).

**3. Class versions.** A method ref carries the descriptor of the method found
at compile time. `Center_Differential` holds `Chassis.getBlock:()L...Part;`
where the shipped `Chassis` returns `Block`; `Garage` holds a ref to
`NetworkEngine.httpRequestAsync`, which no longer exists. Both stay on the
expected-failure list. The same property is why a cache entry has to record
what it was compiled against (2.10).

## 2. Architecture

### 2.1 Decisions

| Question | Decision |
| --- | --- |
| Prototype language | Python 3, standard library only: the reference compiler `slrrc`, consolidated from the spike |
| Final language | C++17, hand-written scanner and recursive-descent parser, no flex / yacc, no generated code, no dependency beyond the standard library |
| Structure | one parse into an AST that keeps token indices, then passes; never emission during the parse |
| Pool order | five interning steps over the AST and the emitted trees, as in `declarations.md` 12 |
| Other classes | an interface `ClassProvider`; own classes are declared from the source before any body is compiled |
| Float conversion | setting A by default; B, C, D selectable |
| Pack names | an interface `PackNames`; default: table, then disk spelling, then as written |
| Errors | diagnostics with line and code, no output on error, never an exception across the interface |
| Output on the host | memory first; a cache directory outside the install; writing beside the source only on request |

### 2.2 Prototype in Python, product in C++

Python first, because everything that proves a byte lives there: the strict
reader and writer, the corpus index, the parser, three models. The spike shows
the distance: one session from separate proofs to 2,649 identical files. The
reference compiler is the spike with the scaffolding removed (one tree, one
emitter, one pool, no options, no statistics), about 2,000 lines, and it stays
after the port for three jobs: it is the executable specification the C++ code
is transliterated from; it is the differential oracle where no shipped class
exists (the 635 sources, mutated sources, synthetic sources for untested
constructs); and it can fill the host's cache directory offline before the
C++ compiler exists (milestone M6).

C++17 for the product because the engine is C++17 on MSVC without
dependencies, and hand-written rather than generated because (a) a yacc
grammar would add a build tool, (b) the corpus needs only the grammar of
`grammar.md` 3, which `lib\parser.py` implements in 1,021 lines of recursive
descent, and (c) what the exe's LALR(1) parser contributes to the bytes, the
line of each node, is a function of token positions that is proven in closed
form (`statements.md` 8), so no LALR machinery is needed to reproduce it.
Estimated size: 4,000 to 5,000 lines.

The compiler does not use the engine's `tufa.cpp`: that loader's pool decoder
is heuristic (`docs/ENGINE.md` 4.3). It carries its own writer and, in the
test tool, its own strict reader.

### 2.3 One parse, then passes

The exe's compiler parses once and builds a tree; while it parses it interns
type descriptors and stamps each construct with the scanner's line; then it
makes passes. The evidence that it does not emit during the parse:

- a jump of an `if` is built when the statement is complete but stands in
  front of the body; the path of a call is built before the arguments and
  stands behind them (lines, `expressions.md` 7);
- refs are interned by row vector (static methods, instance methods, static
  field initialisers, instance field initialisers), not in source order (50
  classes wrong otherwise);
- strings are interned in a later pass in tree order (47 classes wrong when
  interned with the refs, 2,806 when the passes are merged);
- classes declared further down in the file are already known (147 type
  names, 148 member refs).

The design follows the same shape, and every parse-time effect is recovered
from the AST:

| Effect of the exe's parse | How the compiler reproduces it | Proof |
| --- | --- | --- |
| type descriptors interned as rules reduce | a walk over the members in source order that visits the constructs in reduction order: parameter types left to right, return type after the parameters, `new T` before its arguments, a local's type after its initialisers, `instanceof` after its operand, then `LI;` per interface and `LC;` for every class but the first of the file | 2,873 of 2,873 pools |
| line of a node | each AST node keeps the indices of its first, last, operator and name token; a table says per node kind which token's line to take (own, last, next), and `END(statement, dual)` gives the lookahead rule for `if`, `while`, `for` | 590,101 of 590,101 nodes |
| nodes placed in execution order | the emitter walks the AST in emission order (value before target, index before array, arguments before receiver, update of a `for` behind its body, case values before the groups) | 7,570 of 7,570 trees |
| refs by row vector, strings by tree | two passes over the finished trees (2.7, steps 4 and 5) | 2,873 of 2,873 pools |
| later classes visible | all classes of the unit are declared (name, superclass, fields, methods with descriptors) before the first body is compiled | 22 of the 23 multi-class oracle pairs (77 classes) byte-identical; the 23rd is `Garage`, which fails for its stale callee |

Emitting during a hand-written parse was considered and rejected: the pool
could still not be filled in one go, jump offsets need the finished tree, and
the method-ref decision needs the members of classes declared later.

### 2.4 Modules

The reference (`compiler\slrrc\`, Python) and the product (C++) have the same
modules with the same function names, so that the port is a transliteration
and a difference can be traced module by module.

| Module | Python | C++ | Content |
| --- | --- | --- | --- |
| scanner | `lib\lexer.py` (used as it is) | `sc_lexer.cpp` | tokens with kind, text span, line; literal values (int modulo 2^32, string escapes, rid parts) |
| parser | `lib\parser.py` (used as it is) | `sc_parser.cpp`, `sc_ast.hpp` | AST of `grammar.md` 11; notes for constructs outside the proven subset |
| classes | `slrrc\classes.py` | `sc_classes.cpp` | `ClassInfo`, `ClassProvider`, the unit's own declarations, type name resolution, descriptors, field lookup, method lookup with scores |
| pool | `slrrc\pool.py` | `sc_pool.cpp` | interning pool: `utf8`, `cls`, `nat`, `ref`, `rid`; fixed inner order (class, then name, descriptor, nat, then ref) |
| declarations | `slrrc\decl.py` | `sc_decl.cpp` | seed, type walk, names, FILD / MTHD rows, CLSS, tree numbering |
| emitter | `slrrc\emit.py` | `sc_emit.cpp` | statements, expressions, names, calls, locals and scopes, labels, line rules; output: symbolic nodes |
| literals | `slrrc\floatlit.py` | `sc_float.cpp` | float settings A to D |
| writer | `slrrc\write.py` (on `lib\tufa.py`) | `sc_write.cpp` | resolve pass, emit pass, node encoding, chunks |
| driver | `slrrc\__init__.py`, `slrrc\cli.py` | `sc_compile.cpp`, `script_compiler.hpp` | `compile()`, diagnostics, options |
| test tool | `tests\*.py` | `tools\slrrc\main.cpp`, `sc_classfile.cpp` | batch compile, dumps of tokens / AST / rows / trees, strict reader for the class provider |

C++ location: developed as a self-contained directory `compiler\cpp\` with its
own `CMakeLists.txt` until M7 passes (other workflows are editing
`slrr-reverse`), then placed in the engine as `engine/Core/Jvm/Compiler/` with
the public header `engine/include/script_compiler.hpp`; the test tool keeps
building from the same sources.

### 2.5 Data structures

```cpp
namespace inv { namespace sc {

struct Token { uint8_t kind; uint32_t pos, len, line; };        // text = source.substr(pos, len)

// AST: nodes in one vector, children by index, lists in a second vector.
struct Node {
  uint16_t kind;                 // Unit, Class, FieldDecl, MethodDecl, Block, If, ..., Binary, Call, ...
  uint32_t tok0, tok1;           // first and last token
  uint32_t op_tok, name_tok;     // operator / member name token, or NONE
  uint32_t a, b, c;              // children (meaning per kind)
  uint32_t list, count;          // slice of Ast::lists (statements, arguments, members, ...)
  uint32_t flags;                // modifiers, dims, literal kind, ...
};
struct Ast { std::string source; std::vector<Token> toks; std::vector<Node> nodes; std::vector<uint32_t> lists; };

struct FieldInfo  { std::string name, desc; bool is_static; };
struct MethodInfo { std::string name, desc; bool is_static; };
struct ClassInfo  { std::string name, super; std::vector<std::string> interfaces; bool is_interface;
                    std::vector<FieldInfo> fields; std::vector<MethodInfo> methods; };   // source order

enum class EntryKind : uint8_t { Utf8 = 0, Rid = 3, Class = 4, Ref = 5, Nat = 7 };
struct PoolEntry { EntryKind kind; uint32_t a, b; std::string text; };                 // a, b: indices or local id
class Pool {                                                                           // entries + hash index
 public:
  uint32_t utf8(std::string_view); uint32_t cls(std::string_view name);
  uint32_t nat(std::string_view name, std::string_view desc);
  uint32_t ref(std::string_view cls, std::string_view name, std::string_view desc);
  uint32_t rid(std::string_view pack, uint32_t local_id);
};

enum class Operand : uint8_t { None, Imm, Label, Utf8, Class, Ref, Rid };
struct SymNode { uint8_t op; uint16_t line; Operand kind; uint32_t value; };          // value: imm, label id, or index
struct Tree { std::vector<SymNode> nodes; uint32_t max_locals; };                      //   into the tree's operand table
struct Row  { uint32_t flags, name, desc_or_type, tree, locals; };

}}  // namespace inv::sc
```

The emitter's state per tree: the list of live locals as (name, descriptor),
the high-water mark, the stack of enclosing loops and switches (break label,
continue label, number of locals at entry), the labels. An operand that names
a pool entry is kept by content (text, class name, class + name + descriptor,
pack + id) until the two pool passes have run; only then do nodes get indices.
That is the one structural lesson of the spike: the trees must exist in full
before a single ref or string index is known.

### 2.6 Interface

```cpp
struct ClassProvider {            // the classes the compiler can see besides the unit's own
  virtual const ClassInfo* find(std::string_view fqn) = 0;       // may load or declare the class
  virtual ~ClassProvider() = default;
};
struct PackNames {                // canonical file name of a pack, false when it has no file
  virtual bool canonical(std::string_view written_path, std::string* file_name) = 0;
  virtual ~PackNames() = default;
};
enum class FloatMode : uint8_t { DivSingle /*A*/, MulSingle /*B*/, DivDoubleSum /*C*/, DivDouble /*D*/ };
struct Options { uint32_t build = 0x24F9D; FloatMode float_mode = FloatMode::DivSingle; bool strict = false; };
struct Diagnostic { bool error; uint32_t line, column; const char* code; std::string text; };
struct Result {
  bool ok;
  std::vector<uint8_t> class_file;            // all blobs of the source, in order
  std::vector<std::string> class_names;       // one per blob
  std::vector<std::string> consulted;         // classes found through the provider (for the cache)
  std::vector<Diagnostic> diagnostics;
};
Result compile(std::string_view source_bytes, std::string_view file_name,
               ClassProvider& classes, PackNames& packs, const Options& options);
```

`ClassProvider::find` is called with a full name: for a simple type name the
compiler tries the unit's own classes, the own package, the imports in source
order and `java.lang`, as the exe presumably does through its class loader.
In the engine the provider answers from the loaded classes, then the class
loader (which may read a class file, a cache entry, or declare the class from
its source without compiling its bodies, so that two sources that name each
other cannot recurse), then an index of all `scripts` directories for the 24
car model classes that lie outside the classpath. In the tests it answers
from the corpus (`CorpusProvider`).

The compiler has no global state and does no file access; it is called on the
loader's thread.

### 2.7 The algorithm on one page

```
compile(source, name):
  toks  = scan(source)                                   E_LEX
  unit  = parse(toks)                                    E_SYNTAX
  own   = declare every class of the unit                names, superclass, interfaces, fields and
                                                         methods with resolved descriptors
  for each class C of the unit, in source order:
    pool = empty
    1 seed     cls(C); cls(superclass or java.lang.Object) unless interface; cls(each interface)
    2 types    walk the members in source order (2.3); then "LI;" per interface;
               then "LC;" when C is not the first class of the file
    3 names    per member in source order: field -> utf8(name); method -> utf8(descriptor), utf8(name or "<init>")
               rows: stable split by static into the four vectors; flags = OR of the modifier bits;
               one tree counter over fields with initialiser and methods that are not abstract
    emit       per tree owner in source order: the symbolic node list (statements.md 11, expressions.md 3);
               locals word = high-water mark, 0 for an empty tree
    4 resolve  trees by row vector (static methods, instance methods, static field initialisers,
               instance field initialisers), node by node: class entries, member refs,
               the class of the receiver before every further path segment, "LC;" for `this`
    5 emit     trees in tree order, node by node: utf8 of strings and unresolved names; pack utf8 + rid
    encode     nodes with their final indices; chunks TUFA, CONS, [FILD], [MTHD], CLSS, TREE
  return the blobs one behind the other
```

Emitter, the rules that are easy to get wrong (each cost the spike nothing
because the models already had them; a rewrite has to keep them):

- `this` is local 0, parameters follow in reverse order; a declaration takes
  the next index; `break`, `continue` and `return` emit releases without
  closing the scope.
- A name is a local, else a field of the class chain, else a class. A field
  ref names the declaring class for a static field and the static type of the
  receiver (the class being compiled for a simple name) for an instance field.
- A call ends in a method ref only when every argument is a literal, a local,
  a field written as a simple name or `this` (a parenthesised one does not
  count) and the scored lookup on the receiver class finds a method; the ref
  names the receiver class and carries the found descriptor. Otherwise the
  bare name, behind a `path 1` header when the call has no receiver.
- A cast to a class carries a `class` node (pool class entry, step 4); a cast
  to `int` / `float`, `new`, `new T[n]`, `instanceof` and `declare` carry a
  `type` node (utf8, step 2).
- An expression statement has no extra node: its last node is `stmt` instead
  of `expr`.
- A constructor without written `this(..)` / `super(..)` opens with
  `push_int 0; super_init; stmt ctor` on the line of the first token behind
  the `{`; constructors and void methods get the implicit return at the `}`,
  value methods none; then the releases of the body locals and of parameters
  and `this`.

### 2.8 Float conversion

Implemented with explicit IEEE operations so that it does not depend on the
x87 state of the host (the engine is built for SSE2; the file must be compiled
with the default `/fp:precise`). `ip` is the integer part accumulated in a
32-bit unsigned integer, `d` each fraction digit:

```c
/* A */ float  v = (float)ip,  s = 1.0f;          s = s / 10.0f;  v = v + (float)d * s;
/* B */ float  v = (float)ip,  s = 0.1f;          v = v + (float)d * s;  s = s * 0.1f;
/* C */ float  v = (float)ip,  s = 1.0f;          s = (float)((double)s / 10.0);
                                                  v = (float)((double)v + (double)d * (double)s);
/* D */ double v = (double)ip, s = (double)0.1f;  v = v + (double)d * s;  s = s / 10.0;   /* result (float)v */
```

`spec\float_models.py` is the exact-arithmetic reference (fractions with
explicit rounding); `spec\float_closed_forms.py` checks that these four forms
give its bit patterns for all 15,775 distinct literal texts of the consistent
pairs (15,775 of 15,775 for each setting).

### 2.9 Errors

What the exe does is known for one case: a source that does not parse leaves
no class and logs `JVM::compileSource: Syntax error in "%s" at line %d`
(`docs/ENGINE.md` 4.2). Its reaction to an unknown name is not known; one
class file suggests an unknown type compiles to `L;` without an error. The
host does not have to copy unknown behaviour, so the rule is: **a source the
compiler cannot translate with a proven rule either fails cleanly or is
compiled by the plain extension of the rules with a warning, never silently
differently.**

| Case | Default (host) | `strict` (oracle tests) |
| --- | --- | --- |
| character no rule accepts, unterminated string or comment | error `E_LEX`, no output | same |
| token sequence outside the grammar | error `E_SYNTAX`, reported in the exe's wording with file and line, no output | same |
| construct of appendix B (parses, never seen compiled) | compiled by the stated default, warning `W_UNVERIFIED` | error `E_UNSUPPORTED` |
| type name that resolves nowhere | descriptor `L;` as the exe (observed), warning `W_TYPE` | error `E_TYPE` |
| identifier that is no local, field or class | error `E_NAME` | same |
| member not found in a dotted name | error `E_MEMBER` | same |
| method not found, or an argument of unknown type | the bare name (this is the proven rule, not an error) | same |
| source longer than 65,535 lines (the line field is 16 bits) | error `E_LIMIT` | same |
| byte above 0x7F in a string or comment | copied as it is, warning `W_BYTE` (no shipped source has one) | same |

An error means no bytes are returned and nothing is written; the loader then
reports the class as missing exactly as it does today, after logging the
diagnostics. The compiler never throws across its interface and never aborts
the process; the N1 test of `grammar.md` (3,067 of 3,068 single-token
deletions rejected) becomes a robustness test: every mutated source must end
in diagnostics or in a class file, never in a crash.

### 2.10 Where the output goes

The exe writes `X\Name.class` beside `X\src\Name.java` and gives it the
source's time stamp. It considers a class stale when the two time stamps
differ or the header fails its build test, and deletes a stale class before
it compiles (`docs/ENGINE.md` 4.2). The host must not modify the player's
install by default, and must not copy the time rule: time stamps do not
survive a copy (in the corpus copy 1,433 of 2,819 pairs have differing
stamps, `spec\out\stale_detect.txt`), and the rule would recompile most of
the game.

**Lookup order in the class loader** for a class whose file would be
`X\Name.class` (`Jvm::load_class`, `Jvm::load_class_file_named`):

1. the class is loaded: done;
2. `X\Name.class` exists in the install: load it, whatever its age or build
   number (today's behaviour; 159 class files of older builds load as shipped);
3. `X\src\Name.java` exists (the directory is spelled `src`, `Src` or `SRC`):
   a valid cache entry is loaded; otherwise the source is compiled, loaded from
   memory, and written to the cache;
4. otherwise the class is missing, as today.

**Cache directory**: `%LOCALAPPDATA%\slrr-reverse\script-cache\<install key>\`,
where the key is a hash of the install's absolute path; override with
`SLRR_SCRIPT_CACHE_DIR`. Inside, the install-relative path of the class file
(`cars\racers\Axis_data\scripts\Axis_L_taillights_dark.class`): an ordinary
class file that the same loader reads. Beside the files one manifest with,
per entry: size and hash of the source bytes, compiler version, float mode,
and for each class in `Result.consulted` a hash of its declarations. An entry
is valid when all of these match; a mismatch means recompile (a part script
compiles in well under a millisecond in C++; the Python spike needs 5 ms).
Writes are atomic (temporary file, rename); a cache that cannot be written is
not an error.

**Opt-in** `SLRR_SCRIPT_WRITE_INSTALL=1`: additionally write `X\Name.class`
beside the source with the source's time stamp and build number 0x24F9D, which
is what the stock exe would have done and what it accepts as fresh. Never
delete a file of the install.

Other switches: `SLRR_SCRIPT_COMPILE=0` (no compiling, today's behaviour),
`SLRR_SCRIPT_STALE=exe` (opt-in: treat a class as stale by the exe's time
rule and compile its source into the cache instead), `SLRR_SCRIPT_FLOAT=A|B|C|D`.

Two properties of the install the loader has to respect: class names are not
unique (`java.game.cars.kit_Custom_body` and `kit_Tuner_body` are declared by
8 of the 635 sources each, `java.game.track_data` by 21 maps), so a cache
entry is keyed by path, never by class name; and 619 of the 635 sources sit
in directories the classpath does not reach from their package, so they are
found by the path the pack payload names, not by name.

## 3. Milestones

Every milestone has a test script that prints one headline line with the
counts, and a baseline. The numbers under "today" are measured by the spike
or by the analyses; a milestone is reached when its own script prints them
for the code under test. M1 to M5 are run twice: for the Python reference and
again, through the same scripts with `--impl cpp`, for the C++ compiler (M7).

| # | Milestone | Script | Headline it must print | Today |
| --- | --- | --- | --- | --- |
| M0 | Baseline frozen | `spec\run_spec.py` | `all as stated` | done: 2,649 / 2,651, 153 / 153, 635 / 635 |
| M1 | Tokenise | `tests\t1_lex.py` | `M1 tokenise 3454/3454 sources, 852211 tokens` (the end marker of each source not counted) | `lib\check_parse.py` L1 to L3 |
| M2 | Parse | `tests\t2_parse.py` | `M2 parse 3454/3454 sources` (tokens regenerated from the AST equal for all) | `lib\check_parse.py` P1 to P5 |
| M3 | Declarations and pool prefix, from the source alone | `tests\t3_decl.py` | `M3 declarations 2651/2651 oracle pairs` (2,705 classes; CLSS 2,705, FILD 238, MTHD 2,701 chunks; 31,141 prefix entries) | `spec\decl_expected.py` |
| M4 | Trees | `tests\t4_trees.py` | `M4 trees 7568/7570 node for node` with the float setting chosen per file; `7396/7570` with setting A | spike |
| M5 | Whole files | `tests\t5_files.py` | `M5 files 2649/2651 byte-identical` (per-file float setting; 2,561 with A alone); older builds `153/153`; stale `0/15`; pool (CONS) exact for 2,703 of 2,705 classes | spike |
| M6 | The 635 through the cache | `tests\t6_source_only.py`, host self-test | `M6 source-only 635/635 compiled`, hashes equal to the baseline, layout rules and stack simulation hold, statement and expression structure 767 / 767 trees, sibling check 522 / 522; host: `635/635 cache files loaded` | compiled and checked: spike (`spec\spike_source_only.py`); loaded: not tested |
| M7 | C++ compiler equal to the reference | `tests\t7_cpp.py` | `M7 cpp 3454/3454 sources equal to the reference` (tokens, AST dump, rows, trees, bytes); M1 to M5 headlines with `--impl cpp` | - |
| M8 | In the engine, on demand | host self-test, saved career | `635/635 compiled and loaded`; the career whose car has the class-less rim `rim_MT_Mescaline_15_0_15_ET__100_LOD_CATALOG_GARAGE` leaves the main menu | - |
| M9 | The exe confirms or corrects | `tests\t5_files.py --float exe` | one float rule: `2649/2651` without a per-file choice, or the state it depends on written down | - |

Order and reasons:

- **M1 to M3 give numbers on the first day** and need no tree: the scanner,
  the parser and the declarations already exist in `lib\` and are only
  re-homed and wired to the harness. M3 is the first number that needs new
  code in the reference (the provider, the pool, the rows without the
  analysis models).
- **M4 before M5** because a tree can be compared through what its operands
  name before the pool order exists; M5 adds the two pool passes and the
  writer. Sub-steps of M4 in the order of the corpus mass: field initialiser
  trees (2,331), part constructors, then everything.
- **M6 before the C++ port** because it delivers what the host needs (saved
  cars with class-less parts) with a small loader change: the reference fills
  the cache directory offline (`python -I slrrc\cli.py compile-missing
  --install <root> --cache <dir>`), the loader only learns to look there
  (step 3 of 2.10 without the compile). `spec\out\source_only_classes\`
  already has this layout and can be used to try the loader half today.
- **M7** is a port against an oracle that answers for every input; its test
  is equality with the reference on all 3,454 sources at five levels, so a
  difference is located in the module that caused it.
- **M9** needs Ghidra (locked while this was written): the scanner's float
  action; where the compiler gets a pack's name; how it looks up a class by
  simple name; the yacc tables (the LALR(1) reading of the line rule). It can
  run at any time and changes only the float default and three fallbacks.

**Regressions.** `tests\baseline\` stores, per milestone, the sorted list of
passing items: `m1.pass` and `m2.pass` (source paths), `m3.pass` (class
files), `m4.pass` (`file#blob#tree`), `m5.pass` (class files) with
`m5_float.tsv` (the float setting each file needs), `m5_older.pass`,
`m6.sha256` (path and SHA-256 of each of the 635 outputs), and
`expected_fail.tsv` (the 15 stale and the 2 unreproducible pairs with the
reason). A test run computes its pass list and compares: an item of the
baseline that no longer passes is a regression, the script names it and exits
with 1; a new pass is reported and enters the baseline only with `--update`,
which refuses when anything was lost. `tests\run_tests.py` runs all scripts
and prints the headlines. The seed of the baseline is the spike's output:
`spec\out\spike_pass_oracle.txt` (2,649), `spike_pass_older.txt` (153),
`spike_pass_source_only.txt` (635), `spike_source_only.sha256`,
`spike_float_modes.tsv` (the 2,802 identical files of both sets with the first
float setting that reproduces each: 136 without a float literal, A 2,578,
B 60, C 22, D 6).

For the 635 there is no shipped class, so a regression there is any change of
a hash; a wanted change (for example a new float default) is re-baselined on
purpose and still has to pass the structure checks, the sibling check, and,
from M6 on, the host's loader.

## 4. The first implementation step

**Goal**: the reference compiler's package, its test harness and baseline, and
the front half of the compiler (M1, M2, M3) without any import from
`analysis\`. It ends when one command prints the three headlines below.

**Files to create** (all under `C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler\`):

| File | Content |
| --- | --- |
| `slrrc\__init__.py` | `VERSION`; `class CompileError`; `front(text, name, provider) -> [ClassBuild]` (this step); later `compile_source(text, name, provider, packs, options) -> bytes` |
| `slrrc\classes.py` | `FieldInfo(name, desc, is_static)`, `MethodInfo(name, desc, is_static)`, `ClassInfo(name, super, interfaces, is_interface, fields, methods)`; `class ClassProvider: find(fqn)`; `class CorpusProvider(ClassProvider)` built from `lib\oracle.py` (class files through `lib\tufa.py`; sources without a class through `declare_unit`); `class UnitScope(ClassProvider)` (the unit's own classes over another provider); `resolve_type(unit, scope, simple_name) -> fqn or None` (own file, own package, imports in source order, `java.lang`); `descriptor(type_node, unit, scope) -> str`; `method_descriptor(method_node, unit, scope) -> str` (no return part for a constructor); `declare_unit(unit, provider) -> [ClassInfo]` |
| `slrrc\pool.py` | `class Pool`: `utf8(s)`, `cls(name)`, `nat(name, desc)`, `ref(cls, name, desc)`, `rid(pack, local_id)`, `entries`, `index`, `to_cons()` (the tuples `lib\tufa.py` writes) |
| `slrrc\decl.py` | `seed(pool, info)`; `type_walk(pool, class_node, unit, scope, first_in_file)`; `names_and_rows(pool, class_node, info) -> Rows` (four row vectors, `clss`, tree owners in source order); `locals_word(method_node) -> int`; `declare_class(unit, class_node, scope, first_in_file) -> ClassBuild(info, pool, rows, owners)` |
| `tests\harness.py` | corpus iteration over `lib\oracle.py` (oracle pairs, older, stale, source only); `Baseline.load / compare / update`; headline printing; `--impl py\|cpp` switch (cpp raises "not built" until M7) |
| `tests\t1_lex.py` | every source tokenises; token count; tokens, comments and white space tile the file |
| `tests\t2_parse.py` | every source parses; tokens regenerated from the AST (`lib\unparse.py`) equal the tokens read |
| `tests\t3_decl.py` | per oracle pair and class: predicted pool prefix equals the head of the shipped pool; CLSS, FILD, MTHD payloads byte-identical; tree count; the same for the other pairs; and for all 3,454 sources equality with `analysis\decl\poolmodel.py` (steps 1 to 3 and rows) as a cross-check that covers the 635 |
| `tests\run_tests.py` | runs the scripts named on the command line, compares with `tests\baseline\`, exit code |
| `tests\baseline\` | `m1.pass`, `m2.pass`, `m3.pass`, `expected_fail.tsv`; and, copied from `spec\out\`, the seeds for M5 and M6 |

`lib\lexer.py`, `lib\parser.py`, `lib\tufa.py` and `lib\oracle.py` are imported
as they are. The rules to implement are `declarations.md` 12 (the block
"at the class header" to "at the closing brace") and 4.5 (locals word); the
code to read is `analysis\decl\poolmodel.py` (`phase_seed`, `type_events`,
`body_types`, `phase_names`, `member_rows`, `max_live`), which works on a
converted tree; the new code works on the `lib\parser.py` tree directly.

**Test command**

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I tests\run_tests.py m1 m2 m3
```

**What it must print**

```
M1 tokenise      3454/3454 sources, 852211 tokens
M2 parse         3454/3454 sources
M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
baseline         0 lost
```

The M3 number is today's value of `python -I spec\decl_expected.py` (last
line `M3 declarations 2651/2651 oracle pairs`), which computes the same thing
with the analysis model.

**The step after it** (M4, M5): `slrrc\emit.py`, `floatlit.py`, `packs.py`,
`write.py`. Its target is equality with the spike for every source, which the
harness checks by calling `spec\spike_compiler.compile_source` on the same
text: 2,649 + 2 + 153 + 15 + 635 = 3,454 files with identical bytes, of which
2,649 and 153 also equal the shipped class. The code to fold together is
`analysis\stmt\stmtgen.py` (its `Gen`, minus the oracle window and the
statistics) and the subclass `FullGen` in `spec\spike_compiler.py`; the
method lookup is `gen.Model.find_method_ex` with its `type_score` and
`distance`; the two pool passes are `poolmodel.resolve_tree` and `emit_node`.

**The first C++ step** (M7), for orientation: `sc_lexer.cpp` and a `slrrc`
tool with `--dump tokens`; `tests\t1_lex.py --impl cpp` compares the dump with
`lib\lexer.py` token for token and must print the M1 line.

## 5. Risks

**Classes compiled by older builds.** 159 class files carry build 0x24F5C or
0x11AE1; 156 have a source, 153 of those are consistent, and all 153 are
reproduced byte for byte when the header carries their build number (float
setting A). Nothing distinguishes the older compilers on this sample, but the
sample is 154 trees that are almost all part constructors: loops and switches
under the older builds are untested. The exe treats such a class as stale and
recompiles it; the host loads it as shipped (2.10), so the question only
matters for the oracle. They stay in the test as a second set with their own
baseline (`m5_older.pass`).

**Sources that differ from the class that ships.** 17 pairs are known: 15
found by `lib\paircheck.py` (token census and line check) and 2 found only by
compiling (`Center_Differential`, `Garage`). How the candidate tests do
(`spec\stale_detect.py`):

| Test | Finds of the 17 | False alarms among the other 2,802 |
| --- | ---: | ---: |
| rows of CLSS / FILD / MTHD predicted from the source differ from the class (method and field lists) | 0 | 0 |
| token census and line check | 15 | 0 |
| compile and compare | 17 | 0 once floats and pack names are set per file |
| time stamps differ (the exe's rule) | 9 | 1,424 in this copy |

So method lists that disagree are not a usable signal in this install: every
stale pair was edited inside a body (a string, a constant, a float). The
working procedure is the classification `spec\spike_check.py` already does on
a file that is not identical: float bits only (try the other settings); one
pack name; one callee node whose descriptor disagrees with the class database
(compiled against another class version); anything else is a stale source or
a compiler bug and goes to a person. The time stamps of the corpus say
nothing: all 6,375 files carry the day the copy was made.

**Encoding.** All 3,454 sources are 7-bit ASCII with CRLF line ends, 237
without a final line end, every one with TABs; 51 string literals hold a raw
TAB. The compiler treats the source as bytes: no decoding, lines counted by
LF, CR is white space, a TAB inside a string is kept. Untested and therefore
decided here: a byte above 0x7F is copied (warning), a UTF-8 byte order mark
is skipped (warning), a lone CR does not end a line. Path matters too: the
source directory is spelled `src`, `Src` or `SRC`; pack names use
backslashes; class names are not unique across directories.

**Constructs with too few examples.** Proven, but on a handful: `do` (3),
`continue` (1, in a `for`), `?:` (5), `default` (6, always last), `return;`
in a constructor (1), the lookahead rule for a nested `if` / `else` (2) and a
loop (3) in a then-branch, a `for` with condition and no update (1), `^` `~`
`&=` `^=` (1 each), `<<` (2), `>>` (6), `true` / `false` (10), `this(..)`
(13), static methods with code (70), an interface (1, empty). Each of these
is one rule in the emitter; the tests report per rule how many corpus
constructs exercise it, and a rule below ten is listed in the test output so
that a change to it is looked at by hand. None of them occurs in the 635
sources, which are simple (census over their syntax trees): 3,097
assignments, 1,737 calls, 499 written `super(..)`, 285 array elements, 178
binary operators (`==` `>=` `<=` `<` `>` `&&` `|`), 139 unary (`-`, `!`), 131
`if`, 63 local declarations, 37 returns, 16 `new`, 13 casts, 8 compound
assignments, 6 `instanceof`; no loop, no `switch`, no `?:`, no shift, no
static method, no `this(..)`, no `true` / `false`.

**Constructs never compiled** (appendix B). Unknown in the exe. Because the
host runs the result on a port of the game's VM, "what the VM does with the
nodes" defines correctness for a mod even where the exe's emission is
unknown; the defaults of appendix B are the forms the interpreter already
handles. They are flagged `W_UNVERIFIED` and cannot claim byte identity.

**The float conversion** stays state of the exe until the scanner is read
(1.4). Risk for the plan: none for function (3 units in the last place at
most), 88 files for byte identity with a fixed setting. If the exe turns out
to depend on the FPU state of the moment, "byte-identical to the exe" is not
a property a compiler can have for floats, and the criterion becomes
"byte-identical under one of the documented settings", which is what the
tests measure today.

**Dependence on other classes.** A compiled class embeds what other classes
declared at compile time. Consequences: the oracle test needs a provider that
answers from the shipped classes (not from a newer source); the cache must be
keyed on the consulted declarations; and the order in which the host meets
classes can change a result when two versions of a class exist (nine dialog
classes ship twice, three of them differ; no shipped source depends on the
difference).

**The host's loader.** `tufa.cpp` decodes the pool heuristically and is known
to misread two shipped classes. Whether it reads all 635 new class files is
unknown until M6 runs; a failure there is a loader fix, not a compiler change
(the strict reader accepts all 635). The loader also keeps one class per
name, so the second `kit_Custom_body` it meets is ignored.

**Line rules read from line numbers.** That the exe's grammar is the LALR(1)
grammar of the Java specification is inferred from 590,101 lines, not read in
the exe. It cannot be wrong for a shipped source; for a new source with an
unusual layout (a statement split over lines in a then-branch) a line could
differ, which changes an error message of the VM and nothing else.

**Parallel work.** The working tree of `slrr-reverse` changed while this was
written (`git status` listed 15 entries at the start and 23 at the end, all
under `engine/`); this task wrote none of them. The C++ compiler is therefore
developed outside the repo until M7 and enters it as new files only.

## 6. Reproducing

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I spec\run_spec.py            # every number of this document; about 2 minutes
python -I spec\run_spec.py --quick    # without the float simulation
python -I spec\spike_compiler.py game/Trigger          # compile one source, compare
python -I spec\spike_check.py --only game/Navigator    # one pair through the check
```

| File in `spec\` | What it is |
| --- | --- |
| `spike_compiler.py` | the composition spike: `Env()`, `compile_source(text, env, float_mode, pack_rule, build, name, own_from_source, parsers)` |
| `spike_check.py` | the spike over all pairs and the 635; classification of every file that is not identical; writes the pass lists |
| `spike_source_only.py` | compiles the 635 into `out\source_only_classes\`, checks them without an oracle, writes `out\spike_source_only.sha256` |
| `float_models.py`, `float_files.py` | 234 candidate float conversions in exact arithmetic; which files need which |
| `float_closed_forms.py` | the four conversions that occur, in plain float / double arithmetic, checked against the exact simulation |
| `rid_packs.py` | pack name spelling: source, pool, disk |
| `blockers.py` | sources each open question touches |
| `stale_detect.py` | which test finds the stale pairs |
| `decl_expected.py`, `expected_totals.py` | the M3 number; the ceilings of M4 and M5 computed from the separate proofs |
| `spike_parsers.py` | the spike on `lib\parser.py` against the spike on the three analysis parsers, every source |
| `run_spec.py` | runs all of them and compares 18 headline numbers |
| `out\` | the outputs quoted here; `out\source_only_classes\` the 635 compiled class files; `out\spike_pass_*.txt`, `spike_float_modes.tsv`, `spike_source_only.sha256` the seeds of the test baseline |

## Appendix A. Where each rule is written down

| Rule | Text | Code that implements it today |
| --- | --- | --- |
| lexical grammar | `grammar.md` 2 | `lib\lexer.py` |
| syntactic grammar, AST | `grammar.md` 3, 11 | `lib\parser.py` |
| class names, descriptors, rows, flags, tree numbers, locals word | `declarations.md` 3 to 5, `grammar.md` 4 | `lib\symbols.py`, `analysis\decl\poolmodel.py` (`member_rows`) |
| pool steps 1 to 3 | `declarations.md` 6.2 to 6.4, 12 | `poolmodel.py` (`phase_seed`, `type_events`, `phase_names`) |
| pool steps 4 and 5 | `declarations.md` 6.5, 6.6 | `poolmodel.py` (`resolve_tree`, `emit_node`) |
| statements, locals, jumps, epilogue | `statements.md` 2 to 6, 11 | `analysis\stmt\stmtgen.py` |
| line of every node | `statements.md` 8, `expressions.md` 7 | `stmtgen.py` (`end_tok`, the `lt` argument of every `emit`) |
| expression forms, operator codes | `expressions.md` 3, 4; `opcodes.md` | `stmtgen.py` (`expr`), `analysis\expr\gen.py` |
| names, field refs, paths | `expressions.md` 5 | `spec\spike_compiler.py` (`head`, `walk`, `name`), `gen.py` (`head`, `walk`, `access`) |
| calls, method ref or name, lookup | `expressions.md` 6 | `spec\spike_compiler.py` (`arg_type`, `callee`, `call_path`), `gen.py` (`find_method_ex`, `type_score`, `distance`) |
| class file layout | `foundation.md` 3 | `lib\tufa.py` |
| opcodes, node sizes | `opcodes.md` | `lib\opcodes.py`, `engine/include/tree_op_flags.inc` |

## Appendix B. Constructs no source uses, and the default

The parser accepts them (with a note); the exe's emission is unknown. In
`strict` mode each is `E_UNSUPPORTED`. Otherwise:

| Construct | Default | Why |
| --- | --- | --- |
| case group that falls through; several labels on one group; `default` not last; empty group | groups stay contiguous, one `case` node per label in source order, the `default` jump goes to its group wherever it stands | the node layout makes fall-through automatic (`statements.md` 4) |
| `continue` in `while` / `do` / a `for` without update / inside a `switch` in a loop | jump to the condition (`while`), to the condition test (`do`), to the closing jump (`for`); the enclosing loop for a `switch` | one class file without source shows `while` (`statements.md` 10.3) |
| comma lists in a `for` header | each element as an expression statement in order | `stmtgen.py` already does this for a list of one |
| empty statement `;` | no node | - |
| `%=` | kind 0x26, its slot in the kind table | the VM has the body (`opcodes.md` table 2) |
| `>>>`, `<<=`, `>>=`, `>>>=` | error `E_UNSUPPORTED` in both modes | their slots would be 0x0C, 0x29, 0x2A, 0x2B, but the VM has no body for 0x0C and reports the other three as unsupported |
| compound assignment as a value; nested `?:` | the `expr` form of the same kind | principle "value or statement" (`expressions.md` 2) |
| `new T(..);` as a statement | `stmt new` as its first node | one class file without source |
| `super.field` | error `E_UNSUPPORTED` in both modes | the VM's node for it cannot work (`opcodes.md`, op 29) |
| array initialiser, more than one dimension, labels, character literals | error `E_UNSUPPORTED` in both modes | no VM support or no token rule |
| `int a[]`, qualified type names, single-type imports, `final` on a local or parameter | accepted with their obvious meaning | no effect on emission rules |
| a call whose name is also a local in scope | compiled as a call (the local is ignored), `W_UNVERIFIED` | `Texture.<init>(I)` suggests the exe emits the local instead; one case, no source |
| arguments `true` / `false`, `(x)`, `!x`, `new T[n]`, `instanceof` in the method-ref decision | `true` / `false` count as typed (int), the others as untyped: the call ends in the bare name | the bare name is always correct at run time |
| value method that does not end in `return`; void method that ends in `return;` | implicit return for constructors and void methods regardless of the last statement, none for value methods | the reading that needs no flow analysis (`statements.md` 2.2) |
