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

1. Finish the main menu: route the Osd texts through the Osd camera pass
   (they still go through the legacy screen-space text pass), check the
   item labels/animation over a longer run, confirm the icon strip against
   the real game. Verify with `SLRR_PE_BOOT_SHOT`.
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
| Profiling: SLRR_PE_NATIVE_PROF (natives, pump, per-thread), renderer debug dump | c9c2b29 | `[prof]` / `[render-dbg]` at loop end |
| Native declared on the class beats inherited script wrappers; 16-word thunk | 7517397 | Camera.create reaches the renderer |
| Per-viewport camera pass draws script render instances (Osd rectangles) | 106e5f3 | `draw_viewport_cameras()`, frame dump shows the icon strip |
| RenderRef.create parent survives setMatrix(pos,ori) (`tree_parent`) | 106e5f3 | instances were orphaned, members=0 |
| vec3_get/ypr_get read script-built Vector3/Ypr fields | 106e5f3 | every setMatrix pose was (0,0,0) |
| SCX centimetres scaled to script metres in the camera pass | 106e5f3 | 392x294 cm background = full screen at the Osd camera |

## In progress

### 1. Draw the main menu

- Done (106e5f3): `draw_viewport_cameras()` runs after the legacy pass.
  For every camera whose viewport is active it collects the ready meshes
  that share the camera parent's root (xform parent, else the creation
  parent `tree_parent`, else `gameref_get_parent`: Group -> Osd), inverts
  the camera world (own bone00 pose folded) as the view, builds the
  projection from the camera's half aov / dmin / dmax and the viewport
  aspect, scales SCX geometry by 0.01 (cm -> metres) and draws with
  `draw_meshes()` in keep-camera mode. The frame dump now shows the Osd
  background rectangle and the sliding-menu icon strip (part pictures from
  `menuItem.updateTexture` -> `Rectangle.changeTexture`) on top of the
  video and the version banner.
- Three host bugs hid the instances: `setMatrix(pos, ori)` calls the
  4-arg form with bone_ref=0, whose unlink cleared the parent set by
  `RenderRef.create`; `vec3_get`/`ypr_get` only knew host-made vectors, so
  script-built `Vector3`/`Ypr` (Build 940 constructs them in script) read
  as zero; the Rectangle template (`etalon_negyzet_alpha.SCX`) is a 100 cm
  quad while the script works in metres.
- Remaining: the 11 Osd texts are still drawn by the legacy screen-space
  text pass (`draw_osd_texts`) rather than under the Osd camera; the item
  label texts seen so far are the script's placeholders ('a', ' ');
  confirm the strip layout/animation against the real game over a longer
  run (items sit at x = 3.57..7.37, sliding in from the right).
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
- SCX vertices are centimetres (`GameRef_core` extent "cm"); script poses,
  camera planes (Osd dmin 0.1 / dmax 10) and `scaleMesh` factors are
  metres. The camera pass scales geometry by `kScxCmToM`.
- Build 940 constructs `Vector3`/`Ypr` in script: host getters must fall
  back to the TREE fields (`load_vec3`/`load_ypr`), never trust the host
  side maps alone.
- `RenderRef.setMatrix(pos, ori)` = 4-arg with bone_ref=0: it unlinks the
  bone parent only. The creation parent lives in `MeshXform::tree_parent`.
- A Windows exe does not understand Git Bash `/tmp`; pass
  `$(cygpath -w /tmp/x.bmp)` to `SLRR_PE_BOOT_SHOT`.
