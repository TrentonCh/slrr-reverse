# How Street Legal Racing: Redline works, and where the host stands

> Status: draft. Written by the gather/write pass on 2026-10-09; the fact-check pass
> (three section checkers and a fixer) has not run yet. Numbers were counted on the
> install and the repo at c14fc53; re-verify a figure before building a decision on it.

Written 2026-10-09 against `dev` at c14fc53, the Build 940 Steam install on D: and the
pre-940 reference install on E:. `docs/TASKS.md` is the live tracker; `docs/VM.md` is
the progress log with the Ghidra-confirmed Build 940 addresses; `docs/BACKGROUND.md`
has the ecosystem, legal and design notes; `FORK.md` has the physics/FFB plan and the
native contract. This document is the stable explanation: what the game does, what the
host does today, and why the roadmap is ordered the way it is.

## 1. Purpose and how to read this

The audience is anyone who has to decide what to build next without first spending a
week in Ghidra. Every statement carries a status. A plain statement is verified: it was
seen in host code (cited as `file:line`), in an exe string, in a script source line, in
a listing of the install, or in the Ghidra-confirmed table of `docs/VM.md`, and the
parenthetical says which. `[inferred]` marks a statement reasoned from such evidence and
says from what; `[unknown]` marks a gap and says what would settle it. One blanket
caveat: every PE address quoted from host comments (`@0x...`) is an old-exe virtual
address from upstream's IDA work on the E: build; only the VM functions in `docs/VM.md`
have been relocated and confirmed in the Build 940 exe. Counts were made on the D:
install and on the repository at c14fc53 and are reproducible with `find`, `grep -c`
and `tools/tufa_dump.py`.

## 2. The game at a glance

One process, three layers. `StreetLegal_Redline.exe` (2,446,848 bytes; `diag.log`
banner `SL2 build 940 (Dec 14 2025 20:23:29), distr. v2.3.1`) contains the engine
natives (rendering, resources, input, sound, the vehicle simulation), a small Java-like
virtual machine with its own compiler, and a native method table joining them.
Everything a player sees after the first loading screen is script: menus, garage,
career, dialogs, cameras, part rules, car assembly. The scripts are a Java subset
compiled to the exe's own class format (TUFA) and run by that VM; they call the natives,
and the natives call back into scripts for control, animate, timers and events.

| Layer | Lives in | Changed by |
| --- | --- | --- |
| Natives (engine services) | the exe; in the host, `engine/Runtime/*` and `engine/Core/Platform/*` | adding or implementing a native in the host |
| VM (language, loader, scheduler, compiler) | the exe; in the host, `engine/Core/Jvm/*` | rewriting freely in the host |
| Scripts (game rules, UI, career) | `*.java` sources beside `*.class` files in the install | editing the `.java`; the exe recompiles on demand |

What lives where on disk (Build 940 install, 25,778 files, 2.50 GB by `find`):

| What | Count | Where |
| --- | ---: | --- |
| Script sources `.java` | 3,454 | 95 `src/` directories: cars 1,933, parts 1,364, sl 133, multibot 22, misc 2 |
| Compiled classes `.class` | 2,921 | cars 1,515, parts 1,147, sl 140, system 95, multibot 22, misc 2 |
| Source/class pairs | 2,819 | 635 sources have no class; 102 classes have no source (system 95, sl/CareerEvents 7) |
| Resource packs `.rpk` | 146 | root 13, cars 54, multibot 45, parts 15, maps 9, misc 8, sl 2 (149 on disk incl. 2 host stubs in `tree_rpk_scan_boot/` and 1 in `modding_tools/`) |
| Decoded pack text `.rdb` | 118 | beside most packs; human-readable form of the same tables |
| Meshes `.scx` (INVO) | 6,033 | cars 3,551, parts 958, objects 865, maps 536, frontend 34 (703 MB) |
| Textures | 3,296 `.dds`, 1,685 `.png`, 86 `.tga`, 67 `.ptx` | maps/multibot DDS, cars PNG, frontend atlases |
| Audio | 337 `.wav`, 25 `.mp3` | parts 133 (engine samples), sound/wav 73, frontend 30; music in six `Music\*` set folders |
| Video | 2 `.avi` | `data/Fmv/background_motion.avi` (28,080,956 B) and `_HD` (63,212,544 B); no intros ship |
| Save data | `save/career` 29 profiles, `save/cars/database` 871, `save/skins/database` 810 | SDAT containers written by the script `File` natives |

The missing 635 classes are all part or car variants: cars/racers 418, parts/wheels
135, parts/running_gear 38, parts/engines 28, parts/scripts 15, parts/accessories 1
(census over `*/src/*.java` versus `../*.class`). The class files carry three TUFA build
numbers: 2,757 at 0x24F9D (Build 940), 159 at 0x24F5C and 5 at 0x11AE1 (the pre-940
build); the exe contains both 0x24F9D and 0x11AE1. Which build numbers the 940 compiler
accepts is `[unknown]`; the old-exe header check at 0x4166F4 has no 940 decompile yet.

## 3. Boot and the frame

**Startup.** `WinMain -> Engine_boot -> Engine_MainLoop` (host comment
`game_boot_part1.inc:649-676`, old-exe VAs 0x551430 / 0x58C700 / 0x428960).
`Engine_boot` initialises the resource engine, opens `system.rpk`, creates the JVM and
the native table, constructs the `Config` GameType (`script java.util.Config` in
`system.rdb`; its `loadConfig` reads the SDAT file `save/game/options`, 236 bytes, magic
0xFEDCBA98 version 13), opens the D3D9 display, resets input, initialises DirectSound,
plays the boot videos if present (the 940 install has none, `ls data/Fmv`) and enters
the main loop, whose first act is `LoadGameInit("GameInit")`, constructing
`java.game.Init(int)`. The entry is a pack entry: `sl.rpk` carries the alias `Init` with
payload `script java.game.Init`, and the exe carries the string `GameInit` once. How
`GameInit` maps onto the entry named `Init` is `[unknown]` (no `.rdb` or `.rpk` contains
`GameInit`; decompiling `Engine_LoadGameInit` in the 940 project would settle it).

`Init.java` (sl/Scripts/game/src, 196 lines) is the whole boot script: `Frontend.init()`
(line 24: GfxEngine object, two LoadingScreens, `Steam.init`, a `Thread` named
"Hotkey watcher", fonts), `Frontend.loadingScreen.show(frontend:0x3EB)` (25), three
menu sound precaches, `Sound.init()` (39), `Input.initControllers(this,
"save/controls/active_control_set")` (41), the `Config.restart_*` copy, and
`gameLogic = new GameLogic()` (57). From then on `GameLogic` is the application object
(section 5).

**The state machine.** `GameLogic.changeActiveSection(GameState s)`
(GameLogic.java:880-890): `actualState.exit(s)`, `gc.flush()`, `actualState = s`,
`s.enter(old)`. `GameState` is an empty interface (GameState.java, both methods
commented out), so `enter`/`exit` resolve by name at run time. Fifteen classes implement
it (grep `implements.*GameState`): MainMenu, Transit, Garage, CarLot, CarMarket,
Catalog, CarInfo, ClubInfo, PaintBooth, Painter, EventList, RaceSetup, RocInfo, Track
(abstract; City, Valocity, TestTrack, ROCTrack) and MapDebug; 64 lines in the game
sources call `changeActiveSection(`.

**The frame.** The exe is single-threaded around a cooperative VM. One iteration of
`Engine_MainLoop`, in order (host mirror of the IDA call list, `System_natives.cpp:679-794`):

| Step | What it does | Script hooks fired |
| --- | --- | --- |
| `Input_tick(dt)` | poll DirectInput devices, smooth the 76 logical axes of every Controller blob | none directly; feeds `Controller.user_GetAxisVal`, `Input.checkHotkeys`, `Input.lastKey` |
| `Engine_SimulateFrame` | dt = wall dt x timeWarp, clamped to 0.1 s; tick the GII_CONTROL list; physics substeps; after each substep tick timers and drain event queues | `control(float)` on every GII_CONTROL registrant (new VM thread `THRD-RUNVMI <res>.control`); `handleEvent(GameRef,int,int)` for due timers |
| `Jvm_PumpFrame(10.0)` | bump GC generation, GC slice, then `Jvm_RunThreadsBudgeted`: give each runnable green thread a priority-scaled slice of a 10 ms budget; reap DONE threads | every parked script thread (`Thread.run`, animators, LoadingScreen, HotkeyWatcher) resumes here |
| `Engine_DispatchAnimateEvents` | call `animate()` on every GII_ANIMATE registrant | `Osd.animate()`, the one reader of `Input.lastKey` |
| `Engine_DrainEventQueues` | deliver queued events to watchers | `handleEvent(...)` of notification targets |
| Sfx listener pose + voice update | 64-voice table scheduling | none |
| `ResourceEngine_PumpLoadQueue` / `PumpUnloadQueue` | up to 16 loads per pump; update the 32-slot loading ring | `System.isLoading()` result |
| `GfxEngine_PresentFrame` (or AltPresent while a video plays) | walk bound viewports, render each camera's subtree, present | none |
| `notifyAll(Frontend.render)` | wake threads waiting on the GfxEngine object | `LoadingScreen.run` and every `Frontend.render.wait()` |
| AsyncLoad pumps, `PollWindowQuit`, `EndFrame` | file worker hand-off, WM_QUIT | none |

The host reproduces this order in the PE boot loop (`game_boot_part1.inc:752-769`):
`render_d3d9_flush()` (present and the Frontend.render notify), `render_d3d9_pump(0)`
(Win32 messages), `input_live_poll()`, `input_tick(dt)` with dt from `steady_clock`
clamped to 0.1, `engine_simulate_frame()`, `game_boot_mainloop_resource_pumps()`,
`jvm_run_threads_budgeted(10.f)`, `engine_dispatch_animate_events()`. Differences from
the exe: present is at the top of the iteration, the Sfx stages and the AsyncLoad pump
are not called, and `input_tick` runs twice per frame because `input_live_poll` already
calls it (`input_win32.cpp:1203-1208`). `Engine_SimulateFrame` is ported with the dt
clamp and substep rule (`System_part2.inc:1452-1527`); the max substep 0.05 s is a guess
(`System_part2.inc:1345`), the stock `*(physWorld+8)` is `[unknown]`.

## 4. The script layer

### 4.1 The dialect

A census over all 3,454 sources with comments stripped: 3,454 declare `package`, 3,438
use `extends`, 19 `implements`, 2 declare an `interface`. Absent entirely: generics,
try/catch/throw, static blocks, synchronized, labels, enums, anonymous classes, array
initialiser lists, multi-dimensional arrays, and the keywords `boolean`, `char`, `byte`.
Booleans are ints; objects and strings are used as conditions (1,392 `if(identifier)`
in 264 files; MainMenu.java:648 `if(message && message.text && message.text != "")`).
Resource ids are a literal of their own, `pack:0xHEXr` (9,953 uses in 677 sources; 810
in sl/Scripts/game/src), assignable to `ResourceRef` constructors and to `int` fields
(GameLogic.java:31 `new ResourceRef(cars:0x1000r)`, :62 `int SFX_ENTERGARAGE =
frontend:0x007Dr`). `switch`, `instanceof`, casts, loops, 1-D `new T[n]` arrays (1,814
uses) and `native` declarations (114 in 9 files) are in use. 23 sources declare more
than one top-level class and the compiler writes one TUFA blob per class into the single
`.class` (MainMenu.class holds 9 blobs, Dialog.class 11, City.class 8).

### 4.2 Sources and the on-demand compiler

Sources live at `X/src/Name.java` and compile to `X/Name.class`. The exe contains the
compiler: a flex scanner and yacc parser (`yacc stack overflow`, `fatal flex scanner
internal error`), `JVM::compileSource: Syntax error in "%s" at line %d`,
`JVM::compileSource: recompilation needed of "`, `CompileAll: begin file scan...`,
`CompileAll: success!`, and the path fragments `\src\*.java` and `\*.class` (all exe
strings). `System.compileAll(String)I` is a static native next to `openLib`
(System.class MTHD; `GameLogic.java:404-408` calls it in debug mode). Whether the exe
writes the recompiled class back to disk is `[unknown]`; the `recompilation needed`
message and the mixed build numbers on disk suggest it does, and a decompile of
`JVM_compileSource` in the 940 project or a before/after diff of a copied install would
settle it. The host has no compiler: `Jvm::compile_all` only walks directories and
loads existing classes (`jvm_load.cpp:312-325`), and a missing class logs
`[jvm] class file missing <path>` (`jvm_load.cpp:238,247,263`).

Classes are found through a fixed classpath of nine package directories in the exe:
`system\scripts\{lang,net,io,util,render,sound}`, `sl\scripts\game`, `parts\scripts`,
`parts\accessories\scripts` (exe strings). The host's `kClasspathMap` has eight
(`jvm_bridge.cpp:10-19`; `java.net` is missing). Part and car classes are not under any
classpath directory (17 `parts/*/scripts` and 51 `cars/*/scripts` directories, `find`);
the game reaches them by path from pack entries whose payload starts with `script
<path>` `[inferred]` from the `.rdb` payloads and `GameRef_part1.inc:1144-1225`; the PE
payload parser is not decompiled.

### 4.3 The class format (TUFA)

A `.class` is one or more concatenated blobs. Each blob: 12-byte header (`TUFA`, u32 4,
u32 build number), then chunks of 4-byte tag + u32 size (`tufa.cpp:285-325`):

| Chunk | Content |
| --- | --- |
| CONS | constant pool: utf8, int, 12-byte RID `{kind 3, pack-path utf8 index, local id}`, class ref, methodref/fieldref -> nat `{name, descriptor}` (`tufa.cpp:103-260`; the decoder is heuristic, kind-1 int-versus-char is `[unknown]`) |
| FILD | two vectors of 16-byte field records, statics then instance, each naming an optional initialiser TREE (`tufa.cpp:546-620`; the first word `w0` is `[unknown]`) |
| MTHD | 20-byte rows, static container then instance container; natives have an empty tree and flag 0x40 on instance rows (`tufa.cpp:476-540`; static-row words w3/w4 are `[unknown]`, so the host detects natives by the empty tree) |
| CLSS | five u32: name index, superclass index (0xFFFFFFFF for Object), interface count, then interface indices (`tufa.cpp:389-401`; the host drops the interface list) |
| TREE | per tree a list of 3- or 7-byte nodes `{op:u8, slot:u16[, imm:u32]}`; a 48-entry flags table (`engine/include/tree_op_flags.inc`, byte-identical in the 940 exe at 0x5B5260 per docs/VM.md) decides the size; at load the VM materialises 8-byte records and the PC strides 8 |

Methods and field initialisers are trees. `tools/tufa_dump.py` prints all of it
(`--method`, `--tree N`, `--no-cons`); every one of the 2,921 class files parses, and
the host never checks the build number.

### 4.4 Execution model

The interpreter is a PC-based port of the exe's `VMThread_run` (940 VA 0x562D70,
docs/VM.md table) in `jvm_vmthread_part1.inc` and `part2.inc` (4,483 lines). Ops at or
below 0x48 go through the 73-entry case map (`vm_case_map.inc:10`, 940 VA 0x564684);
the high opcodes are locals (0x1001-0x1003), literal 0x1007 with 47 sub-tags,
statement 0x1008 with 12 bodies, field path 0x1011, jumps 0x1014-0x1017, constant-pool
field ref 0x101B, arrays 0x101E/0x101F, and utf8-named calls 0x4025/0x4026
(`jvm_vmthread_part2.inc:1256-1500`). Frames: a call saves the caller PC, allocates a
64-byte CallFrame, puts `this` in local 0 and pops the arguments top-first
(`jvm_vmthread_part2.inc:1518-1579`); nested script calls switch frames on the same
thread instead of recursing, natives run inline as leaf frames. Method resolution is
by name with a scored overload search up the superclass chain (`Class_findMethodSlot`
940 VA 0x54A150, `compatScore` 0x5607D0; host `jvm_vmthread_part1.inc:488-825`); a tie
logs `Class::getMethod: more matching methods found` (exe string). Interfaces are not
modelled by the host loader, so interface-typed parameters get a weak score
(`jvm_vmthread_part1.inc:598-617`). Strings: the exe interns every String payload, so
`==` on strings is content equality (docs/VM.md "String equality"); the host compares
text when both operands are registered strings (`jvm_vmthread_part1.inc:1540`).
Arrays are 1-D only (exe string `Thread::run: multi-dimensional arrays not supported!`),
elements are assignable references, and primitive `int[]`/`float[]` live in a typed side
table (`jvm_vmthread_part1.inc:1369-1452`).

### 4.5 Threads and the pump

Every script entry point runs on a green thread: `GameInit`, `THRD-RUNVMI <res>.<method>`
(named calls from the engine), `THRD-CREATE` (GameType construction),
`THRD-CLASSVAR-INI` and `THRD-INSTVAR-INI` (field initialisers), `THRD-FIN` (finalisers)
(all exe strings). A VMThread is 56 bytes with flag bits 0x1 sync, 0x2 armed, 0x4
RUNNING, 0x8 SLEEP, 0x10 WAITING, 0x20 SUSPENDED, 0x40 DONE, 0x80 STOP
(`jvm.hpp:258-265`). `Thread.start` never creates an OS thread; `Thread.sleep` sets a
deadline, `Object.wait` parks with 0x10, `notify` pops one waiter LIFO
(`IO_part2.inc:667-1035`). `Jvm_RunThreadsBudgeted`: slice 10 ms, skip flags & 0x82,
run when !(flags & 0x44), scale = prio*9.9+1 for prio >= 0 (priority 10 runs 100x),
DONE and not sync -> STOP, STOP and not RUNNING -> destroy
(`jvm_vmthread_part2.inc:2009-2121`). The host adds a 50 ms wall cap
(`SLRR_PE_PUMP_CAP_MS`) and a re-entry guard; the exact 940 scaling constants and
whether the exe has any wall cap are `[unknown]`. GC slice and mark-sweep are empty
stubs in the host; nothing on the menu or garage path needs them.

### 4.6 Natives: how a call crosses into C++

A native is found in a 128-bucket hash keyed by `73 * sum(method bytes) + sum(class
bytes)`, then matched by class, method and interned descriptor (`jvm_register.cpp:29-60`).
The host table `kNativeTable` (`engine/Core/natives_stubs.cpp`, generated) has 377
entries over 37 classes (`grep -c`): upstream's 376 plus `Object.finalize`. The receiver
is passed as the object pointer; only integer-typed parameters receive the `Native.ptr`
handle, and only when the argument's class inherits `java.lang.Native`
(`jvm_vmthread_part1.inc:121-204`); a generic 16-word cdecl thunk carries the call
(`callinfo.cpp:641-680`). Of the 395 distinct natives declared in the 940 system/sl/parts
classes, 54 have no host binding: 16 on Chassis (section 9), all of Steam (5), Replay
(2), Debug (3) and NetworkEngine (1), 6 Sound track natives, 7 GroundRef, 5 Math, 3
Object, `String.getASCII`, `System.memUsage`, `Text.setScale`, `GfxEngine.renderOK` and
`Vehicle.fileVersion` (MTHD rows versus the table). 21 host entries are not natives in
940 (`Object.hashCode`, `Text.setDefaultColor`, `RenderRef.getPos`, ...).
Two misdiagnoses to retire from TASKS.md: `String.getParams(Vector)` is a static script
method in 940 (String.class MTHD, tree 20; the exe has no `getParams` string), so the
`null.trim()` failure in `GameLogic.updateCodeROC` is a resolution problem on a String
receiver, not a missing native `[inferred]`; and `Vehicle.fileVersion` is declared native
(Vehicle.java:143) but the exe contains no `fileVersion` string, so the stock game cannot
bind it either.

### 4.7 Events, timers, callbacks, errors

`GameType` (system/scripts/lang, compiled only) is the base of every engine-bound script
object. `registerCallback(mode)`: 8 GII_CONTROL -> a sim-callback node ticked by
SimulateFrame -> `control(float)`; 9 GII_DRIVE -> a second list; 28 GII_ANIMATE -> a
third list -> `animate()` once per frame (`GameType.cpp:129-132, 743-820`; values
recovered from GameType.class static initialisers). `addTimer(deadline, id)` makes a
one-shot node fired by `Engine_TickTimers` into `handleEvent(GameRef, EVENT_TIME, id)`
when the GameType's event mask allows it (`GameType.cpp:570-650`). `addNotification`
installs a Watch on another GameRef with an event type mask, an alias remap and an
optional custom message or method (`GameType.cpp:232-330`). Event constants live in
GameRef.class statics: EVENT_TIME 0x80, EVENT_CURSOR 0x10000, EVENT_HOTKEY 0x100000,
EVENT_ANY 0x0FFFFFFF. A script error in the exe logs and the thread continues with null
(`Thread::run: [illegal methodcall] null.%s() type:%s`, `Thread::callMethod: "%s%s%s"
not found`, `Thread::callmethod: native implementation missing for "%s.%s"`, exe
strings); the host unwinds the failing frame, pushes null and resumes
(`jvm_vmthread_part2.inc:857-880`). Host status for all of this: callback lists for
CONTROL and ANIMATE are real, DRIVE is a bitmask that nothing dispatches, timers and
watches are per-GameType vectors on the host clock rather than engine lists on the
physics clock.

## 5. The game as written in the scripts

**Frontend** (`java.render.Frontend`, compiled) is a static holder: `render` (the
GfxEngine object), two LoadingScreens, the hotkey watcher thread, `inputQueue` (the Osd
focus stack), the Steam object and eleven font `ResourceRef`s chosen by resolution in
`setFonts()` (tufa_dump CONS). `Frontend.class` bundles a second class,
`java.render.HotkeyWatcher`, whose `run()` sleeps 50 ms and calls
`Input.checkHotkeys(Input.cursor.controller, inputQueue.lastElement())`.

**GameLogic** (GameLogic.java, 1,366 lines) is a static singleton. Its constructor
scans packs (`System.rpkScan` of `parts\engines\`, `parts\`, `cars\racers\`,
`cars\fake_racers\`, `maps\`, `sl\Scripts\game\CareerEvents\` and every
`multibot\maps\<dir>\`, lines 345-358), builds the 25 vehicle types from the children
of `VEHICLETYPE_ROOT = cars:0x1000` (line 31, 595), creates the Garage, one CareerEvent
per child of `EVENT_ROOT = system:0x2000` (59 pack entries) and a freeride template per
child of `MAP_ROOT = system:0x3000`, starts the "Game time refresher" thread, arms a 10 s
timer (each tick adds `timeRefreshRate * timeFactor` = 40 game seconds, line 528; days
roll at 86,400) and ends with `changeActiveSection(new MainMenu())` (line 442). Career
constants: 60 racers (3 clubs of 20), $25,000 start (line 37), prestige 0.3, ROC entry
fee 100,000, 22 car colours, 5 cheat words checked by the native
`GameLogic.kismajomCheck(String[])` (line 521; exe string `kismajomCheck`).

**Main menu.** `MainMenu.enter` shows a `Gates` dialog (MainMenu.java:30, `Gates extends
Dialog`): a full-screen Osd with the background video (`GfxEngine.openVideo(videoData,
1, 1, 1)`, line 158, "supports fit to screen since build 940"), a `SlidingMenu`, logo
rectangles animated on a THOR method-queue thread, and one hotkey `Input.AXIS_SELECT`
(line 181) bound to itself. `Gates.osdCommand(CMD_NEW_CAREER = 0xA006)` runs a modal
`StringRequesterDialog("Enter player name")` (line 418), then `loadDefaults()`,
`updateCodeROC()`, `autoSaveQuiet()` to `save/career/<name>-<club+1>/` and
`changeActiveSection(GameLogic.garage)`. Twelve sliding-menu items run from LOAD CAR to
EXIT GAME (lines 272-290).

**OSD classes.** The UI is script over five natives classes. `Osd` (java.render, 84
methods, 67 KB class) owns one `Viewport` and one `Camera`; `Osd.show()` creates
`new Camera(this, vp, pri, CAM_AOV = 60, 0.1, 10.0)` at `z = CAM_OFFSET (5.48) *
vpHeight` and activates the viewport; `Osd.hide()` deactivates it and destroys the
camera (Osd.class trees 91-92). Groups are `Dummy` GameRefs: `Group.activate()` clears
`WORLDTREELEAF` (0x40), `deactivate()` sets it (Group.class trees 5-6). A `Rectangle` is
a render instance of a `RectangleTemplate`, which duplicates the frontend "etalon" type
(`frontend/meshes/etalon_negyzet_alpha.SCX`, 328 bytes, a 100 x 100 quad), bakes the
size with `scaleMesh(w, h, 1)` and swaps the texture. A `Text` is `Text.create(parent,
charset, x, z)` on a font mesh. `Gadget`, `Button`, `Menu`, `SlidingMenu`, `Slider`,
`StringInput` and the 13 `Dialog` classes compose these (39 compiled classes under
system/scripts/render). `Osd.darken()` is a `Text` of the string "a" with the charset
`fade.SCX`, so the dark curtain is one glyph of a one-glyph font (Osd.class tree 75).

**Dialogs and input flow.** `Dialog.display()` shows, waits on the dialog object and
returns `Dialog.result`. A hotkey press reaches the script as `Input.checkHotkeys ->
EVENT_HOTKEY -> Osd.handleEvent(Hotkey)` on a new VM thread, which either runs a private
menu command or `hk.handler.osdCommand(hk.command)` (Osd.class tree 87). Typed text
reaches `StringInput.key` through `Osd.animate()` reading `Input.lastKey` (tree 86,
node 294). The host reproduces this chain end to end: ENTER, ENTER, a name, ENTER puts
`GameLogic.actualState` at `java.game.Garage` in 40 s (docs/VM.md session 4).

**Garage** (Garage.java, 2,214 lines). `enter()` loads one of four garage GroundRefs by
club, creates the vehicle lift (`misc.garage_objects:0x13` plus a PhysicsRef box, line
221), the Osd menu (16 commands from HITTHESTREET to MAINMENU, `CMD_*` 101-133), the
MOVE PARTS slider group, a `Mechanic` for drag-and-drop part installation (line 266),
and the 3D camera: `camera = new GameRef(map, GameRef.RID_CAMERA, "<pos>,<ori>, 0x13, 1.0,1.0,
0.05", ...)` (line 248) driven by `cam.command("render <vp id> 0 0 1 <flags>")` (line
499). Note that the garage camera is a `camera` GameType (a `native camera` entry,
system.rdb typeid 0x33), not a `java.render.Camera`.

**Driving.** `Track` (2,955 lines, abstract Scene) owns cameras, triggers and the
in-game menu; `City` (3,870 lines) adds night races, police and traffic; `Valocity`
loads `maps.city:1` with a Navigator minimap and traffic via `map.addTraffic`. Career
races go `EventList -> new Track(careerGM, event, trackData)` with one of 8 `Gamemode`
classes driving the race (Gamemodes/stdpack_231.rdb, 8 entries) over 21 `track_data`
scripts (find `multibot/maps/*/data/src/track_data.java`).

**How a car is built.** `new Vehicle(parent, rid, colorIndex, optical, power, wear,
tear)`: `chassis = create(parent, GameRef(rid), "0,0,0,0,0,0", "Vehicle")` instantiates
the car's chassis script (e.g. `Einvagen_110_GT extends Einvagen_models extends Chassis
extends BodyPart extends Part extends GameType`), then `chassis.addStockParts(new
Descriptor(colour, optical, power, wear, tear))` (Vehicle.java:91) and
`chassis.forceUpdate()` (93). `Chassis.addStockParts` picks stock, stage-1 or stage-2
`int[]` rid lists by power thresholds 1.333 and 1.667 (Chassis.java:129-130) and calls
`Part.addPart` for each, which does `GameRef.create(...)`, `setWear/setTear`, the native
command `"install 0 <car> 0 <parent> 0"` (Part.java:847) and recurses. `forceUpdate`
hands the finished tree to the native physics (section 9). 418 of the 635 source-only
classes are car body and light panels and 201 more are wheels, running gear and engines,
so a car built on the host today loses those parts: the classless entry becomes a plain
GameRef and `setWear/setTear/setTexture/addStockParts` report "not found" (TASKS.md "In
progress 2").

**Career and save files.** `GameLogic.save(dir)` writes `dir/main` (SDAT, 0x87654321
version 23: player, 59 bots, dealer stocks, time, day), `careerData/data`,
`eventData/data_v_N`, `trackData/data_v_*` and `PlayerCarN` (0x88664422 version 5: name
then the recursive Part tree, Vehicle.java:17-18) staged through `save/temp/`
(GameLogic.java:942-1225). A profile in the install (`save/career/<name>-1/main`) starts
`SDAT 00 01 02 00`, `21 43 65 87`, `17 00 00 00` (xxd). `PlayerCarN.1` is a JPEG and
`PlayerCarN.2` an `INVO` blob; which native writes them is `[unknown]` (Painter.java
only calls `car.saveSkin`, which writes an SDAT list).

## 6. Resources and data formats

**Packs and rids.** An RPK starts `RPAK`, u32 version (0x200 in every sampled pack),
two limits, then 0x40-byte dependency records (`frontend.rpk`: `system.rpk`,
`misc\garage.rpk`, `cars.rpk`), then a table of contents of records `{parent id, local
id, restype u8, flags u8, file offset, size, name}` (`rpak.cpp:155-240`; `frontend.rdb`
shows the same records as `typeof / superid / typeid / alias` blocks with the payload
text). A full resource id is `(pack slot << 16) | local id`; `System.openLib(path)`
returns the slot (`System_natives.cpp:441-498`; Catalog.java:300-306 compares
`id() >> 16` with it). A cross-pack parent is written with the dependency slot in the
high word (`0x00010008`). Script rid literals compile to the 12-byte CONS entry (pack
path, local id) and are resolved at run time through `ResourceEngine_RemapFromPath`
(`jvm_vmthread_part2.inc:538-556`), so a pack's slot can differ per run. Saved games
store packed ids (`File.write(ResourceRef)`, `main_part3.inc:1265-1272`), so whether the
engine's slot order matches the host's open order matters; it is `[unknown]` until a
stock-written save is diffed against a host-written one. Build 940 opens 126 packs at
boot; the host's slot table was raised from 64 to 1,024 after rid literals past pack 64
resolved to zero (TASKS.md f138803).

**The resource tree and types.** `typeof` is the `RESTYPE` enum of `ResourceRef`
(static initialisers in ResourceRef.class): 1 INSTANCE_GAME, 3 INSTANCE_RENDER, 5
RESOURCE_MESH, 6 RESOURCE_SOUND, 7 RESOURCE_TEXTURE, 8 RESTYPE_GAME, 9 PHYSICS_BODY, 12
RENDER_CAMERA, 13 RENDER_LIGHT, 14 RENDER_OBJECT, 16 INSTANCE_RENDER_TEXT, 18 VIEWPORT,
22 MAX. A typeof-8 entry whose payload has a `script <fqn or path>` line is a scripted
GameType, optionally paired with a `native <type> [cfg]` line (`parts.rdb`: `script
java.game.parts.accessories.stock_Battery_black` / `native part <cfg>`;
`misc/garage_objects.rdb`: `native part ...lift_support.cfg` then `script
...Lift_support.class`). Pure native types are declared the same way:
`camera` is `native camera` (system.rdb typeid 0x33), `cursor` `native cursor`
(frontend.rpk). The host honours only a payload whose first line begins `script `
(`GameRef_part1.inc:1188`) and otherwise guesses `java.game.<alias>`, which is where
the spurious `[jvm] class file missing camera / cursor / lift_support/cfg` lines come
from `[inferred]`: no `camera.java` or `cursor.java` exists anywhere in the install, and
all 635 source-only classes are under `cars/` and `parts/` (census), so these are not
compiler misses. Fixed script-visible roots: `WORLDTREEROOT` flag 0x10,
`RID_DUMMY = system:0x1B`, `RID_CAMERA = system:0x33`, `RID_CURSOR = frontend:0x2A`
(GameRef.class statics); the world tree root used by the host boot is `system.rpk`
local 4 (`game_boot_part1.inc:63`). The 20 `ResourceRef` natives walk and bind the tree
(`load`, `getFirstChild`, `getNextChild`, `getParent`, `duplicate`, `scaleMesh`,
`makeTexture`, `makeSound`, ...); the host keeps a `ResState` per Java object and loads
synchronously instead of the exe's handle-plus-node and async queue.

**Meshes.** `.scx` files are Invictus `INVO` containers. Version 4 (parts, cars) is
chunked: material, meta, vertex block (stride 24-40: position, normal, one UV), 16-bit
index block (`render_d3d9_part3.inc:999-1140` at HEAD). Version 3 (skydome, older
parts) has a header of at least 0x80 bytes and 64-byte vertices (1142-1280). Font SCX
files are version 3 with header 0x38, 1,024 44-byte vertices = 256 glyph quads indexed
by ASCII code, each cell 100 units wide and stacked at `z = -100 * code`. The exe also
knows bones (`bone00`, `wheelbone`, `numbones`, `.bon` sidecar files, 19 in the install),
LOD levels (`lods`, `lod_amp`, `lod_bias`) and 17 material classes (`CMaterial_Tex`,
`_TexSpheremap`, `_TexBump`, `_Water`, ...), none of which the host parses; the encoding
of INVO v4 chunk type 4 and of the bone/LOD/material data is `[unknown]` (decompile the
exe's INVO chunk walk). Units: SCX vertices are centimetres, scripts pose in metres, and
the host applies `kScxCmToM = 0.01` in the camera pass (`render_d3d9_part1.inc:1192` at
HEAD; the 100-unit etalon quad covers a `scaleMesh(3.92, 2.94)` background exactly at
the Osd camera). Where the exe itself folds the factor is `[unknown]` (host comments
record `*10` on camera planes and `*0.1` on text positions).

**Textures.** Payload `sourcefile <path>` plus `flags <n>`, pointing at DDS, PNG, TGA or
PTX (a 36-byte header followed by JPEG mip levels, the exe's own format). The exe loads
through `d3dx9_32.dll` (import) plus a bundled libjpeg; the host resolves `d3dx9_43.dll`
down to `d3dx9.dll` at run time for DDS and uses WIC for PNG/JPEG
(`render_d3d9_part2.inc:416-444, 1095-1230` at HEAD). The 866 `.tex` files under
`objects/meshes` list texture names for the map-object toolchain; no `.rdb` references
one and the exe has no `.tex` string, so they are not a runtime load list `[inferred]`.

**Fonts** ship twice, and both halves are used: `frontend/meshes/Fonts/*.scx` (15 glyph
meshes of 51,272 bytes each plus `scaleable/`) and `frontend/textures/Fonts/*.png|tga`
(the atlases they sample), joined in `frontend.rpk` by a typeof-14 render object per
font (`mesh 0x4A / flags 4100 / texture 0x26` for slii24, frontend.rdb). `frontend/
font.dat` (135 bytes) is the charset. A `Text` in the exe is a render instance
(`r_text`, `INSTANCE_RENDER_TEXT`, exe strings) of that render object; this is why the
UI pass in TASKS.md builds texts from the font SCX rather than from atlas pixels.

**Audio and video.** Sounds are typeof-6 entries with `sourcefile` WAV payloads played
through `SfxRef.nplay`; music is MP3 in six `Music\*` set folders (exe string
`Music\Garage_Shop`); the only videos are the two menu backgrounds. **Configs:** 3,194
`.cfg` files (part, particle, human, frontend; none at the root); the game options are
the SDAT file `save/game/options`, not a `.cfg`. `graph.cfg`, `graph_fade.cfg` and
`clouds/asphalt.cfg` end without a newline, the shape behind the stock tokenizer overrun
in BACKGROUND.md section 1; whether the host's `.cfg` reader copes is `[unknown]`.
**Saves:** every script-written file starts `SDAT 00 01 02 00`; the control set is
`CTRL` version 16 (2,560 bytes, three devices); formats are in section 5.

**Loading protocol.** `ResourceEngine_PumpLoadQueue` runs once per frame, completes up
to 16 loads, and updates a 32-slot ring of per-pump counts; `System.isLoading` is
`sum * 0.03125 > 4.0`, and `System.isLoadingReset` seeds the ring so the loading screen
stays up for about 24 pumps (`Resources_part2.inc:966-1002` at HEAD, docs/VM.md session
3). `LoadingScreen.run` loops `Frontend.render.wait()` (an `Object.wait` on the GfxEngine
object, notified once per present) while `isLoading()`, then `hide()` notifies the
caller. The host ports the ring and the notify; actual loads are synchronous.

## 7. Rendering and the OSD

**In the exe.** Scripts never touch Direct3D. The device is fixed-function D3D9 through
`d3dx9_32.dll` (20 D3DX imports); the exe also names five vertex shaders
(`skinmesh1_diffenvmap.vsh` ... `diff_envmap.vsh`) that are not in the install, so the
"no shaders" line in BACKGROUND.md section 1 is right for what ships and whether a
fallback exists for skinned envmapped meshes is `[unknown]`. The script-visible surface
is `GfxEngine` (13 natives in the 940 class: present pumps, display modes, `printScreen`,
`openVideo` x2, `closeVideo`, `renderOK`, global envmap), `Viewport` (11: a normalised
rect and priority; `RENDERFLAG_CLEARDEPTH` 1, `CLEARTARGET` 2), `Camera` (5: `create`
with half aov, dmin, dmax, LOD bias and amp; `activate`, `deactivate`, `setFog`), `Text`
(8) and `RenderRef` (`create(parent, type, alias)` clones a render type under a node,
`setMatrix(pos, ori)` poses its `bone00`, plus colour, light, flare, `changeResource`,
lines and routes). Every instance lives in the resource tree (RTTI `RenderInstance`,
`CameraNode`, `ViewPortNode`; flags `RIF_WORLDTREEROOT` / `RIF_WORLDTREELEAF`, exe
strings). Each frame the present path walks the bound viewports in priority order and
each camera renders the subtree under its parent with its half-aov/dmin/dmax
projection. A deactivated Group's `WORLDTREELEAF` flag is what hides its subtree
`[inferred]` from the script usage and the flag name; whether the walk stops at the node
or skips it is `[unknown]`.

**The UI is geometry under a camera.** Rectangles are 100 cm quads scaled in script to
metres at `z = pri / 1000`, viewed by the Osd camera at `z = 5.48 * vpHeight` with a 30
degree half angle, near 0.1, far 10; texts are instances of the font mesh. Nothing is
screen-space; a hidden Osd is a deactivated viewport whose camera was destroyed.

**What the host draws today** (committed HEAD; the working tree has an in-progress UI
pass in `render_d3d9_part*.inc`, `RenderNatives.cpp` and `Resources_part2.inc` that this
document does not describe). `render_d3d9_flush()` (`render_d3d9_part1.inc:2069-2153`)
clears, begins the scene, draws the video quad, runs the legacy auto-framing
`draw_meshes()`, then the per-viewport camera pass `draw_viewport_cameras()` (active
viewports sorted by priority, membership by shared root through `mesh_root_of`, camera
world inverted as the view, geometry scaled by 0.01; lines 1342-1488), then
`draw_camera_texts()` (atlas glyph quads in world space), flare sprites, the legacy
screen-space `draw_osd_rects()` and `draw_osd_texts()`, ends the scene, presents and
notifies `Frontend.render`. Reached state: the main menu draws correctly (video, banner,
background, sliding-menu icons, logos, texts) and the Garage state renders its OSD with
211 meshes, 168 textures, 9 cameras, 29 viewports and 46 texts (docs/VM.md session 4).

Known defects at HEAD, all in TASKS.md step 1: hidden state is not honoured (nothing
reads `WORLDTREELEAF`; `GameRef.setFlags` special-cases only 0x10,
`GameRef_core.cpp:725-750`), so the main-menu dialog texts stay on the Garage frame;
texts are atlas quads, not instances, and `Text.setScale` has no native; `fade.SCX` is a
plain INVO v3 mesh (header 0x88) but `Text.create` routes every charset through the font
loader and falls back to `simple20` (`RenderNatives.cpp:511-512` at HEAD), so the darken
curtain draws as the letter "a"; the dialog subtree draws rotated 180 degrees (cause
`[unknown]`: the Ypr convention in `resolve_world`, the -Z view, or a native consumer of
`Osd.orientation`, a field no exe string names); and no 3D garage is drawn because the
GameRef `render` command is parsed in its old three-number form into stand-ins and never
binds a host camera (`GameRef_part1.inc:1646-1746`); Build 940 sends five numbers and
the exe has a `Render %d, %d, %d, %d, %d` format whose fourth and fifth are `[unknown]`.

## 8. Input and controls

Devices are DirectInput8: `SysKeyboard` (type 1, 256 DIK axes), `SysMouse` (type 2,
absolute X/Y/Z, four buttons, relative axes) and joysticks (type 3, with three canned
force-feedback effect slots) (exe strings; `input_win32.cpp:100-200`). A `Controller`
owns a 0x2148-byte device blob: 76 logical axes smoothed every `Input_tick` and a
152-entry feedback map (`input_win32.cpp:29-37, 1053-1120`). The control set file
`save/controls/active_control_set` (SDAT `CTRL` version 16) maps physical axes to
logical ones; `ControlSet.load -> Controller.add -> user_Add` fills the 152-slot axis
map (`IO_part1.inc:563-611`). Logical axis ids the menu uses: 34 SELECT (ENTER, SPACE,
NUMPADENTER), 35 CANCEL (ESC), 55-58 arrows (`input_win32.hpp:60-88`; confirmed by ENTER
reaching `Gates.osdCommand(34)`); the other `Input.AXIS_*` values have not been dumped
from Input.class `[unknown]`. Hotkeys: a 512-slot table; `Input.createHotkey` fills a
slot, `checkHotkeys(controller, osdInFocus)` samples each slot whose owner is null or in
focus, treats a value above 0.2 as down, and on an edge queues `EVENT_HOTKEY` (0x100000)
to the handler (`IO_part2.inc:355-600`), whose `handleEvent(Hotkey)` runs on a fresh VM
thread. `Input.lastKey` returns `DIK scan | (ToAsciiEx char << 16)` from the keyboard's
event buffer and appends the character to a 16-byte cheat ring that `kismajomCheck`
searches backwards with each letter shifted by one (`IO_part2.inc:326-343`;
`IO_part1.inc:117-159`; GameLogic.java:147). The mouse is the script
`java.io.MouseCursor` wrapping a native cursor fed by WndProc NDC coordinates and a scene
ray pick, exposed as `getPos`/`getPickedPos` and `EVENT_CURSOR` (0x10000) events to Osds
(MouseCursor.class). Stock force feedback is three `IDirectInputEffect` slots per
joystick started only when `Input_forceFeedbackEnabled` is set; `FFB_strength_emulated`
only smooths digital axes (BACKGROUND.md section 1; `input_win32.cpp:1091-1130`).

Host: keyboard and mouse only, background non-exclusive mode (`input_win32.cpp:266`)
with a fork-only foreground gate so unfocused test runs do not read the user's typing
(`input_win32.cpp:720-737`); no joystick enumeration, no effect creation, `setFeedback`
returns 0; axis smoothing, the 152-slot map, the hotkey table, `lastKey` packing and the
cheat ring are ported; `SLRR_PE_BOOT_KEYS="t:dik,..."` injects presses for unattended
runs (`game_boot_part1.inc:771-813`). MouseCursor has the two 0-argument natives and a
`Cursor_tick` stand-in that picks OSD gadgets by bounding box instead of a scene ray
(`Cars.cpp:621-760`); the `(Vector3)` overloads are not in the table.

## 9. Vehicles, physics and sound

**The native contract** is listed in FORK.md ("The native contract we must honour") and
is fully present as host symbols: `WheelRef` 36/36, `Chassis` 23/23, `PhysicsRef` 10/10,
`Vehicle` 3/3 (`grep` over `engine/Runtime`). Build 940 widened it: the 940
`Chassis.java` declares 39 natives (compiled class) against 21 in the old class and 23
registered by the old exe; the 16 with no host binding are `getRPM setRPM getMaxSteer
setMaxSteer getShifting getNitro getNitroing getEngineTemp getAngvel getMaterialIndex
getAIparam_float getAIparam_int setAIparam_float setAIparam_int getOSD setOSD` (grep of
natives_stubs.cpp; the exe registration block lists them). `WheelRef` and `PhysicsRef`
did not change between builds (31 and 9 declared natives in both; 36 and 6 registered).

**What the exe does.** `Chassis.forceUpdate` (old-exe 0x43E320 -> 0x448430) first calls
the script's `updatevariables`, clones a 3,100-byte header, walks the physics child list
ingesting each part, copies engine scalars by field name (`engine_mass * 8000`,
`rpm_idle * pi / 30`), rebuilds the torque table from `DynoData`, binds the engine,
exhaust and gearbox SfxTables, writes drag centre and `C_drag`, rebuilds the part slot
table and ends in a tail (`Chassis_part2.inc:352-368, 636-958`). Per-wheel state is a
0x2B4-byte slot with 17 Pacejka floats at +0x1E8 (`world_state.hpp:20-27`). The
simulation is `Physics_Step` (0x4A5190, solver loop max 20) and `Chassis_physWheelTick`
(0x454500; brake scale 0.2, aero `F = -C_drag * |v|^2 * v_hat`), called from the
SimulateFrame substep loop (FORK.md roadmap step 2 bookmarks). Bodies come from
`PhysicsRef.create/createBox/createSphere` (box type 5 with half extents, sphere type
1). Map interaction is `GroundRef` (32 natives in 940: nearest cross, align to road,
route splines, a 356-byte traffic car pool). Engine sound is native: three SfxTables of
16 samples (`sfx_engine_up/down`, `sfx_exhaust`, `sfx_gearbox_fwd/rev`, `sfx_horn`,
`sfx_gearchange`, exe strings) played from the physics tick; the rest of the audio is
DirectSound8 with a 64-voice table scheduled once per frame and MCI music
(`Sound.cpp:14-80`; `audio_win32.cpp:37-93`).

**What the host has.** The bookkeeping half of `forceUpdate` is ported as side-band maps
(header clone, slot table, engine scalars, DynoData lerp, SFX binds, drag) with no body
behind it (`Chassis_part2.inc`, 32 OOS lines). All 36 WheelRef setters store into
`g_chassis_phys_wheels[chassis][8]` and nothing consumes them (`WheelRef.cpp:23-45`).
Motion is a Soft arcade model: semi-implicit Euler with `kGravity = 25` on a flat plane
plus pairwise collisions (`Resources_part4.inc:150-256`) and a velocity-clamp
`physics_drive` called only by the smoke harness and the legacy Valocity shim
(`Resources_part4.inc:318-400`). There is no map geometry: the only ground is
`g_ground_y` plus road segments added by test fixtures (`physics_road_add_segment`
callers are all in `main_part1/2/3.inc`). GII_DRIVE registrants are never dispatched (no
`"drive"` call site in `engine/`). Audio: `audio_win32.cpp` opens DirectSound8 and MCI,
but the `Sound` and `SfxRef` natives are voice-table mirrors that never call it
(`Sound.cpp:698-745`); 6 of 14 Sound natives are missing (`firstTrack`, `randomTrack`,
`playTrack`, `getTotalTracks`, `getNowPlaying`, `stopMusic`). Video: a DirectShow graph
into a D3D texture draws the menu background; only the 3-argument `openVideo` is
registered, and the 4-argument 940 call most likely lands on it through the by-name
fallback in `jvm_part2.inc:995-1025` with the fit argument dropped `[inferred]`.

**Out of scope today**, by upstream's own OOS markers: rigid bodies, the solver,
inertia, the wheel tick, map collision, traffic path following, FFB effect creation,
GC. FORK.md puts new physics in `engine/Runtime/Physics/` behind the contract above.

## 10. The host today

Build: CMake, C++17, one target `slrr_engine` from 43 `.cpp` files whose large bodies
are `*_partN.inc` fragments (README "Engine splits"); the configured generator is
`Visual Studio 18 2026`, platform Win32 (`engine/build/CMakeCache.txt:233,237`; README
and FORK.md still say VS 17 2022); build with `cmake --build engine/build --config
Release --parallel`. Source: 103 files, 83,166 lines (`find | wc`). Fidelity markers
(lines, `grep -c`): PE 3,934 (word match), Soft 1,465, OOS 401, `Fork:` 185. Native
table: 377 entries. The fork is 42 commits over upstream 5c42f3b.

| Subsystem | State | One-line note | Evidence |
| --- | --- | --- | --- |
| TUFA decoder | faithful | all 2,921 classes parse; CONS heuristic, interfaces dropped, build number unchecked | `tufa.cpp:103-620` |
| Class loader | partial | 8 of 9 classpath entries (no `java.net`), cars wildcard, bundled-sibling scan, path loads from `script` payloads | `jvm_bridge.cpp:10-19`, `jvm_load.cpp:63-264` |
| Source compiler | missing | 635 of 3,454 sources have no class; `compile_all` only scans | `jvm_load.cpp:312-325`, exe compiler strings |
| VMThread interpreter | faithful | 73-entry case map, frames, locals pop, jumps, literals, arrays; missing 0x1008 slots 0-2/7-9, op29 on stream | `jvm_vmthread_part2.inc:976-1510` |
| Method resolution | faithful | Ghidra-ported scoring, super walk, typed nulls, (owner, method) slot | docs/VM.md table; `jvm_vmthread_part1.inc:488-825` |
| Object/field model, strings | soft | untyped field bags read through declared types; no PE Value layout, no refcounts; strings compared by text | `jvm_vmthread_part1.inc:1540, 1712-1866` |
| Green threads | partial | cooperative only with `SLRR_PE_BOOT_INIT=1` or `SLRR_PE_GREEN=1`; legacy path uses OS threads | `IO_part2.inc:658-1035` |
| Budgeted pump; GC | faithful; missing | PE formula plus a 50 ms wall cap and re-entry guard; GC slice and mark-sweep are stubs, objects are never freed | `jvm_vmthread_part2.inc:2009-2145`; `part1.inc:206-222` |
| Native table and thunks | partial | 377 entries, PE hash keying, 16-word thunk; 54 of 395 declared 940 natives unbound | `jvm_register.cpp`, `callinfo.cpp:641-680` |
| Events / timers / notifications | soft | per-GameType vectors on the host clock; semantics mirrored | `GameType.cpp:24-130, 400-700` |
| Callbacks | partial | CONTROL and ANIMATE lists real; DRIVE never dispatched | `GameType.cpp:743-820`; no `"drive"` call site |
| Script errors | partial | nested unwind to null as the exe; fatal ops end the thread; outermost failure falls back to `tree_eval` unless strict | `jvm_vmthread_part2.inc:857-880, 1581-1700` |
| Legacy `tree_eval` + `Jvm::invoke` hooks | soft | 5,495 lines kept as fallback; Vector/queueEvent/create/addTraffic hooks still in the call path | `jvm_part2.inc:1250-1420` |
| RPK loader | partial | header, deps, TOC, remap table for all 146 packs; pack id = open order, whole file in RAM, transform floats skipped | `rpak.cpp:155-240` |
| Resource tree walk | partial | TOC links with remap-resolved parents plus runtime children | `Resources_part2.inc:1455-1560` (HEAD) |
| ResourceRef natives | soft | 20 bound; `ResState` per object, synchronous load, no destroy queue | `Resources_part2.inc:1183-1700` (HEAD) |
| Scripted GameType creation | partial | `script` first-line payloads only; `native X` and two-line payloads fall back to `java.game.<alias>` | `GameRef_part1.inc:1185-1221` |
| Meshes and textures | partial | INVO v4/v3 positions, normals, one UV, u16 indices; no bones, LOD, material classes; textures by filename heuristic; DDS via runtime D3DX, PNG/JPEG via WIC, PTX last level only | `render_d3d9_part3.inc:999-1296`, `part2.inc:416-444, 1095-1230` (HEAD) |
| Fonts and Text | soft | atlas glyph path; `Text.setScale` missing; non-font charsets fall back to simple20 | `RenderNatives.cpp:500-716` (HEAD) |
| Present walk | soft | three legacy passes around the Fork camera pass; Present first in the host, last in the exe | `render_d3d9_part1.inc:2069-2153` (HEAD) |
| Viewports / cameras | partial | rect, priority, clear flags, camera pass; single active viewport/camera globals, no LOD/occlusion, one global fog | `RenderNatives.cpp:159-300, 718-900` (HEAD) |
| Hidden state (WORLDTREELEAF) | missing | nothing reads 0x40 | `GameRef_core.cpp:725-750` |
| GameRef camera (`render` command) | missing | three numbers parsed into stand-ins; no host camera bound | `GameRef_part1.inc:1646-1746` |
| Main loop (PE boot) | partial | PE order minus Sfx/AsyncLoad; `input_tick` twice | `game_boot_part1.inc:742-769` |
| SimulateFrame | faithful | dt clamp, warp, substep pick, control tick, timers; substep 0.05 is a guess | `System_part2.inc:1452-1527` |
| Window / device | soft | fixed 1024x768 windowed, no vsync, software vertex processing; `Config.video_*` never applied | `render_d3d9_part1.inc:655-776` (HEAD) |
| DirectInput | partial | keyboard + mouse, background mode with foreground gate; no joysticks | `input_win32.cpp:245-331, 720-737` |
| Controller / ControlSet / hotkeys / lastKey | faithful | 152-slot map, 512-slot hotkey semantics, ascii packing, cheat ring | `IO_part1.inc:563-611`, `IO_part2.inc:326-600` |
| MouseCursor | partial | 0-arg `getPos`/`getPickedPos`, gadget AABB pick | `Cars.cpp:621-760` |
| Force feedback | missing | slots exist, no effect created, no joystick | `input_win32.cpp:836-838, 1081-1088` |
| Steam | missing | 0 of 5 natives; `GameLogic` exits when `Steam.postInit()` is 0, yet the boot reaches Garage: why is `[unknown]` | natives_stubs.cpp; GameLogic.java:335 |
| Vehicle build (`forceUpdate`) | partial | bookkeeping as side maps; no body | `Chassis_part2.inc:636-958` |
| Rigid body / tires / ground | missing | arcade Euler on a plane | `Resources_part4.inc:25-36, 150-400` |
| Audio and video | soft | DS8 backend exists, natives never call it; 10 of 14 Sound natives; DirectShow to texture with the 3-arg `openVideo` only | `Sound.cpp:698-745`, `video_fmv.cpp:251-375` |
| Diagnostics | faithful | 26 `SLRR_*` switches, crash handler with map offsets and VM frames, frame dump, key injection | `main_part1.inc:45-96`; `grep getenv` |

**Two boot modes.** `--game` runs `game_interactive_run`. Without `SLRR_PE_BOOT_INIT`
the C++ Soft path replays the pre-940 splash/menu/GameLogic flow (`game_boot_part1/2.inc`,
`GameRef_part3.inc`, 43 `MainMenuDialog` references), which is what the E: regression
exercises (`vehicleTypes=25`, `hub EXIT ok=1`, exit code 5; TASKS.md header). With
`SLRR_PE_BOOT_INIT=1` (`game_boot_part1.inc:714-721`) the same function constructs
`java.game.Init`, runs `<init>(I)` on the VM and enters the PE frame loop; this boots
Build 940 to the Garage. `--boot` runs the unit smoke in `main_part1-4.inc`. Whether the
E: scripts also boot through `SLRR_PE_BOOT_INIT=1` has not been tried `[unknown]`; it
decides how the Soft shims can be retired without losing the regression.

**Two reference installs.** D: Build 940 (the target; validates the script boot). E:
the pre-940 exe (2,408,448 bytes, 5,322 classes at TUFA 0x11AE1; validates the legacy
path and is the exe every `@0x...` in host comments refers to).

**Diagnostics and tools.** The 26 `SLRR_*` environment switches (stream trace, step
filter, errors-only, census, native profiler, pump cap, heartbeat, `SLRR_PE_BOOT_KEYS`,
`SLRR_PE_BOOT_SHOT`, `SLRR_WINDOW_NOACTIVATE`, ...) are explained in docs/VM.md
"Experimental switches" and TASKS.md "Known problems". Tools that run against this
repository: `tools/tufa_dump.py`, `tools/ghidra940.py` (headless queries on the
`slrr940` project in `..\slrr-ghidra`; the exclusive lock is Ghidra's own
`slrr940.lock`) and `tools/audit_natives.py`; the other 15 carry upstream's `native/`
layout or personal paths. Run etiquette: no desktop screenshots,
`SLRR_WINDOW_NOACTIVATE=1`, frame capture through `SLRR_PE_BOOT_SHOT`; delete
`tree_rpk_scan_boot/` in the game directory after each run (the host writes it,
`game_boot_part1.inc:142`; it is present on D: right now).

**Hygiene.** `engine/data/System.class`, `System.cons.bin` and `class_index.txt` and the
cursors and icon under `engine/assets/` are game-derived and copied beside the exe by
CMake; the install itself is never copied into the repo and no asset is redistributed.
The rules and the plan to read these files from the player's install are in
BACKGROUND.md section 7 and FORK.md "Hygiene before any release".

## 11. Dependency map

- **Drawing the garage scene** needs the per-viewport present walk with hidden state
  (UI pass) because the garage camera is a `camera` GameType bound to the Osd's viewport
  by the `render` command, which must land in the same pass as the OSD; it needs
  multi-line payload parsing because the camera, cursor and lift entries are declared
  `native X` / `native part <cfg>` + `script <class>`; it needs the five-number `render`
  command bound to a host camera; and it needs the `mesh/texture/flags/skeleton`
  render-object recipe plus INVO bones because garage and car meshes bind textures by
  record id and attach to `bone00`/`wheelbone`, not by filename.
- **Mouse support** needs the camera pass because `EVENT_CURSOR` picking maps the screen
  position through the Osd camera onto `PhysicsRef` hotspots (today's pick is an AABB
  over a legacy screen-space layout), and the `(Vector3)` overloads of
  `MouseCursor.getPos/getPickedPos` plus a script-driven `MouseCursor.enable` because
  `Osd.animate` reads `Input.cursor.getPos` every frame; it makes the Mechanic, MOVE
  PARTS, lift, dealer and catalog menus operable, since hotkeys reach only ENTER and ESC.
- **Loading cars** needs the compiler (or a stock-exe `compileAll` stopgap) because 635
  sources have no class: cars/racers 418 (body and light panels, chassis variants),
  parts/wheels 135, parts/running_gear 38, parts/engines 28, parts/scripts 15,
  parts/accessories 1; without them `Part.addStockParts` lands on classless GameRefs and
  the parts are dropped. It also needs the 16 Build 940 `Chassis` natives because
  `Chassis.java` calls them.
- **Driving** needs physics (a rigid body, suspension and tire model behind the FORK.md
  contract), ground collision from the map packs (whose on-disk formats, `maps/<map>/
  box/*.box`, `city.rest`, `.trc`, `.walk`, are `[unknown]`), the GameRef camera path
  above, GII_DRIVE dispatch, the Track HUD (texts and rectangles under its Osd), and
  `GroundRef` traffic and route natives, because `Track`/`City`/`Valocity` drive the car
  and the world entirely through those natives.
- **Retiring the Soft shims** (`game_boot_part*.inc`, `GameRef_part3.inc`) needs the
  script boot to be the default and a replacement for the E: regression, because that
  regression runs through the shims today; it also needs the `Jvm::invoke` behavioural
  hooks removed only after the stream path handles every tree the boot walks, because
  those hooks are also the legacy evaluator's leaves.
- **The compiler** needs nothing from the rest of the roadmap: the language is a small
  Java subset (section 4.1), the emitter side is known from the loader, and 2,819
  shipped source/class pairs are a byte-for-byte oracle. Two preconditions inside it: a
  non-heuristic CONS grammar (kind-1 int versus char, bare-length strings) and the
  meaning of the MTHD static-row words and FILD `w0`, both `[unknown]` until the 940
  `JVM_addClass_fromChunks` chunk cases (0x554F4F) are read in Ghidra.
- **Physics and FFB** need cars loaded in the garage and the drive state, because
  `physics_drive` is only invoked by smoke fixtures today and GII_DRIVE is never
  dispatched to a scripted `Vehicle`; FFB additionally needs joystick enumeration (no
  type-3 device slot exists) and the physics module for rack torque.
- **A modern renderer** needs the present walk and the render native contract
  (`GfxEngine`, `Viewport`, `Camera`, `Text`, `RenderRef`) complete and faithful first,
  because scripts only ever see those natives (BACKGROUND.md section 5).
- **Saves and career** need the `File` natives (`write(GameRef)`, `write(ResourceRef)`,
  `readResID`, `copy`, `delete` with wildcards) to match the stock SDAT layout, and
  stable pack slot ids, because `GameLogic.save/load` round-trips packed resource ids
  through `save/temp/` and `save/career/<name>-<club+1>/`.
- **Audio** needs the drive state because engine sound is played from the physics tick
  through the chassis SfxTables; menu and garage sounds need only the `Sound`/`SfxRef`
  natives wired to the existing DirectSound backend.
- **Linux** needs a platform abstraction (window, input, audio, video, renderer backend)
  because the host is Win32 D3D9/DirectInput/DirectSound/DirectShow throughout, plus
  small build fixes (CMake source paths are lowercase `core/`/`runtime/`; the non-MSVC
  branch links no libraries) and the classpath spelled as on disk (`system/scripts`).

## 12. Roadmap

Ordered by the dependency map. TASKS.md keeps the live steps; this list says why each
comes when it does.

**M1. UI pass: texts as instances, hidden state, one present walk** (TASKS.md step 1).
Why now: every later visual milestone (garage, mouse, HUD) sits on the per-viewport
pass, and the exe's model (texts are instances, hidden groups are `WORLDTREELEAF`, each
frame walks bound viewports) is established in sections 6-7; nothing it needs is
missing. Done looks like: the Garage frame dump shows only the garage OSD, no main-menu
text, no stray "a", a dark quad for the curtain, the dialog upright; the E: regression
still prints `hub EXIT ok=1` and exits 5. Risk: the 180-degree rotation has no
confirmed cause; if it is the Ypr convention it also moves every 3D instance later.

**M2. Garage scene.** Why now: it is the first visible 3D output and the proving ground
for payload parsing (`native camera`, two-line lift entries), the five-number `render`
command, the render-object recipe and bones; it needs M1's pass and nothing else that
does not exist. Done looks like: a frame dump with the garage interior, lift and neon
light under the Osd; the `[jvm] class file missing camera/cursor/lift_support` lines
gone. Risk: the `render` command's last two numbers and the INVO bone/LOD encoding are
unknown and need Ghidra time on the 940 exe.

**M3. Compiler as a standalone tool, run in parallel with M2.** Why now: it needs
nothing from M1-M2, has the 2,819-pair oracle, and gates every car-related milestone;
starting it early forces the CONS grammar and MTHD word questions to be settled. Done
looks like: a tool that compiles all 2,819 paired sources byte-equal to the shipped
classes, then produces the 635 missing ones; the host calls it where `class file
missing` is logged today. Risk: 159 shipped classes carry build 0x24F5C and may differ
in encoding; treat them as a second oracle, not a failure. The stopgap (let the stock
exe compile) depends on the `[unknown]` write-back behaviour and stays local.

**M4. Mouse and garage operation.** Why now: with M1-M3 the garage draws and cars load,
and the Mechanic, MOVE PARTS, lift and dealer menus are click-driven. Done looks like: a
scripted key-and-click run that installs and removes a part, with the mesh count in
`[render-dbg]` changing and no "not found" from `Part.addStockParts`. Risk: the
`Cursor_tick` scene pick needs the `PhysicsRef` hotspot boxes in the camera's space,
which touches the Soft PhysicsRef state.

**M5. Career path and Soft-shim retirement.** Why now: with the garage operable, the
remaining career-path errors (String receiver resolution for `String.getParams`, Steam
natives with a `neutralize` path, field initialisers for classes without `<init>`, the
16 Chassis natives) can be fixed against real runs, and making `SLRR_PE_BOOT_INIT` the
default removes 43 `MainMenuDialog` references and the `Jvm::invoke` hooks. Done looks
like: a career saved and reloaded with `main` and `PlayerCar0` byte-compatible with a
stock profile, `SLRR_PE_STREAM_ERRORS=1` silent from menu to garage, and a regression
replacing the E: hub check (the E: scripts on the script boot, or the 940 key script).
Risk: the `[unknown]` Class_newInstance behaviour and the Steam `postInit` path can
change what the menu does on a clean profile.

**M6. Driving: physics, ground, cameras, HUD, audio** (FORK.md steps 1-3 and 5). Why
now: it needs cars in the garage (M3-M4), the GameRef camera path (M2) and the Track Osd
(M1), plus the map collision formats, the largest remaining reversing task. Done looks
like: `HITTHESTREET` reaches `Valocity.enter`, the car rests on the city ground under
gravity, a frame dump shows the follow camera and the HUD, and the engine SfxTable plays
through DirectSound. Risk: ground data formats and the `GroundRef` traffic natives are
the least documented part of the exe.

**M7. Force feedback** (FORK.md step 4). Why now: rack torque needs M6's tire model;
joystick enumeration is a prerequisite with no other consumer. Done looks like: a
constant-force effect updated per physics tick on a wheel, with a clipping meter in the
log. Risk: low; the DirectInput plumbing is small once a type-3 device exists.

**M8. Modern renderer, platform abstraction, Linux** (BACKGROUND.md sections 5-6). Why
last: it needs the native contract and present walk complete (M1-M2) so the backend can
be swapped behind unchanged natives, and a Linux build needs the whole platform layer.
Done looks like: the same frame dumps from a second backend, and a MinGW or native Linux
build passing the 940 key script. Risk: the fixed-function material semantics (envmap
flag, 17 material classes) must be recovered before a shader backend matches the look.

Independent of the order above and safe to do any time: add the ninth classpath entry
(`java.net`), fix the CMake path casing, read the game-derived `engine/data` files from
the install, and update README/FORK.md for the VS 18 2026 generator.
