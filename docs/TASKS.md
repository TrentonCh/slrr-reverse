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

## Next steps (ordered, 2026-10-09 late)

Where it stands: the 940 scripts boot to the main menu, ENTER opens the
sliding menu, NEW CAREER opens the name dialog, typing works, ENTER
confirms and `GameLogic.actualState` becomes `java.game.Garage` (garage
OSD drawn: lift, money, day, welcome dialog). Unattended check:
`SLRR_PE_BOOT_KEYS="13:0x1C,16:0x1C,19:0x14,19.4:0x12,19.8:0x20,22:0x1C"`
with `SLRR_PE_BOOT_SECONDS=40`.

0. Fact-check `docs/ENGINE.md` (draft: how the game and engine work, host
   status per subsystem, dependency map, roadmap). Three section checkers
   against code, exe strings, scripts and the install, then a fixer; drop
   the draft banner when done.
1. UI pass (one renderer change, keep the E: path green):
   - Texts become render instances built from the font SCX (glyph quads
     in cm, one `TextInst` per `Text`, rebuilt on changeText / colour /
     scale / align; `Text.setScale` native), so they take the owner
     camera's view like rectangles do. Drops `draw_camera_texts` and the
     atlas glyph path.
   - fade.SCX and other non-font charsets load as plain meshes (the
     darken curtain currently draws as the letter 'a').
   - Alpha from the instance colour; units folded into `resolve_world`.
   - Hidden instances are not drawn: WORLDTREELEAF / `Osd.hide` state.
     The Garage frame still shows the main-menu dialog texts and the 'a'
     width probe because the legacy `draw_osd_texts` draws every text.
   - One `draw_present_frame()` over the bound viewports (persistent
     clear flags, viewport order) replacing draw_meshes /
     draw_viewport_cameras / draw_camera_texts / draw_osd_texts; keep the
     legacy auto-framing only when no viewport is bound (old build).
   - Dialog subtree draws rotated 180 degrees (OK/CANCEL mirrored at the
     top, title below the box): check the dialog camera / group pose sign
     convention (ypr) in `resolve_world`.
2. Garage scene: the state reaches `java.game.Garage` (211 meshes, 9
   cameras, 29 viewports) but the frame shows no 3D garage. Check the
   garage camera/viewport binding and the resource loads once the present
   pass is in.
3. Mouse: `MouseCursor` (Controller `Mouse` device axes, `getPos`,
   `getPickedPos`, EVENT_CURSOR to the Osd), `Osd.hoverEffect` /
   `click` / `mapClickPosH`, pointer visibility.
4. Career-path script errors: `String.getParams` native missing
   (`GameLogic.updateCodeROC` -> `null.trim()`), `Object.finalize` has no
   binding (12 warnings per run), compiled classes missing (`camera`,
   `cursor`, `SplashScreen`, `lift_support/cfg`, `stock_Battery_silver`;
   the exe compiles .java on demand, the host cannot).
5. Field initialisers for classes without an explicit `<init>` (confirm
   Class_newInstance behaviour in the exe).
6. Retire the C++ Soft boot shims; then physics / FFB behind the native
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
| String `==` compares text (the exe interns String payloads) | faa15eb | `Rectangle.validAxle` returned -1, no OSD animation ever ran |
| Camera-pass texts (world-space glyph quads under the Osd camera) | faa15eb | `draw_camera_texts()` |
| Step trace filter by `Class.method`, comma list | faa15eb | `SLRR_PE_STREAM_STEPS=java.game.Gates.run,java.util.Vector.elementAt` |
| Typed elements for primitive arrays (int[]/float[]) | 0200c16 | ControlSet key maps were all zero; 132 `user_Add` now |
| Scripted key presses `SLRR_PE_BOOT_KEYS`, hotkey table-slot sampling, hotkey event on a VM thread | 4c5ff31 | ENTER reaches `Gates.osdCommand(34)` |
| Physical input only when the window is foreground | 1c7d850 | unfocused test runs read the user's typing as game keys |
| `string_is()`: String `==` only for real strings | 9282e8f | `Object.equals` said two Osds were equal, focus queue dropped the wrong Osd; ENTER now reaches the menu |
| Foreground gate for GetAsyncKeyState, errors-only trace, pump wall cap, crossing-based key injection | 191ad2e | `SLRR_PE_STREAM_ERRORS=1`, `SLRR_PE_PUMP_CAP_MS` |
| Invoke runs the resolved (owner, method) slot; deferred VM thread delete; text instance destroy; pump re-entry guard | bb4baa7 | by-name fallback picked the wrong overload; heap corruption after NEW CAREER; 577 `ResourceRef.<init>(null)` errors gone |
| PE frame loop (SimulateFrame + Input tick + animate dispatch), GII_ANIMATE registry, scripted keys in `Input.lastKey`, runtime `ResourceRef` children | 9674d59 | `Osd.animate` runs every frame; typing reaches `StringInput.key`; MouseCursor null errors gone |
| Scripted keys carry the ToAsciiEx character | 438a265 | ENTER (13) finishes the name dialog; career starts, `actualState = Garage` |

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
- Done (faa15eb): texts under an Osd camera draw in the camera pass.
  The menu animations run: `Rectangle.validAxle` compares the axle string
  with literals from another class's pool, and the exe interns every
  String payload (`FUN_0055b980` looks the text up in a string table
  before allocating), so its pointer compare is content equality. With
  the host doing the same, the item rescale and the two logo slides run
  (`Rectangle.run` steps them, `a_finished_x` goes to 1), `Gates.run`
  reaches `message.changeText("PRESS ENTER")` and then polls
  `showMenu` / `messageSeek`, which only `osdCommand` (hotkeys) changes.
- Frame dump at loop end shows the icon strip slid into view with the
  logos and the gradient background. Item label texts are the script's
  placeholders until a selection happens.
- ENTER works end to end (9282e8f): prompt -> `Gates.osdCommand(34)`
  -> `showPrimaryVisuals` -> `SlidingMenu.teleport`, label "NEW CAREER";
  ENTER again -> `SlidingMenu.click` -> `osdCommand(CMD_NEW_CAREER)`.
  The earlier "mask 0" symptom was the focus queue removing the wrong Osd
  (`Object.equals` compared any two objects as equal because the string
  text helper answered for non-strings).
- Input (4c5ff31): the control set loads (`save/controls/active_control_set`,
  SDAT/CTRL v16; keys 34 ENTER/SPACE/NUMPADENTER, 35 ESC, 55-58 arrows as
  logical axes), `Input.checkHotkeys` samples the registered slot and
  fires `Osd.handleEvent(Hotkey)` on a VM thread. Hotkeys are only sampled
  for the OSD in focus; the Gates ENTER hotkey (cmd 34) lives on the Osd
  that is in focus before the prompt, the OSD in focus afterwards has
  event mask 0. Placeholder text 'a' (width probe) is still drawn.
- Note: earlier runs saved a zeroed control set back to disk (the int[]
  bug round-tripped through `ControlSet.save`); it was restored from
  `save/controls/Defaults` (same format). Keep the `Defaults` file.
### 2. `ResourceRef.<init>(ResourceRef)` called with a null argument

- Gone since bb4baa7: the invoke now runs the (owner, method) slot that
  `pe_find_method` resolved instead of re-resolving by name, so the
  0-arg `super()` no longer lands on `<init>(GameRef)`.
- Side effect: parts whose compiled class is missing now reach the
  `Part.addStockParts` native (no binding) and `GameRef.setWear/setTear/
  setTexture/addStockParts not found` (2 per run) instead of being
  silently skipped. Missing-class limitation, not a resolver bug.

### 3. Classes without an explicit `<init>`

- Field initialisers only run on the `<init>` path; confirm in the exe
  whether Class_newInstance runs the FILD trees (MainMenu, GfxEngine, ...).

### 4. `Thread.methodStatus` null before `addMethod` (race, harmless)
## Backlog

- Missing natives seen so far: `Steam.initAPI`, `Thread.run`.
- `careerComplete` unwinds (null `GameLogic.player`) - check once the menu
  draws whether the player object is created by the script path.
- `String.getParams(Vector)` native (ROC prize code), `Object.finalize`
  no-op binding.
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
- The host polls DirectInput in background mode. Physical keyboard/mouse
  are ignored unless the game window is the foreground window; use
  `SLRR_PE_BOOT_KEYS="t:dik,..."` (DIK codes, e.g. 0x1C ENTER, 0xC8 UP,
  0xD0 DOWN, 0xCB LEFT, 0xCD RIGHT) for unattended runs. With the stream
  trace on the boot is ~2x slower; "PRESS ENTER" shows at ~20 s instead
  of ~12 s, schedule keys accordingly.
- The exe interns String payloads: `==` on strings is content equality.
- Physical typing goes through the DirectInput event buffer (PE
  `Input_lastKeyEvent`); the keyboard device needs `DIPROP_BUFFERSIZE`
  or every read fails and `Input.lastKey` stays 0. Events typed while
  the window is not in the foreground are discarded.
- `Input.lastKey` returns `scan | (ascii << 16)` and `Osd.animate` only
  forwards keys with an ASCII half to `StringInput.key`; injected keys
  go through `input_scan_to_ascii` (ToAsciiEx, no modifiers).
- `SLRR_PE_STREAM_ERRORS=1` hides the per-frame `Osd.animate` named call;
  the full trace shows it (one line per frame).
- `ResourceRef.getFirstChild/getNextChild` fall back to runtime children
  (creation order, `ResState.seq`) when the rpak walk has none.
