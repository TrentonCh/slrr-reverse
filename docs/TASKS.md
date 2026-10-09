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

## In progress

### 1. Career events: `careerEvents[i].init()` reports GameLogic.init not found

- `GameRef.create` now derives `java.game.CareerEvents.JuniorCurcuit` from the
  entry payload's `script <path>` line (`script_fqn_for_res`), but the node's
  script class is seeded elsewhere (`host_mid_seed_script_class` callers in
  Resources_part2.inc:1083 and Resources_part4.inc:1761) so the created
  object still has no class and `init()` resolves against the caller.
- Plan: route both seeds through `script_fqn_for_res`, confirm the class
  loads from `sl/Scripts/game/CareerEvents/*.class`, then `careerComplete`.

### 2. Draw the main menu

- 11 OSD texts reach the renderer now; rectangles/buttons (`osd=0`) do not.
  Check `Rectangle.create` / `RenderRef.create` natives feed `g_osd` and
  whether `Osd.show` / `Group.activate` visibility is honoured.

### 3. Classes without an explicit `<init>`

- `GfxEngine`, `Steam`, `HotkeyWatcher`, `MainMenu`, `InventoryItem` report
  `<init>()V not found`; field initialisers only run on the `<init>` path.
- Plan: confirm in the exe whether Class_newInstance runs the FILD trees.

### 4. `ResourceRef.<init>(ResourceRef)` with a null argument (132/run)

- From `GameType.<init>()` -> `GameRef.<init>(GameRef)`; probably harmless.

### 5. `Thread.methodStatus` null before `addMethod` (3/run)

- `Gates.run` polls `mmaThread.methodStatus(k)` before the ctor finished
  `addMethod`; the budgeted scheduler interleaves. Harmless race.
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
