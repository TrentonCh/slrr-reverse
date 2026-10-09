# Faithful script VM for Build 940

Decision (2026-10-08): the fork targets the current Steam exe, SL2 Build 940
(`StreetLegal_Redline.exe`, 2,446,848 bytes, PE timestamp 2025-12-14, DRM-free,
TUFA build 0x24F9D). Upstream's host was developed against the pre-940 exe
(TUFA build 0x11AE1); a complete copy of that build lives on E: and stays the
A/B reference.

## Why a VM rewrite is the first job

Build 940 rewrote the script layer: `sl/Scripts` grew from 50 to 140 classes,
`SplashScreen` and `MenuDialog` are gone, the main menu is a new `Gates`
dialog, and 23 system classes were added. Upstream's host drives the game
through two mechanisms that do not transfer:

- `engine/Core/Script/tree_eval*.inc`: a heuristic pattern-matcher tuned to
  the old compiled trees (for example it resolves `window.show()` to
  `MainMenu.show` instead of `Gates.show`).
- C++ shims in `GameRef_part3.inc` and `game_boot_part*.inc` that re-implement
  old script flow (`MainMenuDialog` is referenced 71 times).

The class format itself did not change. The 48-entry tree flag table and the
73-entry opcode case map are byte-identical in the 940 exe (relocated to
0x5B5260 and 0x564684). Only the scripts and the build number changed.

Upstream already has the right core: `engine/Core/Jvm/jvm_vmthread*.inc` is a
PC-based port of the exe's `VMThread_run` (old-exe VA 0x420FF0) with an
explicit frame and operand model. With call ops enabled
(`SLRR_PE_STREAM_INVOKE=1`) it executes 313 of the first 320 method bodies on
the 940 boot path to completion; `System.rpkScan` and `MainMenu.enter` bail.

## Plan

Work on the VM lands in `engine/Core/Jvm/` and `engine/Core/Vm/` (new). The
heuristic `tree_eval` is legacy: do not add Build 940 cases to it.

1. Diagnostics first. Print node index, op and reason when the stream bails;
   keep `SLRR_TREE_TRACE` for the legacy path. Add a `--dump-class` mode or
   use `tools/tufa_dump.py`.
2. Make the stream self-sufficient for calls. Script callees must push a frame
   and continue the same loop (the exe's afterPc path) instead of recursing
   into `Jvm::invoke`; drop the thread-local depth gate. This is what removes
   the heuristic layer from the call path.
3. Method resolution as the exe does it. Decompile `Thread_callMethod`,
   `Class_lookupMethod_nameSig`, `Class_findMethodSlot` and
   `NativeSigDesc_compatScore` in the 940 exe (Ghidra) and port the scoring.
   Export methodref class and descriptor from the constant pool (`tufa.cpp`
   currently keeps only the name).
4. Typed object model: per-class field slots from `instance_fields` /
   `static_fields`, typed values, arrays with primitive elements and
   assignable element references, field initializers run by the VM.
5. Remaining opcodes (low op 29, the 0x1008 statement groups 0-2 and 7-9,
   other 0x1007 tags, multi-dimensional arrays, numeric string concat).
6. Script-error semantics as the exe (`ScriptError` + continue), never fall
   back to `tree_eval` after side effects.
7. Remove behavioural hooks from `Jvm::invoke` and the `<init>` exclusion.
8. Green threads through `jvm_run_threads_budgeted`; refcount/GC.
9. Natives Build 940 added (not in upstream's 376-entry table): `Chassis`
   gained 18 (`setTorque setAckermann setMaxSteer getMaxSteer setRPM getRPM
   getShifting getNitro getNitroing getEngineTemp getAngvel getMaterialIndex
   get/setAIparam_int get/setAIparam_float get/setOSD`), `Vehicle.fileVersion`.
   Census the full set with `tools/inventory_natives.py` against 940 classes.
10. Source compiler: 635 of 3,454 `.java` files in the stock install have no
    `.class` (optional kits and parts); the stock exe compiles on demand.

## Reference material

- Old-build exe and classes: `E:\NFS.SLRR Edition\NFS.SLRR Edition` (tables at
  upstream's VAs 0x5F0914 / 0x4239B0). Upstream's address bookmarks apply there.
- Build 940 exe: tables at 0x5B5260 / 0x564684; everything else must be
  re-located in Ghidra. Entry 0x56B21A.
- Tools: `tools/tufa_dump.py` (class dumper), `SLRR_TREE_TRACE=<method>`
  (legacy interpreter step trace), `SLRR_PE_STREAM_TRACE=1`,
  `SLRR_PE_STREAM_CENSUS=1`, `SLRR_PE_STREAM_INVOKE=1`.

## Progress log

### 2026-10-08 (session 1)

Experimental switches (all opt-in until the stream path is the default):
`SLRR_PE_STREAM_INVOKE=1` (call ops on the stream), `SLRR_PE_STREAM_INIT=1`
(constructors on the stream), `SLRR_PE_STREAM_STATIC_INIT=1` (static and
instance field initializer trees on the stream), `SLRR_PE_STREAM_STRICT=1`
(never fall back to `tree_eval` after a partial run), `SLRR_PE_STREAM_TRACE=1`
(enter/leave/bail/fail-line diagnostics), `SLRR_PE_STREAM_STEPS=<class>`
(per-step operand/locals dump), `SLRR_JVM_INVOKE_TRACE=1`.

With all switches on, Build 940 boots through pack scanning (51 hits, 68
packs) and builds all 25 vehicle types entirely on the VM, including the
`*_VT` constructors (1,000+ nodes each), `VehicleModel` field initializers
and `Vector` constructor chains. No bails until the main menu.

VM bugs fixed in `jvm_vmthread_part*.inc` / `tufa.cpp`:

- call ops left the argc box and args on the caller operand stack;
- native return values were dropped instead of pushed on the resumed frame;
- after an invoke the loop resumed at the node after the call op, not after
  the name path;
- 0x1003 popped the operand stack; it releases the last local;
- relative jumps clamped negative offsets to zero (every loop spun);
- `pop_frame_restore_pc` followed a stale dllist pointer to a freed frame
  (double release, frames aliased across nested threads);
- `this` was derived from local 0 even in static frames (frame instance is 0);
- `ClassName.method()` popped a receiver it does not have;
- a leading methodref segment is an implicit-this call;
- methodrefs now carry their descriptor (`JvmClass::const_mref_sig`), and
  the resolver prefers the exact overload (FindFile.first wrapper recursion);
- inherited natives resolve on the declaring class;
- field refs: CP index from the code's class, slot search from the context
  class;
- CONS RID entries are 12 bytes, not 16 (Build 940 emits them back to back);
- `field_inits` derived from the two-vector FILD layout (old heuristic
  misaligned by 8 bytes for classes with no statics);
- VM entry result pick: frame TOS for DONE, mirror only after op43.

Next blocker: `Gates.<init>` reads `GameLogic.player`, `careerComplete` reads
`GameLogic.goals`. Both are set by the script `GameLogic` constructor, which
the host replaces with C++ ("Soft") state. Milestone: boot through the
script's `Init(int)` -> `new GameLogic()` on the VM and retire the Soft
GameLogic/Splash/MainMenu shims in `game_boot_part*.inc` / `GameRef_part3.inc`.

## Build 940 VM functions (Ghidra project `slrr940`, see tools/ghidra940.py)

| 940 VA | Role | Old-exe VA (upstream name) |
|---|---|---|
| 0x562D70 | VMThread_run (opcode loop, ends 0x56463C) | 0x420FF0 |
| 0x562540 | Thread_evalName (op 0x24 call target) | 0x4208E0 |
| 0x562280 | Thread_evalName_fieldPath (op 0x1011) | 0x420D20 |
| 0x561700 | Thread_callMethod (name lookup + invoke) | 0x4207C0 |
| 0x561830 | VMThread_invokeMethod (frame build, pops args) | 0x41FBC0 |
| 0x5494C0 | Class_lookupMethod(name, sig) with cache | 0x404910 |
| 0x54A150 | Class_findMethodSlot (compat-scored search, supers) | 0x405920 |
| 0x5607D0 | NativeSigDesc_compatScore | — |
| 0x560930 | NativeSigDesc_ctorFromOperands | 0x41DCB0 |
| 0x562CB0 | T_Container_pop | — |
| 0x562A30 | VMThread_popOperand (frame+0x28) | 0x41F7D0 |
| 0x55D3B0 | TREE_nodeBytes (flags table 0x5B5260) | 0x41A5C0 |
| 0x564684 | opcode case map (73 bytes) | 0x4239B0 |

Confirmed semantics (decompiled 2026-10-09):

- op 0x1003 pops the frame LOCALS container (frame+0x18) and releases the
  value; the operand stack is untouched.
- Thread_evalName: node 0x101A forces count=1 and stays; otherwise count=imm
  and the count node is skipped. First segment: 0x1019 resolves a class and
  subtracts 2 from the count; otherwise the context is frame+0x30 (instance)
  if non-zero, else frame+0x34 (class). 0x1001 loads locals[imm]; 0x101B
  reads a field (cached slot when the fieldref class is the context class,
  else by name); 0x101A pops and releases one operand, resolves the method
  through the cached slot or Class_lookupMethod(name, descriptor from the
  nat) and, for non-static methods, sets instance = current object; a bare
  utf8 segment is first tried as a field on the current object (not on the
  first segment) and otherwise becomes the script-method marker (slot -2)
  with instance = current object (null after a classname).
- Class_findMethodSlot: for each class in the super chain, statics vector
  first then instance vector, scanning from the END of the vector; same
  name candidates are scored by NativeSigDesc_compatScore (arg count must
  match; per-arg type score summed; 0 = exact, stops); lowest score wins,
  a tie at the best score logs "more matching methods found".
- VMThread_invokeMethod pops argc values from the caller operand stack into
  the callee locals (top first), after `this` for instance methods.

### 2026-10-09 (session 2, Ghidra online)

Ghidra 12.1.4 + pyghidra installed; project `slrr940` analysed (6,630
functions). `tools/ghidra940.py` queries it headlessly. Decompiled and
ported: Class_findMethodSlot / compatScore (scored overload resolution),
Thread_evalName (receiver rules), T_Container_pop at 0x1003 (locals),
JVM_addClass_fromChunks (MTHD = static container then instance container).

Loader fixes: static methods marked from the first MTHD container;
`const_mref_class` exported. VM fixes: Int resource ids accepted for
ResourceRef params; typed defaults for unset fields and fresh locals;
null receiver on a field path or method call is a script error that
yields null; unresolved script methods are "not found" with no call;
inherited natives resolve on the declaring class.

State: with all switches on, Build 940 boots through pack scanning, all
25 vehicle types, the Gates/Dialog constructors and `createText` on the
VM. Remaining failures are reads of state the host never sets because its
C++ boot replaces the scripts' `Init(int)`: `Frontend.*Font` statics are
null (Text/Osd calls), `GameLogic.player`/`goals` are null (careerComplete,
Gates.show node 200, createButton node 142).

Next milestone: boot through the script `Init(int)` -> `Frontend.init()`,
`Sound.init()`, `Input.initControllers()`, `new GameLogic()` on the VM and
retire the Soft splash/menu/GameLogic shims in `game_boot_part*.inc` and
`GameRef_part3.inc`. Natives those paths touch must then be real.

### 2026-10-09 (session 3, script boot)

`SLRR_PE_BOOT_INIT=1` boots Build 940 through the scripts' own
`java.game.Init.<init>(I)` instead of the C++ Soft boot. The Init call runs
on one VM thread until it parks; the host main loop then presents a frame,
pumps resources and runs the cooperative scheduler every iteration
(`SLRR_PE_BOOT_FRAMES=N` or `SLRR_PE_BOOT_SECONDS=N` bound it).

VM: nested script calls build a callee frame on the same thread
(frame-switch) instead of recursing; a script error inside a nested frame
unwinds that frame, yields null and continues (PE ScriptError). Green
threads: `Thread.start` queues `run()` on the Java thread's VmThread,
`Object.wait` parks it (flag 0x10), `notify` pops the LIFO waiter,
`Thread.sleep` sets the sleep deadline; the budgeted pump skips parked and
sleeping threads. The VM clock is a steady clock relative to start (the
GetTickCount float lost an 8 ms budget to rounding). A static native
reached through an instance call drops the receiver (`this.sleep(300)`
was handing the object pointer to Thread.sleep).

Loading-screen protocol (confirmed in the exe): `GfxEngine` has no `wait`;
`Frontend.render.wait()` is `Object.wait` on the GfxEngine instance and the
Present path notifies it once per frame. `System.isLoading` is the resource
pump's flag from a 32-slot load ring; `isLoadingReset` (called by
`LoadingScreen.track`) seeds the ring so `run()` sees loading for ~24
pumps, shows the dialog, runs the SoftTimer/Fade/FlashText threads and
finally `hide()` -> `termSig.notifyAll()` wakes Init. The seed is only
applied on the script boot (the legacy C++ mirror spins on it).

State: Init -> Frontend.init -> GameLogic -> loading screen -> MainMenu:
`GameLogic.actualState` is `java.game.MainMenu` after the loop and
`GameLogic.handleEvent` runs on the stream. Still failing: `Gates.show`
(`Osd.createButton` node 142 calls through `groups.lastElement()` which is
null; `Osd.<init>` reported `Osd.equals` not found), `careerComplete`
(null player), classes without an explicit `<init>` (MainMenu, GfxEngine,
Steam, HotkeyWatcher, InventoryItem) report "not found" and skip field
inits, `ResourceRef.<init>(ResourceRef)` with a null argument.

Old-build regression (E:) still passes: vehicleTypes=25, menu chrome=1,
hub EXIT ok=1, exit 5.

Later in session 3: array elements are assignable (op 0x20 pushes an
element reference; `a[i] = x` stores through it) and `array.length` answers
the element count, which makes the script `java.util.Vector` work
(addElement / lastElement / ensureCapacityHelper). With that, every
`Osd.createButton` / `createHotkey` / `changeSelection` null error is gone,
but `Osd.changeSelection(II)` (reached from `Osd.show` -> `resetSelection`)
now loops forever at node 121: a cursor search over `group.gadget`
(`elementAt(i)`, `.disabled`, OR-chains at nodes 99-103 / 122-125) never
finds an acceptable gadget. Next: step-trace `java.render.Osd` into
`changeSelection` and compare the `disabled` / `active` reads with the
dump. Old-build regression still passes.
