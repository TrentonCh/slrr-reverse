# Compiler foundation: tools, proofs, and what is settled

Written 2026-10-11. Everything here lives under
`C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler\`; nothing in the
slrr-reverse repo or in the game installs was touched, no exe and no Ghidra was
run. The corpus is the read-only copy of the Build 940 install at `D:\SLRR-test`.

The goal of this line of work is a compiler whose output is byte-identical to
what the game's own compiler wrote. This document is the base for that: a
strict reader and writer of the class format, a disassembler, the corpus index
with the list of pairs that can serve as an oracle, and an opcode reference.
Every statement below carries its evidence: **proven** means a script in `lib/`
checks it against the whole corpus and the count is quoted; **observed** means
seen in examples; **unknown** is said outright.

## 1. What is here

| File | What it is |
| --- | --- |
| `lib/tufa.py` | Strict reader and writer of `.class` files (format "TUFA") |
| `lib/disasm.py` | Disassembler, text and JSON, with `--class --blob --method --tree --field` filters |
| `lib/opcodes.py` | The opcode table as data: 48 op bytes, 47 expression kinds, immediates, stack effects |
| `lib/oracle.py` | Corpus index and helpers: iterate pairs, load class / source, compare blobs |
| `lib/srclex.py` | Light tokenizer for the script dialect (analysis only, not the compiler's scanner) |
| `lib/paircheck.py` | Decides whether a class still belongs to its source (token census, line check) |
| `lib/check_roundtrip.py` | Proof 1: read and re-write every class file byte for byte |
| `lib/check_layout.py` | Proof 2: 33 rules about the layout (30 must hold, 3 measurements), over every blob |
| `lib/check_opcodes.py` | Proof 3: opcode structure, stack simulation, token census, line check |
| `lib/check_selftest.py` | Proof 4: the tools themselves (strictness, compare, disassembler) |
| `lib/gen_opcodes_md.py` | Writes `notes/opcodes.md` from the table and the corpus |
| `lib/study_float_literals.py` | Data for the one open question that blocks byte identity today |
| `lib/run_all.py` | Runs all of the above in order (about 95 s) |
| `notes/foundation.md` | This file |
| `notes/opcodes.md` | Opcode reference for a compiler writer (generated) |
| `notes/corpus.md`, `notes/pairs.json` | Corpus summary and the machine-readable index (generated) |
| `notes/*.txt`, `notes/*.json` | Output of the last run of each proof |

All scripts need only the standard library: `python -I lib/<script>.py`. The
corpus root defaults to `D:\SLRR-test`; set `SLRR_CORPUS` or pass `--corpus` to
change it.

## 2. Using the tools

```
python -I lib/disasm.py sl/Scripts/game/Trigger.class --method finalize
python -I lib/disasm.py sl/Scripts/game/MainMenu.class --class TextLine --no-pool --raw
python -I lib/disasm.py sl/Scripts/game/Marker.class --field RR_PLAYER --json
python -I lib/oracle.py show game/Marker          # index entry
python -I lib/oracle.py diff mine.class theirs.class
python -I lib/tufa.py some.class                  # round-trip one file
python -I lib/run_all.py                          # regenerate notes, re-run proofs
```

Paths given to `disasm.py` may be relative to the corpus. From Python:

```python
import sys; sys.path.insert(0, r'C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler\lib')
import oracle, tufa, disasm

for e in oracle.iter_oracle_pairs():        # 2,651 Build 940 pairs fit for byte comparison
    text  = oracle.load_source(e)           # str, one character per byte
    blobs = oracle.load_class(e)            # [tufa.Blob]
    mine  = my_compiler(text)               # [tufa.Blob] or bytes
    for i, d in oracle.compare_files(mine, blobs):
        if not d.equal:
            print(e.cls, i, d)              # first differing chunk, offset, what sits there

e = oracle.entry('game/Marker')             # one entry by name
b = oracle.load_class(e)[0]
b.name, b.super_name, b.cons, b.static_fields, b.instance_methods, b.trees
tufa.serialise(blobs)                       # bytes of a class file
tufa.serialise_blob(b, tufa.canonical_chunks(b))   # a blob built from scratch
print(disasm.disassemble(blobs, path=e.cls))
```

`compare_blobs` reports like this (one immediate changed in Marker):

```
first difference in TREE at +0x373 (tree 19 (method <init>(Ljava.util.resource.RenderRef;)) node 3 imm (op 0x01)); sizes 932 / 932
```

Other iterators: `oracle.iter_pairs(build, consistent)`,
`oracle.iter_entries(category)` (for the 635 sources without a class),
`oracle.iter_blobs(build)`, `oracle.iter_class_files()`.

`tufa.py` keeps pool strings as Python `str` decoded with latin-1 and
`oracle.load_source` decodes sources the same way, so source text and pool text
compare directly and no byte is ever lost. `tufa.Node` is `(op, slot, imm)`;
`.line` is an alias of `.slot` (section 3.6) and `.code` the 16-bit opcode.

## 3. The class format, settled

Proof 1 (`check_roundtrip.py`): **2,921 of 2,921 class files (8,433,322 bytes,
2,993 blobs) parse with the strict reader and serialise back byte for byte.**
The same holds through the JSON form (`Blob.to_dict` / `from_dict`) and when
the chunk list is derived from the content instead of copied from the file.
No file fails. Proof 4 feeds the reader 1,104 damaged variants of 8 files
(truncations, wrong sizes, padding bytes, unknown tags and pool kinds, bad op
bytes, counts off by one): 1,103 raise `TufaError`, 1 stays a well-formed
layout and is rejected by `tufa.validate` (a row count moved by one swallowed
exactly the next count word), none is accepted.

Rule ids (M1, T4, ...) refer to `check_layout.py`; counts are "holds for".

### 3.1 File and chunks

- A file is a flat stream of chunks `{tag:4 bytes, size:u32, payload}`, little
  endian, no padding anywhere. The 12-byte "header" is itself a chunk: tag
  `TUFA`, size 4, payload = build number. The exe's loader
  (`JVM_addClass_fromChunks`, 940 VA 0x554EE0, decompiled in
  `slrr-ghidra/addClass_940.c`) walks the stream to the end of the file: `CONS`
  opens a class, `TREE` closes it. So a file with several classes is one blob
  after the other with nothing between them (29 files hold 2 to 11 blobs).
- Chunk order inside a blob is always `TUFA CONS [FILD] [MTHD] CLSS TREE`.
  `FILD` is present exactly when the class has a field, `MTHD` exactly when it
  has a method (2,993 of 2,993 blobs: 2,659 without FILD, 328 with both, 4 with
  FILD only, 2 with neither).
- Build numbers: 0x24F9D in 2,757 files (2,829 blobs), 0x24F5C in 159 files,
  0x11AE1 in 5 files. No file mixes builds. Every rule of this document holds
  for the older builds too; the layout did not change between them.

### 3.2 CONS (constant pool)

`u32 count`, then entries `u32 kind` + payload:

| Kind | Name | Payload | Entries |
| ---: | --- | --- | ---: |
| 0 | utf8 | `u32 len`, len bytes, one NUL (len excludes it) | 99,620 |
| 3 | rid | `u32` utf8 index of the pack file name, `u32` local id | 7,242 |
| 4 | class | `u32` utf8 index of the dotted class name | 10,920 |
| 5 | ref | `u32` class entry, `u32` nat entry (fields and methods alike) | 43,264 |
| 7 | nat | `u32` utf8 name, `u32` utf8 descriptor or type | 42,543 |

- No other kind occurs. Ints and floats are node immediates, never pool
  entries.
- P1: no pool holds the same entry twice (203,589 of 203,589 entries).
- P3: an entry refers only to entries with a lower index (103,969 of 103,969).
- P4: every utf8 payload is 7-bit and free of NUL (99,620 of 99,620). 172
  entries hold a TAB or LF: string escapes are processed by the compiler.
- C2, C3: `pool[0]` is the utf8 of the class name and `pool[1]` its class
  entry (2,993 of 2,993); when there is a superclass `pool[2]` and `pool[3]`
  are its utf8 and class entry (2,989 of 2,989).
- P2 (measurement): 2,537 entries in 1,399 blobs are referenced by nothing:
  1,442 one-letter type names (`V`, `I`, `F`), 754 `L...;` type names, 6 array
  type names, 335 class entries. The pool is a log of everything the compiler
  interned, not a minimal table. What order it interns in is **unknown** and is
  the first thing a byte-identical compiler has to learn.
- Class entries of array types are named `[I`, `[F`, `[java.lang.String`
  (bracket + element class, without `L...;`); 129 such entries. They are the
  class of member refs like `array.length`.
- Descriptors: field types `I`, `F`, `Lpkg.Name;`, `[I`, `[F`, `[Lpkg.Name;`;
  method descriptors `(params)ret`. A constructor's descriptor has **no return
  part** at all: `<init>` carries `(...)`, every other method `(...)V` etc.
  (M2: 6,660 of 6,660 rows; 3,117 are constructors, by coincidence the
  same number as there are initialiser trees).

### 3.3 FILD and MTHD

Each is two counted vectors, statics first, then instance members:
`u32 n, n rows, u32 m, m rows`.

- Field row, 16 bytes: `flags, name utf8, type utf8, initialiser tree or
  0xFFFFFFFF`. 5,018 rows.
- Method row, 20 bytes: `flags, name utf8, descriptor utf8, tree or
  0xFFFFFFFF, locals`. 6,660 rows.
- Flags (one mask for both): 0x1 public, 0x2 protected, 0x4 private, 0x8
  static, 0x10 abstract, 0x20 final, 0x40 native, 0x80 synchronized.
  M7: for every row of every pair the mask equals the modifier keywords of a
  declaration of that name in the source (3,805 of 3,805 field rows, 5,411 of
  5,411 method rows). That proves 0x1, 0x4, 0x8, 0x10, 0x20, 0x40. 0x2 (3
  fields) and 0x80 (20 methods: 18 of `java.util.Vector` with flags 0xA1, whose
  JDK 1.0 originals are `public final synchronized`, and `Signal.set` /
  `Signal.waitFor` with 0x81) occur only in class files without a source, so
  their names are inferred. No other bit occurs.
- M1, F1: 0x8 is set on every row of the first vector and on none of the
  second (6,660 and 5,018 rows).
- **The fifth word of a method row is the number of local variable slots the
  body uses**: `this` (instance methods) + parameters + declared locals at the
  point of deepest nesting. M5b: it equals 1 + the highest local index used by
  any `local` / `declare` / `release` node of the tree (6,107 of 6,107 methods
  with code). M5c: equally `max(arguments including this, 1 + highest declared
  index)`. M5a: it is 0 when the tree is empty or absent (553 of 553). The
  interpreter's call frame constructor takes a slot count as the capacity of
  its locals vector; that this word is what it receives is inferred, not read
  from the exe.
- M3: the tree index is 0xFFFFFFFF exactly on the rows with the abstract bit
  (5 rows, all `public abstract`). M4: every native row owns an **empty** tree,
  0 nodes (418 of 418). 130 further rows own an empty tree without being
  native, 60 methods and 70 constructors: bodies written as `;` or as an empty
  `{}` (observed: `public AmateurDriftCup();`, `public void install() {}`).
  81 constructors consist of the implicit super call and `ret` alone
  (observed: `public TimeAttackTest(){}`), while no method consists of the
  bare `push_void; ret_value`.

### 3.4 CLSS

`u32 0xFFFFFFFF, u32 interface flag, u32 name, u32 superclass, u32 n, n * u32`,
all pool class entries (C1: 2,993 of 2,993). The interface flag is 1 on 3
blobs. The superclass is 0xFFFFFFFF only for `java.lang.Object` and the three
interfaces (C4). 28 blobs list one interface, 3 list two.

### 3.5 TREE

`u32 tree count`, then per tree `u32 node count` and the nodes. No per-tree
header beyond the node count, no terminator, no padding. 9,772 trees, 647,650
nodes (74,364 of 3 bytes, 573,286 of 7 bytes).

- T1: every tree is owned by exactly one row: 3,117 by a field (its
  initialiser), 6,655 by a method (9,772 of 9,772).
- T4, T5: trees are numbered in **source order**, fields and methods
  interleaved as they are declared: the line span of each tree ends before the
  next begins (6,243 of 6,243 neighbours) and the same across the blobs of a
  file (67 of 67). T2: inside each of the four row vectors the tree indices
  ascend, so the rows are in source order too (11,972 of 11,972 vectors).
- F3: an initialiser tree ends with op 0x10 and op 0x10 stands nowhere else
  (3,117 of 3,117).

### 3.6 Nodes

A node is `op:u8, line:u16[, imm:u32]`.

- `op` indexes the 48-entry table of `engine/include/tree_op_flags.inc`. The
  table value is the 16-bit opcode the VM dispatches on (0x1007, 0x4009, ...).
  Its high nibble is the class of the immediate: 0 none, 1 u32, 2 float bits,
  4 utf8 index. **The node is 7 bytes when that nibble is 1, 2 or 4, else 3.**
  N1: only 40 of the 48 op bytes occur; 0, 15, 19, 29, 30, 31, 46, 47 never do.
- **The u16 that ENGINE.md calls `slot` is the source line number.** The VM
  copies it to `jvm+0x70` before each instruction (940 `VMThread_run`), for
  error messages. N3: every node of every pair has a line inside its source
  file (596,793 of 596,793). Line check (`check_opcodes.py`, all 2,819 pairs):
  the token a node carries stands on the line the node records for string
  literals 12,780 of 12,791, int literals 26,051 of 26,058, float literals
  35,527 of 35,534, resource ids 9,063 of 9,063, member names 100,158 of
  100,174. All 41 exceptions lie in the 15 stale pairs of section 4. In the
  consistent pairs a member name is on the exact line 99,905 times and one line
  above 3 times (a call split as `x.setMatrix` / `(args)`), which shows that
  the line is the one the parser had reached when it built the node, not the
  line of the token itself.
- N2: every immediate that the interpreter uses as a pool index names an entry
  of the kind it expects (152,652 of 152,652): 0x1006, 0x4009, 0x4012, 0x401C,
  0x4025, 0x4026 utf8; 0x100E rid; 0x1019 class; 0x101A a ref with a `(...)`
  descriptor; 0x101B a ref with a type descriptor.

## 4. The corpus

`notes/corpus.md` has the tables; `notes/pairs.json` the index (one entry per
source, plus one per class without a source).

| What | Count |
| --- | ---: |
| Source files `<dir>/src/<Name>.java` (directory spelled src 3,363, SRC 63, Src 28) | 3,454 |
| Class files / blobs | 2,921 / 2,993 |
| Pairs, every blob at build 0x24F9D (`pair_940`) | 2,663 |
| Pairs at another build (`pair_other`: 152 at 0x24F5C, 4 at 0x11AE1) | 156 |
| Sources without a class (`source_only`) | 635 |
| Classes without a source (`class_only`: 95 under system/scripts, 7 under sl/Scripts) | 102 |

- Blob order and names: for all 2,819 pairs the top-level `class` / `interface`
  declarations of the source, prefixed with the package, equal the blob names
  in the same order. 23 sources declare more than one type.
- Encoding: all 3,454 sources are 7-bit ASCII with CRLF line ends only, no BOM,
  no NUL, every one contains TABs, 237 end without a line end. So "the right
  encoding" is ASCII; the helper decodes latin-1 to stay loss-free on foreign
  files.
- Dialect, by token census of all sources (`srclex.py`, strict, no source
  fails): no character literal, no float with an exponent or without digits on
  both sides of the point, 230 floats with an `f` suffix, 478 hex ints, no
  int with a leading zero, string escapes `\"` `\\` `\n` `\t` only, 51 strings
  holding a raw TAB, `true` / `false` exist (10 uses), no `synchronized`,
  `protected`, `try`, `throw`.

### Stale pairs: do not use them as an oracle

A shipped class was compiled from some version of its source. `paircheck.py`
finds the pairs where the source was edited afterwards, with two independent
tests: the per-file token census (55 token classes) and the line check of
section 3.6. Result: **2,651 of 2,663 `pair_940` and 153 of 156 `pair_other`
are consistent; 15 pairs are stale.** `oracle.iter_oracle_pairs()` yields the
2,651.

| Stale pair | Build | Evidence |
| --- | --- | --- |
| cars/racers/Baiern_data/scripts/Baiern_R_windshield_2 | 0x24F9D | class says "platsic", source "plastic" |
| cars/racers/Codrac_data/scripts/Codrac_L_taillights | 0x24F9D | 2 strings differ |
| cars/racers/Coyot_data/scripts/Coyot_RL_brake_light, Coyot_RR_brake_light | 0x24F9D | 2 strings differ each |
| parts/engines/.../Cyclone_4speed_transmission, V10_adjustable_6_speed_AWD_automatic_transmission, Duhen_D20CV_manual_transmission, Faktun_Group_Tran6_manual_transmission, Shimutshibu_RC_T5600R1_manual_transmission, V6_adjustable_6_speed_RWD_automatic_transmission, LP_V12_transmission_street | 0x24F9D | class holds `type = 1;` (2, 3), source `type = TT_AUTO;` |
| parts/running_gear/scripts/shocks/shock_absorber_SunStrip_22_rear | 0x24F9D | class 4100.0, source 3300.0 |
| parts/neons/scripts/neontube_L_Aqua, neontube_L_Blue, neontube_R_Aqua | 0x24F5C | lines shifted, strings and counts differ |

A pair that passes both tests can still differ from the shipped class in ways
no test here sees (section 7, floats); the tests only say the token stream is
the same.

## 5. Opcodes

`notes/opcodes.md` is the reference: table 1 lists every op byte with node
size, immediate, pops, pushes and corpus count; table 2 the 47 expression kinds
carried by `0x1007` (value used) and `0x1008` (statement); then name paths, the
proofs, and ten annotated examples with their source. Semantics come from
`jvm_vmthread_part1/2.inc` and from the Build 940 decompilation that already
lay in `slrr-ghidra` (`VMThread_run_940.c`, `Thread_evalName_real_940.c`,
`Thread_callMethod_fieldPath_940.c`); where the two differ the 940 code wins.

Headline results of proof 3 (`check_opcodes.py`, 9,772 trees, 647,650 nodes):

- **No opcode that occurs is unexplained.** 40 op bytes and 41 expression
  kinds occur; each has a body in the interpreter and a place in the tables.
- 27 positional rules: 25 hold without exception, 2 with the one irregular
  call named below. Examples: a literal carrier stands only behind
  `expr literal` (88,854), a `call` is preceded by its argument
  count and followed by a path that ends in a method (26,535 of 26,536), the
  count before a call through a method ref equals the parameter count of its
  descriptor (18,470), jump targets are inside the tree and never on a carrier
  (13,331).
- A simulation of every tree along all control-flow paths with the pops and
  pushes of the tables: 428,675 node executions, no stack underflow, depth and
  local count agree at all 9,115 joins, the stack is empty after every one of
  67,923 statements, `end` is reached with exactly one value, `ret` with none,
  `ret_value` with one and no live local (9,874 of 9,875).
- Locals: `declare` always takes the next free index, `release` always names
  the top one, every `local` is below the live count (48,186 of 48,186). The
  parameters are numbered in **reverse** (last parameter lowest): where a
  parameter is the receiver of a member ref, its type under that numbering
  names the class of the ref in 2,414 of 2,414 cases, under forward numbering
  in 2,072.
- Token census over the 2,663 Build 940 pairs, 55 token classes (every
  operator, every literal kind, `new`, `instanceof`, `case`, `switch`, `do`,
  casts, `super.x(`, `this(`, `[expr]`): 54 agree with the node counts in all
  2,663 files (unary `+` by having no node at all), int literals in 2,656 (the
  7 stale transmission classes). This is what ties each expression kind to its
  operator.
- Two irregular emissions exist, both in class files without a source:
  `Texture.<init>(I)` holds a `call` followed by a bare `local` (the callee
  name resolved to a local variable), and `Frontend` holds the only `new T(..);`
  used as a statement, which leaves the object on the stack.

## 6. What the compiler does (learned on the way)

Proven:

1. One blob per top-level type, in source order; class name = package + "." +
   name (2,819 pairs).
2. Chunk list, pool head, tree numbering, row order, flags, descriptors and the
   locals word as in section 3.
3. One node per operator, literal and keyword occurrence. Nothing is folded:
   `-1` is `int 1` + `expr neg` (9,521 unary minus tokens = 9,521 `neg` nodes).
   Nothing is removed: 21,400 nodes are unreachable, 20,753 of them `release`
   nodes behind a return. A unary `+` emits nothing (86 tokens).
4. `true` / `false` compile to the carrier 0x100D with 1 / 0 (13 of 13).
5. Every constructor with code holds exactly one `this_init` or `super_init`
   (3,044 of 3,044); the sources of the 940 pairs hold 2,469 `super(` tokens
   for 2,680 `super_init` nodes, so 211 are implicit, emitted as
   `push_int 0; super_init; stmt ctor`. `ret` (0x2A) stands only in
   constructors; every other method leaves through `ret_value`, a void method
   with the dummy `push_void` (10,825 of 10,825 nodes).
6. A string literal's pool text is the literal after escape processing (12,780
   of 12,791; rest stale).
7. A resource id literal `a.b.c:0xHEXr` becomes a pool rid entry whose local
   id is the hex value and whose pack name is `a\b\c.rpk` up to letter case:
   9,056 of 9,058 rid nodes of the consistent pairs carry a literal of their
   line in that sense; the other 2 are the fallback of question 3 in section 7.
   The letter case is not always the source's (same question).
8. The integer part of a float literal wraps modulo 2**32:
   `1000000000000000.0` compiles to 2764472320.0 and `50000000000.0` to
   2755359744.0 (9 nodes in 3 consistent pairs, all as predicted).

Observed (examples, not yet rules):

- Statement shapes: `if`, `for`, `do`/`while`, `switch`, `&&`/`||`, `?:`,
  declarations with initialiser, the two-fold release sequence after the final
  return. See the examples in `opcodes.md`.
- Assignment is compiled right-hand side first, then the target, then the
  assign node; array access index first, then the array.
- The update expression of a `for` is emitted behind the body and carries the
  line of the `for` header.

## 7. Open questions, most blocking first

1. **Float literals.** The bit pattern of a 0x200A node is not the correctly
   rounded value of its token: of 35,084 float nodes in the consistent 940
   pairs, 28,459 are correctly rounded, 6,605 are off by 1 ulp and 20 by 2.
   Worse, it is not a function of the token: 95 texts compile to more than one
   pattern (`0.0001` is 0x38D1B716 in `Baiern_oil_pan` and 0x38D1B718 in
   `LP_V12_oil_pan_racing`; the nearest float is 0x38D1B717). Two candidate
   conversions in 32-bit float arithmetic (digit by digit with a scale divided
   by 10, or multiplied by 0.1f) reproduce 34,481 and 33,873 samples, together
   35,012, and with correct rounding as a third variant all but 6. A class
   file follows one variant throughout: 188 files need the first, 60 the
   second, 10 look mixed, 2,256 cannot tell. So the variant is chosen per
   compilation: x87 precision state at compile time, or two compiler builds
   with one build number. The scanner's float action in the exe has to be
   read. Data: `notes/float_literals.json`, script `study_float_literals.py`.
2. **Pool order.** Which entries are interned, in which order, including the
   2,537 that nothing references (section 3.2).
3. **Pack names of resource ids.** In 1,019 of 9,058 rid nodes of the
   consistent pairs the pool spells the pack path with other letter case than
   the source (`cars.racers.baiern` gives `cars\racers\Baiern.rpk`). The pool
   spelling equals the file name on disk for 7,111 of 7,121 rid entries, some
   source spelling for 7,048, and 73 match no spelling in their source, so the
   compiler takes the name from somewhere else: the resource system's pack
   table is the likely source (**unknown**). `TimeAttackTest` compiles a
   literal whose pack does not exist (`old_stdpack_231`) to `system.rpk:0`,
   while 7 sibling classes keep that pack name (14 entries): the compiler
   looks the pack up at compile time (observed, one file).
4. **Symbol resolution.** When a call path ends in a method ref (8,208) and
   when in a bare name (8,065); when a member is reached by `field` / `path`
   and when by `getfield` / `call_on`; which class a ref names.
5. **Line attribution.** The exact rule (the parser's line at reduction time)
   for nodes that carry no token: jumps, releases, the implicit super call.
6. **Scopes.** Where `release` nodes are emitted and why the sequence after
   the last return appears twice.
7. **Empty bodies.** Which of `;` and `{}` gives an empty tree, for methods
   and for constructors (numbers in section 3.3).
8. **Older builds.** Whether 0x24F5C and 0x11AE1 classes differ from 940
   output in anything but the build number. Their layout does not.
9. **Flags 0x2 and 0x80**: names inferred, no source uses them.

## 8. Reproducing

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler\lib
python -I run_all.py
```

| Step | Output | Result of the last run |
| --- | --- | --- |
| `oracle.py build` | `pairs.json`, `corpus.md` | 3,454 sources, 2,921 class files, 0 problems |
| `check_roundtrip.py` | `roundtrip.txt` | 2,921 of 2,921 identical, 0 failures |
| `check_layout.py` | `layout.txt` | all 30 rules hold; 3 measurements |
| `check_opcodes.py` | `opcode_checks.txt` | all rules hold; 3 known exceptions in 2 class files |
| `gen_opcodes_md.py` | `opcodes.md` | 810 lines |
| `check_selftest.py` | `selftest.txt` | 1,104 damaged inputs rejected, compare 34 of 34, disassembler stable on 2,921 files |
| `study_float_literals.py` | `float_literals.txt` | numbers of question 1 |
