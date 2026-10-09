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

## In progress

### 1. `Osd.changeSelection(II)` loops forever (blocks the whole run)

- Reached from `Osd.show()` -> `show(I)` -> `resetSelection()` ->
  `changeSelection(II)`, right after the Gates menu is built.
- Node 121 calls `group.gadget.elementAt(i)` and tests `.disabled`
  (nodes 122-125 are an OR chain); the search never finds an acceptable
  gadget, so the run hits the timeout with no `loop end`.
- Plan: step-trace `java.render.Osd` into `changeSelection`, compare the
  `disabled` / `active` reads and the loop counter with the dump
  (`tools/tufa_dump.py .../render/Osd.class --method changeSelection`).
- Suspects: a gadget field never set (field inits on Button/Gadget), the
  `0x1c` member-access-on-TOS op, or the loop counter store.

### 2. Classes without an explicit `<init>`

- `GfxEngine`, `Steam`, `HotkeyWatcher` (and earlier `MainMenu`,
  `InventoryItem`) report `Thread::callMethod: X.<init>()V not found`.
- The object is still created, but field initialisers are only applied on
  the `<init>` path, so such objects keep null fields.
- Plan: confirm in the exe whether Class_newInstance runs the FILD trees
  (then move `jvm_apply_field_inits_chain` to op 0x21 NEW), or whether the
  compiler always emits a ctor and these classes are special.

### 3. `ResourceRef.<init>(ResourceRef)` with a null argument

- 31-51 "illegal fieldaccess null.<seg 1> in ResourceRef" per run, from
  `GameType.<init>()` -> `GameRef.<init>(GameRef)` -> `ResourceRef.<init>(ResourceRef)`.
- Probably harmless (the exe would also script-error and continue), but
  verify the ctor overload picked is the one the exe picks.

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
