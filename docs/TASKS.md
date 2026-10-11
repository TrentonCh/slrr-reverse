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

## Next steps (ordered, 2026-10-11)

Where it stands: the 940 scripts boot to the main menu, NEW CAREER takes
a typed name and enters `java.game.Garage`, ENTER closes the welcome
dialog. The 2D layer is pixel identical to the stock frames at 1366x768
(75569af) and the 3D garage behind it matches the stock frame (06384ba:
luminance difference of the 3D rows 0.52 of 255, every shell, prop and
lift-support material mean equal). A 48 s garage run logs one script
error (the `trim` null) and one legacy `class file missing` line.
Unattended check, garage with the welcome dialog closed:
`SLRR_PE_BOOT_KEYS="13:0x1C,16:0x1C,19:0x14,19.4:0x12,19.8:0x20,22:0x1C,36:0x1C,42:0x1C"`
with `SLRR_PE_BOOT_SECONDS=48`, then
`python ../slrr-ghidra/notes/m2/m2_check.py <frame.bmp>` (A must be 0 / 0,
B1 at most 7). Without the last two keys and 40 s the frame has the
welcome dialog. Stock frames: `../slrr-ghidra/notes/reference_frames/`.

1. UI pass: done (75569af). What the exe does and what the host got wrong
   is in ENGINE.md section 7 and docs/VM.md session 5. Left over, none
   visible on the tested screens:
   - instance colour modes (render type flags 0x80 + 0x200 / 0x400) are
     plumbed but no tested screen shows a paint can;
   - prelit meshes: the baked lighting is the second vertex colour
     (D3DFVF_SPECULAR), combined per material pass; not ported;
   - int/float to String conversion happens at call binding only, not at
     stores into String fields and locals (the exe converts in every
     assign; the host still uses Int 0 as null in places);
   - `resolve_world` applies a parent's bone00 pose twice to a child
     attached to bone 0 in the camera pass (no tested screen has a posed
     parent);
   - the script's own `Config.<init>/loadConfig` is not run; only
     `video_x/video_y` are seeded from `save/game/options`;
   - `System.buildNumber` now returns 940 on the script boot, so
     `System.nextGen()` is true and scripts take their Build 900+
     branches (credits, hall of fame, event list, track screens may call
     natives the host lacks: `Sound.playTrack`, `getAIparam_float`,
     `getMaterialIndex`, `errorVM`);
   - texture addressing clamp per texture entry (thin seams between map
     tiles on the old-build minimap), per-instance hide bit 22, the old
     build's minimap pass has no reference frame.
2. Garage scene: done (06384ba). The facts are in
   `../slrr-ghidra/notes/m2/` (`m2_spec.md`, four maps, step reports
   `impl_*.md`) and docs/VM.md session 6. Left over:
   - Q10: environment and specular of untextured metal sections (the
     lift arm's `black_metal` is pure black, a grey strip inside the left
     column is flat); the lift's shadow lines on the floor (render type
     flag 8, `rg_shadow`).
   - Q13: prelit materials WITHOUT a lightmap use the baked second vertex
     colour; 1,456 of the 1,542 ground materials of the install are of
     that kind, so every other map needs it. The colour is loaded, no
     stage setup reads it yet.
   - The ground mesh is one flat list drawn without culling: fine for a
     garage (10 sections), not for the city (624 POLY chunks, 257,560
     vertices). Game objects of nested instance nodes are not built.
   - The `car` native type has no host object: parts installed on a car
     get no pose and are not drawn (383 native parts alive in a garage
     run). Part natives (`setSlotPos`, `getSlots`, `partOnSlot`, ...)
     still answer from the legacy tables; part commands `remove`,
     `repair`, `texture`, `paint`, `decal`, `filter` are not ported.
   - No physics bodies (ground, parts, the camera body does not collide).
   - Leaving the garage (camera, lift and light destroy, `map.unload`) is
     read but not exercised by any run; the id recycling and the destroy
     cascade were checked with a temporary hook only.
   - Mode 3 for objects covers the two material classes that were read
     (Tex, MixVCA); 12 of 97 lightmapped v4 materials of the install use
     others. Wide vertices for prelit v3 objects are not filled.
   - CULL_NONE remains for lit groups, the horizon and the 2D layer (the
     exe default is CW); anisotropic filtering is not ported.
   - Why the welcome dialog needs two ENTER presses in the key script.
3. Next visible milestone: a car in the garage. A new career has no car;
   the script reaches one through the Car Dealer (arrow keys and ENTER
   work on the garage icons) or LOAD CAR. It needs the `car` native type
   (96 pack entries), parts posed on a car through their slots, lit
   materials with environment maps (Q10), and for the dealer the CarLot
   and CarMarket states with their own cameras.
3b. Mouse: `MouseCursor` (Controller `Mouse` device axes, `getPos`,
   `getPickedPos`, EVENT_CURSOR to the Osd), `Osd.hoverEffect` /
   `click` / `mapClickPosH`, pointer visibility.
4. Career-path script errors and loader gaps (none is a compiler case):
   - `String.getParams(Vector)` is a static script method that returns
     null on the host (`GameLogic.updateCodeROC` -> `null.trim()`).
   - The six `class file missing` lines: `camera`, `cursor` and
     `lift_support/cfg` are `native <type>` payloads (step 2);
     `stock_Battery_silver` was a dotted `script <class name>` payload
     (137 such entries), fixed;
     `SplashScreen` is a pre-940 class only the legacy path asks for.
   - The autosave on NEW CAREER writes a `main` that is not stock
     compatible (`write(new GameRef(id))` comes out empty). Test runs
     create `save/career/ted-1` in the install; real profiles are not
     touched.
5. Field initialisers for classes without an explicit `<init>` (confirm
   Class_newInstance behaviour in the exe).
6. Retire the C++ Soft boot shims; then physics / FFB behind the native
   contract (FORK.md).
7. Source compiler as a standalone tool (ENGINE.md M3, parallel work).
   Now needed by real saves: since the File natives work (see Done),
   LOAD CAREER builds the saved car, and a stock-written 2023 career in
   the install has a rim whose class exists only as source
   (`parts/wheels/rims/scripts/src/rim_MT_Mescaline_15_0_15_...java`);
   its part becomes a plain GameRef, the Part scripts fail and the load
   never leaves the main menu. The
   stock exe compiles `X/src/Name.java` to `X/Name.class` on every class
   load when the class is missing or its time stamp differs from the
   source; 635 sources ship without a class (lights, doors, kits, some
   wheels and running gear; no chassis). Oracle: 2,819 shipped pairs.
8. Frame-loop differences listed in ENGINE.md section 3 (rate gates, four
   `input_tick` calls per iteration, the load rate that never reaches
   the pump). The window size from `save/game/options` is done (75569af).
9. Loader quick wins (ENGINE.md section 12, "independent"). Done: the
   MTHD rows are read at their real offsets and an empty script method
   (`Part.addStockParts`, `Part.check4warnings`, `Osd.pressEffect`: no
   0x40 flag, zero nodes) returns at once instead of going down the native
   branch; about 470 `no native binding` lines per garage run are gone.
   Also done: dotted `script <class name>` payloads load through the
   classpath (`stock_Battery_silver` is a real part again; the four
   `GameRef.setWear/setTear/setTexture/addStockParts not found` errors
   are gone) and the ninth classpath entry (`java.net`) exists. A 48 s
   garage run now logs one script error (the `trim` null) and four
   `class file missing` lines (three native types, one legacy ask).
   Open: the CONS decoder
   misreads a class ref that is the last pool entry (`GameState.class`);
   21 host native entries shadow methods that have script bodies in 940
   (`RenderRef.getPos`, `Text.setDefaultColor`, `Object.hashCode`, ...).

Ground truth for anything visual: the stock-game reference frames
(`../slrr-ghidra/notes/reference_frames/stock_*.png`, 1366x768, outside
the repo) and the measured positions in
`../slrr-ghidra/notes/ui_findings.md`.

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
| Buffered DirectInput keyboard events | 7c99b60 | real typing reaches `Input.lastKey`; confirmed by hand |
| docs/ENGINE.md: how the game and engine work, fact-checked | 597da74 | 151 corrections from eight section checkers |
| UI pass: bound viewport list, RH camera with full-angle fovy, WORLDTREELEAF, material-driven blending, screen-space texts from the charset mesh, window size from the options file | 75569af | 2D layer pixel identical to the stock frames at 1366x768 |
| Loader: real MTHD access flags, empty script methods return at once | bab4c05 | about 470 false `no native binding` lines per garage run gone |
| Loader: dotted `script <class name>` payloads through the classpath; `java.net` | 3622789 | the battery part loads; four `not found` errors gone |
| Garage scene: native ground / camera / part types, POLY shell, prelit materials with lightmaps, horizon group, lights, node identity and `install` | 06384ba | 3D garage matches the stock frame (luminance difference 0.52, was 82.7) |
| Static `File.exists / delete / copy / move` take their String argument | (this commit) | they read a `name` field of the String, got null and worked on a file called `<null>`; saves never cleaned or copied files, a loaded career never got its car |

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
