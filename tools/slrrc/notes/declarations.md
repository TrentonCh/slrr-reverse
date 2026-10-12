# Declarations and the constant pool: how the compiler builds CLSS, FILD, MTHD and CONS

Written 2026-10-11. Scripts: `compiler\analysis\decl\`. Nothing in the
slrr-reverse repo, the installs or Ghidra was touched; no exe was run. The
corpus is the read-only copy of the Build 940 install at `D:\SLRR-test`.
This note builds on `notes\foundation.md` (format, tools, corpus) and answers
its open question 2 (pool order) in full.

Status words as in the foundation: **proven** = a script checks the rule on
the whole corpus and the count is quoted; **observed** = seen in examples;
**unknown** = said outright. "Undetermined" means the corpus holds no case
that could tell two rules apart.

## 1. Result

Everything of a class file except the bodies of the trees is now reproduced
byte for byte.

| What | Predicted from | Oracle pairs (2,651 pairs, 2,705 classes) | All pairs (2,819 pairs, 2,873 classes) |
| --- | --- | ---: | ---: |
| CONS (entry list, exact order) | source + the trees as oracle | 2,705 of 2,705 | 2,873 of 2,873 |
| CLSS | source alone | 2,705 of 2,705 | 2,873 of 2,873 |
| FILD (present in 238 classes) | source alone | 238 of 238 | 238 of 238 |
| MTHD (present in 2,869 classes) | source alone | 2,701 of 2,701 | 2,869 of 2,869 |
| Chunk list, tree count | source alone | 2,705 of 2,705 | 2,873 of 2,873 |
| Whole class file rebuilt around the shipped TREE | | 2,651 of 2,651 files | 2,819 of 2,819 files (7,837,802 bytes) |

- The 12 stale Build 940 pairs and the 156 pairs of older builds (0x24F5C,
  0x11AE1) are reproduced as well: no rule here differs between the builds.
- The four chunks are 49.3% of the bytes of the pairs (CONS 45.0%, MTHD 2.0%,
  CLSS 1.0%, FILD 0.8%, the TUFA headers 0.4%); TREE is the other 50.7%.
- The pool rules that need no source hold on all 2,993 blobs of the corpus,
  the 120 blobs without a source (runtime library) included (section 6.9).
- All 2,537 pool entries that nothing refers to are explained (section 6.10).
- For the 635 sources without a class file every class name resolves, CLSS /
  FILD / MTHD are fully determined, and none of the undetermined rules
  matters (section 10).

"The trees as oracle" means: the model reads from the shipped TREE chunk
which class, member (class, name, descriptor), string and resource id every
node names, in node order. That is the output of symbol resolution and of
code emission, which this note does not derive. It never reads a pool index;
the indices are what it predicts.

## 2. Scripts

All need only the standard library; run from `compiler\analysis\decl`.

| File | What it is |
| --- | --- |
| `srcparse.py` | Recursive-descent parser of the dialect (analysis parser; accepts 3,454 of 3,454 sources) |
| `universe.py` | The classes of the corpus by package (3,585 class names in 53 packages), cached in `out\universe.json` |
| `poolmodel.py` | **The model**: `Pool` (interning pool) and `Model` (steps 1 to 5, CLSS / FILD / MTHD rows, the locals word). Every rule is an option, so alternatives can be tested |
| `check_parse.py` | Parser acceptance; declared members against row counts |
| `check_decl.py` | **Proof run**: CONS, CLSS, FILD, MTHD and whole files, byte for byte, on every pair |
| `check_alternatives.py` | One rule switched at a time: how many classes then fail; accounting of entries by rule |
| `check_tail.py` | The pool rules checked without a source on all 2,993 blobs (T1 to T8), rule variants, the twin classes, the unreferenced entries |
| `check_source_only.py` | The 635 sources without a class file |
| `libbridge.py`, `check_bridge.py` | The model on the AST of `lib\parser.py` (the parser of the language work): same pools and rows from both parsers for 3,508 of 3,508 classes of all sources; all four chunks byte for byte for 2,873 of 2,873 pair classes |
| `study_refclass.py` | Which class a member ref names |
| `stats_decl.py` | Census: flags, descriptors, bodies, sharing |
| `poolwho.py` | Tool: pool of a class with who refers to each entry; `--model` adds the rule that interned it; `--src` the source |
| `explore.py` | Tool: first mismatch per class under any option (`--opt k=v --show N`) |
| `run_decl.py` | Runs all of the above (about 110 s), logs under `out\` |

```
python -I run_decl.py
python -I poolwho.py game/Trigger --model --src
python -I explore.py --opt ret=before --show 2
```

From Python:

```python
import sys; sys.path[:0] = [r'...\compiler\analysis\decl', r'...\compiler\lib']
import oracle, srcparse, universe, poolmodel
e = oracle.entry('game/Navigator')
unit = srcparse.parse(oracle.load_source(e))
blob = oracle.load_class(e)[0]
m = poolmodel.Model(unit, unit.classes[0], blob, universe.load()).run()
m.pool.entries      # predicted pool, content form: ('utf8', s) ('class', name)
                    # ('nat', name, desc) ('ref', class, name, desc) ('rid', pack, id)
m.pool.phase        # the rule that interned each entry
m.member_rows()     # static fields, fields, static methods, methods, CLSS, tree count
poolmodel.to_indexed(m.pool.entries)    # -> tufa.Blob.cons
poolmodel.Model(unit, cls, None, uni)   # no class file: steps 1 to 3 and the rows

import parser as libparser, libbridge   # the same on lib\parser.py
unit = libbridge.convert(libparser.parse(oracle.load_source(e)))
```

## 3. Blobs and CLSS

- **One blob per top-level type, in declaration order; every blob has its own
  pool**, built from nothing: the model predicts each blob on its own, the 54
  later classes of the 23 multi-class sources included. **proven** (2,873
  classes).
- CLSS is `{0xFFFFFFFF, interface flag, name, superclass, n, n interfaces}`:
  - interface flag 1 for `interface`, else 0;
  - name = the class entry of the class, always pool[1];
  - superclass = pool[3]; a class without `extends` gets `java.lang.Object`
    (39 classes of the 3,454 sources); an interface has 0xFFFFFFFF and no
    superclass entries in the pool;
  - interfaces in the order of the `implements` list (pool[5], pool[7]).
  **proven** (2,873 of 2,873 chunks identical).
- The modifiers of the class (`public`, `abstract public`: 2 classes) leave no
  trace in the class file. **proven** (same count).

## 4. FILD and MTHD

All of this section is **proven** by byte identity of the chunks: FILD in 238
of 238 classes that have one (3,805 rows), MTHD in 2,869 of 2,869 (5,411 rows).

The grammar analysis found the same row rules on its own, from another parser
(`notes\grammar.md` 4.4, 4.5, 5.2 and its rules F1 to F6, M1 to M9, checked
field by field). The two agree in every number both state: the table of
bodies in 4.5, 5 and 184 methods for the two scoping alternatives. What this
note adds is the pool index behind every column, hence the bytes.

### 4.1 Rows and vectors

- The members are distributed over the two vectors by the `static` keyword;
  inside a vector the rows keep source order (a stable partition). 33 classes
  declare a static field after an instance field, 20 a static method after an
  instance method, 16 a field after a method.
- A field declaration with several declarators gives one row per declarator,
  each with the flags and type of the declaration and its own initialiser
  (`float vp_l=0.03, vp_t=0.78, vp_w=0.2, vp_h=0.18;`; 365 rows share their
  declaration with the row before).
- A constructor is an instance method row named `<init>` whose descriptor has
  no return part. `<init>` is the only name in angle brackets in the corpus
  (3,117 rows over all 2,993 blobs).
- **Nothing is implicit.** The 12 classes whose source declares no constructor
  have no `<init>` row; there is no static initialiser method (the dialect has
  no static block; a static field's initialiser is a tree of its FILD row);
  no row exists that the source does not declare (row counts equal declared
  members in 2,873 of 2,873 classes).
- Native methods stay where they are declared (their vector, source order).

### 4.2 Flags

The flags word is the OR of the modifier keywords of the declaration, nothing
else: 0x1 public, 0x4 private, 0x8 static, 0x10 abstract, 0x20 final, 0x40
native. Every combination the sources of the pairs use:

| Flags | Field rows | Method rows |
| --- | --- | --- |
| 0x00 | no modifier 2,465 | no modifier: 37 methods, 1 constructor |
| 0x01 | `public` 1 | `public`: 2,277 methods, 2,912 constructors |
| 0x04 | `private` 5 | `private` 1 |
| 0x08 | `static` 204 | |
| 0x09 | `public static` 4 | `public static` 69 |
| 0x0C | | `private static` 1 |
| 0x10 | `abstract` 2 (`abstract Vector3 posStart;`, Track.java:21-22) | |
| 0x20 | `final` 13 | |
| 0x28 | `final static` 1,079 | |
| 0x29 | `final public static` 31 | |
| 0x40 | | `native` 2 |
| 0x41 | | `public native` 109 |
| 0x48 | `static native` 1 (Chassis.java, `installSet`) | `native static` 1 |
| 0x49 | | `public static native` 1 |

0x2 (protected, 3 field rows of `java.util.Vector`) and 0x80 (20 method rows
of `Vector` and `Signal`) occur only in class files without a source; the
abstract method bit 0x10 likewise (5 rows, all 0x11, the methods of the
interfaces `Runnable` and `Callback`). No pair source declares an abstract
method or an interface member (the only interface with a source,
`GameState`, is empty).

### 4.3 Spelling of types and descriptors

| Source | String |
| --- | --- |
| `int`, `float`, `void` | `I`, `F`, `V` |
| class `Name` | `L` + fully qualified name with dots + `;` (`Ljava.util.Vector;`) |
| `T[]` | `[` + descriptor of T (`[I`, `[F`, `[Ljava.lang.Vector3;`) |
| method | `(` + parameter descriptors without separator + `)` + return descriptor: `(Ljava.util.resource.GameRef;I)V`, `()Ljava.lang.String;`, `()[I` |
| constructor | `(` + parameters + `)` and **no return part**: `(I)`, `()` |
| constructor name | `<init>` |

Field types by shape in the pairs: `I` 1,689, class 987, `F` 797, class
array 181, `[I` 123, `[F` 28. Return parts: none (constructors) 2,913, `V`
1,847, class 293, `I` 255, `F` 99, `[I` 3, class array 1. No source uses
`[]` behind a name, a qualified type name, more than one array dimension in a
declaration, or the keywords `boolean`, `char`, `byte`.

Class entries (kind 4) hold the dotted name without `L;`. The class of an
array is spelled `[` + element (`[I`, `[java.lang.Vector3`); all 129 such
entries exist for the one member `length:I`.

### 4.4 Trees

One counter runs over the members in source order, fields and methods mixed:

- a field declarator with `= expression` takes the next tree index (2,331 of
  3,805 rows); a field without initialiser has 0xFFFFFFFF;
- a method or constructor takes the next tree index, also when it is native
  or its body is `;` or `{}` (its tree is then empty);
- an abstract method has 0xFFFFFFFF and takes no index (5 rows, class files
  without a source only: **observed** there, no pair exercises it).

The tree count predicted from the source equals the TREE chunk's in 2,873 of
2,873 classes.

### 4.5 The fifth word of a method row

It is the number of local slots, and it follows from the source:

```
0                                              when the tree is empty
(0 if static else 1) + parameters + L          otherwise
```

where L is the largest number of declared locals alive at one time. A
declaration lives to the end of the enclosing block; a declaration in a `for`
header to the end of that `for` statement; **a declaration directly in a
`switch` body to the next `case` / `default` label** (the compiler releases
the locals of a case group at the next label).

The tree is empty when the body is written `;` (64 constructors, 7 methods,
113 natives) and when a **method** body is `{}` with no statement (36). A
**constructor** written `{}` (60) is not empty: it holds the implicit super
call, and its word is 1 + parameters.

Of the 5,411 rows 220 have the word 0, 4,049 have code and no local, 1,142
declare locals (largest word 34); in 184 the scoping matters (fewer slots
than declarations), 14 declare directly in a switch body and 5 of those give
another number under plain block scoping. All 5,411 words are reproduced.

## 5. Class names

- A simple class name of the source is resolved to a fully qualified name
  when it is parsed. In the model: a class declared in the same source, then
  the own package, then the on-demand imports (`import a.b.*;`, the only
  import form in use), then `java.lang`. Counting every class name a source
  writes once (the names behind `extends` and `implements` and every type
  that names a class): 15,757 occurrences in the sources of the pairs, 7,652
  resolved by an import, 6,768 by `java.lang`, 1,050 by the own package, 287
  by the same source; 16,539 in all 3,454 sources (8,416 / 6,785 / 1,051 /
  287). **proven** as far as the result goes: every occurrence resolves, and
  the type strings it produces are part of the byte-identical chunks.
  `notes\grammar.md` 7.2 reaches the same rule on its own.
- **`java.lang` is visible without an import**: only 4 sources import it.
- The search order is **undetermined**: 0 of the 16,539 occurrences name a
  class that exists in more than one of the visible packages (only 2 simple
  names exist in two packages at all, and no source uses them as a class
  name).
- Imports never enter the pool. 26 sources import something in which the
  corpus has no class and compile all the same:
  `java.game.parts.rgearpart.shockabsorber.*` 21, three other packages once
  each, and the line `import parts:part.root.BodyPart.*;` in `City.java` and
  `Baiern_FR_quarterpanel.java`. **observed**.
- The package enters only as the prefix of the class name. The superclass
  enters as pool[2], pool[3] (section 6.2) and nowhere else by itself.
- **Classes declared later in the same source are visible**: 147 occurrences
  name a class that is declared further down (`Bot` uses `database`,
  `botCard`) and get the right type string (83 name an earlier class, 57 the
  class itself), and the code of earlier classes holds 148 resolved refs to
  members of later classes with full descriptors, every one in agreement with
  the later class as shipped. How the compiler knows them at that point is
  **unknown** (section 11).
- An unresolvable class name compiles to an empty name: the field `curlang`
  of `java.util.Thesaurus` has type `L;` and its method descriptor is
  `(L;)V`. **observed** (1 class, no source); no error is raised.

## 6. The constant pool

### 6.1 Five steps

The pool is one interning table per class: `utf8` by text, `class` by name,
`nat` by (name, descriptor), `ref` by (class, nat), `rid` by (pack name, id).
An entry that exists is reused, whatever it was first interned for; nothing
is ever duplicated (foundation P1). The compiler appends to it in five steps:

| Step | When | What is appended |
| --- | --- | --- |
| 1 seed | class header | class name, superclass, interfaces: utf8 + class entry each |
| 2 types | while the class body is parsed | type descriptors (utf8), in the order of the parser's reductions; at the end of the body the interface types and, for a later class of the file, the own type |
| 3 names | when the member table is built | per member in source order: field name; method descriptor, then method name |
| 4 resolve | a pass over the code, by row vector | class entries, nat and ref entries of resolved members, the class of a receiver, the own type for `this` |
| 5 emit | when the trees are written, in tree order | utf8 of string literals and of unresolved names; pack name utf8 + rid entry per resource id |

Entries of the 2,873 pair classes by step: seed 11,534; types 7,135; names
13,666; resolve 138,134; emit 21,609; total 192,078 (utf8 93,244, ref
41,050, nat 40,377, class 10,283, rid 7,124).

Which node operand is interned when:

| Node | Entry | Step |
| --- | --- | --- |
| 0x1006 `type` (declare, new, newarray, instanceof, cast to int / float) | utf8 descriptor | 2 |
| 0x1019 `class` (class prefix of a path, cast to a class) | class | 4 |
| 0x101A `method`, 0x101B `field` | ref | 4 |
| 0x4009 `str`, 0x4012 `name`, 0x401C `getfield`, 0x4025 `call_on`, 0x4026 `call_super` | utf8 | 5 |
| 0x100E `rid` | rid | 5 |

The step 5 opcodes other than `rid` are exactly the ones whose op-flag has
immediate class 4 in `tree_op_flags.inc`: their operand is a string until the
tree is written (inferred from that match, not read in the exe).

### 6.2 Step 1: seed

`utf8 name, class name, utf8 superclass, class superclass`, then `utf8, class`
per interface in `implements` order. `java.lang.Object` and interfaces have
no superclass pair. **proven**: 2,993 of 2,993 blobs (T1); with the
superclass first 2,872 of 2,873 pair classes fail, with the interfaces before
the superclass 19 of the 19 classes that implement one.

### 6.3 Step 2: types, at parse time

The parser interns a type descriptor at the moment it reduces the construct
that carries it. In source order of those moments:

| Construct | Interned | When | Pinned by |
| --- | --- | --- | --- |
| field | its type | at the declaration (before or after the initialiser: undetermined) | |
| parameter | its type | left to right, also for native and bodiless methods | 2,699 classes fail without it |
| method | return type | **after the parameters**, before the body | "before" fails for 352 classes, "after the body" for 435 |
| constructor | nothing for the return | | |
| `new T(args)` | `LT;` | **before** the arguments | 4 classes (`new Pori(new Vector3(..), new Ypr(..))` gives Pori, Vector3, Ypr) |
| `new T[n]` | the array type only (`[LT;`, `[I`), never `LT;` | at the expression | |
| `(int)x`, `(float)x` | `I`, `F` | at the expression | |
| `(T)x`, T a class | **nothing** (its class entry comes in step 4) | | `LT;` here fails for 30 classes, the class entry here for 48 |
| `x instanceof T` | `LT;` | after x | |
| local `T a = e;` | its type | **after its initialisers** | 5 classes (`Dialog d = new NoYesDialog(..)`, CarMarket.java:1024, gives NoYesDialog, then Dialog) |
| end of the class body | `LI;` per implemented interface, in order | | 7 pair classes, 11 blobs without a source |
| end of the class body | `LC;` of the class itself **when it is not the first class of its source file** | after the interface types | 72 of 72 later classes; order: 4 blobs |

A type that is already in the pool adds nothing, so only first occurrences
show. Examples from `Navigator` (`poolwho.py game/Navigator --model`):
`Marker[] routeMarker;` interns `[Ljava.game.Marker;` alone; `public Marker
addMarker( Racer rc )` interns `Ljava.game.Racer;` and then
`Ljava.game.Marker;`; `implements Runnable` gives `Ljava.lang.Runnable;` as
the last type string, pool[29], before the first member name.

The own type of a later class is the oddest rule and has three independent
proofs: (a) 54 of 54 later classes of the pairs and 18 of 18 in class files
without a source hold `LC;` at exactly that place, also the 7 that no other
class of their file mentions; (b) switching the rule off, or on for every
class, fails 54 and 2,803 pair classes; (c) nine dialog classes ship twice,
as later classes of `Dialog.class` and as files of their own, and for the 5
of them that were compiled from the same code (`YesNoDialog`, `NoYesDialog`,
`OKCancelDialog`, `WarningDialog`, `CsDialog`) the pool inside `Dialog.class`
is the stand-alone pool plus this one entry. Why the compiler does it is
**unknown**.

**proven** for the pairs by the byte-identical pools; for all 2,993 blobs by
T8 of section 6.9, which rebuilds the types region without a source.

### 6.4 Step 3: names

After the whole body is parsed, the members are walked in **source order**,
fields and methods mixed, statics and instance members mixed:

- field: utf8 of its name (its type is there since step 2);
- method: utf8 of its descriptor, then utf8 of its name; a constructor:
  descriptor, then `<init>`.

No nat or ref is made for a declaration; the rows of FILD and MTHD point at
these utf8 entries directly. **proven**: names by row vector instead of
source order fail 54 classes, name before descriptor 2,869; T3 to T5 hold for
all 2,991 blobs that have members.

### 6.5 Step 4: resolve

The trees are visited **by row vector: static methods, instance methods,
static field initialisers, instance field initialisers**, each vector in row
(= source) order, and every tree node by node in emission order. What is
interned:

1. A `class` node: utf8 of the class name, class entry.
2. A resolved member (`field` or `method` node): the class of the ref (utf8,
   class entry), then utf8 name, utf8 descriptor, nat, then the ref. Parts
   that exist are reused.
3. **The class of the receiver.** Before a path segment other than the first
   is looked up, the class entry of the static type of the segment before it
   is interned: the declared type of a local or parameter, the type of a
   field ref. This happens whether the lookup then succeeds, fails or is not
   run at all: `turboSlotIDList.addElement(new Integer(84))` leaves
   `java.util.Vector` in the pool although the call stays an unresolved
   `name` (by `notes\expressions.md` 6.2 the method lookup is not even run
   when an argument is a `new` expression). When the member is found the ref usually names the
   same class and nothing shows; it shows when a static field is reached
   through an instance (`part.GII_COMPATIBLE`: `java.game.parts.Part` is
   interned, then the ref to `java.lang.GameType`).
4. `this` used as an expression (a `local` node that names the receiver in
   instance code): utf8 `LC;` of the own class. `this` is never a path
   segment; `this.x` is `local 0` + `getfield`.

The order is the order of the emitted nodes, not the order in which the
parser built them: `nav.remMarker( marker )` interns the ref of the argument
`marker` before the class and ref of `Navigator.remMarker`, although the
nodes of a call path are created before the arguments (their line numbers
show that, `notes\expressions.md` section 10).

Nothing is interned for: `new` (the constructor is found at run time; no
pool holds a nat or ref named `<init>`), `getfield` / `call_on` on a computed
receiver (no static type is consulted: 817 + 1,187 nodes), literals, an array
receiver beyond its `length` ref.

An unresolved `name` is always the last segment of a call path: 8,065 of
8,065 `name` nodes (2,508 are the whole path, 4,797 follow a receiver of
class type, the rest a class prefix). A field segment of a path is always a
resolved ref, so nothing resolved ever follows an unresolved name (T7: 2,991
of 2,991 blobs).

**proven**. Witnesses when one rule is changed (classes whose pool is then
wrong, of 2,873):

| Alternative | Classes wrong |
| --- | ---: |
| trees in tree (source) order | 50 |
| methods in source order, then field initialisers | 18 |
| instance methods before static methods | 18 |
| instance field initialisers before static ones | 2 |
| field initialisers before methods | 35 |
| `this` interns nothing | 42 |
| receiver class not interned | 327 |
| receiver class interned only before an unresolved name | 1 (`CatalogInventoryPanel`, 6 nodes) |
| ref: nat before class | 1,620 |
| nat: descriptor before name | 2,811 |

Walk counters over all blobs: 398 `this` nodes, 24,159 segments with a
receiver of class type (11,974 field, 7,388 method, 4,797 unresolved name),
2,027 of which found the class not yet in the pool, 476 of them before a name.

### 6.6 Step 5: emit

A separate pass over **all trees in tree-index order** (= source order,
fields and methods mixed), node by node:

- `str`: utf8 of the literal after escape processing;
- `name`, `getfield`, `call_on`, `call_super`: utf8 of the member name;
- `rid`: utf8 of the pack file name, then the rid entry.

**proven**: in the order of step 4 instead, 47 classes fail; with steps 4 and
5 merged into one pass per tree, 2,806 (row vector order) or 2,813 (tree
order). So the pool cannot be filled while the code is generated tree by
tree: all refs of the class come first, all strings after.

Since CONS precedes TREE in the file and step 5 is driven by the trees, the
writer has to walk (or serialise) the trees before it writes CONS.

### 6.7 Sharing

One utf8 serves every use of the same text. In the pairs: 3,002 utf8 are a
field name and the name of a nat; 780 a method name and an unresolved name;
727 + 559 a nat descriptor and a type carrier; 589 a method descriptor and a
nat descriptor; 24 a method name and a string literal (`City`: the literal
`"event_handlerNrFinish"` is the utf8 of the method of that name; `Navigator`
pool[59] `marker` is a field name, a string literal and a nat name). A nat is
shared by refs of different classes (`destroy:()V` under `Camera`, `Viewport`
and `RenderRef`).

### 6.8 Example

`sl/Scripts/game/Trigger.class`, 59 entries (`poolwho.py game/Trigger --model --src`):

| Entries | Step | Content |
| --- | --- | --- |
| 0-3 | 1 | `java.game.Trigger` + class, `java.lang.Object` + class |
| 4-9 | 2 | field types `I`, `L..GameRef;`, `L..Navigator;`, `L..Marker;`, `L..Vector3;`, `L..String;` |
| 10 | 2 | `F`, parameter `float r` of the second constructor (line 31) |
| 11 | 2 | `V`, return type of `finalize` (line 51) |
| 12-17 | 3 | `DEFAULT`, `trigger`, `nav`, `marker`, `pos`, `alias` |
| 18-23 | 3 | constructor descriptor, `<init>`, two more constructor descriptors, `()V`, `finalize` |
| 24 | 4 | `Ljava.game.Trigger;`: `this.pos = pos` in the first constructor |
| 25-26 | 4 | nat and ref of `DEFAULT` (second constructor, line 34) |
| 27-32 | 4 | `type.id()`: `java.util.resource.GameRef` + class, `id`, `()I`, nat, ref |
| 33-38 | 4 | `pos.toString()`: `java.lang.Vector3` + class, name, descriptor, nat, ref |
| 39-40 | 4 | nat and ref of `trigger` |
| 41-53 | 4 | `finalize`: `destroy`, `nav`, `marker`, `Navigator.remMarker` |
| 54-55 | 5 | `system.rpk` + rid 0x34: tree 0, the initialiser of `DEFAULT` (line 13) |
| 56-58 | 5 | `",0,0,0,sphere,"`, `",0,0,0,box,"`, `","` (trees 2 and 3) |

The rid of line 13 is the first thing in the source and the last but three in
the pool; the ref of `DEFAULT`, used in line 34, sits 29 entries before it.

### 6.9 The same rules without a source

`check_tail.py` checks what a class file alone can show, on all 2,993 blobs:

| Rule | Statement | Holds for |
| --- | --- | ---: |
| T1 | seed as in 6.2 | 2,993 of 2,993 |
| T2 | between seed and names only utf8 spelled as type descriptors; field type columns point there | 2,993 of 2,993 |
| T3 | the names are one contiguous block of utf8, each named by a row | 2,991 of 2,991 |
| T4 | the block is a merge of the four row vectors in row order, descriptor before name | 2,991 of 2,991 |
| T5 | rows that own a tree appear in tree order across the vectors | 2,991 of 2,991 |
| T6 | everything behind the names is exactly what steps 4 and 5 intern | 2,991 of 2,991 |
| T7 | no resolved segment behind an unresolved name | 2,991 of 2,991 |
| T8 | the types region is what step 2 interns for some order of the members that respects row and tree order | 2,993 of 2,993 |

Of these blobs 120 have no source (112 of Build 940; 95 class files of the
runtime library under `system/scripts`, 7 under `sl/Scripts`); every rule
holds for them (2 blobs of the corpus have no member, hence 2,991). T8 with
one rule replaced fails as follows (all blobs / blobs without a source):
return type before the parameters 364 / 12; after the body 467 / 32; local
type before its initialiser 5 / 0; no interface types 18 / 11; no own type
for later classes 72 / 18; own type for every class 2,892 / 89; **own type
before the interface types 4 / 4** (`HotkeyWatcher` in `Frontend.class`,
three classes of `LoadingScreen.class`), which settles the one order the
pairs leave open.

### 6.10 The entries that nothing refers to

Foundation P2 counted 2,537 such entries in 1,399 blobs. All are accounted
for:

| Entries | Kind | Where from |
| ---: | --- | --- |
| 871 | utf8 `V` | step 2, a return type |
| 571 | utf8 `I` / `F` | step 2, parameter or return type |
| 589 | utf8 class type | step 2, parameter or return type |
| 6 | utf8 array type | step 2, parameter or return type |
| 80 | utf8 `LC;` of the own class | step 2 (72 of them the end-of-body rule for later classes) |
| 20 | utf8 `LI;` | step 2, the end-of-body rule for interfaces |
| 65 | utf8 `LC;` | step 4, `this` |
| 335 | class | step 4, the class of a receiver whose call stayed an unresolved name (lookup failed or not run) |

## 7. What a member ref names

Measured on every `field` and `method` node of the corpus
(`study_refclass.py`); R is the class the lookup starts from (the own class
for a bare member, the class of a class prefix, the static type of the
segment before), D the class that declares the member.

| Member | The ref names | Nodes | Of them inherited (R is not D) |
| --- | --- | ---: | ---: |
| static field | **D, the declaring class** | 13,243 of 13,243 | 3,862 |
| instance field | **R** | 64,536 of 64,536 | 38,304 |
| method, static or not | **R** | 18,468 of 18,468 | 9,306 (94 static) |
| `length` of an array | the array class | 454 of 454 | |

So `name = "..."` in a part class makes a ref to the part class itself
although `name` is declared in a superclass, while `PACKAGE` (a static of
`Part`) makes a ref to `java.game.parts.Part`. D can be several steps above
R (up to 6 for bare fields, static or not); the classes in between are not
interned. Two
method refs name a member that the shipped class of R no longer declares
(`Garage` to `NetworkEngine.httpRequestAsync`, `Center_Differential` to
`Chassis.getBlock:()Ljava.game.parts.Part;`): compiled against an older
version of the other class. **proven** as a measurement. The expression
analysis states the same rule for fields (`notes\expressions.md` 5.3) and
answers when a call becomes a ref at all (its 6.2 and 6.3); the table adds
the methods and the inheritance depth.

## 8. Sources with several classes

- 23 sources declare more than one class (2 to 9; 77 classes); the class file
  holds the blobs in declaration order. **proven** (2,819 pairs).
- Pools are per blob and start from nothing; nothing interned for one class
  appears in another's pool. **proven** (each blob predicted on its own).
- The only trace of the position in the file is the own type `LC;` that
  every class but the first interns at the end of its body (section 6.3).
- Ten class names are defined by more than one class file of the install:
  nine dialog classes both inside `system/scripts/render/osd/dialog/
  Dialog.class` and as stand-alone files of the same build, and
  `java.game.track_data` once per map (21 files).

## 9. One rule changed at a time

`check_alternatives.py`, classes of the pairs whose pool is then wrong (of
2,873); 0 = the pairs cannot tell.

| Step | Alternative | Classes wrong |
| --- | --- | ---: |
| 1 | superclass before the class itself | 2,872 |
| 1 | interfaces before the superclass | 19 |
| 2 | return type before the parameters | 352 |
| 2 | return type after the body | 435 |
| 2 | parameter types not interned at parse time | 2,699 |
| 2 | field type after its initialiser | 0 |
| 2 | local type before its initialisers | 5 |
| 2 | local type after each declarator | 0 |
| 2 | type of `new T(args)` after the arguments | 4 |
| 2 | type of `new T[n]` after n | 0 |
| 2 | type of `(int)x` after x | 0 |
| 2 | a cast to a class interns `LT;` | 30 |
| 2 | the class entry of a cast interned at parse time | 48 |
| 2 | type of `x instanceof T` before x | 0 |
| 2 | no `LI;` at the end of the body | 7 |
| 2 | no `LC;` for later classes | 54 |
| 2 | `LC;` for every class | 2,803 |
| 2 | own type before the interface types | 0 (4 blobs without a source decide it, 6.9) |
| 3 | names by row vector | 54 |
| 3 | method name before descriptor | 2,869 |
| 4 | see the table in 6.5 | |
| 5 | strings in the order of step 4 | 47 |
| 4+5 | one pass, row vector order / tree order | 2,806 / 2,813 |

## 10. The 635 sources without a class file

`check_source_only.py`: 635 sources, 635 classes, 6 field rows, 765 method
rows, 771 trees. Every class name resolves (782 occurrences: 764 by import,
17 `java.lang`, 1 own package; none ambiguous). None of the undetermined rules
changes the prediction for any of them. None uses a declaration form the
pairs do not exercise (no second class, interface, abstract method,
`protected`, `synchronized`, `[]` behind a name, qualified type, single-type
import). So CLSS, FILD, MTHD and the first three steps of the pool (4,915
entries) are fully determined for them today; steps 4 and 5 need the trees.

## 11. Undetermined and unknown

Undetermined (no case in 2,993 blobs; none matters for the 635):

1. Field type before or after its initialiser. The model takes "before".
2. A local with several declarators (60 in the sources): type after each
   declarator or after the last.
3. `new T[n]`, `(int)x`, `x instanceof T`: before or after the types inside
   n / x. The model takes before, before, after.
4. The search order among own package, imports and `java.lang`.

Unknown:

5. How a class declared later in the same source is known while an earlier
   class is parsed (147 type names) and resolved (148 member refs), and why a
   later class interns its own type. Both point at file-level bookkeeping
   that the pool of the first class does not show. If the compiler only knows
   later classes from a previous compilation, a first compile of a new
   multi-class source would differ; the corpus cannot tell.
6. What an unresolvable class name does beyond compiling to `L;` (1 class).
7. How a source spells an abstract or interface method (5 rows, flags 0x11,
   tree 0xFFFFFFFF, word 0, no source); `GameState.java` has the commented
   line `//abstract public void enter( GameState oldgs );`.
8. Flags 0x2 and 0x80 (no source).
9. Whether the grammar accepts what no source uses: `[]` behind a name,
   qualified type names, single-type imports, `final` on parameters or locals.
10. What `import parts:part.root.BodyPart.*;` means to the scanner (2
    sources; no effect on any pool).
11. The spelling of pack file names in rid entries (foundation question 3):
    the model takes it from the tree oracle.

Not this note's subject, but needed for steps 4 and 5 without an oracle:
which names and calls resolve to a ref (`notes\expressions.md` 5 and 6) and
the emission order of the nodes (`notes\expressions.md`,
`notes\statements.md`). Given trees that are right, the pool follows.

## 12. For the compiler

Per class, with one `Pool` (`poolmodel.Pool`: `utf8`, `cls`, `nat`, `ref`,
`rid`, each interning its parts in the order of 6.5):

```
at the class header
    pool.cls(own name); pool.cls(superclass or java.lang.Object) unless interface / Object
    for each interface: pool.cls(interface)
while parsing the body, in reduction order
    field declaration            pool.utf8(type)
    parameter                    pool.utf8(type)
    method header complete       pool.utf8(return type)        (not for a constructor)
    "new T" seen                 pool.utf8("LT;")              (before the arguments)
    "new T[n]"                   pool.utf8("[" + T)
    "(int)x" / "(float)x"        pool.utf8("I" / "F")
    "x instanceof T"             pool.utf8("LT;")
    local declaration reduced    pool.utf8(type)               (after its initialisers)
at the closing brace
    for each interface: pool.utf8("LI;")
    if this is not the first class of the file: pool.utf8("LC;")
    for each member in source order
        field:   pool.utf8(name)                    -> FILD row (statics / instance)
        method:  pool.utf8(descriptor); pool.utf8(name or "<init>")   -> MTHD row
    trees are numbered in the same walk (4.4); the locals word as in 4.5
resolve: for vector in (static methods, methods, static fields, fields), rows in order, nodes in order
    class prefix / class cast    pool.cls(class)
    segment k > 0 of a path      pool.cls(class of the static type of segment k-1), then look up
    member found                 pool.ref(class per section 7, name, descriptor)
    this                         pool.utf8("LC;")
emit: for every tree in tree order, nodes in order
    string literal / unresolved name    pool.utf8(text)
    resource id                         pool.rid(pack file name, id)
write TUFA, CONS, [FILD], [MTHD], CLSS, TREE
```

## 13. Notes on the foundation

No bug was found in `lib\tufa.py`, `oracle.py`, `disasm.py` or the notes; the
foundation files are unchanged. Additions to what `foundation.md` says:

- Its open question 2 (pool order, the 2,537 unreferenced entries) is closed
  by sections 6.1 to 6.10; question 7 (empty bodies) by 4.5: `;` always gives
  an empty tree, `{}` gives an empty tree on a method and the implicit super
  call on a constructor.
- The locals word (M5) follows from the source (4.5), including the case
  group scope of `switch`.
- Part of question 4: which class a ref names (section 7), and that the class
  of a receiver is interned before every lookup (6.5).
- `L;` is a legal type string (Thesaurus); ten class names have more than one
  definition in the install (section 8).

Relation to the notes written in parallel: `grammar.md` (rows of FILD / MTHD /
CLSS, name resolution) and `expressions.md` (which class a field ref names)
reach the same rules where they overlap with sections 4, 5 and 7, each from
its own code; no statement of theirs contradicts one here. Both leave the
pool order open; it is this note's result. `check_bridge.py` runs the model
on `lib\parser.py` as it stood on 2026-10-11; if that AST changes,
`libbridge.py` has to follow.
