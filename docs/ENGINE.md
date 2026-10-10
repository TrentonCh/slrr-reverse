# How Street Legal Racing: Redline works, and where the host stands

Written 2026-10-09 and fact-checked 2026-10-10 against `dev` at f43c741, the Build 940
Steam install on D: and the pre-940 reference install on E:. `docs/TASKS.md` is the live
tracker; `docs/VM.md` is the progress log with the Ghidra-confirmed Build 940 addresses;
`docs/BACKGROUND.md` has the ecosystem, legal and design notes; `FORK.md` has the
physics/FFB plan and the native contract. This document is the stable explanation: what
the game does, what the host does today, and why the roadmap is ordered the way it is.

## 1. Purpose and how to read this

The audience is anyone who has to decide what to build next without first spending a
week in Ghidra. Every statement carries a status. A plain statement is verified: it was
seen in host code (cited as `file:line` at f43c741), in an exe string, in a script
source line or class dump, in a listing or count of the install, in a disassembly or
Ghidra decompile of the Build 940 exe (cited by address), or in the confirmed table of
`docs/VM.md`, and the parenthetical says which. `[inferred]` marks a statement reasoned
from such evidence and says from what; `[unknown]` marks a gap and says what would
settle it.

Addresses come from two exes. An `@0x...` in an upstream host comment is an old-exe
address from upstream's IDA work on the E: build; comments added by the fork quote Build
940 addresses in the same form or as `FUN_00xxxxxx` (`jvm_vmthread_part1.inc:483-487`,
`tufa.cpp:500`), so check a comment's address against `docs/VM.md` first. Here "940 VA"
and `FUN_00...` mean Build 940 and "old-exe" means E:. Beyond the VM functions of
`docs/VM.md`, Build 940 addresses now exist for the main loop and the class-load compile
check (sections 3 and 4, static disassembly) and for the present, camera and text paths
(section 7, Ghidra). Counts were made on the D: install and the repository at f43c741
with `find`, `grep -c`, `tools/tufa_dump.py` and short walkers over the class, pack and
mesh files.

## 2. The game at a glance

One process, three layers. `StreetLegal_Redline.exe` (2,446,848 bytes; `diag.log` banner
`SL2 build 940 (Dec 14 2025 20:23:29), distr. v2.3.1`) contains the engine natives
(rendering, resources, input, sound, the vehicle simulation), a small Java-like virtual
machine with its own compiler, and a native method table joining them. Everything a
player sees after the first loading screen is script: menus, garage, career, dialogs,
cameras, part rules, car assembly. The scripts are a Java subset compiled to the exe's
own class format (TUFA) and run by that VM; they call the natives, and the natives call
back into scripts for control, animate, timers and events.

| Layer | Lives in | Changed by |
| --- | --- | --- |
| Natives (engine services) | the exe; in the host, `engine/Runtime/*` and `engine/Core/Platform/*` | adding or implementing a native in the host |
| VM (language, loader, scheduler, compiler) | the exe; in the host, `engine/Core/Jvm/*` | rewriting freely in the host |
| Scripts (game rules, UI, career) | `*.java` sources beside `*.class` files in the install | editing the `.java`; the exe recompiles on demand |

What lives where on disk (Build 940 install: 25,790 files and 2.50 GB on 2026-10-10;
host runs write into it, 20 of those files are theirs, so the counts drift):

| What | Count | Where |
| --- | ---: | --- |
| Script sources `.java` | 3,454 | 95 `src/` directories: cars 1,933, parts 1,364, sl 133, multibot 22, misc 2 |
| Compiled classes `.class` | 2,921 | cars 1,515, parts 1,147, sl 140, system 95, multibot 22, misc 2 |
| Source/class pairs | 2,819 | 635 sources have no class; 102 classes have no source (system 95, sl/CareerEvents 7) |
| Resource packs `.rpk` | 146 | root 13, cars 54, multibot 45, parts 15, maps 9, misc 8, sl 2 (149 on disk incl. 2 host stubs in `tree_rpk_scan_boot/` and 1 in `modding_tools/`) |
| Decoded pack text `.rdb` | 118 | beside most packs; human-readable form of the same tables |
| Meshes `.scx` (INVO) | 6,033 | cars 3,551, parts 958, objects 865, maps 536, frontend 34 (703 MB); one is an empty sample file |
| Textures | 3,296 `.dds`, 1,685 `.png`, 86 `.tga`, 67 `.ptx` | maps/multibot DDS, cars PNG, frontend atlases |
| Audio | 337 `.wav`, 25 `.mp3` | parts 133 (engine samples), sound/wav 73, frontend 30; music in six `Music\*` set folders |
| Video | 2 `.avi` | `data/Fmv/background_motion.avi` (28,080,956 B) and `_HD` (63,212,544 B); no intros ship |
| Save data | `save/career` 31 profile directories (28 dated with the install, 3 written by host runs), `save/cars` 871 files (`database` 804, `special/DTM` 67), `save/skins/database` 810 | SDAT containers written by the script `File` natives |

The 635 missing classes are all part or car variants: cars/racers 418, parts/wheels 135,
parts/running_gear 38, parts/engines 28, parts/scripts 15, parts/accessories 1 (census
of `*/src/*.java` against `../*.class`). The class files carry three TUFA build numbers:
2,757 at 0x24F9D (Build 940), 159 at 0x24F5C and 5 at 0x11AE1 (pre-940). The 940 exe
accepts build 0x24F9D, or `TUFA` version 4 with build 0x11AE1, and treats anything else
as stale (940 VA 0x559A65-0x559A87); its class writer always stamps 0x24F9D (0x5593C9).
The 159 files at 0x24F5C are therefore leftovers of an intermediate build: 152 paired
car-part classes in eight `cars/racers/*_data/scripts` directories, which the stock exe
recompiles, and the 7 source-less `sl/Scripts/game/CareerEvents` test events
(CircuitTest, DebugMode, DerbyTest, DragTest, DriftTest, FreerideTest,
HotActionCopTest), which it cannot load.

## 3. Boot and the frame

**Startup.** `WinMain -> Engine_boot -> Engine_MainLoop` (old-exe 0x551430 / 0x58C700 /
0x428960; host comments `main_part2.inc:517`, `game_boot_part1.inc:649-676`).
`Engine_boot` initialises the resource engine, opens `system.rpk`, creates the JVM and
the native table, constructs the `Config` GameType (`script java.util.Config` in
`system.rdb`; `loadConfig` reads the SDAT file `save/game/options`, 236 bytes, magic
0xFEDCBA98 version 13), opens the D3D9 display, resets input, initialises DirectSound
and enters the main loop. Boot videos are a pre-940 stage: the E: exe names
`Data\FMV\Activision.avi`, `Invictus.avi` and `StreetLegal.avi`, the 940 exe has no
`.avi` or FMV string (string dumps), so it has no such stage `[inferred]` and the host's
`WARN --game boot FMV intros missing` (`game_boot_part1.inc:686-689`) is expected on
940. The loop first creates the game object: `GetConfigData("init")` (940 VA 0x47C110)
is bound as a type-8 handle and passed to `LoadGameInit(worldTreeRoot, type, "",
"GameInit")` (0x523C80; disassembly 0x47D7A0-0x47D7D4). `GameInit` is only the instance
alias; the type is the static `Config.init`, initialised to the rid `sl.rpk:0x40`
(Config.class), the pack entry `Init` with payload `script java.game.Init`. The host
knows this (`game_boot_part1.inc:26-30, 90-92`) but passes a null type and constructs
`java.game.Init` directly.

`Init.java` (sl/Scripts/game/src, 196 lines) is the whole boot script: `Frontend.init()`
(line 24: GfxEngine object, two LoadingScreens, `Steam.init`, a `Thread` named "Hotkey
watcher", fonts), `Frontend.loadingScreen.show(frontend:0x3EB)` (25), three menu sound
precaches, `Sound.init()` (39), `Input.initControllers(this,
"save/controls/active_control_set")` (41), the `Config.restart_*` copy, and
`gameLogic = new GameLogic()` (57). From then on `GameLogic` is the application object
(section 5).

**The state machine.** `GameLogic.changeActiveSection(GameState s)`
(GameLogic.java:880-907): `actualState.exit(s)` (884), `gc.flush()` (894),
`actualState = s` (896), `s.enter(old)` (900). `GameState` is an empty interface
(GameState.java:10), so `enter`/`exit` resolve by name at run time. Fifteen classes
implement it (CLSS interface lists): MainMenu, Transit, Garage, CarLot, CarMarket,
Catalog, CarInfo, ClubInfo, PaintBooth, Painter, EventList, RaceSetup, RocInfo, Track
(abstract; City, Valocity, TestTrack, ROCTrack) and MapDebug; the sources hold 63 call
sites of `changeActiveSection(`.

**The frame.** The exe is single-threaded around a cooperative VM. One iteration of the
Build 940 `Engine_MainLoop` (0x47D760, body 0x47D860-0x47DD12; static disassembly,
callees named from their bodies and the old-exe call list in
`System_natives.cpp:679-688`), beside what the host's PE boot loop does
(`game_boot_part1.inc:744-769`):

| Step (940 VA) | Runs | What it does, which script hooks fire | Host at HEAD |
| --- | --- | --- | --- |
| AsyncLoad HasWork / PumpOne / FinishSlice (0x4563D0 / 0x4562C0 / 0x4563C0) | every iteration | file-worker hand-off; at most `res_lock + 1` pumps while the load-work scale is positive, unbounded otherwise | not called: `asyncload_pump_one` and the file-worker pump are reached only from the legacy mirror (`System_natives.cpp:730-780`) |
| `PollWindowQuit` (0x56A1C0) | every iteration | WM_QUIT sets the exit flag | `render_d3d9_pump(0)`, second in the loop |
| `Input_tick(dt)` (0x566160) | when >= 16 ms since its last run | poll DirectInput, smooth the 76 logical axes of every Controller blob; feeds `Controller.user_GetAxisVal`, `Input.checkHotkeys`, `Input.lastKey` | four times per iteration: `render_d3d9_flush()` ends with a pump (`render_d3d9_part1.inc:2142`), every pump starts with `input_live_poll()` (`:2173`), which calls `input_tick` (`input_win32.cpp:1230`); then the loop's own pump, its own poll and the explicit `input_tick(dt)` |
| `Engine_SimulateFrame` (0x47E050), `Engine_TickTimers` (0x47B700) | same gate; dt = time since that run | dt x timeWarp clamped to 0.1 s; GII_CONTROL list -> `control(float)` on a new VM thread `THRD-RUNVMI <res>.control`; physics substeps; due timers -> `handleEvent(GameRef,int,int)` | every iteration; measures its own dt with `GetTickCount` (`System_part2.inc:1456-1466`) |
| `Jvm_PumpFrame(10.0)` (0x55CF20) | when >= 8 ms since its last run | GC generation and slice, then `Jvm_RunThreadsBudgeted`: each runnable green thread gets a priority-scaled slice of 10 ms; every parked script thread resumes here | `jvm_run_threads_budgeted(10)` directly, skipping `jvm_pump_frame` (`jvm_vmthread_part2.inc:2132-2143`), after the resource pumps |
| `Engine_DispatchAnimateEvents(min(0.1, frame dt))` (0x47BA10) | every iteration | `animate()` on every GII_ANIMATE registrant: `Osd.animate()`, the one reader of `Input.lastKey` | every iteration, last |
| `Engine_DrainEventQueues` (0x47D070) | every iteration | deliver queued events to the watchers' `handleEvent(...)` | only inside `engine_simulate_frame` (`System_part2.inc:1518-1525`) |
| Sfx listener pose (0x568BB0), `Sfx_UpdateVoices` (0x5690A0) | every iteration | 64-voice table scheduling | not called |
| `PumpLoadQueue` (0x517F60), `PumpUnloadQueue` (0x518F80) | every iteration | complete loads until the per-pump count reaches the load rate; update the 32-slot ring behind `System.isLoading()` | before the VM pump; rate stuck at 16: the Config value goes to `inv::g_engine_resource_loadrate` (`System_natives.cpp:396`) while the pump reads a file-local `= 16` (`Resources_part1.inc:1597`) |
| notifyAll on `java.render.Frontend.render` (0x54D3A0) | every iteration | wake every `Frontend.render.wait()` (`LoadingScreen.run`) | `Object.notify`, one waiter (`GameRef_part2.inc:1347-1359`, `IO_part2.inc:993-1012`), after the present |
| `PresentFrame` (0x47B5B0 -> 0x421920); AltPresent (0x44EE30) when the gate 0x44E980 is set (a video plays `[inferred]`) | when >= `fps_lock` ms since the last present | walk bound viewports, render each camera's tree, present | `render_d3d9_flush()`, first in the loop, every iteration |

`fps_lock` and `res_lock` are read once before the loop (`GetConfigData`; Config.class
defaults 1 and 25; both strings exist only in the 940 exe). The load rate is a global
with static value 16 (VA 0x5DB02C) that the config apply overwrites with
`Config.resource_loadrate`, default 128 (0x47C402-0x47C411). Against the old exe (host
comment: Input_tick, SimulateFrame, Jvm_PumpFrame, animate, drain, Sfx, pumps, present,
notifyAll, AsyncLoad, optional Sleep, PollWindowQuit, EndFrame) Build 940 moved
AsyncLoad and PollWindowQuit to the top, notifies before it presents, has no EndFrame
and no Sleep, and rate-gates input and simulation, the VM and the present. The host runs
every stage on every iteration, in the order present, Win32 pump, input, simulate,
resource pumps, VM, animate. `Engine_SimulateFrame` is ported with the dt clamp and
substep rule (`System_part2.inc:1452-1527`); the max substep 0.05 s is a guess
(`:1345`), the stock `*(physWorld+8)` is `[unknown]`.

## 4. The script layer

### 4.1 The dialect

A census over all 3,454 sources with comments stripped: 3,454 declare `package`, 3,438
use `extends`, 19 `implements`, 1 declares an `interface` (GameState.java:10; `Runnable`
and `Callback` exist only as system/scripts/lang classes). Absent entirely: generics,
try/catch/throw, static blocks, synchronized, labels, enums, anonymous classes, array
initialiser lists, multi-dimensional arrays, and the keywords `boolean`, `char`, `byte`.
Booleans are ints; objects and strings are used as conditions (1,392 `if(identifier)` in
264 files; MainMenu.java:648 `if(message && message.text && message.text != "")`).
Resource ids are a literal of their own, `pack:0xHEXr` (9,953 uses in 677 sources; 774
in sl/Scripts/game/src), assignable to `ResourceRef` constructors and to `int` fields
(GameLogic.java:31 `new ResourceRef(cars:0x1000r)`, :62
`int SFX_ENTERGARAGE = frontend:0x007Dr`). `switch`, `instanceof`, casts, loops, 1-D
`new T[n]` arrays (1,814 uses) and `native` declarations (114 in 9 files) are in use. 23
sources declare more than one top-level class and the compiler writes one TUFA blob per
class into the single `.class` (MainMenu.class holds 9 blobs, Dialog.class 11,
City.class 8).

### 4.2 Sources and the on-demand compiler

Sources live at `X/src/Name.java` and compile to `X/Name.class`. The exe contains the
compiler: a flex scanner and yacc parser (`yacc stack overflow`,
`fatal flex scanner internal error`), `CompileAll: begin file scan...`, and the path
fragments `\src\*.java` and `\*.class` (exe strings). `System.compileAll(String)I` is a
static native (System.class; `GameLogic.java:404-408` calls it in debug mode). The exe
compiles to disk and then loads from disk: `JavaMachine_loadClass` (940 VA 0x55BEC0)
calls `JVM_compileSource(dir, name)` (0x5598F0) on every class load, ignores the result
and reads `dir\Name.class`. The class is stale when it is missing, when
`dir\src\Name.java` exists and the two packed DOS write times differ (inequality, not
"source newer"; 4 s resolution), or when the header fails the build test of section 2. A
stale class is deleted, the source is parsed, a `.class` still missing is reported as
`JVM::compileSource: Syntax error in "%s" at line %d`, and the new class gets the
source's time; `recompilation needed of "<src>" but source not exists` is the message
for a stale class with no source, which is deleted all the same (disassembly
0x5598F0-0x559CD0). So running the stock exe, a `compileAll` stopgap included, rewrites
classes in the install: 136 class files already carry an exact even-second mtime and no
source does (the DOS stamp `[inferred]`), and at least 415 of the 2,819 pairs still
differ by 4 s or more. The host has no compiler and no stale check: `Jvm::compile_all`
only loads existing classes (`jvm_load.cpp:312-325`) and a missing one logs
`[jvm] class file missing <path>` (`jvm_load.cpp:238,247,263`).

Classes are found through a fixed classpath of nine package directories:
`system\scripts\{lang,net,io,util,render,sound}`, `sl\scripts\game`, `parts\scripts`,
`parts\accessories\scripts` (exe strings). The host's `kClasspathMap` has eight
(`jvm_bridge.cpp:10-19`; `java.net` is missing). The two under parts/ hold the Part base
hierarchy (`java.game.parts.*`, 87 classes) and 13 accessory classes; concrete part and
car classes sit in the other 15 `scripts` directories under parts/ and 51 under cars/
(`find -iname scripts`), which the game reaches by path from pack payloads with a
`script <path>` line `[inferred]` from the `.rdb` payloads and
`GameRef_part1.inc:1149-1208`; the PE payload parser is not decompiled.

### 4.3 The class format (TUFA)

A `.class` is one or more concatenated blobs: a 12-byte header (`TUFA`, u32 4, u32 build
number), then chunks of 4-byte tag + u32 size (`tufa.cpp:285-325`). The layouts hold for
all 2,993 blobs of the install (strict walk with exact chunk consumption); the last
column is where the host decoder departs from them:

| Chunk | Content | Host decoder |
| --- | --- | --- |
| CONS | u32 count, then entries of u32 kind + payload: 0 utf8 `{u32 len, bytes, NUL}`; 3 RID `{pack file-name utf8, local id}`; 4 class `{utf8}`; 5 member ref `{class entry, nat entry}` for methods and fields alike; 7 nat `{name utf8, descriptor utf8}`. No other kind occurs (99,620 / 7,242 / 10,920 / 43,264 / 42,543 entries); ints and floats are TREE immediates | heuristic (`tufa.cpp:103-244`), wrong when a class ref is the last pool entry: `sl/Scripts/game/GameState.class` gets a name of four NUL bytes, so `java.game.GameState` cannot be found by name, and `system/scripts/lang/Class.class` a garbage superclass |
| FILD | two counted vectors of 16-byte records `{flags, name, type, initialiser tree or 0xFFFFFFFF}`, statics then instance | as laid out (`tufa.cpp:575-620`) |
| MTHD | two counted vectors of 20-byte rows `{flags, name, descriptor, tree, w4}`, statics then instance; w4 is 0 on every native row, its meaning is `[unknown]` | rows read from offset 8 instead of 4 (`tufa.cpp:485`): right for instance rows, 4 bytes late for static rows, whose flags are then taken from w4; natives detected by an empty tree (`:536`) |
| CLSS | `{0xFFFFFFFF, 1 for an interface else 0, name, superclass, interface count}` then the interface entries, all class (kind 4) pool entries; superclass 0xFFFFFFFF for `java.lang.Object` and the three interfaces; 20, 24 or 28 bytes (2,962 / 28 / 3 blobs) | name and superclass read (`tufa.cpp:398-400`), interface list dropped |
| TREE | per tree a list of 3- or 7-byte nodes `{op:u8, slot:u16[, imm:u32]}`; a 48-entry flags table (`engine/include/tree_op_flags.inc`, byte-identical in the 940 exe at 0x5B5260 per docs/VM.md) decides the size; at load the VM materialises 8-byte records and the PC strides 8 | as laid out |

The flags word is one access mask in FILD and MTHD: 0x1 public, 0x4 private, 0x8 static
(on all 306 static method rows and all 2,041 static field rows, on no instance row),
0x10 abstract, 0x20 final, 0x40 native (0x2 protected `[inferred]`). 418 method rows
carry 0x40 (93 static, 325 instance), all with an empty tree; one field does too
(`static native int installSet`, Chassis.java:53) and must be backed in C++. The host's
empty-tree test also catches 135 non-native rows: 127 empty public instance methods
(`Object.finalize`, `Part.install() {}`), 2 with flags 0, 5 abstract rows and the static
`Steam.finalize`; testing 0x40 after fixing the row offset is exact. Methods and field
initialisers are trees; `tools/tufa_dump.py` prints all of it (`--method`, `--tree N`,
`--no-cons`; it mirrors the host's static-row misread). Every one of the 2,921 class
files parses without error, and the host never checks the build number, so it loads the
159 stale classes as shipped.

### 4.4 Execution model

The interpreter is a PC-based port of the exe's `VMThread_run` (940 VA 0x562D70,
docs/VM.md table) in `jvm_vmthread_part1.inc` and `part2.inc` (4,483 lines). It is
opt-in: by default `vmthread_try_stream_eval` refuses any body with a call op
(0x21-0x24, 0x4025, 0x4026) unless `SLRR_PE_STREAM_INVOKE=1`, refuses `<init>`/`init`
unless `SLRR_PE_STREAM_INIT=1`, runs field initialisers only with
`SLRR_PE_STREAM_STATIC_INIT=1`, and after a bail re-runs the body in the legacy
pattern-matching `tree_eval` unless `SLRR_PE_STREAM_STRICT=1`
(`jvm_vmthread_part2.inc:1592-1631, 1691-1700`). What follows is what runs under the
standard 940 switch set of docs/TASKS.md. Ops at or below 0x48 go through a 73-entry
case map (`vm_case_map.inc:10`); Build 940 bounds at 0x4D with a 78-entry map at
0x564684 whose first 73 bytes are identical. The high opcodes are locals
(0x1001-0x1003), literal 0x1007 with 47 sub-tags, statement 0x1008 with 47 sub-tags over
12 bodies in the old exe and the host (`vm_case_map.inc:21-33`) and 11 in Build 940 (tag
map 0x564814: tags 0x14-0x16 share the pop-one-operand body of tag 0x1B, which the host
has for 0x1B only, `jvm_vmthread_part2.inc:599-603, 632-633`), field path 0x1011, jumps
0x1014-0x1017, constant-pool field ref 0x101B, arrays 0x101E/0x101F, and utf8-named
calls 0x4025/0x4026 (`jvm_vmthread_part2.inc:1256-1500`). Frames: a call saves the
caller PC, allocates a 64-byte CallFrame, puts `this` in local 0 for instance methods
only and pops the arguments top-first, so the locals are `[this,] arg[n-1] .. arg[0]`
(`jvm_vmthread_part2.inc:1527-1563`); nested script calls switch frames on the same
thread instead of recursing, natives run inline as leaf frames. Method resolution is by
name with a scored overload search up the superclass chain (`Class_findMethodSlot` 940
VA 0x54A150, `compatScore` 0x5607D0; host `jvm_vmthread_part1.inc:488-825`); a tie logs
`Class::getMethod: more matching methods found` (exe string). Interfaces are not
modelled by the host loader, so interface-typed parameters get a weak score
(`jvm_vmthread_part1.inc:598-617`). Strings: the exe interns every String payload, so
`==` on strings is content equality (docs/VM.md "String equality"); the host compares
text when both operands are registered strings (`jvm_vmthread_part1.inc:1540`). Arrays
are 1-D only (exe string `Thread::run: multi-dimensional arrays not supported!`),
elements are assignable references, and primitive `int[]`/`float[]` live in a typed side
table (`jvm_vmthread_part1.inc:1369-1452`).

### 4.5 Threads and the pump

Every script entry point runs on a green thread named by an exe string:
`THRD-RUNVMI <res>.<method>` (named calls from the engine), `THRD-CREATE` (GameType
construction, the `GameInit` instance included `[inferred]`), `THRD-CLASSVAR-INI` and
`THRD-INSTVAR-INI` (field initialisers), `THRD-FIN` (finalisers). A VMThread is 56 bytes
with flag bits 0x1 sync, 0x2 armed, 0x4 RUNNING, 0x8 SLEEP, 0x10 WAITING, 0x20
SUSPENDED, 0x40 DONE, 0x80 STOP (`jvm.hpp:258-265`). In the exe `Thread.start` never
creates an OS thread; `Thread.sleep` sets a deadline, `Object.wait` parks with 0x10,
`notify` pops one waiter LIFO. The host does the same only with `SLRR_PE_BOOT_INIT=1` or
`SLRR_PE_GREEN=1` (`IO_part2.inc:658-666`); otherwise `Thread.start` detaches a
`std::thread` (`:749`), `Thread.sleep` calls Win32 `Sleep` (`:890`) and `Object.wait`
blocks on a condition variable (`:985`). `Jvm_RunThreadsBudgeted` (old exe): slice 10
ms, skip flags & 0x82, run when !(flags & 0x44), scale = prio*9.9+1 for prio >= 0
(priority 10 runs 100x), DONE and not sync -> STOP, STOP and not RUNNING -> destroy
(`jvm_vmthread_part2.inc:2009-2121`). The host adds a 50 ms wall cap
(`SLRR_PE_PUMP_CAP_MS`) and a re-entry guard. The 940 scaling constants and whether the
exe has a wall cap are `[unknown]`: a host comment names `FUN_0055caf0` as the 940 pump
(`jvm_vmthread_part2.inc:2035-2038`), which is not in the confirmed table; decompiling
it settles both. GC slice and mark-sweep are empty stubs in the host; nothing on the
menu or garage path needs them.

### 4.6 Natives: how a call crosses into C++

A method is native when bit 0x40 is set in its MTHD flags. The exe finds the
implementation in a 128-bucket hash keyed by `73 * sum(method bytes) + sum(class bytes)`
and matches class, method and interned descriptor (`jvm_register.cpp:29-60`). The host's
`resolve_native` relaxes the last step: with no descriptor match it returns the first
entry of that class and name (`jvm_register.cpp:237-257`), so a missing overload is not
reported, it runs another overload's C++ function. The host table `kNativeTable`
(`engine/Core/natives_stubs.cpp`) has 377 entries over 37 classes: upstream's 376 plus
`Object.finalize`. Its header says auto-generated, but `tools/gen_native_stubs.py` reads
and writes a `native/` tree that is not in this repo, so it is hand-maintained: a new
native needs a row there and a prototype in `engine/include/natives_table.inc`. The
receiver is passed as the object pointer; only integer-typed parameters receive the
`Native.ptr` handle, and only when the argument's class inherits `java.lang.Native`
(`jvm_vmthread_part1.inc:121-204`); a generic 16-word cdecl thunk carries the call
(`callinfo.cpp:641-680`). The 940 classes declare 394 distinct natives (418 rows, 41
classes; strict walk) and the exe makes 422 registration calls over 41 classes (static
scan of calls to 0x55C690). Against the host table:

| Group | Count | Which |
| --- | ---: | --- |
| Declared native, no host entry | 53 | 16 Chassis (section 9), Steam 5, Replay 2, Debug 3, NetworkEngine 1, 6 Sound track natives, 7 GroundRef, 5 Math, 3 Object, `String.getASCII`, `System.memUsage`, `Text.setScale`, `GfxEngine.renderOK`, `Vehicle.fileVersion` |
| Bound by name only: another overload's function runs | 6 | `GameRef.getPos(Vector3)`, `getOri(Ypr)`, `getVel(Vector3)`, `MouseCursor.getPos(Vector3)`, `getPickedPos(Vector3)` (the host rows take no parameter, so the caller's vector is never filled; City.java:725, Painter.java:551) and `GfxEngine.openVideo(String,int,int,int)` (the host row has three ints; MainMenu.java:158). The exe also registers `Text.getPos(Vector3)`, which no class declares |
| Wrong kind | 1 | `Painter.doPaint` is `native static` in 940 (Painter.java:349) and non-static in the table |
| Host entries that no 940 class declares native | 21 | the exe still registers 14 of them (`Object.hashCode`, `Text.getDefaultColor/setDefaultColor`, `Part.getLogo/getSlotIndex/isSlotDisabled/setSfxLoopParams`, `WheelRef.getBrake/getHBrake/setOppWheel`, `Sound.has3DHardware/hasMixHardware`, `GameRef.getDetail`, `GroundRef.removePedestrianType`): dead rows, not wrong ones |
| ... of those, methods the 940 classes implement in script | 4 | `Part.getLogo` (`return manufacturer;`, Part.java:730) and `RenderRef.getPos()` (67 nodes) have bodies, `Part.setSfxLoopParams` and `Object.finalize` are empty; `vmthread_invoke_method` consults the native table first (`jvm_vmthread_part1.inc:390-391`), so the C++ leaf runs instead of the script: audit or remove these rows |

The `Object.finalize` row is a workaround for the empty-tree native test of section 4.3;
the fix is the 0x40 test. Two misdiagnoses to retire: `String.getParams` "native
missing" (docs/TASKS.md:62 and :188, docs/VM.md:397-398) and `Vehicle.fileVersion` as a
940 native to add (docs/VM.md:61). `String.getParams(Vector)` is a static script method
(String.class, flags 0x09, 57 nodes; no `getParams` string in the exe), so the
`null.trim()` in `GameLogic.updateCodeROC` (GameLogic.java:855) is a failure inside a
static script call; why the host gets null is `[unknown]`: trace it with
`SLRR_PE_STREAM_STEPS=String.getParams`. `Vehicle.fileVersion` is declared native
(Vehicle.java:143), but the exe has no such string and no shipped source calls it, so
the missing binding is harmless.

### 4.7 Events, timers, callbacks, errors

`GameType` (system/scripts/lang, compiled only) is the base of every engine-bound script
object. `registerCallback(mode)`: 8 GII_CONTROL -> a sim-callback node ticked by
SimulateFrame -> `control(float)`; 9 GII_DRIVE -> a second list; 28 GII_ANIMATE -> a
third list -> `animate()` once per frame (`GameType.cpp:129-132, 743-820`; values from
GameType.class static initialisers). `addTimer(deadline, id)` makes a one-shot node
fired by `Engine_TickTimers` into `handleEvent(GameRef, EVENT_TIME, id)` when the event
mask allows it (`GameType.cpp:516-650`). `addNotification` installs a Watch on another
GameRef with an event type mask, an alias remap and an optional custom message or method
(the two natives at `GameType.cpp:423-514`; applied in `fire_watch_notification`,
`:251-296`). Event constants (GameRef.class statics): EVENT_TIME 0x80, EVENT_CURSOR
0x10000, EVENT_HOTKEY 0x100000, EVENT_ANY 0x0FFFFFFF. A script error in the exe logs and
the thread continues with null (`Thread::run: [illegal methodcall] null.%s() type:%s`,
`Thread::callMethod: "%s%s%s" not found`, `Thread::callmethod: native implementation
missing for "%s.%s"`, exe strings). The host does the same in place for a null receiver
and for an unresolved method: it drops the arguments, pushes null and continues in the
same frame (`jvm_vmthread_part2.inc:1403-1422, 1430-1444`); other failures inside a
nested frame unwind it and hand null to the caller, skipping the rest of the failing
method (`:857-880`), and a failure in the outermost frame ends the stream. Host status:
callback lists for CONTROL and ANIMATE are real, DRIVE is a bitmask that nothing
dispatches, timers and watches are per-GameType vectors on the host clock rather than
engine lists on the physics clock.

## 5. The game as written in the scripts

**Frontend** (`java.render.Frontend`, compiled) is a static holder: `render` (the
GfxEngine object), two LoadingScreens, the hotkey watcher thread, `inputQueue` (the Osd
focus stack), the Steam object and eleven font `ResourceRef`s assigned in `setFonts()`,
where only `largeFont`, `mediumFont` and `smallFont` are chosen by resolution
(`Config.video_y`/`video_x` against 1024/768 down to 320/240) and the other eight always
get the same rid (Frontend.class tree 19). `Frontend.class` bundles a second class,
`java.render.HotkeyWatcher`, whose `run()` sleeps 50 ms and calls
`Input.checkHotkeys(Input.cursor.controller, inputQueue.lastElement())`.

**GameLogic** (GameLogic.java, 1,366 lines) is a static singleton. Its constructor scans
packs (`System.rpkScan` of `parts\engines\`, `parts\`, `cars\racers\`,
`cars\fake_racers\`, `maps\`, `sl\Scripts\game\CareerEvents\` and every
`multibot\maps\<dir>\`, lines 345-358), builds the 25 vehicle types from the children of
`VEHICLETYPE_ROOT = cars:0x1000` (line 31, 595), creates the Garage, one CareerEvent per
child of `EVENT_ROOT = system:0x2000` (59 pack entries) and a freeride template per
child of `MAP_ROOT = system:0x3000`, starts the "Game time refresher" thread, arms a 10
s timer (each tick adds `timeRefreshRate * timeFactor` = 40 game seconds, line 528; days
roll at 86,400) and ends with `changeActiveSection(new MainMenu())` (line 442). Career
constants: 60 racers (3 clubs of 20), $25,000 start (line 37), prestige 0.3, ROC entry
fee 100,000, 22 car colours, 5 cheat words checked by the native
`GameLogic.kismajomCheck(String[])` (line 521; exe string `kismajomCheck`).

**Main menu.** `MainMenu.enter` shows a `Gates` dialog (MainMenu.java:30,
`Gates extends Dialog`): a full-screen Osd with the background video
(`GfxEngine.openVideo(videoData, 1, 1, 1)`, line 158, "supports fit to screen since
build 940"), a `SlidingMenu`, logo rectangles animated on a THOR method-queue thread,
and one hotkey `Input.AXIS_SELECT` (line 181) bound to itself.
`Gates.osdCommand(CMD_NEW_CAREER = 0xA006)` runs a modal
`StringRequesterDialog("Enter player name")` (line 418), then `loadDefaults()`,
`updateCodeROC()`, `autoSaveQuiet()` to `save/career/<name>-<club+1>/` and
`changeActiveSection(GameLogic.garage)`. `buildSlidingMenu()` has 13 `slider.addItem`
calls (lines 272-290), 9 of them unconditional: a cold boot shows LOAD CAR, LOAD CAREER,
DELETE CAREER, NEW CAREER, FREERIDE, QUICK RACE, EXTRAS, OPTIONS and EXIT GAME, and
`slider.activate(3)` lands on NEW CAREER (SAVE CAR, SAVE CAREER and BACK TO GARAGE
depend on game state, MULTIPLAYER on `GameLogic.DEBUG_MODE`, which is 0).

**OSD classes.** The UI is script over five native classes. `Osd` (java.render, 84
methods, 67 KB class) owns one `Viewport` and one `Camera`; `Osd.show()` creates `new
Camera(this, vp, 2, CAM_AOV = 60, 0.1, 10.0)` at `z = CAM_OFFSET (5.48) * vpHeight` and
activates the viewport; `Osd.hide()` deactivates it and destroys the camera (Osd.class
trees 89-92). The `Camera` constructor halves the angle before the native `create`
(Camera.class tree 10), so the native receives 30, the full vertical field of view
(section 7). Groups are `Dummy` GameRefs: `Group.activate()` clears `WORLDTREELEAF`
(0x40), `deactivate()` sets it (Group.class trees 5-6). A `Rectangle` is a render
instance of a `RectangleTemplate`, which duplicates the frontend "etalon" type
(`frontend/meshes/etalon_negyzet_alpha.SCX`, 328 bytes, a 100 x 100 quad), bakes the
size with `scaleMesh(w, h, 1)` and swaps the texture. A `Text` is
`Text.create(parent, charset, x, y)`: an `r_text` render instance whose two floats
become a screen anchor, not 3D geometry (section 7). `Osd.darken()` is a `Text` of the
string "a" whose charset is the render type `frontend:0x1A25` (mesh `fade.SCX`), so the
dark curtain is whatever the text path draws for character code 97 of that mesh
(Osd.class tree 75). `Gadget`, `Button`, `Menu`, `SlidingMenu`, `Slider`, `StringInput`
and the dialogs compose these: 39 `.class` files under system/scripts/render hold 55
blobs and 46 distinct classes. Nine dialog classes are compiled twice with different
bytes, inside `Dialog.class` (11 blobs) and as their own file (StringRequesterDialog:
3,985 bytes and 4 methods against 2,385 and 3). The host keeps whichever definition
loads first (`jvm_load.cpp:151-160`), so dump the blob inside `Dialog.class` when
debugging a dialog; which copy the exe uses is `[unknown]`.

**Dialogs and input flow.** `Dialog.display()` shows, waits on the dialog object and
returns `Dialog.result`. A hotkey press reaches the script as `Input.checkHotkeys ->
EVENT_HOTKEY -> Osd.handleEvent(Hotkey)` on a new VM thread, which either runs a private
menu command or `hk.handler.osdCommand(hk.command)` (Osd.class tree 87). Typed text
reaches `StringInput.key` through `Osd.animate()` reading `Input.lastKey` (tree 86, node
294). The host reproduces this chain end to end: ENTER, ENTER, a name, ENTER puts
`GameLogic.actualState` at `java.game.Garage` in 40 s (docs/VM.md session 4). One limit:
the `autoSaveQuiet()` on that path does not write a stock-compatible `main`. The three
host-written profiles carry `03 00 00 00 01 00 00 00 00` 157 times each (a stock file at
most 8), which is `saveGame.write(new GameRef(id))` of a VehicleDescriptor
(VehicleDescriptor.java:31) coming out as an empty string `[inferred]` from the source
line and the bytes; their trailer lists no packs and their ROC code starts with `null`.
Such runs write into the real install's `save/career`, beside `steam_autocloud.vdf`:
test against a copy of `save/`.

**Garage** (Garage.java, 2,214 lines). `enter()` loads one of four garage GroundRefs by
club (line 217), creates the vehicle lift (`misc.garage_objects:0x13` plus a PhysicsRef
box, line 221), the Osd menu (18 commands in career mode, lines 536-562: HITTHESTREET,
TESTTRACK, EVENTLIST, ROC, CARLOT, BUYCARS, BUYCARSUSED, TRADEIN, CATALOG, CLUBINFO,
CARINFO, MECHANIC, TUNE, LIFT, PAINT, TEST, TIME, MAINMENU, `CMD_*` values 101-133; the
ROC branch has 9), the MOVE PARTS slider group, a `Mechanic` for drag-and-drop part
installation (line 266), and the 3D camera: `camera = new GameRef(map,
GameRef.RID_CAMERA, "<pos>,<ori>, 0x13, 1.0,1.0, 0.05", ...)` (line 248) driven by
`cam.command("render <vp id> 0 0 1 <flags>")` (line 499). Note that the garage camera is
a `camera` GameType (a `native camera` entry, system.rdb typeid 0x33), not a
`java.render.Camera`.

**Driving.** `Track` (2,955 lines, abstract Scene) owns cameras, triggers and the
in-game menu; `City` (3,870 lines) adds night races, police and traffic; `Valocity`
loads `maps.city:1` with a Navigator minimap and traffic via `map.addTraffic`. Career
races go `EventList -> new Track(careerGM, event, trackData)` with one of 8 `Gamemode`
classes driving the race (Gamemodes/stdpack_231.rdb, 8 entries) over 21 `track_data`
scripts (find `multibot/maps/*/data/src/track_data.java`).

**How a car is built.** `new Vehicle(parent, rid, colorIndex, optical, power, wear,
tear)` (Vehicle.java:106): `chassis = create(parent, new GameRef(rid), "0,0,0,0,0,0",
"Vehicle")` (111) instantiates the car's chassis script (e.g. `Einvagen_110_GT extends
Einvagen_models extends Chassis extends BodyPart extends Part extends GameType`), then
`chassis.addStockParts(new Descriptor(colour, optical, power, wear, tear))` (117) and
`chassis.forceUpdate()` (119); the constructor at lines 82-104 takes an existing chassis
instead. `Chassis.addStockParts` picks stock, stage-1 or stage-2 `int[]` rid lists
against thresholds 1.333 and 1.667: the engine list and the eight running-gear lists by
`desc.power`, the nine body lists by `desc.optical` (Chassis.java:129-130, 172-173,
211-212, 802-865). It does not add every listed part: `(1 - tear) * 10` randomly chosen
lists go through a crash loop (786, 881-917) and each part of the others is added only
`if (random() <= desc.optical)` (938), so a car with `optical < 1` lacks parts by
design. Each part goes through `Part.addPart`: `GameRef.create(...)`, `setWear/setTear`,
the native command `"install 0 <car> 0 <parent> 0"` (Part.java:847), recursion.
`forceUpdate` hands the finished tree to the native physics (section 9). The 635
classless sources are not on a stock car's path: 418 are under cars/racers (273 light
units, 48 front doors, 40 `Set` kits, 57 other panels), the rest wheels, running gear
and engines, and the `stock_parts_list_*` rids of all 70 chassis variants, followed
through every sub-part class, reach no entry whose class file is absent; the stage lists
add such entries for four chassis only `[inferred]` from a static walk of the rid
literals. The `GameRef.setWear/setTear/setTexture/addStockParts not found` lines of a
940 run (TASKS.md "In progress 2") come from `stock_Battery_silver`, whose class exists:
the host reads its dotted `script` payload as a file path (section 6).

**Career and save files.** `GameLogic.save(dir)` writes `dir/main` (SDAT, 0x87654321
version 23: player, 59 bots, dealer stocks, time, day), `careerData/data`,
`eventData/data_v_N`, `trackData/data_v_*` and `PlayerCarN` (0x88664422 version 5: name
then the recursive Part tree, Vehicle.java:17-18) staged through `save/temp/`
(GameLogic.java:942-1225). A profile's `save/career/<name>-1/main` starts
`SDAT 00 01 02 00`, `21 43 65 87`, `17 00 00 00` (xxd). Every SDAT type-2 file ends with
a trailer `{u32 unsaved count, {ordinal, restype, size}..., u32 pack count, {ordinal,
path length, path}..., u32 trailer length}`, and a resource id in the body is
`(file-local pack ordinal << 16) | local id`, not an engine pack slot: across the saved
cars the id 0x10006 resolves through ordinal 1 to 16 different car packs. All 31 `main`
and all 69 `PlayerCarN` files parse this way; stock `main` files list 16 to 36 packs.
`PlayerCarN.<k>` are numbered side files, one per unsaved (runtime) resource of the car:
426 on this install, k = 1..25, 377 JPEG (restype 7, a painted texture) and 49 `INVO`
version 4 (restype 5, a mesh). Each car file's trailer lists them by ordinal and restype
(all 69 match); `Part.save` writes the texture and the mesh through
`File.write(ResourceRef)` (Part.java:209-214) and the list is flushed at `File.close`
(host comments `IO_part1.inc:425-427, 934-946`). The routine that emits the side file
itself is `[unknown]` (start from the old-exe `File_SdatType2_WriteUnsavedList`,
0x484FA0).

## 6. Resources and data formats

**Packs and rids.** An RPK starts `RPAK`, u32 version (0x200 in all 146 game packs), two
u32 counts whose sum is the number of 0x40-byte dependency records that follow
(`frontend.rpk`: `system.rpk`, `misc\garage.rpk`, `cars.rpk`), then
`{u32 TOC byte size, u32 entry count, 2 x u32}` and the TOC records `{+0 parent id u32,
+4 local id u32, +8 restype u8, +9 flags u8, +10 isparentcompatible f32, +14 file offset
u32, +18 size u32, +22 name length u8, +23 name}`, with 12 floats of transform appended
when flags bit 0 is set. The +10 float is the `.rdb` field `isparentcompatible` (1.0 in
22,089 of the 22,163 entries, 0.0 in 61, 2.0 in 13), which `rpak.cpp` treats as padding
(record walk `rpak.cpp:155-214`, header and dependencies `:271-358`; the `.rdb` files
show the same records as `typeof / superid / typeid / alias` blocks with the payload
text). A full resource id is `(pack slot << 16) | local id`; `System.openLib(path)`
returns the slot (`System_natives.cpp:441-498`; Catalog.java:306 stores it and :391-394
compare `parts.id() >> 16` with it). A cross-pack parent is written with the dependency
slot in the high word (`0x00010008`). Script rid literals compile to the CONS RID entry
and are resolved at run time through `ResourceEngine_RemapFromPath`
(`jvm_vmthread_part2.inc:538-556`), so a pack's slot can differ per run. Saved games do
not depend on slots either: every SDAT file carries its own pack table (section 5),
`File.open` reopens each listed pack by path and `readResID` maps the ordinal back to
the current slot (`IO_part1.inc:351-463, 934-999`); what remains open is the host's
handling of unsaved (0xFFFF) resources, whose trailer rows it writes with size 0. Build
940 opens 126 packs at boot; the host's slot table was raised from 64 to 1,024 after rid
literals past pack 64 resolved to zero (TASKS.md f138803).

**The resource tree and types.** `typeof` is the `RESTYPE` enum of `ResourceRef` (static
initialisers in ResourceRef.class): 0 RESTYPE_INVALID, 1 INSTANCE_GAME, 2
INSTANCE_PHYSICS, 3 INSTANCE_RENDER, 4 RESOURCE_FORCEFX, 5 RESOURCE_MESH, 6
RESOURCE_SOUND, 7 RESOURCE_TEXTURE, 8 RESTYPE_GAME, 9 RESTYPE_PHYSICS_BODY, 10
RESTYPE_PHYSICS_CONSTRAINT, 11 RESTYPE_PHYSICS_PARTICLE, 12 RESTYPE_RENDER_CAMERA, 13
RESTYPE_RENDER_LIGHT, 14 RESTYPE_RENDER_OBJECT, 15 INSTANCE_RENDER_SPRITE, 16
INSTANCE_RENDER_TEXT, 17 INSTANCE_RENDER_HORIZON, 18 RESOURCE_VIEWPORT, 19
RESOURCE_ANIMATION, 20 RESTYPE_RENDER_MICROPHONE, 21 RESOURCE_SOUNDMIXER, 22
RESTYPE_MAX. Entry counts over the 146 game packs: 14 x 4,710, 8 x 4,531, 7 x 4,315, 5 x
4,095, 9 x 3,776, 6 x 507, 13 x 162, 1 x 33, 19 x 18, and one to three each of 2, 3, 4,
10, 11, 12, 15, 16, 17, 18 and 20. A typeof-8 entry whose payload has a `script <class>`
line is a scripted GameType, optionally paired with a `native <type> [cfg]` line
(`parts.rdb`: `script java.game.parts.accessories.stock_Battery_silver` /
`native part <cfg>`). The script argument is a `.class` path in 3,320 entries and a
dotted class name in 137 (133 in parts.rpk, 2 in sl.rpk, 2 in system.rpk). Pure native
types are declared the same way: `camera` is `native camera` (system.rdb typeid 0x33),
`map` is `native ground` (0x35), `cursor` is `native cursor` (frontend.rpk); 3,193
entries carry `native part` and 96 `native car`. The host honours only a payload whose
first line begins `script `, joins the argument to the game root as a file path
(`GameRef_part1.inc:1185-1208`) and otherwise guesses `java.game.<alias>`. That explains
the six `[jvm] class file missing` lines of a 940 run: `SplashScreen` (a pre-940 class
the legacy path asks for; Build 940 ships none), `camera`, `cursor` and
`lift_support/cfg` (native types and a two-line payload; no such source exists) and
`stock_Battery_silver` twice (a dotted name read as a path); none is a compiler miss.
Fixed script-visible roots: `WORLDTREEROOT` flag 0x10, `RID_DUMMY = system:0x1B`,
`RID_CAMERA = system:0x33`, `RID_CURSOR = frontend:0x2A` (GameRef.class statics); the
world tree root of the host boot is `system.rpk` local 4, the entry `ins_game`
(`game_boot_part1.inc:44, 79`). The 20 `ResourceRef` natives walk and bind the tree
(`load`, `getFirstChild`, `getNextChild`, `getParent`, `duplicate`, `scaleMesh`,
`makeTexture`, `makeSound`, ...); the host keeps a `ResState` per Java object and loads
synchronously instead of the exe's handle-plus-node and async queue.

**Meshes.** `.scx` files are Invictus `INVO` containers in two versions, and version 3
is the majority: 3,532 of the 6,032 files (cars 2,232, maps 535, parts 463, objects 204)
against 2,500 at version 4 (cars 1,319, objects 661, parts 495, humans 16).

| Layout | Files | Structure | Host (`render_d3d9_part3.inc` at HEAD) |
| --- | ---: | --- | --- |
| Version 4 | 2,500 | `INVO`, u32 4, u32 chunk count, `{u32 type, u32 offset}` per chunk from 0x0C; each chunk starts `{u32 type, u32 size}`. Types: 0 material (4,215 chunks), 1 a 12-byte record whose payload is 0 in all 4,311 instances, 4 vertex block `{count, format flags, vertices}` and 5 index block `{count, u16 indices}` (4,365 each), 2 (three chunks in parts/running_gear), 3 (51 chunks in the 16 humans meshes; bone palettes `[inferred]`). Vertex stride by flags: 12 bytes (0x1, position only, 115 blocks), 24 (0x41, 970), 32 (1,646), 36 (23), 40 (1,518), 44 (7), 48 (82), 56 (4) | reads the chunk table 4 bytes late, so its type numbers are shifted (material 1, vertices 5, indices 0); accepts strides 24-40 and falls back to 32 (`:999-1066`), so 17 files with 44-, 48- or 56-byte blocks are read at the wrong stride, among them the prop meshes of all four garages (`misc/garage/g1/garazs_01_targyak.scx` and its g2, g3, g4 counterparts), `misc/dealer/meshes/marketkulso.scx` and the skinned humans |
| Version 3, header 0x88 | 3,499 | per block: header, u32 vertex count, 64-byte vertices, u32 triangle count, three u32 indices per triangle; blocks repeat and the file ends with a zero u32 | positions, normals, one UV (`:1142-1280`) |
| Version 3, header 0x38 | 28 | the font layout: 1,024 44-byte vertices, glyph c is vertices 4c..4c+3 in the order (x0,y0), (x1,y0), (x0,y1), (x1,y1), stacked at `z = -100 * c`. 15 files in `frontend/meshes/Fonts` (51,272 bytes each), 5 in `scaleable/`, 5 in `cars/meshes` (lcdfont, osdfont, P_osd_gear, slider, symbol_damage); 3 particle smoke meshes share the header | fonts only, by file name from `frontend/meshes/Fonts`, UVs only (`:504-551`) |
| Other version 3 | 5 | `data/Geometry/Maps/waterplane.SCX` (header 0x68) and four 12-byte stubs in maps/ | not parsed |

The exe also knows bones (`bone00`, `wheelbone`, `numbones`, `.bon` sidecar files, 19 in
the install), LOD levels (`lods`, `lod_amp`, `lod_bias`) and 30 material classes (RTTI
for 30 `CMaterial_*` subclasses, from `_Color` and `_Tex` to `_TexBumpSpecular`,
`_Water`, `_Holo`, `_Weld` and `_ShadowMap`), none of which the host parses; the
encoding of the material chunk, of chunk types 2 and 3, of the vertex format flags and
of the bone and LOD data is `[unknown]` (decompile the exe's INVO chunk walk).

Units: the exe's internal unit is the script metre x 10. `FUN_00567570` multiplies every
script position by 10.0 when it builds a bone matrix and the getters multiply by 0.1
(`Text.getPos`, the camera position; constants at VA 0x59373C and 0x5935D4). Mesh SCX
vertices are centimetres, so mesh units x 0.1 are engine units and x 0.1 again script
metres `[inferred]`; whether the exe scales the vertex buffer at upload or in a matrix
is `[unknown]` (decompile the mesh upload). The check that holds: the 100-unit etalon
quad under `scaleMesh(3.92, 2.94)` exactly fills the exe's Osd view (2 x 5.48 x tan 15
deg = 2.94 m high, x 4/3 = 3.92 m wide). The host applies `kScxCmToM = 0.01` in the
camera pass (`render_d3d9_part1.inc:1192` at HEAD) but draws that background at 46% size
(section 7). Font vertices are tenths of a pixel, not centimetres.

**Textures.** A texture entry's payload is `sourcefile <path>` with optional
`flags <n>`, `lod_amp` and `lod_bias` lines (38, 118 and 8 of the 4,086 entries that
have a sourcefile, out of 4,315 typeof-7 entries). Sources: DDS 2,608, PNG 1,203, PTX 83
(a 36-byte header followed by JPEG mip levels, the exe's own format), TGA 77, JPG 70
(the engine default texture `system:0x08` is `Data\TEXTURES\Default.jpg`), BMP 1, and 44
`.ztx` names for which no file exists; 112 further sourcefiles are missing on disk, so a
loader must tolerate dangling references. The exe loads through `d3dx9_32.dll` (import)
plus a bundled libjpeg; the host resolves `d3dx9_43.dll` down to `d3dx9.dll` at run time
for DDS and uses WIC for PNG/JPEG (`render_d3d9_part2.inc:416-444, 1095-1230` at HEAD).
The 866 `.tex` files under `objects/meshes` list texture names for the map-object
toolchain; no `.rdb` references one and the exe has no `.tex` string, so they are not a
runtime load list `[inferred]`.

**Fonts.** A font is a typeof-14 render object in `frontend.rpk` that joins a glyph mesh
and an atlas texture (`mesh 0x4A / flags 4100 / texture 0x26` for slii24; 21 render
objects carry flags 4100, the 19 fonts and the two fade types). All 19 font texture
entries name PNG files; the 10 `.tga` files beside them are referenced by nothing in the
install, yet the host tries `<name>.tga` before `<name>.png`
(`render_d3d9_part3.inc:590-595`), so for five fonts it samples a file the game does not
use. In the glyph mesh the quads are not fixed cells: x and y are the glyph box in
tenths of a pixel (xy x 0.1 = atlas pixels, 1 texel per pixel), x0 can be negative (a
left bearing) and `vertex[4c+1].x x 0.1` is the advance (slii24 'A': x -50..180, y
0..450, that is 23 x 45 px on the 512 x 256 atlas with an 18 px advance; simple20 'A':
110 x 230, that is 11 x 23 px). The exe indexes this vertex buffer directly by byte code
(section 7), so it needs no charset map: `frontend/font.dat` (135 bytes, the codes
0x21..0xA7) is named by no exe string, pack, script or `.cfg`, so the game does not use
it `[inferred]`; only the host reads it, and refuses to load a font without it
(`:569-577`). A charset is not always in the 0x38 layout: `fade.SCX` and `fade_HD.SCX`
(71,964 bytes) are charset meshes in the 0x88 variant (1,056 64-byte vertices, that is
264 slots of four, and 352 triangles), bound as font-flagged render objects
(`fade_render` frontend:0x1A25 with `mesh 0x1A4A / texture 0xA8 / flags 4100`, and
0x1A26) that `Osd.darken` uses as the charset of its curtain text.

**Audio and video.** Sounds are typeof-6 entries with `sourcefile` WAV payloads played
through `SfxRef.nplay`; music is MP3 in six `Music\*` set folders (exe string
`Music\Garage_Shop`); the only videos are the two menu backgrounds, opened by script
(MainMenu.java:155-158). **Configs:** 3,194 `.cfg` files (part, particle, human,
frontend; none at the root); the game options are the SDAT file `save/game/options`, not
a `.cfg`. 173 of them end without a newline: 169 end with the bare token `eof` and four
end in the middle of data (`particles/scripts/skids/frontend/graph.cfg`,
`graph_fade.cfg`, `multibot/scripts/particles/clouds/asphalt.cfg`,
`particles/scripts/particle_clouds/spark.cfg`), the shape behind the stock tokenizer
overrun in BACKGROUND.md section 1; whether the host's `.cfg` reader copes with either
shape is `[unknown]` (a test should cover both). **Saves:** every script-written file
starts `SDAT 00 01 02 00` and ends with the pack-table trailer of section 5; the control
set is `CTRL` version 16 (2,560 bytes, three devices).

**Loading protocol.** `ResourceEngine_PumpLoadQueue` runs once per loop iteration, stops
when its per-pump count reaches the load rate (128 from Config in Build 940, 16 in the
host; section 3) and updates a 32-slot ring of per-pump counts. `System.isLoading` is
`sum * 0.03125 > 4.0`, and `System.isLoadingReset` seeds every slot with the rate, so
with idle pumps the flag stays set while `32 * rate - rate * n > 128`: 23 pumps at rate
16, 30 at rate 128 (`Resources_part2.inc:966-1002` at HEAD, docs/VM.md session 3).
`LoadingScreen.run` loops `Frontend.render.wait()` (an `Object.wait` on the GfxEngine
object, notified once per loop iteration) while `isLoading()`, then `hide()` notifies
the caller. The host ports the ring and the notify; actual loads are synchronous.

## 7. Rendering and the OSD

**In the exe.** Scripts never touch Direct3D. The device is fixed-function D3D9 through
`d3dx9_32.dll` (20 D3DX imports). The exe also names 11 vertex shaders
(`skinmesh1..4_diffenvmap.vsh`, `diff_envmap.vsh`, `bump.vsh`, `bump_spec.vsh`,
`water_tss.vsh` and three `mixta*` variants) and 5 pixel shaders (`clear_below.psh`,
`blur.psh`, `bump.psh`, `bump_spec.psh`, `mixtabumpspec_vcprelit.psh`), loaded from
`system\shaders\%s` through the imported `D3DXAssembleShaderFromFileA`; the install has
no `system\shaders` directory and no `.vsh` or `.psh` file, so the "no shaders" line in
BACKGROUND.md section 1 is right for what ships, and what the exe falls back to for
bump, water, blur and skinned envmapped meshes is `[unknown]` (decompile the callers of
that import). The script-visible surface is `GfxEngine` (13 natives in the 940 class:
present pumps, display modes, `printScreen`, `openVideo` x2, `closeVideo`, `renderOK`,
global envmap), `Viewport` (11: a normalised rect and a priority;
`RENDERFLAG_CLEARDEPTH` 1, `CLEARTARGET` 2), `Camera` (5 declared: `create` with the
vertical field of view, dmin, dmax, LOD bias and amp, `destroy`, `setFog`; `activate`
and `deactivate` are declared native but neither exe registers them, while the host
binds both and its `render_d3d9_camera_activate` forces the camera's viewport to be the
single active one), `Text` (8 declared, 11 registered) and `RenderRef`
(`create(parent, type, alias)` clones a render type under a node, `setMatrix(pos, ori)`
poses its `bone00`, plus colour, light, flare, `changeResource`, lines and routes).
Every instance lives in the resource tree (RTTI `RenderInstance`, `CameraNode`,
`ViewPortNode`; flags `RIF_WORLDTREEROOT` / `RIF_WORLDTREELEAF`, exe strings).

**The UI is geometry under a camera, except its text.** Rectangles are 100 cm quads
scaled in script to metres at `z = pri / 1000`, viewed by the Osd camera at
`z = 5.48 * vpHeight` with a 30 degree vertical field of view, near 0.1, far 10. Texts
are screen-space: only their anchor comes from the tree, and y runs down for text and up
for rectangles (`Osd.convertTextCoordinates` converts between the two). A hidden Osd is
an unbound viewport whose camera was destroyed. Sizes are pixels of the configured mode:
the scripts lay out from `Config.video_x/video_y` (font choice, `Text.getWidth`,
`Osd.screenAspect`), and the stock window takes its size from `save/game/options`
`[inferred]` from the boot order in the host comment (`game_boot_part1.inc:649-660`:
Config is applied before `GfxEngine_InitDisplay`). The host opens a fixed 1024 x 768
window (`:642`), so its frames cannot be compared with stock frames until the window
matches.

**What the host draws today** (committed HEAD f43c741; the UI pass that implements the
exe's model below is in progress in the working tree and is not described here).
`render_d3d9_flush()` (`render_d3d9_part1.inc:2069-2153`) clears, begins the scene,
draws the video quad, runs the legacy auto-framing `draw_meshes()`, then the
per-viewport camera pass `draw_viewport_cameras()` (membership by shared root through
`mesh_root_of`, geometry scaled by 0.01, texts of each camera after its meshes; lines
1342-1488), then flare sprites and the legacy screen-space `draw_osd_rects()` and
`draw_osd_texts()`, ends the scene, presents and notifies `Frontend.render`. Reached
state: the main menu draws all of its elements (video, banner, background, sliding-menu
icons, logos, texts), though not correctly, and in the Garage state 211 meshes, 168
textures, 9 cameras, 29 viewports and 46 texts are alive in the renderer (docs/VM.md
session 4), which is not the same as drawn.

**The exe's model and the known defects at HEAD** (TASKS.md step 1 for the UI rows, step
2 for the last one). The exe column is Build 940 as decompiled in Ghidra; the render
natives are registered in the block `FUN_004d5fb0`.

| Aspect | Build 940 exe | Host at HEAD (`render_d3d9_part1.inc` unless noted) |
| --- | --- | --- |
| Present walk | `PresentFrame` (`FUN_00421920`) walks the bound viewport list in list order. `Viewport.activate` (`FUN_004e6ab0` -> `FUN_004200d0` -> `FUN_00421e40`) moves a viewport from the unbound to the bound list, inserted by descending priority, so the highest priority is drawn first, as background; `deactivate` (`FUN_00421f50`) moves it back; any number are bound at once. Per viewport: D3D viewport from the normalised rect. Per camera hooked on it (`FUN_00460ba0`): matrices, clear with `cam+0x1a4 & vp+0x24` (bit 0 depth, bit 1 target; the Java renderflags of `Viewport.activate(I)` are dropped by the native, where the two masks get their values is `[unknown]`: find their setters), tree walk from the camera's root node, render the groups, texts last | the legacy `draw_meshes()`, `draw_osd_rects()` and `draw_osd_texts()` passes run around the camera pass whatever the viewport binding. One viewport is active at a time (`render_d3d9_viewport_activate` clears the previous one, 2299-2304), so of the Garage's 29 viewports and 9 cameras only the last-activated viewport's cameras are drawn, where the stock game shows every Osd layer at once; a camera with no mesh members is skipped (1428); the clear uses the Java renderflags |
| Camera | `Camera.create` is `FUN_004d8de0`. View = inverse of the camera's bone00 world matrix (`FUN_00461dd0`, inverse `FUN_00567630`); projection = `D3DXMatrixPerspectiveFovRH(fovy = aov * 0.0174533, aspect, dmin, dmax)` (`FUN_00422e80`; constant at VA 0x593A50). Right-handed: the camera looks down -Z with +Y up and +X to the right, and the native angle is the full vertical field of view. For the Osd that is 60 x 0.5 = 30 degrees, a 15 degree half angle; the script constants agree: `CAM_OFFSET * tan(15 deg) = 5.48 * 0.26795 = 1.468`, which is `Osd.SCALE_3D` (1.469) and `SCALE_FS` (1.47), so the 3.92 x 2.94 m background fills the viewport. The camera type factory `FUN_00524dc0` was not decompiled, so that it stores angle and aspect unchanged is `[inferred]` | the native angle is stored as `half_aov` (`RenderNatives.cpp:211`) and used as the half angle (1440-1443): every Osd rectangle is drawn at tan 15 / tan 30 = 46% of its size. The view is the camera position and -Z axis fed to an LH look-at with a fixed world up, under an LH projection (1133-1178, 1448-1458): world +X lands on the left, so every camera-pass instance and its texture is mirrored horizontally, and camera roll is dropped. Geometry is scaled by 0.01 |
| Hidden subtrees | the camera's tree walk (`FUN_0045eb10`) tests `node+0x84 & 0x40` and does not descend into such a node (`FUN_0045f030` marks its cached entries hidden), so the whole subtree of a deactivated Group is skipped. `GameRef.setFlags` (`FUN_004db900`) ORs into node+0x84 (the +0x54 of the host comments is the old exe). Texts additionally honour bit 22 of that word (setter not located). `Osd.hide` is `vp.deactivate(); cam.destroy()` and nothing else | `GameRef.setFlags/clearFlags` keep the bits (`GameRef_core.cpp:725-752`) but nothing in the renderer reads `WORLDTREELEAF`: deactivated Groups stay visible. The main-menu dialog texts on the Garage frame have a second cause: `draw_osd_texts()` draws every text the camera pass did not draw, whatever the state of its Osd's viewport and camera (1985-1987) |
| Texts | `Text.create` (`FUN_004e5b30` -> `FUN_004e8f40`) makes a type-3 render instance named `r_text` whose resource is the charset render object, with bone00 local (x, y, 0). At sync (`FUN_0046a110` / `FUN_0051db70`) the only thing taken from its transform is the bone00 world translation: `(m41, m42) * 0.1`, the script-space (x, y), used directly as an NDC anchor in the current viewport with y down (ndc_y = -1 is the top). The draw (`FUN_0044c400`, `FUN_0044c4a0`, `FUN_0044bdc0`, flush `FUN_0044cb60`) converts the anchor to pixels (`px = w * (ndc_x + 1) * 0.5 + left`) and per character c emits four XYZRHW vertices = pen + font vertex[4c + i].xy - 0.25, UVs copied, text colour as diffuse, then advances the pen by `vertex[4c+1].x * scale`. Align 2 leaves the pen (left), 1 subtracts half the width (centre), 0 the whole width (right). No culling, POINT sampling, SRCALPHA / INVSRCALPHA, texture = the font render object's texture; ESC 'C' plus eight hex digits sets a run colour, ESC 'c' restores it. No rotation or scale of the parent chain reaches the glyphs, but texts are part of the camera's tree walk (drawn last in its pass), so an unbound viewport or a `WORLDTREELEAF` ancestor hides them. `Text.setScale` (`FUN_004e5d80`) scales the charset mesh sections; no class in system/scripts calls it | `draw_camera_texts()` runs inside the camera pass, once per camera after its meshes (1481), and draws atlas glyph quads in world space (mirrored by the view, LINEAR filter); the legacy `draw_osd_texts()` draws y-up screen quads ("PRESS ENTER" and the version banner at the top instead of the bottom). Both space letters by the atlas quad width instead of `vertex[4c+1].x` and drop the bearings. Turning texts into 3D instances would be the wrong fix: the exe does not draw them as 3D geometry either. `Text.setScale` and `Text.getPos(Vector3)` have no host native |
| The curtain | `Osd.darken` does `createText("a", Frontend.getFadeRes(), ALIGN_CENTER, 0.0, 0.7, 0)` and animates the colour from 0x0F000000 to 0xFF000000 (Osd.class tree 75), so the exe draws it through the generic text path with the alpha taken from the colour. On disk the four vertices of code 97 (388..391) are not a regular quad, two of them coincide; how they come to cover the screen is `[unknown]` (compare a stock frame, or decompile the text draw for a 0x88-header mesh) | `fade.SCX` is rejected by the font parser and `Text.create` falls back to `simple20` (`RenderNatives.cpp:509-513`), so the curtain draws as a small letter "a" |
| Instance colour | text: the ARGB colour is the vertex diffuse. Mesh: what the exe does with the alpha byte of an instance colour (`RenderRef.setColor`) is `[unknown]`: decompile the setter and its consumer | `draw_meshes` turns blending off for textures that are not DXT3/5 (1665-1667), so the alpha of an instance colour never shows |
| GameRef `render` command | both forms are live in Build 940. A vehicle receives `render <vp id> <controller id> <camera number>` (Track.java:1295, 1313, 1576; CarMarket.java:882). A `camera` GameType receives `render <vp id> 0 0 1 <flags>`: the fifth number is `RENDERFLAG_CLEARDEPTH` OR `RENDERFLAG_CLEARTARGET` (Garage.java:499, comment "vp id, cam id, flags"), which may be where a camera's clear mask comes from `[inferred]`; numbers two to four are `0 0 1` at all 12 call sites and their meaning is `[unknown]` (decompile the `camera` type's command handler). The exe scans the command with `%*s %d %d %d %d %d`; `Render %d, %d, %d, %d, %d` is a statistics line, not this command (exe strings) | parsed as three numbers into stand-ins; no host camera is bound, so no 3D garage is drawn (`GameRef_part1.inc:1646-1746`) |

Two conventions are ruled out as causes: the exe's `Ypr_toMatrix` (`FUN_005666d0`) and
bone-matrix build (`FUN_00567570`) equal the host's `build_local_world` rotation cell
for cell, and `Osd.orientation`, set by `Dialog.show`, is never read by Osd. The
"rotated" dialog of the host frames is the X mirror of the camera pass together with the
text y direction: OK/CANCEL mirrored, buttons above the box, title below it.

## 8. Input and controls

`Input_tick` polls DirectInput8 devices: `SysKeyboard` (type 1, 256 DIK axes),
`SysMouse` (type 2, absolute X/Y/Z, four buttons, relative axes) and joysticks (type 3,
with three canned force-feedback effect slots) (exe strings; `input_win32.cpp:100-200`).
The control set file `save/controls/active_control_set` (SDAT `CTRL` version 16) maps
physical axes to logical ones.

| Aspect | Exe (old-exe addresses in the host comments) | Host at HEAD |
| --- | --- | --- |
| Devices | DirectInput8 keyboard, mouse and joysticks | keyboard and mouse only, background non-exclusive mode (`input_win32.cpp:265-266`). The keyboard carries a 64-event `DIPROP_BUFFERSIZE` buffer (`:272-286`); without it every `GetDeviceData` failed with DIERR_NOTBUFFERED and physical typing never reached `Input.lastKey`. A fork-only foreground gate zeroes polled keyboard and mouse state while the game window is not the foreground window (`:743-760`), and `di8_last_key_event` flushes the event buffer in that case (`:484-490`), so in `SLRR_WINDOW_NOACTIVATE` runs only `SLRR_PE_BOOT_KEYS="t:dik,..."` reaches the scripts (`game_boot_part1.inc:771-815`) |
| Logical axes | the 72 `Input.AXIS_*` constants (Input.class static initialisers) run from 0 to 75, consistent with the 76-axis device blob: 0 NULL, 1-3 TURN_LEFTRIGHT/UPDOWN/CCWCW, 4-6 MOVE_LEFTRIGHT/UPDOWN/FWDBACK, 7-13 LOOK_LEFTRIGHT/UPDOWN/CCWCW/FWDBACK/ZOOM/PREVNEXT/REAR, 14-16 MENU_LEFTRIGHT/UPDOWN/PREVNEXT, 17 FIRE_PRI, 18 FIRE_SEC, 19 USE, 20-22 PRI/SEC/TARGET_PREVNEXT, 23-24 TARGET_LEFTRIGHT/UPDOWN, 25-27 CONTROL_PREVNEXT/LEFTRIGHT/UPDOWN, 28 THROTTLE, 29 BRAKE, 30 SHIFT_UPDOWN, 31 NITRO, 32 START, 33 PAUSE, 34 SELECT, 35 CANCEL, 36 MENU, 37 MAP, 38 HELP, 39-40 CHEAT_1/2, 41 SHIFT_LOOK, 42-44 CURSOR_X/Y/Z, 45-47 CURSOR_BUTTON1-3, 48 HANDBRAKE, 49 CLUTCH, 50 GEAR_UPDOWN, 51 GEAR_SET, 52 MUSIC_VOLUME, 53 MUSIC_SELECT, 54 HORN, 55-58 MENU_UP/DOWN/LEFT/RIGHT, 59-60 MUSIC_VOLUME_UP/DOWN, 61-62 MUSIC_SELECT_NEXT/PREV, 63 PRINTSCREEN, 68-75 SPECIAL_1-8. The menu uses 34 (ENTER, SPACE, NUMPADENTER), 35 (ESC) and 55-58 (arrows); ENTER reaching `Gates.osdCommand(34)` confirms the binding. | every constant in `input_win32.hpp:65-87` matches |
| Axes | a `Controller` owns a 0x2148-byte device blob: 76 logical axes smoothed every `Input_tick` and a 152-entry feedback map; `ControlSet.load -> Controller.add -> user_Add` fills the 152-slot axis map | the blob and its `Input_Device_tickAxes` pass are mirrored but inert (`input_win32.cpp:33-40, 1047-1179`): only `input_device_init_axes_blob_soft` ever writes the blob, so no axis has a target, a rate, an analog flag or a map slot. What scripts read through `user_GetAxisVal` and `checkHotkeys` comes from a separate host table (`g_axis_maps`, capped at 152 entries per Controller, `IO_part1.inc:567-590`) and a host smoothing filter on the steady clock (`IO_part1.inc:720-795`); this matters for the FFB plan in FORK.md, which builds on the blob path |
| Hotkeys | a 512-slot table; `Input.createHotkey` fills a slot, `checkHotkeys(controller, osdInFocus)` samples each slot whose owner is null or in focus, treats a value above 0.2 as down and on an edge queues `EVENT_HOTKEY` (0x100000) to the handler, whose `handleEvent(Hotkey)` runs on a fresh VM thread | follows the PE semantics on a host side table (`IO_part2.inc:355-600`) |
| `Input.lastKey` | returns `DIK scan OR (ToAsciiEx char << 16)` from the keyboard's event buffer and appends the character to a 16-byte cheat ring that `kismajomCheck` searches backwards with each letter shifted by one (GameLogic.java:147) | ported (`IO_part2.inc:326-343`; `IO_part1.inc:117-159`) |
| Mouse | the script `java.io.MouseCursor` wraps a native cursor fed by WndProc NDC coordinates and a scene ray pick, exposed as `getPos`/`getPickedPos` and `EVENT_CURSOR` (0x10000) events to Osds (MouseCursor.class) | the two 0-argument natives and a `Cursor_tick` stand-in that picks OSD gadgets from the script-made `PhysicsRef` boxes instead of a scene ray (`Cars.cpp:621-760`, `Resources_part4.inc:768-850`); the `(Vector3)` overloads are not in the table |
| Force feedback | three `IDirectInputEffect` slots per joystick, started only when `Input_forceFeedbackEnabled` is set. `FFB_strength_emulated` scales the software smoothing step of every logical axis whose analog flag is clear, that is, axes driven by keys or buttons `[inferred]` from host comments: `Input_mapAxis_add` stores `analogFlag = Input_physAxisIsAnalog` at the record's +0x04 (`IO_part1.inc:567-576`) and the smoothing pass skips a record whose +0x04 is non-zero (`input_win32.cpp:1115-1153`). Two wordings disagree with that reading: the pass's own comment calls the flag `axisIsDigital`, and BACKGROUND.md section 1 says the value smooths only the steering axis, while the loop covers all 76 | no joystick enumeration, no effect creation, `setFeedback` returns 0 (`input_win32.cpp:859-861, 1104-1111`); the host filter does not use `FFB_strength_emulated` |

## 9. Vehicles, physics and sound

**The native contract** is listed in FORK.md ("The native contract we must honour") and
is fully present as host symbols (`grep` over `engine/Runtime`). Counted three ways
(native methods declared in the class, registration calls in the exe, rows in the host
table):

| Class | 940 class | 940 exe | Old class / old exe | Host rows | Note |
| --- | ---: | ---: | --- | ---: | --- |
| `Chassis` | 39 | 39 | 21 / 23 | 23 | Build 940 widened it; no host binding for `getRPM setRPM getMaxSteer setMaxSteer getShifting getNitro getNitroing getEngineTemp getAngvel getMaterialIndex getAIparam_float getAIparam_int setAIparam_float setAIparam_int getOSD setOSD` |
| `WheelRef` | 32 (31 names, `setDamping` twice) | 36 | 32 / 36 | 36 | unchanged between builds; the host rows are 28 setters, 7 getters and `finalize` |
| `PhysicsRef` | 10 (9 names, `getVel` twice) | 6 | 10 / 6 | 10 | both exes register only `create createBox createSphere setMatrix getPos getOri`; `setStatic`, both `getVel` and `getAngVel` are declared and listed in FORK.md's contract but never bound by the stock game (no `setStatic` or `getAngVel` string in the 940 exe), so their behaviour cannot be read from the exe |
| `Vehicle` | 4 | 3 | | 3 | `fileVersion` is declared only (section 4.6) |
| `GroundRef` | 31 (30 names) | 30 | 21 / 25 | 26 | the host binds 24 of the 31 and lacks `setTrafficCrossStopOffset haltTrafficRoad clearHalts getMaterialName getMaterialCount getTrafficCarPos getPedestrianPos` |
| `Sound` | 15 (14 names) | 17 | | 10 | 7 declared methods are missing (`firstTrack`, `randomTrack`, `stopMusic`, `getTotalTracks`, `getNowPlaying` and both `playTrack` overloads, `(I)V` and `(String)V`, which need two rows matched by descriptor, not by argument count); `has3DHardware` and `hasMixHardware` are registered by the exe but not declared |

| Aspect | What the exe does (old-exe addresses from host comments) | What the host has |
| --- | --- | --- |
| `Chassis.forceUpdate` | (0x43E320 -> 0x448430) first clones the 3,100-byte header when it still aliases the shared template (cloneHdr 0x43E3E0). Then, only when `suspend_update == 0`, it resets the wheel scratch, copies the 17 Pacejka floats, calls the script's `updatevariables` (0x44855D), walks the physics child list ingesting each part, copies engine scalars by field name (`engine_mass * 8000`, `rpm_idle * pi / 30`), rebuilds the torque table from `DynoData`, binds three sound resources by field name (`SFX_trans_fwd`, `SFX_trans_rev`, `SFX_ignition`, type 6, at hdr+0x7D4/+0x7E4/+0x7F4; the three SfxTables at hdr+0xF8/+0x340/+0x588 travel with the header clone), writes drag centre and `C_drag` and rebuilds the part slot table. The tail (0x4479D0) always runs (`Chassis_part2.inc:352-368, 636-958`) | the bookkeeping half as side-band maps (header clone, slot table, engine scalars, DynoData lerp, SFX binds, drag) with no body behind it (`engine/Runtime/Parts/Body`: 32 OOS lines, 18 in `Chassis_part1.inc`, 8 in `Chassis_part2.inc`, 6 in `Chassis.h`) |
| Wheels and simulation | per-wheel state is a 0x2B4-byte slot with 17 Pacejka floats at +0x1E8 (`world_state.hpp:20-27`). `Physics_Step` (0x4A5190) is called from the SimulateFrame substep loop with a solver loop of at most 20 (`world_state.cpp:434-451`); `Chassis_physWheelTick` (0x454500) has brake scale 0.2 and aero `F = -C_drag * v^2 * v_hat` (`Chassis_part2.inc:385-400`, `world_state.hpp:37-38`); where the wheel tick is called from is `[unknown]` (take the xrefs of 0x454500) | the 28 WheelRef setters store into `g_chassis_phys_wheels[chassis][8]` and nothing consumes them (`WheelRef.cpp:23-45`). Motion is a Soft arcade model: semi-implicit Euler with `kGravity = 25` on a flat plane plus pairwise collisions (`Resources_part4.inc:150-256`) and a velocity-clamp `physics_drive` called only by the smoke harness and the legacy Valocity shim (`Resources_part4.inc:318-400`, `GameRef_part6.inc:1831`). GII_DRIVE registrants are never dispatched (no `"drive"` call site in `engine/`) |
| Bodies and ground | bodies come from `PhysicsRef.create/createBox/createSphere` (box type 5 with half extents, sphere type 1); map interaction is `GroundRef` (nearest cross, align to road, route splines, a 356-byte traffic car pool) | no map collision geometry: the ground is `g_ground_y` plus road segments. Test fixtures add some (`main_part1/2/3.inc`), and the old-build path has a derived spine for `alignToRoad` / `getNearestCross`: a hard-coded six-segment spine between the club garages plus centre lines derived from the city road meshes (`physics_road_seed_valocity`, `physics_road_add_from_scx`, `physics_road_seed_from_rpak("city.rpk")`, `Resources_part3.inc:1549-1707`; called from the Valocity shim, `GameRef_part6.inc:453`) |
| Engine sound | native: three SfxTables of 16 samples (`sfx_engine_up/down`, `sfx_exhaust`, `sfx_gearbox_fwd/rev`, `sfx_horn`, `sfx_gearchange`, exe strings) played by `Chassis_SfxTable_play` (0x48F0E0, called four times from sub_4518C0, `SfxTable.cpp:34-47`); whether that runs in the physics tick or per frame is `[unknown]` (take the xrefs of sub_4518C0) | table mirrors, never played |
| Audio and music | DirectSound with a 64-voice table scheduled once per frame (`Sound.cpp:14-80`): both exes import DSOUND.dll ordinal 1, `DirectSoundCreate`, and contain no IDirectSound8 GUID. Music is MCI only in the old exe (WINMM `mciSendCommandA`); the Build 940 exe imports no MCI function and has no `mci` string, so how it plays the `Music\...` MP3 sets is `[unknown]` (both exes embed the DirectShow FilterGraph CLSID; settle in Ghidra) | `audio_win32.cpp` opens DirectSound8 and MCI (the host's choices, lines 752 and 890-916), but the `Sound` and `SfxRef` natives are voice-table mirrors that never call it (`Sound.cpp:698-745`; only `SfxRef.stop` reaches the backend, `Resources_part4.inc:878`) |
| Video | the menu background, opened by script with the 4-argument `openVideo` | a DirectShow graph into a D3D texture; only the 3-argument `openVideo` is registered, and the 940 call lands on it through the by-name fallback of section 4.6 with the fit argument dropped |

**Out of scope today**, by upstream's own OOS markers: rigid bodies, the solver,
inertia, the wheel tick, map collision, traffic path following, FFB effect creation, GC.
FORK.md puts new physics in `engine/Runtime/Physics/` behind the contract above.

## 10. The host today

Build: CMake, C++17, one target `slrr_engine` from 43 `.cpp` files whose large bodies
are `*_partN.inc` fragments (README "Engine splits"); the configured generator is
`Visual Studio 18 2026`, platform Win32 (`engine/build/CMakeCache.txt:233,237`; README
and FORK.md still say VS 17 2022); build with `cmake --build engine/build --config
Release --parallel`. Source at f43c741: 103 files, 83,189 lines. Fidelity markers
(lines, `grep -c`): PE 3,935 (word match), Soft 1,465, OOS 401, `Fork:` 186. Native
table: 377 entries. The fork is 44 commits over upstream 5c42f3b.

State values: faithful = ported from the exe's code or data layout, listed gaps aside;
soft = a host stand-in on host data structures that mimics the observable behaviour;
partial = some of the exe's behaviour implemented, the rest absent; missing = nothing
usable; host-only = no exe counterpart. PE / Soft / OOS are upstream's code markers
(BACKGROUND.md section 3); `Fork:` marks code added by this fork.

| Subsystem | State | One-line note | Evidence |
| --- | --- | --- | --- |
| TUFA decoder | partial | all 2,921 classes parse without error; CONS heuristic wrong on 2 classes, static MTHD rows read 4 bytes late, natives detected by empty tree (135 false positives), interfaces dropped, build number unchecked | `tufa.cpp:103-620`; section 4.3 |
| Class loader | partial | 8 of 9 classpath entries (no `java.net`), cars wildcard, bundled-sibling scan, path loads from `script` payloads; dotted `script` names are not resolved through the classpath; the first definition of a class wins | `jvm_bridge.cpp:10-19`, `jvm_load.cpp:63-264` |
| Source compiler | missing | 635 of 3,454 sources have no class; `compile_all` only scans; no stale check | `jvm_load.cpp:312-325`; exe 0x5598F0 |
| VMThread interpreter | faithful | opt-in: by default only leaf trees run on it (section 4.4); 73-entry case map, frames, locals pop, jumps, literals, arrays; missing 0x1008 slots 0-2/7-9, op29 on stream | `jvm_vmthread_part2.inc:976-1510, 1592-1631` |
| Method resolution | faithful | Ghidra-ported scoring, super walk, typed nulls, (owner, method) slot | docs/VM.md table; `jvm_vmthread_part1.inc:488-825` |
| Object/field model, strings | soft | untyped field bags read through declared types; no PE Value layout, no refcounts; strings compared by text | `jvm_vmthread_part1.inc:1540, 1712-1866` |
| Green threads | partial | cooperative only with `SLRR_PE_BOOT_INIT=1` or `SLRR_PE_GREEN=1`; legacy path uses OS threads | `IO_part2.inc:658-1035` |
| Budgeted pump; GC | faithful; missing | PE formula plus a 50 ms wall cap and re-entry guard; GC slice and mark-sweep are stubs, objects are never freed | `jvm_vmthread_part2.inc:2009-2145`; `part1.inc:206-222` |
| Native table and thunks | partial | 377 entries, PE hash keying with a by-name fallback, 16-word thunk; 53 of the 394 natives declared in the 940 classes have no entry and 6 more run the wrong overload; the exe registers 422 | `jvm_register.cpp`, `callinfo.cpp:641-680`; section 4.6 |
| Events / timers / notifications | soft | per-GameType vectors on the host clock; semantics mirrored | `GameType.cpp:24-130, 400-700` |
| Callbacks | partial | CONTROL and ANIMATE lists real; DRIVE never dispatched | `GameType.cpp:743-820`; no `"drive"` call site |
| Script errors | partial | null receiver and unresolved method continue in place with null, as the exe; other nested failures unwind the frame; an outermost failure falls back to `tree_eval` unless strict | `jvm_vmthread_part2.inc:857-880, 1403-1444, 1581-1700` |
| Legacy `tree_eval` + `Jvm::invoke` hooks | soft | 5,495 lines kept as fallback; Vector/queueEvent/create/addTraffic hooks still in the call path | `jvm_part2.inc:1250-1420` |
| RPK loader | partial | header, deps, TOC and remap table for all 146 packs; pack id = open order, whole file in RAM, transform floats skipped, `isparentcompatible` read as padding | `rpak.cpp:155-214, 274-361, 363-409, 495-518` |
| Resource tree walk | partial | TOC links with remap-resolved parents plus runtime children | `Resources_part2.inc:1455-1560` (HEAD) |
| ResourceRef natives | soft | 20 bound; `ResState` per object, synchronous load, no destroy queue | `Resources_part2.inc:1183-1700` (HEAD) |
| Scripted GameType creation | partial | first-line `script <path>` payloads only; dotted class names, `native X` types and two-line payloads fall back to `java.game.<alias>`; the `.cfg` an entry names is never read | `GameRef_part1.inc:1185-1221` |
| Meshes and textures | partial | INVO v4/v3 positions, normals, one UV, u16 indices; v4 strides 24-40 only (17 files read at the wrong stride); no bones, LOD, material classes; textures by filename heuristic; DDS via runtime D3DX, PNG/JPEG via WIC, PTX last level only | `render_d3d9_part3.inc:999-1296`, `part2.inc:416-444, 1095-1230` (HEAD) |
| Fonts and Text | soft | glyph table built from the font SCX UVs only (advance and bearings ignored), drawn as world-space quads under the camera or as y-up screen quads that ignore viewport binding and hidden state; the exe draws screen-space quads from the font vertex buffer at a y-down NDC anchor; `Text.setScale` and `Text.getPos(Vector3)` unbound; a charset the font parser rejects (`fade.SCX`) falls back to simple20 | `RenderNatives.cpp:498-716`, `render_d3d9_part1.inc:1233-1340, 1955-2067`, `render_d3d9_part3.inc:504-551` (HEAD) |
| Present walk | soft | legacy `draw_meshes`, `draw_osd_rects` and `draw_osd_texts` passes run around the Fork camera pass whatever the viewport binding or hidden state, where the exe draws only what each bound viewport's cameras collect in their tree walk, meshes then texts; in the main loop the host presents first and the exe last (section 3) | `render_d3d9_part1.inc:2069-2153` (HEAD); exe `FUN_00421920` |
| Viewports / cameras | soft | rect and priority stored, but one viewport is active at a time where the exe keeps a bound list; the native angle is used as the half angle under a left-handed look-at (46% size, X mirrored) where the exe uses it as the full vertical angle, right-handed; the clear uses the Java renderflags, which the exe's `Viewport.activate` drops; no LOD or occlusion, one global fog | `RenderNatives.cpp:159-317, 717-944`, `render_d3d9_part1.inc:1133-1189, 1342-1488, 2285-2307` (HEAD); exe `FUN_00461dd0`, `FUN_004e6ab0` |
| Hidden state (WORLDTREELEAF) | missing | flags are stored, nothing in the renderer reads 0x40; the exe's camera walk does not descend (`FUN_0045eb10`) | `GameRef_core.cpp:725-752` |
| GameRef camera (`render` command) | missing | three numbers parsed into stand-ins; no host camera bound; the five-number camera form is not handled | `GameRef_part1.inc:1646-1746` |
| Main loop (PE boot) | partial | every stage on every iteration: present, Win32 pump, input poll, `input_tick`, SimulateFrame, resource pumps, scheduler, animate. Against Build 940: present first instead of last, no rate gates, no Sfx or AsyncLoad stage, resource pumps before the VM pump, scheduler called without `jvm_pump_frame`, notify wakes one waiter, `input_tick` four times, load rate stuck at 16 | `game_boot_part1.inc:744-769`; exe 0x47D760; section 3 |
| SimulateFrame | faithful | dt clamp, warp, substep pick, control tick, timers; substep 0.05 is a guess; measures its own dt with `GetTickCount` | `System_part2.inc:1452-1527` |
| Window / device | soft | fixed 1024x768 windowed, no vsync, software vertex processing; `Config.video_*` never applied although the scripts lay out from them | `render_d3d9_part1.inc:655-776` (HEAD) |
| DirectInput | partial | keyboard (64-event buffer) + mouse, background mode with foreground gate; no joysticks | `input_win32.cpp:245-346, 743-760` |
| Controller / ControlSet / hotkeys / lastKey | soft | host axis-map table (152 per Controller) and host smoothing filter, the PE blob is mirrored but inert; 512-slot hotkey semantics, ascii packing and cheat ring follow the PE | `IO_part1.inc:563-611, 720-795`, `IO_part2.inc:326-600` |
| MouseCursor | partial | 0-arg `getPos`/`getPickedPos`; gadget pick over script `PhysicsRef` boxes, tied to the Soft state lookup, which is null on the script boot | `Cars.cpp:621-760`, `Resources_part4.inc:768-850` |
| Force feedback | missing | slots exist, no effect created, no joystick | `input_win32.cpp:859-861, 1104-1111` |
| Steam | missing | 0 of 5 natives; `GameLogic` exits when `Steam.postInit()` is 0, yet the boot reaches Garage: why is `[unknown]` | natives_stubs.cpp; GameLogic.java:335 |
| Vehicle build (`forceUpdate`) | partial | bookkeeping as side maps; no body | `Chassis_part2.inc:636-958` |
| Rigid body / tires / ground | missing | arcade Euler on a plane; a derived road spine on the old-build path | `Resources_part4.inc:25-36, 150-400`, `Resources_part3.inc:1549-1707` |
| Audio and video | soft | DirectSound8/MCI backend exists, natives never call it; 10 of the 17 Sound natives the 940 exe registers are bound (8 of the 14 declared names); DirectShow to texture with the 3-arg `openVideo` only | `Sound.cpp:698-745`, `video_fmv.cpp:251-375` |
| Diagnostics | host-only | 26 `SLRR_*` switches, crash handler with map offsets and VM frames, frame dump, key injection | `main_part1.inc:45-96`; `grep getenv` |

**Boot modes.** `--boot` runs `game_boot_run` (`game_boot_part1.inc:198`, dispatched at
`main_part2.inc:509-515`), the console boot sequence. With neither `--boot` nor `--game`
the host runs the unit smoke in `main_part1-4.inc` (prints `ok=1`, exit 0 or 2; its
render, audio and road half needs `--window`). `--game` runs `game_interactive_run`.
Without `SLRR_PE_BOOT_INIT` the C++ Soft path replays the pre-940 splash/menu/GameLogic
flow (`game_boot_part1/2.inc`, `GameRef_part3.inc`, 43 `MainMenuDialog` references),
which is what the E: regression exercises (`vehicleTypes=25`, `hub EXIT ok=1`, exit code
5; TASKS.md header). With `SLRR_PE_BOOT_INIT=1` (`game_boot_part1.inc:714-721`) the same
function constructs `java.game.Init`, invokes `<init>(I)` and enters the PE frame loop.
That switch alone does not put the boot on the stream VM: the four `SLRR_PE_STREAM_*`
switches of section 4.4 are separate, and with `--no-wait` the loop ends after 60 frames
unless `SLRR_PE_BOOT_FRAMES=0` (bound it with `SLRR_PE_BOOT_SECONDS=N`;
`main_part2.inc:530`, `game_boot_part1.inc:739-742, 819-826`). The run that boots Build
940 is the "Standard 940 run" line at the top of TASKS.md; it stops at the main menu,
and the Garage is reached only when ENTER, NEW CAREER and a name are typed or injected
with `SLRR_PE_BOOT_KEYS` (TASKS.md "Next steps"). Whether the E: scripts also boot
through `SLRR_PE_BOOT_INIT=1` has not been tried `[unknown]`; it decides how the Soft
shims can be retired without losing the regression.

**Two reference installs.** D: Build 940 (the target; validates the script boot). E: the
pre-940 exe (2,408,448 bytes; 5,322 `.class` files, 5,319 of them TUFA 0x11AE1, 2 empty
and 1 with another header word), which validates the legacy path and is the exe
upstream's `@0x...` comments refer to (section 1).

**Diagnostics and tools.** The host reads 26 `SLRR_*` environment switches. Eleven are
explained in docs/VM.md "Experimental switches" and TASKS.md "Known problems", seven
more appear elsewhere in those two files, and eight are documented nowhere and readable
only from the code: `SLRR_PE_HEARTBEAT`, `SLRR_PE_GREEN`, `SLRR_PE_STREAM`,
`SLRR_PE_STREAM_STEPS_MAX`, `SLRR_PE_PUMP_TRACE`, `SLRR_PE_FIELDPATH_TRACE`,
`SLRR_RENDER_TRACE`, `SLRR_TREE_TRACE_MAX`. Tools that run against this repository:
`tools/tufa_dump.py`, `tools/ghidra940.py` (headless queries on the `slrr940` project in
`..\slrr-ghidra`; the exclusive lock is Ghidra's own `slrr940.lock`),
`tools/split_source.py` (it produced the `*_partN.inc` split) and
`tools/audit_natives.py`, which scans only `*.cpp` and so reports the native bodies that
live in `.inc` fragments as missing. `tools/gen_runtime_progress.py` needs
`docs/native_registry.json`, which is not in the repo; the other 13 carry upstream's
`native/` layout or personal paths. Run etiquette: no desktop screenshots,
`SLRR_WINDOW_NOACTIVATE=1`, frame capture through `SLRR_PE_BOOT_SHOT`; delete
`tree_rpk_scan_boot/` in the game directory after each run (the host writes it,
`game_boot_part1.inc:142`), and remember that a key-script run also writes a profile
into `save/career` (section 5).

**Hygiene.** `engine/data/System.class` and `class_index.txt` and the cursors and icon
under `engine/assets/` are game-derived and copied beside the exe by CMake
(`engine/CMakeLists.txt:73-86`); `engine/data/System.cons.bin` is game-derived and
tracked too, but CMake does not copy it and nothing in `engine/` or `tools/` reads it.
`System.class` is the pre-940 build's (byte-identical to the E: install's). The install
itself is never copied into the repo, but these 14 files (10 cursors, 1 icon, 3 data
files) are tracked in git, inherited from upstream, so removing them from the published
tree and from any release is still open (BACKGROUND.md section 7, FORK.md "Hygiene
before any release").

## 11. Dependency map

| To get | It needs, and why |
| --- | --- |
| **Drawing the garage scene** | Needs the per-viewport present walk of section 7 (bound list, right-handed camera, hidden state) because the garage camera is a `camera` GameType bound to the Osd's viewport by the `render` command and must land in the same pass as the OSD. It needs the native GameTypes behind the payload lines, not only their parsing, and `ground` first of all (system:0x35, alias `map`): the garage is `map = new GroundRef(...)` (Garage.java:217) on a typeof-1 instance with payload `gametype 0x00010035` / `params 0x00000005,0,0,0,0,0,0` (misc.garage:0x7A), whose render object 0x05 (`mesh 0x07`, `flags 2048`, 12 textures) is the garage shell. The camera, lift, physics boxes and light are all created under that map, and `maps/city.rdb` uses the same type, so M2 and M6 share it; `camera` (system:0x33) and `part` with its `.cfg` (the lift) come next. The host has no code that recognises a `native` payload line, and nothing reads the stored `params`. It also needs the five-number `render` command bound to a host camera, the `mesh/texture/flags/lod_amp` render-object recipe because garage and car meshes bind textures by record id, not by filename (1,174 of the 4,710 render objects also carry `shd_*` shadow lines; `skeleton` occurs in 17, all in humans.rpk), INVO bones for the `bone00`/`wheelbone` attachment, and the wide v4 vertex strides of the garage prop meshes (section 6). |
| **Mouse support** | Osd gadget picking does not need the camera pass. Today's pick, `physics_pick_osd_gadget`, already tests the script-created `PhysicsRef` boxes in Osd units through `Osd.SCALE_3D` (`Resources_part4.inc:768-850`), but it only considers the Osd of the Soft `game_logic_actual_state()`, which is null on the script boot and lives in `GameRef_part3.inc`, a file M5 retires; no gadget is picked on the 940 boot until the state lookup moves to `GameLogic.actualState`. `Osd.animate` calls the 0-argument `MouseCursor.getPos()`, which the host binds (Osd.class tree 86); the `(Vector3)` overloads are needed by the paint booth and race setup only (Painter.java:551, 682; RaceSetup.java:95, 383). `MouseCursor.enable` is a script method that drives the native `cursor` type through commands, so it needs that type. Only the 3D scene pick (Mechanic drag-and-drop on the part `click` shapes) needs the garage camera. Menus do not need the mouse at all: `Osd.show` calls `createMenuKeys()`, which registers the AXIS_MENU_UP/DOWN/LEFT/RIGHT and AXIS_SELECT hotkeys (Osd.class trees 91 and 137), and the garage sets `osd.defSelection = 5`, so HITTHESTREET, the lift, the dealer and the catalog are reachable with arrows and ENTER, which `SLRR_PE_BOOT_KEYS` can inject. |
| **Loading cars** | Needs the native `part` and `car` types before anything else: 3,193 pack entries carry `native part <cfg>` and 96 `native car`, and that cfg (`render 0x..`, `click 0x..`, `slot`, `compatible` lines) is where a part gets its mesh, pick shape and slots. The host never reads the cfg an entry names: `cfg_path` is set at two hard-coded smoke sites only (`GameRef_part5.inc:183, 194`) and car visuals come from a fixed list of body SCX file names (`valocity_ensure_car_parts`, `GameRef_part6.inc:931-1000`). It needs dotted `script <class name>` payloads resolved through the classpath (137 entries; this is what drops `stock_Battery_silver`, the only part lost in today's 940 run). It needs `Chassis.getMaxSteer` and `setMaxSteer`, which `Chassis.java` calls when `System.nextGen()` (lines 246 and 1218), and `getAIparam_float(3)` in `getSteer()` (1206); the other 13 unbound Chassis natives are called from Track.java, the Gamemodes or no shipped source and belong to driving. The compiler (or a stock-exe `compileAll` stopgap, which rewrites classes in the install) gates complete cars, not car loading as such: the classless sources are lights, doors, kits and some wheels and running gear, no chassis class is missing and no shipped class extends a missing one. |
| **Driving** | Needs physics (a rigid body, suspension and tire model behind the FORK.md contract) and ground collision from the native `ground` type that every map and garage instantiates (`maps/city.rdb`: `gametype 0x00010035`, `params 0x00000010,0,0,0,0,0,0,maps\city\meshes\city.scm,maps\city\ai\city.trc`); how that type builds collision is `[unknown]` (decompile the 940 `ground` type). Its side files are mostly text: `.scm` is a material-name list (`materials 108`), `box/*.box` are rows of nine floats, `box/*.cfg` hold `bounds` and `body ... box hx hy hz` lines, `.rest` and `.walk` are comma-separated text (`.walk` with an encoded tail); only `.trc` is binary. `box/` exists for city and city2 only (57 files each), not for the 21 track maps. Driving also needs the GameRef camera path in both command forms, GII_DRIVE dispatch, the Track HUD (texts and rectangles under its Osd), joystick enumeration for a wheel, and `GroundRef` traffic and route natives, because `Track`/`City`/`Valocity` drive the car and the world entirely through those natives. |
| **Retiring the Soft shims** | Retiring `game_boot_part*.inc` and `GameRef_part3.inc` needs the script boot to be the default, which means five switches and not one (`SLRR_PE_BOOT_INIT` plus the four `SLRR_PE_STREAM_*` of section 4.4), and a replacement for the E: regression, because that regression runs through the shims today. Deleting the three files removes 43 of the 75 `MainMenuDialog` references; 32 remain in nine other files, and the cursor pick still reads `game_logic_actual_state()`. The `Jvm::invoke` behavioural hooks can go only after the stream path handles every tree the boot walks, because those hooks are also the legacy evaluator's leaves. |
| **The compiler** | Needs nothing from the rest of the roadmap: the language is a small Java subset (section 4.1), the emitter side is known from the loader, and the 2,819 shipped source/class pairs are an oracle (2,663 carry build 0x24F9D and can be compared byte for byte; 152 carry 0x24F5C and 4 carry 0x11AE1 and are compared with the header excluded). The class grammar is no longer a precondition: CONS, MTHD, FILD and CLSS are settled by the strict walk of section 4.3. What remains is the meaning of the MTHD fifth word, to be read from the 940 `JVM_addClass_fromChunks` (docs/VM.md records it as decompiled but gives no address; 0x554F4F comes from a host comment, `tufa.cpp:500`). |
| **Physics and FFB** | The module itself needs nothing from M1-M5. FORK.md builds it standalone and engine-agnostic with its own harness (`engine/tools_physics/`, own CMake target) from the WheelRef/Chassis parameter contract, exactly like the compiler. Binding it to the game needs cars loaded in the garage and the drive state, because `physics_drive` is invoked only by the smoke fixtures and the legacy Valocity shim and GII_DRIVE is never dispatched to a scripted `Vehicle`; FFB additionally needs joystick enumeration (no type-3 device slot exists) and the physics module for rack torque. A first in-game check needs no map format: the garage floor is a script-made `PhysicsRef.createBox` (Garage.java:233). |
| **A modern renderer** | Needs the present walk and the render native contract complete and faithful first (`GfxEngine`, `Viewport`, `Camera`, `Text`, `RenderRef`, `Animation`, `ParticleSystem`, the mesh and texture natives of `ResourceRef` such as `scaleMesh` and `makeTexture`, and the `render` commands of the `camera` and vehicle GameTypes), because scripts only ever see those natives (BACKGROUND.md section 5). |
| **Saves and career** | Need the 14 `File` natives of the 940 class (`open`, `close`, `write(int)`, `write(float)`, `write(ResourceRef)`, `write(String)`, `readInt`, `readFloat`, `readResID`, `readString` and the static `delete(String)`, `copy(String,String)`, `move(String,String)`, `exists(String)`; all 14 are bound) to match the stock SDAT layout, pack-table trailer included. There is no `write(GameRef)`, and the wildcard `delete/copy/move(dir, mask)` are script methods in File.class built on `FindFile`, which is therefore the native the wildcard forms depend on. Pack slots need not be stable (section 6). This path is already live: NEW CAREER calls `autoSaveQuiet()`, so every run of the standard key script executes `GameLogic.save` inside the install, including `File.delete(<profile>, "*")` (GameLogic.java:947-950), and its output is already wrong (section 5). Save-format checks are therefore available from M1 on, not from M5. |
| **Audio** | Needs the drive state for engine sound, which is played through the chassis SfxTables; menu and garage sounds and music need only the `Sound`/`SfxRef` natives wired to the existing backend plus the missing `Sound` track natives. Nothing in the host does that wiring today (no caller of `audio_sfx_play` or the music functions outside `audio_win32.cpp`). |
| **Linux** | Needs a platform abstraction (window, input, audio, video, renderer backend) because the host is Win32 D3D9/DirectInput/DirectSound/DirectShow throughout. It also needs a 32-bit x86 target or a 64-bit clean-up first: the native-call thunk passes every argument, pointers included, as a 32-bit word through a cdecl function pointer (`callinfo.cpp:641-680`), and PE-layout structs with pointer members are size-asserted (`render_d3d9_part1.inc:469, 476`). It needs small build fixes (CMake source paths are lowercase `core/`/`runtime/`; the non-MSVC branch links no libraries) and case-insensitive, backslash-tolerant path resolution for everything read from packs, not only the classpath: 1,356 of the 18,675 file references in pack payloads match a file only when case is ignored (pack `cars/meshes/speedo.scx`, disk `speedo.SCX`), the classpath directories are `system/scripts` but `sl/Scripts`, and source directories are spelled `src`, `Src` and `SRC`. The non-Windows branch of `jvm_load.cpp` also stubs out the sibling-class scan and the `cars/racers/*` wildcard (`:103-128, 244-262`). |

## 12. Roadmap

Ordered by the dependency map. TASKS.md keeps the live steps for M1, M2, M4 and M5 only:
it has no compiler step, FORK.md roadmap step 6 still calls the compiler a later
milestone, and TASKS.md step 4 lists `camera`, `cursor`, `SplashScreen`,
`lift_support/cfg` and `stock_Battery_silver` as classes the host cannot compile,
although none of them is a source without a class (section 6). Those two files should be
brought in line; this table says why each milestone comes when it does.

| Milestone | Why now | Done looks like | Risk |
| --- | --- | --- | --- |
| **M1. UI pass: camera field of view and handedness, screen-space texts, hidden state, one present walk** (TASKS.md step 1, in progress) | Every later visual milestone (garage, mouse, HUD) sits on the per-viewport pass, and the exe's model is decompiled (section 7): the native camera angle is the full vertical field of view, the exe is right-handed, a `Text` contributes only an NDC anchor with y down and its glyphs are screen-space quads from the font mesh, bound viewports are a list drawn by descending priority, and the camera walk does not descend into a `WORLDTREELEAF` node. | The Garage frame dump shows only the garage OSD, no main-menu text and no stray "a"; the 3.92 x 2.94 m Osd background fills its viewport; dialog text reads left to right with the title above the box and OK/CANCEL below it; "PRESS ENTER" and the version banner sit at the bottom of the screen; letters are spaced by the glyph advance; the curtain is code 97 of the `fade.SCX` charset drawn by the text path with the colour's alpha, not a mesh quad; the E: regression still prints `hub EXIT ok=1` and exits 5. | The handedness and field-of-view fixes move every camera-pass instance, so the garage camera (M2) must use the same convention and the E: path must be re-checked; three items are still open in the exe: mesh-instance alpha, the camera type factory `FUN_00524dc0`, and how the fade vertices cover the screen. |
| **M2. Garage scene** | It is the first visible 3D output and the proving ground for the native `ground`, `camera` and `part` types, the five-number `render` command, render objects bound by record id and bones; it depends on no other milestone than M1. | A frame dump with the garage shell (`garazs_01`), its props (`garazs_01_targyak`) and the lift under the Osd, lit by the `neon` directional light (misc.garage:0x1D, Garage.java:273-275); the `[jvm] class file missing camera/cursor/lift_support` lines gone. | Numbers two to four of the camera `render` command, the INVO material, bone and LOD encoding and the `ground` type are undecoded and need Ghidra time on the 940 exe; the three-number vehicle form of `render` is still live and M6 needs it too. |
| **M3. Compiler as a standalone tool, run in parallel with M2** | It needs nothing from M1-M2, has the 2,819-pair oracle, and gates complete cars (lights, doors, kits, some wheels and running gear); starting it early also forces the loader fixes of section 4.3. | A tool whose output is byte-equal to the 2,663 paired classes at build 0x24F9D and equal past the header for the other 156, and which then produces the 635 missing classes; the host calls it where `class file missing` is logged, although none of the six lines a 940 run logs there today is a compiler case. | The oracle is untested until the first class is compiled; what is established so far is weak (where a source declares methods, 767 pairs, the class's method names match in all). The stopgap (let the stock exe compile) works but rewrites classes in the install and stays local. |
| **M4. Mouse and garage operation** | With M1-M2 the garage draws, and the Mechanic, MOVE PARTS and paint booth are pointer-driven; menus already work from the keyboard. Two prerequisites come first: a cursor and click injector beside `SLRR_PE_BOOT_KEYS` (only `input_inject_key` exists, and physical mouse input is ignored unless the window is in the foreground), and a car, because a new career starts with money and no car (Garage.java:1693-1701): the run must load a stock-written car or career (LOAD CAR / LOAD CAREER, which pulls the save format forward from M5) or buy one in the CarMarket state. | A scripted key-and-click run that installs and removes a part, with the mesh count in `[render-dbg]` changing and no "not found" from `Part.addStockParts`. | The `Cursor_tick` scene pick needs the `PhysicsRef` hotspot boxes in the camera's space, which touches the Soft PhysicsRef state. |
| **M5. Career path and Soft-shim retirement** | Nothing in the dependency map ties these items to M1-M4, so they are parallel work rather than a stage after the mouse. The career-path errors reproduce on today's key script (the null from `String.getParams`, Steam natives with a `neutralize` path, field initialisers for classes without `<init>`, the host-written `main`), and shim retirement needs only the five boot switches as defaults, a replacement for the E: regression and the `Jvm::invoke` hooks gone; remove the shim files last, because the cursor pick still reads `game_logic_actual_state()`. | A career saved and reloaded with `main` and `PlayerCar0` byte-compatible with a stock profile, `SLRR_PE_STREAM_ERRORS=1` silent from menu to garage, and a regression replacing the E: hub check (the E: scripts on the script boot, or the 940 key script). | The `[unknown]` Class_newInstance behaviour and the Steam `postInit` path can change what the menu does on a clean profile. |
| **M6. Driving: bind physics, ground, cameras, HUD, audio** (FORK.md step 5; steps 1-3, the standalone module, run in parallel from now) | Binding needs cars in the garage (M2-M4), the GameRef camera path (M2) and the Track Osd (M1), plus the `ground` type's collision, the largest remaining reversing task, and joystick enumeration, which a wheel needs as much as FFB does (the stock control set already names a third, non-system device). | `HITTHESTREET` reaches `Valocity.enter`, the car rests on the city ground under gravity, a frame dump shows the follow camera and the HUD, and the engine SfxTable plays through DirectSound. | The `ground` type and the `GroundRef` traffic natives are the least documented part of the exe. |
| **M7. Force feedback** (FORK.md step 4) | Rack torque needs M6's tire model and its type-3 device slot. | A constant-force effect updated per physics tick on a wheel, with a clipping meter in the log. | Low; the DirectInput plumbing is small once a type-3 device exists. |
| **M8. Modern renderer, platform abstraction, Linux** (BACKGROUND.md sections 5-6) | It needs the native contract and present walk complete (M1-M2) so the backend can be swapped behind unchanged natives, and a Linux build needs the whole platform layer. | The same frame dumps from a second backend, and a MinGW or native Linux build passing the 940 key script. | The fixed-function material semantics (envmap flag, 30 `CMaterial_*` classes) must be recovered before a shader backend matches the look. |

Independent of the order above and safe to do any time: add the ninth classpath entry
(`java.net`); fix the CMake path casing and give the non-MSVC branch its link libraries;
test native flag 0x40 in the class loader; resolve dotted `script <class name>` payloads
through the classpath; make the config load rate reach the pump; wire the menu and
garage sounds; stop shipping the game-derived `engine/data` files (`System.class` and
`System.cons.bin` have no reader in `engine/` and only need to leave the CMake copy
step, while `class_index.txt` exists in no install, is generated by
`tools/export_class_index.py` and is read only by the unit smoke,
`main_part3.inc:1252`); and update README/FORK.md for the VS 18 2026 generator.
