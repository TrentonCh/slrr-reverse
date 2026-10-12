# What the Build 940 exe does: float literals and pack names (M9)

Written 2026-10-11. Until this note the compiler had two open points
(`compiler_spec.md` 1.4, items 1 and 2): which float literal conversion the
game uses, and where the pool spelling of a pack comes from. Both were read in
the exe. This note holds the two rules, the addresses, and the numbers that
prove them on the corpus.

The exe is `StreetLegal_Redline.exe` of the Build 940 install, SHA-256
`f44fb88a4335c657e20701cdb84afb8d1d85f7c3aceb93d30113e6e6e44fefc2`, PE time
stamp 0x693EF2B3 (2025-12-14 17:24:03 UTC), linker 14.0, C runtime linked
statically. It was read in Ghidra and as bytes. It was never run.

Status words: **confirmed** = the quoted instructions or decompiled lines
were read, and where the corpus can tell, a test checks the rule on it;
**inferred** = concluded, not read; **open** = said outright. The instruction
bytes and constants quoted in section 1 were read a second time straight from
the file when the rules were applied, and `tests\t9_exe.py` checks 14 of them
at their addresses on every run.

## 0. Result

| Point | Rule | Proof on the corpus |
| --- | --- | --- |
| Float literals | The exe has **one** conversion: the engine's own `sscanf("%f")`, whose digit loop is scalar SSE single precision. It is setting **B** of `slrrc\floatlit.py`. No x87 arithmetic is on the path, so no machine state the game changes can alter a bit; there is one code path. | The 136 class files of the corpus that this exe itself wrote: **136 of 136 byte-identical under B**, no per-file choice (90 under A). B is the exact model on 15,954 of 15,954 literal texts. |
| Pack names | The compiler does not choose the spelling. It resolves every resource id through the resource engine and writes the name of the engine's **pack record**: the spelling of the pack's **first registration in that session**. A pack without a file is global id 0, which is `system.rpk` id 0. | The table replayed as a session builds it (start of the game, the pack scans of `GameLogic`, then the sources): **126 of 126** spellings of the shipped pools, **9,058 of 9,058** resource id nodes, the same for any order of compilation. Without the scans 119 or 123. |

What stays unexplained is small and is stated where it belongs: which of
three earlier forms of the float routine each of 215 shipped files was
compiled with (1.7), and which session compiled the shipped classes (2.7).

Changes that follow for the compiler: the default float setting is B (was A);
the 126-entry pack table is a test oracle and no input any more; 10 of the 635
class files of the cache change (section 3).

## 1. Float literals

### 1.1 The path (confirmed)

`JVM::compileSource` 0x5598F0 calls the yacc parser 0x54DB20 (its only
caller), which calls the flex scanner `yylex` 0x5523F0 (its only caller).
`compileSource` has two callers, `JavaMachine::loadClass` 0x55BEC0 (for every
class it loads) and `CompileAll` 0x555510 (the native
`java.lang.System.compileAll`, function 0x4E54C0, registered at 0x4D6168; the
one script that calls it is `GameLogic.java` line 407, under `DEBUG_MODE`).
Every way to compile reaches the same scanner.

The float action of the scanner (cases 0x25 to 0x28 of its switch):

```
00552f2e  PUSH EBX                              ; yytext
00552f2f  CALL 0x005693c0
00552f34  ADD ESP,0x4
00552f37  MOV EAX,0x14c                         ; token FLOAT
00552f3c  FSTP float ptr [0x007a2514]           ; yylval
```

0x5693C0 is `sscanf(text, "%f", &result)` with the result preset to 0.0:

```
005693c7  MOV dword ptr [EBP + -0x4],0x0        ; result = 0.0
005693cf  PUSH 0x5b6420                         ; "%f"
005693d4  PUSH dword ptr [EBP + 0x8]            ; text
005693d7  CALL 0x00569380                       ; -> 0x527140, the engine's own vsscanf
005693dc  FLD float ptr [EBP + -0x4]
```

0x527140 is a hand-written `vsscanf` (not the C runtime's; the engine parses
its configuration files with it). Its case `f` calls 0x527530 with the
precision of the format, 0x400 when the format has none. `FLD m32` and
`FSTP m32` are the only x87 instructions on the path, and both are exact.

### 1.2 The routine, 0x527530 (confirmed)

```
0052753f  CMP byte ptr [EDX],0x2d               ; '-' (never part of a token of the scanner)
00527549  MOV AL,byte ptr [EDX]
0052754b  CMP AL,0x30 / JL 0x0052760c           ; first character no digit:
00527553  CMP AL,0x39 / JG 0x0052760c           ;   return, nothing stored
00527564  MOVSX EAX,AL                          ; the integer part:
00527567  LEA ECX,[ECX + ECX*0x4]
0052756b  LEA ECX,[ECX + -0x18]
0052756e  LEA ECX,[EAX + ECX*0x2]               ;   ip = ip*10 + c - 48, 32 bits, wraps
00527577  MOVD XMM0,ECX
0052757b  CVTDQ2PD XMM0,XMM0                    ; (double)(int32)ip
0052757f  SHR ECX,0x1f
00527582  CMP byte ptr [EDX],0x2e               ; '.'
00527585  ADDSD XMM0,qword ptr [ECX*0x8 + 0x593fb0]   ; + {0.0, 4294967296.0}: ip as UNSIGNED
0052758e  CVTPD2PS XMM1,XMM0                    ; v = (float)ip, one rounding
00527598  MOVSS XMM3,dword ptr [0x005935d4]     ; 0.1f, bytes cd cc cc 3d
005275a0  MOVAPS XMM2,XMM3                      ; s = 0.1f
005275a7  MOV ECX,dword ptr [EBP + 0x10]        ; n = 0x400 fraction digits at most
005275b4  TEST ECX,ECX / JLE 0x005275db
005275bc  SUB EAX,0x30 / DEC ECX
005275c0  MOVD XMM0,EAX
005275c6  CVTDQ2PS XMM0,XMM0                    ; (float)d
005275c9  MULSS XMM0,XMM2                       ; t = d * s      single
005275cd  MULSS XMM2,XMM3                       ; s = s * 0.1f   single
005275d1  ADDSS XMM1,XMM0                       ; v = v + t      single
005275db  ...                                   ; the digits beyond the 1,024th are skipped
00527601  MOVSS dword ptr [EAX],XMM1            ; *result = v
```

Every value is a binary32 in an XMM register from the first instruction on.
The only state this code depends on is MXCSR (rounding mode, flush to zero).
In Ghidra's listing of the program 36 instructions load the x87 control word,
the x87 environment or MXCSR; all lie in the statically linked C runtime and
its math library (lowest address 0x56A8F5), and none sets a rounding mode on
behalf of the engine. The compile therefore runs with the process default:
round to nearest even, gradual underflow. (Whether a DLL changes MXCSR on the
compiling thread cannot be excluded by reading the exe; the 136 files of 1.6
show that it does not happen.)

Direct3D does switch the device thread to 24-bit x87 precision in this
process: none of the three `IDirect3D9::CreateDevice` attempts in 0x425A30
passes `D3DCREATE_FPU_PRESERVE` (`PUSH 0x44`, `PUSH 0x84`, `PUSH 0x24` at
0x425D89, 0x425DCE, 0x425DF7). It does not matter for Build 940, because
there is no x87 arithmetic to affect. (What `d3d9.dll` does without the flag
is Microsoft's documentation, not read.)

The parser stores the bits untouched in a `float` node (yacc rule 4:
`JTree(0x200a, value)`). A minus is an operator node (rule 0x110: `expr`,
kind 0x16, the operand as its child); the scanner has no signed number, so
nothing is ever folded into a literal. The corpus agrees: 9,521 unary minus
tokens, 9,521 `neg` nodes.

### 1.3 The rule

For a float literal token T (the suffix is not read; a sign is never part of
the token):

```
if T[0] is not a digit:        bits = 0x00000000            # ".5" is 0.0
uint32 ip = 0
for each leading digit c:      ip = ip * 10 + (c - '0')     # wraps modulo 2**32
float v = (float)ip                                        # unsigned -> binary32, to nearest even
if the next character is '.':
    float s = 0.1f                                         # 0x3DCCCCCD
    for each following digit c, at most 1,024 of them:
        float t = (float)(c - '0') * s                     # one binary32 product
        s = s * 0.1f                                       # one binary32 product
        v = v + t                                          # one binary32 sum
bits = the 32 bits of v
```

Every operation is one IEEE-754 binary32 operation, round to nearest even,
gradual underflow, no fused multiply-add, nothing wider in between. This is
`floatlit.float_bits('B', int_digits, frac_digits)`.

Two consequences that a decimal reader would not have. Both were read, not
run, and no shipped source has either form (`tests\t9_exe.py`: 0 of 15,954
literal texts, longest fraction 34 digits):

- A literal with no digit before the point compiles to **0.0**: the scanner
  matches `.5` as a float (flex rule 0x26), the routine returns at the point
  without storing, and the wrapper returns its preset 0.0.
- Only the first 1,024 fraction digits enter the sum. No bit depends on it:
  the scale `s` is zero from the 46th fraction digit on in single precision
  (from the 324th in the double form D).

### 1.4 What settings A, C and D are

They are the same routine in **x87** arithmetic, which this exe does not
contain. The 2005 build of the game on this machine
(`E:\NFS.SLRR Edition\...\StreetLegal_Redline.exe`, linker 6.0, writes build
0x11AE1, no SSE) has the same own `vsscanf` at 0x553890 with the routine in
that form (confirmed):

```
00553b98  CMP CL,0x30 / JL ... / CMP CL,0x39 / JG ...     ; the same exit for ".5"
00553bd0  FILD qword ptr [ESP + 0x20]           ; v = ip (unsigned, exact)
00553bd9  FLD float ptr [0x005f08e8]            ; s = 0.1f            (cd cc cc 3d)
00553bea  TEST ESI,ESI / JLE ...                ; the same count of 0x400 digits
00553c00  FILD dword ptr [ESP + 0x18]           ; d
00553c04  FMUL ST1                              ; d * s
00553c06  FADDP ST2,ST0                         ; v = v + d * s
00553c08  FMUL double ptr [0x005f0d20]          ; s = s * 0.1         (the double 0.1)
00553c35  FSTP float ptr [EBP]                  ; result = (float)v
```

`v` and `s` stay in x87 registers, so every product and sum is rounded to the
precision control of the moment. Exact models of this code against the four
settings, on all 15,775 distinct literal texts of the consistent pairs:

| Setting | Machine form | Texts that differ |
| --- | --- | ---: |
| A | the x87 code under 24-bit precision control (what Direct3D leaves on the device thread) | 0 |
| D | the x87 code at 53 or 64 bits, `v` and `s` in registers | 0 |
| C | the same statements at 53 or 64 bits with `v` and `s` stored to `float` after each statement | 0 |
| B | the SSE code of Build 940 | 0 |

So A, C and D are **arithmetic identities** with x87 forms of the routine
(confirmed on the corpus texts), and B is what ships. That the shipped class
files which need A, C or D were written by earlier developer builds of the
2.3.1 exe is **inferred**: those builds are not on this machine. What is
certain is the negative: the shipped exe cannot produce those bits, and none
of the three other SSE rounding modes explains them (at most 27 of the 216
files B does not reproduce). One corner is left as it was because no literal
of the corpus decides it: an integer part above 2^24 with fraction digits
(`FILD` keeps it exact until the first sum; `floatlit` A rounds it first).

### 1.5 The other literal forms (confirmed in the flex tables; none was run)

The scanner's DFA was run on its own tables (`yy_accept` 0x5E0758, `yy_ec`
0x5E0CB8, `yy_meta` 0x5E10B8, `yy_base` 0x5E11B0, `yy_def` 0x5E1758, `yy_nxt`
0x5E1D00, `yy_chk` 0x5E2608) over about 110 inputs.

| Shape | Example | The exe | The reference |
| --- | --- | --- | --- |
| `[0-9]*\.[0-9]+[fF]?` | `1.5`, `1.5f`, `.5` | FLOAT (`.5` is 0.0) | the same; `.5` and suffix `F` with `W_UNVERIFIED` |
| `[0-9]+\.[fF]?`, `[0-9]+[fF]` | `5.`, `1f` | FLOAT | the same (`5.` with `W_UNVERIFIED`) |
| the float shapes with `d` / `D` | `1.5d` | FLOAT and the error "NOT SUPPORTED DATA TYPE" | `E_UNSUPPORTED` |
| exponent | `1.5e3` | no rule: the float `1.5`, then the identifier `e3` | `E_UNSUPPORTED` |
| `0`, `[1-9][0-9]*` | `123`, `4294967296` | INT, modulo 2^32 | the same |
| `0[xX]hex+` | `0x1F`, `0xFFFFFFFFF` | INT, the low 32 bits | the same |
| `0[0-7]+` | `012` | error "lexer: octal values not supported"; the value is not assigned | **differs**: read as decimal, `W_UNVERIFIED` |
| `0` then `8` or `9` | `08` | two tokens | **differs**: read as decimal, `W_UNVERIFIED` |
| decimal with `l` / `L` | `123L` | error "NOT SUPPORTED DATA TYPE" | `E_LEX` |
| hex with `l` / `L` | `0x10L` | INT 0x10, no error | **differs**: `E_LEX` |
| `[1-9][0-9]*[rR]`, `0[xX]hex+[rR]` | `12r`, `0x1000r`, `0x1R` | RID token carrying the number (decimal or hex) | hex with `r` only, as one token with the pack name (2.1) |

The three rows marked "differs" are not applied (section 4).

### 1.6 The class files this exe wrote, and the proof

`compileSource` recompiles a class when the class file is missing, or when
class and source differ in the stamp `(time >> 1) | date << 0xf` of
`FileTimeToDosDateTime` (0x5453B0), and after a compile it gives the class the
stamp of its source (`DosDateTimeToFileTime(v >> 0xf, (short)v * 2)`,
`SetFileTime`, 0x545CB0). So a class file the exe wrote carries the write time
of its source in 4-second steps with no fraction of a second, and a file
Steam wrote carries the moment of the download.

`diag.log` of the install shows one run of the stock game, 2026-10-09
00:43:49 to 00:44:11 UTC, right after the download. In the corpus 136 of the
2,819 pair class files have a whole-second write time, each equal to that
stamp of its source; in the Steam folder exactly those 136 have an NTFS
creation time inside that run (00:43:51.26 to 00:43:54.77 UTC), and the other
2,683 have creation time = write time. These 136 are output of the Build 940
exe. The list is carried in `tests\baseline\m5_exe940.tsv` (a test must not
recompute it from file times; `t5_files.py -v` says whether the stamps of the
copy at hand still agree: 136 of 136, 0 others).

| Class files | Files | Identical under B | Under A | By the settings that reproduce them |
| --- | ---: | ---: | ---: | --- |
| written by the Build 940 exe | 136 | **136** | 90 | B 40, BD 6, AB 1, ABC 18, ABCD 45, no float literal 26 |
| as Steam shipped them | 2,515 | 2,298 | 2,471 | not under B: A 22, AC 150, ACD 14, AD 1, C 12, CD 10, D 6 (215), and the 2 expected failures |
| all oracle pairs | 2,651 | 2,434 | 2,561 | 2,649 with the setting chosen per file |

46 of the 136 are reproduced by neither A nor C. The example of
`foundation.md` is this exactly: `0.0001` is 0x38D1B716 in `Baiern_oil_pan`
(shipped) and 0x38D1B718 in `LP_V12_oil_pan_racing` (written by the exe): the
bits of A and of B.

A by-product for the host: 646 more oracle pairs sit in another 4-second stamp
than their source (2,005 of 2,651 agree), so the stock game recompiles them,
with B, the first time it loads them. A Steam install drifts towards
B-compiled classes as it is played.

### 1.7 What stays unexplained

Which of the x87 forms A, C, D each of the 215 shipped files was compiled
with. Nothing in a class file or in the install records it, the builds are not
at hand, and no rule is made up: `tests\baseline\m5_float.tsv` keeps the first
setting that reproduces each file, and `t5_files.py -v` prints the 215 by the
settings that reproduce them. Of the 60 files that table lists under B, 46 are
files the exe wrote; the other 14 are shipped (2 that only B reproduces,
`Shimutshibu_VT` and `Wheel`, and 12 that C reproduces too). All 22 C files
and all 6 D files are shipped files.

### 1.8 In the reference

- `floatlit.DEFAULT_MODE` is `'B'` (`EXE_MODE`); `float_bits` has the two
  exits; A, C, D stay selectable (`--float`, `Options.float_mode`) for the
  oracle tests only.
- `floatlit.SELF_TEST` (15 literals, 4 settings) and `EDGE_TEST` (the forms a
  library conversion reads otherwise) are what a port runs at start; a build
  that converts with `strtof` differs on 4,748 of the 15,954 literal texts of
  the corpus under B.
- Tests: `m5` prints `2434/2651 with setting B ... alone` and `written by the
  Build 940 exe 136/136 under B`; `m9` repeats the 136 and compares B with the
  exact model on every literal text; `m6` compiles the 635 under B and shows
  that setting A still gives the old bytes.

## 2. Pack names

### 2.1 Scanner and parser (confirmed)

There is no `pack:0xHEXr` token. `0x...r` (rule 0x24) and `123r` (rule 0x23)
are tokens of their own (0x14A) whose value is the number, read by the same
own `sscanf("%d")` (0x5693F0, format at 0x5B641C): hexadecimal modulo 2^32
when the text starts with `0x` / `0X` (0x527490), else decimal (0x527420);
suffix `r` or `R`. The pack name is the grammar's dotted name in front of
`:`.

The parser builds the written path (rule 0x24, three symbols):

```c
pcVar13 = (char *)FUN_0055c1d0(DAT_007a25c0,puVar2[-2]);        // name -> "a\b\c.rpk"
DAT_007a2530 = (undefined4 *)FUN_0054b200(pcVar3,*puVar2);      // {path, number} in the pool of the source
```

0x55C1D0 joins the parts with `\` (0x5B5028, bytes `5C 00`) and appends
`.rpk` (0x5B502C). No case change and no lookup. That pool belongs to the
source being compiled and is never written to a file.

### 2.2 Emission: where the spelling changes (confirmed)

`JVM::compileHook` 0x5557C0 runs 0x54B310 over every tree before it writes a
class (call sites 0x556A7D, 0x556B46, 0x556B61, 0x559233). For a node with op
0x100E:

```
0054b400  PUSH dword ptr [ECX + 0x8]        ; the number
0054b403  MOV EAX,dword ptr [EAX + 0x8]     ; the written path
0054b407  CALL 0x0055cad0                   ; gid = engine.resolve(path, number)   (-> 0x5189E0)
0054b426  CALL 0x0054b170                   ; the class pool entry for gid
```

and 0x54B170 writes the name of the engine's record:

```
0054b182  CALL 0x00517b80                   ; the record of gid >> 16
0054b187  LEA ESI,[EAX + 0xc]               ; its name
0054b19d  CALL 0x0054b260                   ; utf8 of that name in the class pool
0054b1a2  MOVZX EBX,BX                      ; gid & 0xffff
0054b1d6  MOV dword ptr [EAX + 0xc],ESI     ; rid entry: utf8 index
0054b1d9  MOV dword ptr [EAX + 0x8],EBX     ;            local id
```

### 2.3 The engine's pack table (confirmed)

The resource engine is the object at `*0x5DB008`. Its records: array at
engine + 0x1667B4, 0x70 bytes each, count at engine + 0x166740. A record:
`+8` own index, `+0xC` name (`char[0x40]`), `+0x50` state (0 never loaded, 2
loaded), `+0x54` and `+0x58` the two counts of the file header, `+0x5C` its
external reference table.

**find-or-add**, 0x518940: the first record `i` with
`_stricmp(record[i].name, name) == 0`, else a new record. `_stricmp` 0x57C11E
folds A to Z only (`if (c - 0x41 < 0x1a) c += 0x20`); nothing else is folded,
so `a/b.rpk` and `a\b.rpk` are two records. A new record (0x51E2A0) keeps the
name as given, cut to 63 characters (`copy(record + 0xc, name, 0x40)`). The
only callers of 0x51E2A0 are find-or-add and the engine's constructor.

**load on first use**, 0x51E310: when the state is 0, try to open the file
(0x565200: `/` read as `\`, then `CreateFileA(OPEN_EXISTING)` relative to the
working directory). If that fails, return 0 and leave the record as it is.
Else read `RPAK`, u32 0x200, u32 n1, u32 n2, then n1 + n2 records of 0x40
bytes `{u16, u16, name, u32}`, and for each of them **find-or-add its name**
and note the record number (the named packs are registered, not loaded).

**resolve**, 0x5189E0 with the id mapping 0x51E860:

```c
i = find_or_add(name);
if (!load(i)) return 0;                                   // no file: global id 0
lo = id & 0xffff;  hi = id >> 0x10;
if ((lo != 0) && (hi < count)) {
  if (hi != 0) {
    if (hi <= n1 + n2) return ext[hi].record << 0x10 | lo;   // the hi-th external reference
    return 0;
  }
  return i << 0x10 | lo;
}
return 0;
```

**the record of a global id**, 0x517B80: record `gid >> 16`, loaded on the
way; a null pointer when its file cannot be opened.

Global id 0 is record 0, and record 0 is `system.rpk`: the start-up code
0x4ED680 calls `find_or_add("system.rpk")` right after the engine's
constructor has set the count to 0. So a pack without a file compiles to
`system.rpk` id 0, and the failed lookup still leaves a record with the
written spelling.

### 2.4 Who registers a name

Callers of find-or-add: 0x4ED680, 0x4E0470, 0x51E310, 0x5189E0, 0x4D5C80,
0x479240.

| Registrant | Code | The spelling it registers | Status |
| --- | --- | --- | --- |
| start of the game | 0x4ED680 | `system.rpk`, record 0 | confirmed |
| `System.openLib(String)` | native 0x4E0470, registered at 0x4D6143 | the script's string, unchanged; then the pack is loaded | confirmed |
| `System.rpkScan(String)` | **a script method**, not a native (no such string in the exe; tree 38 of `system\scripts\lang\System.class`): `name = ff.first(dir + "*.rpk"); while (name) { System.openLib(dir + name); name = ff.next(); }` | the directory **as the script spells it** + the file name **as `FindFile` returns it** | confirmed |
| `FindFile.first` / `next` | native 0x4D9E90 -> 0x5652C0 -> 0x545520: `FindFirstFileA`, `cFileName` copied unchanged, the list sorted with `qsort` | the spelling of the directory listing | confirmed (the comparison function 0x564E40 was not read) |
| a pack that is loaded | 0x51E310 | every name of its external reference table, as the file spells it | confirmed |
| a resource id node the VM executes | 0x54B900 -> 0x55CAD0, called from the interpreter 0x562D70 at 0x5637DF | the pool spelling of that class file | confirmed for the call; that this is the `rid` case of the interpreter is inferred from the single caller |
| a data file opened for reading | `File.open` native 0x4D9830 -> 0x4D5C80 | the pack names stored in the file | confirmed |
| the compiler | 2.2 | the written path | confirmed |
| literals of the exe | resolve with `system.rpk`, `frontend.rpk`, `cars.rpk`, `sl.rpk`, `particles.rpk`, `sound.rpk`, `decals.rpk`, `humans.rpk`, `part.rpk` | lower case, as on disk | confirmed |

The constructor of `GameLogic` (`sl\Scripts\game\src\GameLogic.java`, lines
345 to 360) scans `parts\engines\`, `parts\`, `cars\racers\`,
`cars\fake_racers\`, `maps\`, `sl\Scripts\game\CareerEvents\` and
`multibot\maps\<D>\` for every directory D of `multibot\maps`. No other
shipped source calls `rpkScan`.

### 2.5 The rule

**For the compiler.** A resource id literal is a dotted name `p1.p2...pk`,
`:`, and a number `n`:

```
path = p1 + "\" + p2 + ... + pk + ".rpk"          # the letter case of the source
(name, local) = resolve(path, n)                  # the pack table of the session; asking registers
pool: utf8(name), then rid {utf8 index, local}    # found again by (utf8 index, local)
```

The compiler asks when it writes the class, tree by tree in tree order. It
never changes a letter, never reads a directory and never checks that the
pack holds the id.

**For the provider** (the pack table; `packs.PackTable`):

```
records: ordered list of {name, loaded, ext[]};  record 0 is "system.rpk"
find_or_add(s):  the first i whose name equals s with A-Z folded; else append {s cut to 63 characters}
load(i):         not loaded: the file records[i].name cannot be opened -> false
                 else loaded; for each external reference name of the file, in order: ext.append(find_or_add(name))
resolve(path, n):
    i = find_or_add(path)
    if not load(i):                        return ("system.rpk", 0)
    lo = n & 0xffff;  hi = n >> 16
    if lo == 0 or hi >= len(records):      return ("system.rpk", 0)
    if hi == 0:                            return (records[i].name, lo)
    if hi <= number of external references of pack i:
        j = the hi-th of them;             return (records[j].name, lo)     # the exe faults when pack j has no file
    return ("system.rpk", 0)
```

**Feeding the table in a host.** Register, in the order the events happen:
`system.rpk` at start; every `System.openLib` argument unchanged, then load;
the external names on each first load; the pool name of every resource id
node the VM executes; the pack names of a data file opened for reading; the
compiler's own lookups. The host must not register its own internal opens (a
name with forward slashes is another record), and it must keep the first name
of a record verbatim. The current registry of the host
(`engine\Core\Platform\rpak.cpp`) keys a pack by its resolved path with
forward slashes and keeps the base name, so it cannot serve as it is.

**Without a session** (the command line, the tests): replay the start of one,
`packs.session_940`: `system.rpk`; `openLib(directory + file name as listed)`
for every `*.rpk` of each directory the constructor of `GameLogic` scans; then
compile. In closed form, for the Build 940 install:

```
no file                                            -> system.rpk, id 0
the file lies in a scanned directory               -> the scan argument + the listed file name
else, an external reference table read earlier
      names the pack                               -> that name
else                                               -> the written path (the first one asked for)
```

### 2.6 Proof on the corpus

All numbers are printed by `tests\t9_exe.py` (`m9`); the first two also by
`tests\t4_trees.py` (`m4`), where they come out of the emitter.

| What | Result |
| --- | --- |
| The scan list of the package against the `rpkScan` calls of `GameLogic.java` | equal: 6 directories and the loop over `multibot\maps\` |
| Records after the start of the game and the scans | 88 |
| Spellings of the shipped pools that are the name of a record after the sources were compiled | **126 of 126** (first registered by: the start of the game 1, a scan 61, an external reference table 12, a literal 52) |
| Resource id nodes of the consistent pairs with the pack name and id of the rule | **9,058 of 9,058** |
| Rid entries of all shipped pools: 7,242; with local id 0; with a local id above 16 bits | 1 (`system.rpk` in `TimeAttackTest.class`, whose source names the pack without a file); 0 |
| Literals of all sources (9,952) with the same answer in index order, in reverse order, and for each source against a table of its own | 9,952 |
| ... and equal to the answer of the 126-entry table over the pack files | 9,952 |
| Control, without the scans: spellings reproduced | 119 (index order), 123 (reverse order) |
| `packs.resolve` over the interface against the functions of the exe followed literally (global id, then the record) | 13,042 questions, 0 differences |
| External reference tables: pack files, references, distinct names, names with more than one spelling | 147, 869, 60, 0 |
| Whole files with the replayed table in place of the 126-entry table | unchanged: 2,649 of 2,651 (`m5`); under `--float A` the 635 outputs keep their old hashes |

The two groups the old rules could not explain:

- **9 packs under `cars\racers\`** (`Baiern`, `Duhen`, `Einvagen`, `Enula`,
  `Focer`, `MC`, `Nonus`, `Prime`, `SuperDuty`) are written in lower case by
  84 sources and pooled in the case of the listing: the scan of that directory
  registered them before those scripts compiled.
- **3 packs** are pooled in the case of the sources against the disk
  (`cars\traffic\CivilVan_2.rpk`, `CivilVan_3.rpk` where the files are
  `Civilvan_2`, `Civilvan_3`; `maps\ROC_Track\smallmap.rpk` where the
  directory is `ROC_track`): nothing scans `cars\traffic\` or
  `maps\ROC_track\`, so the listing never reaches the table.

### 2.7 Not read, inferred, open

- That no code rewrites the name of a record later: **inferred** from the two
  callers of 0x51E2A0 and from 126 of 126; writes through computed addresses
  were not searched.
- 0x527BF0 / `DAT_007a5b50` (a second file source asked before the file
  system in the existence test) and the open routine 0x565760: not read.
- The symbol between name and number in rule 0x24 is taken to be `:` from the
  token stream; the yacc symbol tables were not decoded. That white space or a
  comment may stand between name, `:` and number is **inferred** from it.
- The two id cases (a low word of 0; a high word): **read, never seen**. All
  9,952 literals of the sources have a low word and no high word, so the
  corpus cannot test them.
- A pack file that does not start with `RPAK`: the exe reads its header
  unchecked. Not modelled (the reference takes such a file for a pack without
  external references).
- **Open:** which session and which order of compilation produced the shipped
  classes. Only the table it left can be read off the pools. Where an external
  table, a scan and a literal spell a pack alike, the corpus cannot say which
  came first, and it cannot say anything about the directory part of a scan
  (the script and the listing spell every scanned directory alike here).

### 2.8 In the reference

- `packs.PackNames` keeps `canonical(written_path)` (a name, or `None` for a
  pack without a file) and gains `external(written_path, k)` for an id with a
  high word; `packs.resolve(names, path, number)` is the rule of 2.5 on top of
  the two and returns `(name, local id, why)`.
- `packs.PackTable` is the engine's table behind that interface
  (`find_or_add`, `load`, `open_lib`, and `global_id` / `pool_entry` as the
  exe has them, for the tests). A host answers for its files through
  `packs.PackFiles`; `install.DiskPackFiles` does it for a tree on disk.
- `packs.session_940` is the replay; `install.install_packs` and the tests
  use it. `packs.SCANS_940` is the scan list.
- The 126-entry table moved to `tests\pack_oracle.py`. Nothing in the package
  reads it.
- An id with a low word of 0 or with a high word is compiled by the rule and
  reported as `W_UNVERIFIED` (`E_UNSUPPORTED` in strict mode); where the exe
  would read through a null record, and for a `PackNames` that knows no
  external references, it is `E_UNSUPPORTED`.
- The table has state, so a compilation is not a function of its source and
  the install alone: the first spelling asked for stays. For the shipped
  sources this changes nothing (every source spells a pack outside the scanned
  directories alike); a manifest entry records the spelling an output was
  written with.

## 3. What changed in the outputs

- **Shipped classes:** nothing. Every headline that existed before is
  unchanged (`2649/2651`, `7568/7570`, `153/153`).
- **The 635 class files of the cache: 10 changed, 625 are byte-identical to
  what they were.** The 10 hold 33 float literals (of 1,817 in the 635 sources)
  that B converts to other bits than A:
  `European_V6_connecting_rods` (1), `stock_suspension_FL` / `FR` / `RL` /
  `RR` (1 each), `Prime_DLH_700_FL_McPherson_strut` and `..._FR_...` (9 each),
  `Prime_DLH_700_RL_trailing_arm` and `..._RR_...` (4 each), `Block_Vee_OHV`
  (2). `tests\baseline\m6.sha256` was re-baselined for these 10 on purpose;
  `tests\baseline\m6_float_a.sha256` keeps their hashes under setting A, and
  `m6` checks on every run that setting A still gives them, so nothing but the
  default moved. The pack rule changed no byte.
- `slrrc.VERSION` is 0.5; the manifest names float setting B and the version
  of the pack table replay.

## 4. Read and not applied

| Finding | Why it is not in the reference |
| --- | --- |
| `0[0-7]+` is the octal rule of the exe and an error; `08` is two tokens | The reference keeps "read as decimal" with `W_UNVERIFIED` (an error in strict mode); the warning now says what the game does. What the error reporter 0x569570 does to the compile was not followed, the value of the token is undefined in the exe, and no source has the form. |
| `0x10L` is accepted as an int | The reference refuses it (`E_LEX`). No source has the form. |
| A resource id is three tokens: white space or a comment may stand inside it, and the number may be decimal (`12r`) or end in `R` | The reference scans `name:0xHEXr` as one token. The token streams of all 3,454 sources are compared with a second scanner in `m1` and `m2`; changing the token model for forms no source uses (all 9,952 literals are `0x...r` without white space) was not worth that, and the white space part is inferred, not read. |
| A decimal `sscanf("%d")` value for `123r` | follows from the row above |
| `(float)ip` in the x87 routine is exact until the first sum | concerns settings A, C, D only, and only an integer part above 2^24 with fraction digits; no literal of the corpus decides it, and A, C, D are not used for new compilations |
| The exe faults when system.rpk itself cannot be opened and a resource id resolves to global id 0 | The reference answers `system.rpk` id 0 without looking for the file. A tree without `system.rpk` is not an install. |
| The special record 0xFFFF and the 0xFFFF check of find-or-add | It would take 65,535 packs. |
| The host's pack registry cannot serve as the provider | A change of the host (`engine\Core\Platform\rpak.cpp`), outside this directory. What it needs is written in 2.5. |
| The stock game recompiles 646 more shipped classes on first load | A property of the game, not of the compiler; relevant to the host's stale check. |

## 5. Addresses

| Address | What |
| --- | --- |
| 0x5598F0 | `JVM::compileSource` (recompile test, time stamp of the class) |
| 0x55BEC0, 0x555510 | its callers: `JavaMachine::loadClass`, `CompileAll` |
| 0x5557C0 | `JVM::compileHook`: writes the class |
| 0x54DB20 | the yacc parser (rule 4 float node, rule 0x24 resource id, rule 0x110 unary minus) |
| 0x5523F0 | the flex scanner; tables 0x5E0758 to 0x5E2608; `yytext` 0x7A25D4, `yylval` 0x7A2514 |
| 0x5693C0, 0x5693F0 | `sscanf(text, "%f")`, `sscanf(text, "%d")`; formats 0x5B6420, 0x5B641C |
| 0x569380 -> 0x527140 | the engine's own `vsscanf` |
| 0x527530 | **the float routine**; 0.1f at 0x5935D4; {0.0, 2^32} at 0x593FB0 |
| 0x527420, 0x527490 | decimal and hexadecimal integer routines |
| 0x425A30 | device creation (three `CreateDevice` calls without `FPU_PRESERVE`) |
| 0x5453B0, 0x545CB0 | DOS time stamp of a file; set the time stamp |
| 0x55C1D0 | written pack path; `\` at 0x5B5028, `.rpk` at 0x5B502C |
| 0x54B200, 0x54B310, 0x54B170 | rid entry in the pool of the source; resolve pass over a tree; class pool entry for a global id |
| 0x55CAD0 -> 0x5189E0 | **resolve(path, id)** |
| 0x518940 | **find-or-add**; compare 0x51E2F0 (`_stricmp` 0x57C11E); new record 0x51E2A0 |
| 0x51E310 | load a pack on first use, register its external references |
| 0x51E860 | id of a pack -> global id |
| 0x517B80 | record of a global id |
| 0x5DB008 | pointer to the resource engine (records at + 0x1667B4, count at + 0x166740) |
| 0x4ED680 | start-up: `find_or_add("system.rpk")` |
| 0x4E0470 | native `System.openLib` (registered at 0x4D6143) |
| 0x4D9E90 -> 0x5652C0 -> 0x545520 | native `FindFile.first`: `FindFirstFileA` |
| 0x54B900, 0x562D70 | resolve of an executed resource id node; the interpreter |
| 0x4D9830 -> 0x4D5C80 | native `File.open`: registers the pack names of a data file |
| 0x553890 (2005 exe) | the same `vsscanf` with the float routine in x87 code (0x553B88) |

## 6. Reproducing

```
python -I tests\run_tests.py m9                  # the two rules on the corpus, about 15 s
python -I tests\run_tests.py m4 m5 m6            # the same numbers where the compiler produces them
python -I tests\t5_files.py -v                   # the 136, the 215 by setting, the time stamps
```

Ghidra is not needed for any of them. `m9` reads the exe file of the corpus as
bytes (its hash, 14 quoted byte sequences, one string search) and never runs
it.
