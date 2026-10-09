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
