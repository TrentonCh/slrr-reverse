# Implementation step M1 to M3: the front half of `slrrc`

Written 2026-10-11. First implementation step of `notes\compiler_spec.md`
(section 4): the package `compiler\slrrc\` with scanner, parser, classes, pool
and declarations, the test harness under `compiler\tests\`, and the baseline.
Nothing under `lib\`, `analysis\`, `spec\`, in the corpus `D:\SLRR-test`, in the
game installs or in the slrr-reverse repo was changed; no exe and no Ghidra was
run. There was no previous step, so there was no earlier code to fix.

## 0. Result

```
cd C:\Users\trent\source\repos\TrentonCh\slrr-ghidra\compiler
python -I tests\run_tests.py m1 m2 m3
```

prints (21 s, exit code 0):

```
M1 tokenise      3454/3454 sources, 852211 tokens
M2 parse         3454/3454 sources
M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
baseline         0 lost
```

These are the four lines the spec asks for, with its numbers. Every number was
reached; no file is special-cased anywhere in the package or in the tests.

A fourth test that the spec does not name looks at what the corpus cannot show
(`python -I tests\run_tests.py extra`, 55 s):

```
extra            dialect 181/181 cases; deletions 3067/3068 rejected, lib 3067; mutations 9060 texts, 0 exceptions, 0 differences from lib
baseline         0 lost
```

The package imports nothing from `lib\`, `analysis\` or `spec\` and nothing
outside the standard library (at present nothing at all outside itself).
`run_tests.py` checks that before every run, and `t_extra.py` runs the front
half in an interpreter of its own with only the package on the path.

## 1. What was built

### The package `compiler\slrrc\` (3,300 lines)

| File | Lines | Content |
| --- | ---: | --- |
| `scanner.py` | 589 | `scan(source, diags, comments=None) -> [Token]`. Hand-written, character by character, no regular expression. `Token(kind, pos, length, line)`; every keyword (35) and operator (46) has a kind of its own. Literal values: `int_value`, `string_value`, `rid_parts`, `float_digits`. |
| `parser.py` | 1,307 | `parse(source, toks, diags) -> Ast`. Recursive descent with precedence climbing. One `Node` type (`kind, index, tok0, tok1, op_tok, name_tok, a, b, c, items, flags`), 43 kinds, the layout of spec 2.5. `children(n)` gives the children in source order. |
| `classes.py` | 248 | `FieldInfo`, `MethodInfo`, `ClassInfo`, `ClassProvider.find(fqn)`, `UnitScope` (the classes of the source over the provider, the list of consulted classes), `resolve_type`, `descriptor`, `method_descriptor`, `declare_unit`. |
| `pool.py` | 105 | `Pool` with `utf8`, `cls`, `nat`, `ref`, `rid`; `PoolEntry(kind, a, b, text)` holds final indices; `to_cons()` for the tests. |
| `decl.py` | 380 | `seed`, `type_walk` (with `stmt_types`, `expr_types`, `local_types`), `names_and_rows`, `locals_word`, `max_live`, `declare_class -> ClassBuild(node, info, first_in_file, pool, rows)`; `Rows` holds the four row vectors, the CLSS record and `owners`, the tree owners in source order. |
| `write.py` | 98 | `cons_payload`, `fild_payload`, `mthd_payload`, `clss_payload`, `has_fild`, `has_mthd`. |
| `dump.py` | 328 | `dump_tokens`, `dump_ast` (one line per node, the form the C++ tool is to print), `ast_tokens` (the tokens regenerated from the tree). |
| `diag.py` | 95 | `Diagnostic(error, line, column, code, text)`, `Diagnostics(strict)`, the codes of spec 2.9, `exe_wording()`. |
| `__init__.py` | 149 | `VERSION`, `Options(build, float_mode, strict)`, `front(source, name, provider, options) -> FrontResult(name, ok, diagnostics, ast, scope, classes, consulted)`, `declare_source(...)` for a provider that has to declare a class from its source. |

The core does no file access and holds no global state (module level: constant
tables only). Bad input never raises: the parser unwinds with a private
exception that `parse()` catches, everything else returns through the
diagnostics.

### The tests `compiler\tests\` (1,800 lines)

| File | Content |
| --- | --- |
| `harness.py` | corpus groups from `lib\oracle.py` (oracle pairs 2,651, other pairs 168, older 153, stale 15, source only 635); `CorpusProvider`; `Baseline`; `Outcome`, `settle` (headlines, baseline line, `--update`); `--impl py\|cpp` (`cpp` answers "not built", exit 2); `package_is_self_contained()` |
| `t1_lex.py` | M1 |
| `t2_parse.py` | M2 |
| `t3_decl.py` | M3 |
| `t_extra.py` | the checks outside the corpus |
| `run_tests.py` | `m1 .. m6`, `extra`; a milestone whose script does not exist yet answers "not built yet", exit 2 |
| `seed_baseline.py` | writes `expected_fail.tsv` and copies the spike's seeds |
| `baseline\` | `m1.pass` 3,454 sources, `m2.pass` 3,454 sources, `m3.pass` 2,819 class files, `m3_model.pass` 3,508 `source#class`, `extra.pass` 181 cases; `expected_fail.tsv` (15 stale pairs with the evidence of the index, 2 pairs compiled against another class version); seeds of the next milestones copied from `spec\out\`: `m5.pass` 2,649, `m5_older.pass` 153, `m5_float.tsv` 2,802 (the float setting each file needs), `m6.pass` 635, `m6.sha256` 635 |

### What a source has to do to pass

All three milestone tests run the package in strict mode, so a pass also says
that the source holds no construct outside the proven subset.

**M1** (`t1_lex.py`, 4 s), per source: (a) no diagnostic; (b) tokens,
comments and white space tile the file; (c) the token stream equals the one of
`lib\lexer.py` in kind, position, text and line, and the comments are the
same; (d) int values, string texts after escapes, resource id parts and float
digits equal the ones `lib\lexer.py` computes.

**M2** (`t2_parse.py`, 7 s), per source: (a) no diagnostic; (b) every token
consumed, every node reachable from the root exactly once, token ranges nest
and siblings are in source order; (c) `dump.ast_tokens(ast)` equals the tokens
read; (d) the modifier bits of every declaration are the OR of its modifier
keywords; (e) the tree is the tree of `lib\parser.py`: the same nodes in the
same order with the same `tok0`, `tok1`, `op_tok`, `name_tok` and the same
names, modifiers, dimensions, literals and operators (506,625 nodes; 486,738
of them have a counterpart in `lib\parser.py`, the rest are the helper nodes
of section 2).

**M3** (`t3_decl.py`, 9 s), per pair and class, from `slrrc.front(text, name,
CorpusProvider)`: (a) class name and number of classes; (b) the pool after
steps 1 to 3 equals the head of the shipped pool, entry for entry and byte for
byte (`write.cons_payload` against the shipped CONS payload); (c) CLSS, FILD,
MTHD payloads byte-identical and present exactly when shipped; (d) tree count.
For every class of all 3,454 sources: (e) pool prefix and rows equal the ones
of `analysis\decl\poolmodel.py` on the tree of `lib\parser.py`. The class file
of the source under test never reaches the compiler: its classes are declared
from the source (`UnitScope`), the provider answers for the others.

**extra** (`t_extra.py`): 181 synthetic sources, one construct each, with the
diagnostic codes expected in default and in strict mode; value checks of
descriptors, resolution order, a hand-written pool prefix and rows; the N1
sample of `grammar.md` 10.1 (3,068 single-token deletions: 3,067 rejected, the
one accepted is the one `lib\parser.py` accepts, no exception); 9,060 mutated
texts (lines moved or copied, random byte edits, random token edits; fixed
seed): no exception, the same tokens and the same tree as `lib` whenever both
accept, nothing rejected that `lib` accepts except the constructs of section
4, and for the 1,707 accepted texts that can be compared the same pool prefix
and rows as `poolmodel.py`.

**Regressions.** A run compares its pass lists with `tests\baseline\`; a lost
item is printed (`LOST m1.pass: <item>`), the baseline line counts it and the
exit code is 1. `--update` stores new passes and refuses when anything was
lost. `--only TEXT` narrows a run to the sources whose path holds TEXT and
skips the baseline (`python -I tests\run_tests.py m3 --only Trigger -v`).

## 2. Decisions inside the spec's frame

**AST.** The spec gives the C++ layout (2.5); the Python tree has exactly
those fields, so that `dump_ast` is the same line per node in both
implementations and the port needs no mapping. Children are object references
in Python (`n.a`), indices in C++; `n.index` is the position in `ast.nodes`
(creation order, which a transliterated parser reproduces). A handler unpacks
`a, b, c, items` into named locals; the table in the docstring of `parser.py`
says what they are per kind. Names are not copied into the tree
(`ast.text(n.name_tok)`), an operator is the kind of its token, modifiers are
the bits of the row flags word (`flags & ACC_MASK` is what goes into FILD /
MTHD). Helper nodes that `lib\parser.py` does not have: `QNAME` (package and
import names), `LIST` (the `implements` names, the labels of a switch group,
the init expressions of a `for`), and a `TYPE` node for the class behind
`extends` and each one behind `implements`. Token ranges are those of
`lib\parser.py` for every node both have (test M2 e), which is what the line
rules of `statements.md` 8 were proven on.

**Scanner.** No regular expressions: the code is what the C++ scanner will
be. It reproduces the rule order of `lib\lexer.py` including its lookaheads (a
float must not be followed by a letter, digit, `_` or `.`; an int not by a
letter, digit or `_`; a resource id is tried before an identifier). A dotted
name that turned out not to be a resource id is not looked at again for the
identifiers inside it, which keeps the scan linear.

**Type names.** `resolve_type(scope, name)`: a class of the same source, the
own package, the imports in source order, `java.lang`; through
`scope.find(full name)`, so every question goes to the provider with a full
name, as spec 2.6 asks. The provider is not asked for the classes of the
source itself, and `java.lang.Object` as implicit superclass is not looked
up. `scope.consulted` lists what the provider found, in order of first use
(for the cache manifest of spec 2.10).

**Pool.** Entries hold final indices from the start (an entry only refers to
lower ones), so the prefix can be compared with the shipped pool directly and
CONS can be written without a conversion. Two maps: text to index, and
(kind, a, b) to index.

**Tree owners.** `rows.owners[t]` names the member that owns tree `t` (the
`DECLARATOR` of a field or the `METHOD_DECL`), with `is_static`: the emitter
walks this list, and the writer finds the four row vectors of step 4 in
`rows`.

**Limits.** `parser.MAX_DEPTH = 200` bounds the recursion of the parser and,
separately, the height of the tree (a chain `a + b + c + ...` is parsed in a
loop but is as deep as it is long), so every later pass may recurse over the
tree. The deepest shipped source, `Garage.java`, reaches 51 and 56. Beyond the
limit: `E_LIMIT`. A token behind line 65,535 is `E_LIMIT` as well (white space
and comments behind the last token do not count: they stamp no node).

## 3. Deviations from the spec, with reasons

| Spec (section 4 and 2.4) | Built | Reason |
| --- | --- | --- |
| scanner and parser are `lib\lexer.py` and `lib\parser.py`, "imported as they are" | `slrrc\scanner.py`, `slrrc\parser.py`, written new | the task forbids imports from `lib\` inside the package and asks for code that can be ported: `lib\lexer.py` is one regular expression, the nodes of `lib\parser.py` get their fields through `__dict__.update`. Equality with both is tested on every source and on 9,060 mutated texts |
| `class CompileError` | no exception; `front()` returns a `FrontResult` with `ok` and `diagnostics` | the task: diagnostics with line and code instead of exceptions |
| `front(text, name, provider) -> [ClassBuild]` | `front(source, name, provider, options) -> FrontResult`; the list is `result.classes` | the diagnostics and the consulted classes have to come back too |
| `CorpusProvider` in `slrrc\classes.py` | in `tests\harness.py` | it reads class files with `lib\tufa.py` and the index of `lib\oracle.py`; the package may not import them. The package has the interface (`ClassProvider`) and `declare_source()` for the classes that exist as source only |
| `resolve_type(unit, scope, simple_name)`, `descriptor(type_node, unit, scope)`, `method_descriptor(method_node, unit, scope)`, `declare_unit(unit, provider) -> [ClassInfo]`, `type_walk(pool, class_node, unit, scope, first_in_file)`, `declare_class(unit, class_node, scope, first_in_file) -> ClassBuild(info, pool, rows, owners)` | `resolve_type(scope, name)`, `descriptor(scope, type_node, extra_dims)`, `method_descriptor(scope, method_node)`, `declare_unit(ast, provider, diags) -> UnitScope` (`scope.own` is the list), `type_walk(pool, scope, class_node, info, first_in_file)`, `declare_class(scope, class_node, info, first_in_file) -> ClassBuild(node, info, first_in_file, pool, rows)`, owners in `rows.owners` | the scope carries the tree and the diagnostics, so one argument replaces two; `extra_dims` is the brackets behind a declared name |
| `write.py` in the next step, on `lib\tufa.py` | `write.py` exists with the four chunk payloads that need no tree | M3 compares bytes, and the package cannot use `lib\tufa.py` |
| `tests\t2_parse.py` regenerates tokens with `lib\unparse.py` | `slrrc\dump.py` (`ast_tokens`) | `lib\unparse.py` reads the other tree; the dump module is also where the C++ comparison of M7 gets its formats |
| baseline `m3.pass` (class files) | `m3.pass` holds the class files of all 2,819 pairs that pass (oracle and other); `m3_model.pass` holds the 3,508 classes equal to the model | the third part of the headline needs a list of its own |
| - | `tests\t_extra.py`, `tests\seed_baseline.py` | the corpus says nothing about rejected input, warnings or limits |

`lib\lexer.py`, `lib\parser.py`, `lib\tufa.py`, `lib\oracle.py` and
`analysis\decl\poolmodel.py` (with `libbridge.py`, `universe.py`) are used by
the tests as oracles, unchanged.

## 4. Constructs outside the proven subset

Rule of spec 2.9 and appendix B: where a default is stated the construct is
compiled with `W_UNVERIFIED` and is `E_UNSUPPORTED` in strict mode; where the
appendix says so, or where no default exists, it is `E_UNSUPPORTED` in both
modes. `grammar.md` 3.4 lists more constructs that `lib\parser.py` accepts than
appendix B decides; those decisions were made here and are marked **new**.
Each row is a case of `t_extra.py`. Emission-level items of appendix B
(compound assignment as a value, `continue` outside a `for` with update, a
call whose name is a local, a value method without `return`) are not visible
to the front half and are left to the emitter.

**Warning `W_UNVERIFIED`, error in strict mode**

| Construct | Source of the decision |
| --- | --- |
| `import a.b.C;` (resolves `C` in its place among the imports) | appendix B |
| class name written with its package (type, `new`, `extends`, `implements`) | appendix B |
| `int a[]` (field, parameter, local) | appendix B |
| `final` on a local or a parameter | appendix B |
| empty statement `;` | appendix B |
| several expressions in a `for` header | appendix B |
| several labels on one group; a group that does not end in `break`, `return` or `continue` and is not the last; a `case` behind `default`; a last group without statements | appendix B |
| `%=` | appendix B |
| `?:` inside `?:` (anywhere inside, also through parentheses or arguments) | appendix B |
| `new T(..);` as a statement | appendix B |
| a `switch` without any label | **new**: the empty case of the same rule |
| several variables declared in a `for` header | **new**: all 444 declarations there declare one |
| import behind a type declaration; `;` between top-level declarations or between members | **new**: nothing to emit |
| source without `package` (the class name has no prefix) | **new** |
| interface with members (rows as written) | **new**; unknown 11 of the spec |
| constructor whose name is not the class name (compiled as a constructor) | **new**: `grammar.md` 3.1, a member that starts with `Identifier (` |
| the same modifier twice; `protected`, `synchronized`; a class modifier other than `public` and `abstract`; a constructor modifier other than `public`; `final` or `abstract` on a method (an abstract method owns no tree, `declarations.md` 4.4) | **new**: the flags word is the OR of the keywords |
| float `.5`, `5.`, suffix `F`; decimal int with a leading zero (read as decimal) or above 2147483647; int or resource id above 32 bits (modulo 2^32) | **new**: the conversions are defined for them |
| string escape other than `\n \t \" \\` (`\r` is CR, `\0` is NUL, any other character stands for itself: what the spike's `srclex.string_value` does) | **new** |

**Error `E_UNSUPPORTED` in both modes**

| Construct | Source of the decision |
| --- | --- |
| `>>>`, `<<=`, `>>=`, `>>>=`; `super.field`; array initialiser; more than one dimension; label, `break L`, `continue L`; character literal | appendix B |
| float with an exponent or with suffix `d` / `D` | **new**: spec 2.8 defines no conversion |
| `new T[]`, `new T[n][]`, `new T[n].x` | **new** (`grammar.md` 3.3: nothing follows an array creation) |
| initialiser block; interface with `extends` or `implements`; brackets behind a parameter list; `native` or `abstract` method with a body | **new**: no row or tree rule known |
| modifier other than `final` on a local or parameter; a declaration as the unbraced body of `if` / `else` / a loop; `void` as the type of a field, parameter or local, `void[]` | **new** |
| `this(..)` / `super(..)` anywhere but as the first statement of a constructor | **new**: the grammar `statements.md` 8.2 infers has it nowhere else |
| assignment, `++`, `--` on something that is not a name, a field or an element; cast to an array type; `instanceof` with a primitive or an array type; a second `default` | **new** |
| a type declared twice in one source | **new** |

**Error `E_SYNTAX`** for everything outside the grammar. Three things that
`lib\parser.py` accepts are syntax errors here, on purpose: an expression
statement that is not an assignment, a call, `++` or `--` (`x;`, `a + b;`,
`(a = b);`, also in a `for` header; `statements.md` 8.2 infers the
`StatementExpression` of the Java grammar), `class C extends A, B`, and
`new int(3)`. A source without any class is `E_SYNTAX` too.
`Diagnostic.exe_wording(file)` gives the line the game logs for a syntax
error.

**Other codes.** A type name that resolves nowhere: descriptor `L;` with
`W_TYPE`, `E_TYPE` in strict mode; an unknown superclass or interface is
`E_TYPE` in both modes (no class file shows what the exe does there). A byte
above 0x7F in a string or comment and a UTF-8 byte order mark: `W_BYTE`, in
both modes a warning. `E_LEX`: a character no rule accepts, a string not
closed on its line, a block comment not closed. `E_LIMIT`: section 2.

## 5. Findings

1. **No number had to be chased.** `t1_lex.py` was written first and failed
   for want of a package; once the modules of a milestone existed, its test
   printed the full numbers on its first run. The changes after that were
   hardening (limits, diagnostics for constructs outside the subset, the
   comparisons on mutated texts) and none of them moved a number. So there
   is no minimal example of a miss to report.
2. **`lib\lexer.py` reads an unclosed `/*` as the operators `/` and `*`** (the
   comment alternative of its expression fails and the operator alternative
   matches). The package reports `E_LEX` as spec 2.9 demands. No shipped
   source has an unclosed comment, so no number of any note is affected.
3. `lib\parser.py` keeps the imports and the types of a unit in two lists, so
   `Node.walk()` yields an import that follows a type in front of it. Only a
   mutated text shows it; `t2_parse.lib_walk` orders by token.
4. `poolmodel.py` skips imports without `.*` and ignores brackets behind the
   name of a local. Neither occurs in the corpus; the test skips the model
   comparison for texts that have them.
5. Five classes that exist only as source are asked of the provider in a
   whole run (`Block_Vee_OHV`, `OHV_Camshaft`, `SOHC_Camshaft`,
   `OHV_CylinderHead`, `SOHC_CylinderHead`; spec 1.4 item 5 counts 1 + 4
   uses); it declares each from its source with `slrrc.declare_source`. The
   other 616 class names that only a source declares are never asked for.
6. The equality with `poolmodel.py` also holds on 1,707 mutated sources, so
   the type walk and the scopes of the locals word agree with the model
   outside the corpus as far as random edits reach.
7. Two larger runs were made once and are not stored as tests: 16,320
   damaged texts (truncated, cut, random bytes, inserted bytes; both modes;
   `str` and `bytes`) through `front()` ended without an exception and with
   `ok` exactly when no error was reported; of 18,840 line- and token-edited
   texts 4,114 were accepted by both parsers and gave the same pool prefix
   and rows as `poolmodel.py`.
8. Limitation of the mutation checks: they compare with `lib\`, so they say
   nothing about input both reject or about the constructs of section 4,
   where the package decides alone. Those are covered only by the 181
   dialect cases.

## 6. What remains

Not started, in the order of the spec:

- **M4 trees**: `emit.py` (statements, expressions, names, calls, locals,
  labels, lines), `floatlit.py` (settings A to D selectable, A the default,
  the per-file table `tests\baseline\m5_float.tsv` for the oracle tests),
  `packs.py` (the `PackNames` interface and the 126-entry table behind it for
  the tests), field lookup and the scored method lookup in `classes.py`,
  `tests\t4_trees.py`.
- **M5 files**: the resolve and emit passes of the pool, node encoding and
  the file assembly in `write.py`, `compile_source` in `__init__.py`,
  `tests\t5_files.py`.
- **M6**: `cli.py`, `tests\t6_source_only.py`.
- The open points are untouched: nothing here depends on the float
  conversion or on the spelling of pack names. The scanner hands over the
  digits (`float_digits`) and the pack name as written (`rid_parts`).

What the next step finds ready:

- `front()` gives per class the `ClassBuild` with the pool after step 3, the
  rows and `rows.owners`; `res.scope` resolves type names (`classes.descriptor`)
  and reaches other classes (`scope.find`, own classes first).
- The line of a node is `ast.line(tok)` of a token index the tree keeps; the
  token ranges equal the ones the line rules were proven on.
- `ClassInfo.fields` and `.methods` are lists in source order (from a class
  file: statics before instance fields; methods by tree number). The lookup
  functions will want a name index on top.
- `CorpusProvider` follows the duplicate rule of `analysis\expr\classdb.py`
  (first class file in index order, a file named like the class wins over a
  blob inside another file), so that results can be compared with the spike.
- Emission-level warnings of appendix B are still to be raised by the
  emitter through `diags.unverified`.

## 7. Notes for the C++ port

(Later, `impl_fix.md` section 5 decides one point left open here: the parser
keeps its exception in C++; "or an error flag checked in the loops" is
withdrawn. It also adds a vector `deepest` beside `heights`.)

- Character sets (`frozenset`) are 256-entry tables; `KEYWORDS` and
  `OPERATORS` are a switch or a small map; operators are matched 4, 3, 2, 1
  characters long.
- The parser unwinds with one private exception type after the first
  diagnostic; in C++ that is a private exception caught in `parse()`, or an
  error flag checked in the loops. Nothing else in the package raises.
- `Parser.done` computes the height of every node from its children; the
  port needs the same `heights` vector, parallel to the nodes.
- `names_and_rows` interns a method's descriptor before its name and a
  field's name before its (already present) type in separate statements;
  keep them separate, C++ does not order the evaluation of arguments.
- In the Python reference a level of nesting costs 3 to 15 interpreter
  frames. A host that wants `MAX_DEPTH` to be the limit that triggers raises
  `sys.setrecursionlimit` to 4000 or more (the tests set 20000). Under
  Python's default of 1000 frames the limit of 200 is still what stops
  statements, blocks and operator chains (193 levels inside a method), while
  nested parentheses stop at about 163 and nested calls at about 140, with the
  same `E_LIMIT`, through the `RecursionError` net in `parse()`. The C++
  parser has no such second limit.
- `dump_tokens` and `dump_ast` are the formats `tests\t7_cpp.py` can compare:
  one token per line (`line kind text`), one node per line (`index kind tok0
  tok1 op_tok name_tok a b c flags [items]`, `-` for none, flags in hex).
