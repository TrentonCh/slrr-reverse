# Task tracker (Build 940 on the VM)

Living list of what is being worked on, where it stands, and what is in the
way. Updated at each checkpoint; the narrative history lives in `docs/VM.md`.

Standard 940 run (from the game dir):

```
SLRR_PE_BOOT_INIT=1 SLRR_PE_BOOT_FRAMES=0 SLRR_PE_BOOT_SECONDS=25 \
SLRR_PE_STREAM_INVOKE=1 SLRR_PE_STREAM_STRICT=1 SLRR_PE_STREAM_STATIC_INIT=1 \
SLRR_PE_STREAM_INIT=1 SLRR_PE_STREAM_TRACE=1 slrr_engine.exe --game --no-wait
```

Old-build regression (from `E:\NFS.SLRR Edition\NFS.SLRR Edition`):
`timeout 150 slrr_engine.exe --game --no-wait` must print vehicleTypes=25,
`hub EXIT ok=1` and exit 5. Delete `tree_rpk_scan_boot` after every run.

## Next steps (ordered, 2026-10-09)

1. Draw the menu items: queue script render instances when ready under a
   viewport, draw per viewport with its hooked camera instead of the
   auto-framing preview, find who destroys the Osd camera. Verify with
   `SLRR_PE_BOOT_SHOT`.
2. Hotkeys and mouse into the sliding menu (start a career from the VM).
3. Quiet the remaining script errors (`ResourceRef.<init>(null)` chain,
   two `MouseCursor` null field reads).
4. Field initialisers for classes without an explicit `<init>` (confirm
   Class_newInstance behaviour in the exe).
5. Retire the C++ Soft boot shims; then physics / FFB behind the native
   contract (FORK.md).

## Goal

Boot the Steam Build 940 scripts to a drawn main menu on the faithful VM
(`jvm_vmthread`), then retire the C++ "Soft" boot shims. Physics/FFB work
comes after the menu is up.

## Done

| Task | Commit | Notes |
| --- | --- | --- |
| Faithful method resolution, static MTHD container, typed defaults | 6370055 | session 2 |
| Script boot: `Init(int)` on the VM, host loop pumps frames/threads | 5e4f63a | `SLRR_PE_BOOT_INIT=1` |
| Green threads (start/wait/notify/sleep), nested frames on one thread | 5e4f63a | |
| Loading-screen protocol (render.wait notify per Present, load ring) | 5e4f63a | Init resumes, `actualState = MainMenu` |
| Steady VM clock (budget rounding fix) | 5e4f63a | |
| Static native via instance drops the receiver | 5e4f63a | `this.sleep(300)` |
| Array element references + `array.length` | 500ceb9 | script `java.util.Vector` works |
| AND/OR short-circuit keeps its conditional skip | 3c9fb4b | fixed Osd.changeSelection loop |
| Typed null args, resolution walks into java.lang.Object, null by-name call | 64cc4da | File.write(String) overload, Osd.equals |
| String concat stringifies numbers (`+`, `+=`) | fd7f949 | version text |
| rpk child lookup resolves parent ids through the remap table | 9084e7d | EVENT_ROOT children were brakes |
| Native.ptr unboxing only for integer params | 1ab8920 | script texts reach the renderer (txt=11) |
| GameRef.create constructs scripted GameType entries (payload `script <path>`) | d827a92 | career events |
| Class table is a deque; classes load once | d827a92 / f138803 | dangling frame pointers |
| Untagged receivers resolve as Object/String, recursion guard | d827a92 / e9bbb81 | CareerEvent.init recursion |
| Pack slot cap 1024, packs looked up by relative path | f138803 / 8b22678 | every rid literal resolves |
| Crash handler + linker map | f138803 | `[crash]` lines, slrr_engine.map |
| Real printScreen (BMP back-buffer dump), SLRR_WINDOW_NOACTIVATE, SLRR_PE_BOOT_SHOT | 325e978 | frame capture without desktop screenshots |
| Profiling: SLRR_PE_NATIVE_PROF (natives, pump, per-thread), renderer debug dump | (this commit) | `[prof]` / `[render-dbg]` at loop end |

## In progress

### 1. Draw the main menu

- Verified with the engine's own frame dump (`SLRR_PE_BOOT_SHOT`): the
  animated background video and the version banner render in the right
  font; the sliding-menu items (icon rectangles + labels) do not.
- Init finishes in ~9 s; `Gates.buildSlidingMenu` runs and the 'sliding
  menu animation thread' exists. The renderer dump at loop end shows
  meshes=63 ready=63 textures=57 viewports=11 but queue=0 and cameras=0.
- Two gaps: (a) script-made render instances (`RenderRef.create` clones)
  are never queued for drawing (`render_d3d9_mesh_queue_add` is only
  called by the legacy world/sky paths); (b) `draw_meshes` is a Soft
  auto-framing preview (look-at around all queued bounds), not the exe's
  per-viewport draw with the hooked camera, and the Osd's cameras are
  gone by loop end (`Camera.destroy`), so there is nothing to draw with.
- Plan: draw per active viewport with its camera (ortho-like OSD camera
  from `Camera.create` aov/dmin/dmax), queue render instances when they
  become ready under a viewport, and find out who destroys the Osd camera
  (`Osd.hide` from `Gates.run`?).
### 2. `ResourceRef.<init>(ResourceRef)` called with a null argument (574/run)

- `GameType.<init>()` calls `super()`; GameRef only declares
  `<init>(GameRef)` and `<init>(GameRef,GameRef,String,String)`. The exe's
  callMethod finds no 0-arg ctor and reports "not found"; the host still
  selects `<init>(GameRef)` and runs it with null. Find which resolution
  path admits the arity mismatch (pe_type_score Int->ResourceRef?).

### 3. Classes without an explicit `<init>`

- Field initialisers only run on the `<init>` path; confirm in the exe
  whether Class_newInstance runs the FILD trees (MainMenu, GfxEngine, ...).

### 4. `Thread.methodStatus` null before `addMethod` (race, harmless)
## Backlog

- Missing natives seen so far: `Steam.initAPI`, `Thread.run`.
- `careerComplete` unwinds (null `GameLogic.player`) - check once the menu
  draws whether the player object is created by the script path.
- Primitive arrays: host arrays hold objects only; `int[]` element stores
  become null (`VmRefKind::Elem` in `vmthread_ref_assign`).
- Retire the Soft boot shims (`game_boot_part*.inc`, `GameRef_part3.inc`)
  once the script boot draws the menu.
- Physics/FFB behind the native contract (FORK.md).

## Known problems / gotchas

- Patch scripts: write them as files under `..\slrr-ghidra\patch_*.py`;
  heredocs collapse backslashes, so build escaped newlines as
  `chr(92) + "n"`. Engine sources are CRLF.
- The legacy C++ loading-screen mirror spins on `isLoading()` from an OS
  thread; the load-ring seed is therefore gated to `SLRR_PE_BOOT_INIT=1`.
- Build with cmake `--parallel`, never MSBuild `/m` from Git Bash.
