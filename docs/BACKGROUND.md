# Background: SLRR internals, ecosystem and project decisions

Research notes gathered on 2026-10-08 before forking. Everything here was verified
against upstream source, public repos or community posts on that date unless marked
as a claim. Keep this updated when something turns out to be wrong.

See `FORK.md` for the working plan, Windows setup and the native contract.

---

## 1. How Street Legal Racing: Redline is built

- **One exe, two halves.** `StreetLegal_Redline.exe` contains a custom Java-like VM
  plus roughly 360 "native" methods written in C++. Game logic (menus, garage, career,
  camera behaviour, part rules) lives in Java-syntax scripts under the game's
  `Scripts/` folders. The natives do rendering, resource loading, input, sound and
  the vehicle simulation.
- **Scripts compile on demand.** The stock exe compiles a `.java` file into its own
  class format the first time the class is referenced and no compiled file exists.
  That is why dropping edited `.java` files into the install works.
- **Script dialect.** The community source-pack notes describe it as Java "with a few
  extensions, lots of simplification and an unknown number of bugs". Compiled classes
  carry a header with magic, version and build number, so classes compiled by one
  exe build are rejected by another. This is why mods break on every exe update.
- **Physics is native, not scripted.** The exe has a per-wheel physics tick
  (`Chassis_physWheelTick`), Pacejka-style tire parameters stored per wheel, aero drag
  and primitive collision. `Chassis.java` (about 25 KB) and the part RPKs only feed
  parameters in. Community "physics mods" are parameter tuning. The fall-through-ground
  and curb-flip bugs are in the native integrator and collision code and cannot be
  fixed from scripts.
- **Rendering** is fixed-function Direct3D 9: render states, texture stages,
  `D3DRS_LIGHTING`, `SetMaterial`. No shaders. Meshes are an "INVO v3" format with
  vertex position, normal, one UV set, bones and LOD levels. Materials carry diffuse,
  specular, alpha and a global environment-map flag for reflective surfaces.
- **Resources** ship in RPK packs. `resdecode.exe` turns an RPK into editable RDB text
  and `resconvert.exe` turns it back. Part definitions reference a click mesh, a shape
  mesh, textures (a `.tex` file lists texture load order) and render properties.
- **Stock force feedback** is minimal: a DirectInput path gated on device type 3 with
  three canned effect slots, plus a config value `FFB_strength_emulated` that only
  smooths the steering axis in software.
- **Known exe bugs** (Silent's 2018 analysis of v2.3.1): off-by-one `memmove` and
  `strncpy` copies, a config tokenizer that overruns when a `.cfg` lacks a trailing
  newline (`asphalt.cfg`, `graph.cfg`, `graph_fade.cfg`), and 4-byte tag reads past
  short lines. None reproduce reliably; they show up as random crashes. The exe was
  later updated without a build-number change, which broke the Proofix plugin.

## 2. State of the ecosystem (October 2026)

- **Source code.** Raxat at ImageCode, author of v2.3.0 and v2.3.1, received the
  Invictus source (per Silent's blog, secondhand). It has never been released. In a
  2019 Steam thread a former contributor (Malinko / OsageSparky) claimed to have the
  full source on backup and ImageCode asked for it by email; no outcome was posted.
- **Street Tuning Evolution.** The Invictus Kickstarter was cancelled by the creator
  at $23,788 of $150,000. ImageCode's version is listed on Steam (app 905240, UE4 +
  PhysX) as "to be announced"; community threads in 2025 report no visible progress.
  Do not plan around it.
- **Community tools found:** `resdecode` / `resconvert` (RPK to RDB and back),
  SLRReditor by Sparky (car and career editing), the Java source pack v1.1 on
  slmodders.com, a "Launch Game Editor" option in the Steam launcher, and a
  `modding_tools` folder in the install. MethanolFuel's LE2MWM GitHub repo bundles a
  `ResourceConverter` folder and `StreetLegalDevKit.exe`.
- **Mesh tooling is the gap.** No Blender importer was found. The community has used
  ZModeler and 3ds Max filters. Upstream slrr-reverse decodes the mesh format in its
  renderer, which is the best public reference for it.
- **Communities:** GOM-Team and VStanced Discords, slrr.pl, streetlegalmods.com,
  promods.ru, the Steam discussions for app 497180.
- **Prior native-hook projects**, all proving the exe can be hooked and all stalled:
  - CookiePLMonster/SLRR-Proofix (2018, C++ DLL using MemoryMgr, MIT, archived 2019).
  - nedobylskiy/SLRRMultiplayer (2016, Frida + Node, GPL-2, "development frozen").
  - bobberdolle1/slrr-macos-resolution-fix (2026, Python, CrossOver/Wine).

## 3. Assessment of upstream slrr-reverse (commit 5c42f3b, 2026-10-08)

**What it is.** A C++ host that replaces the entire native side of the exe so the
stock Java scripts run unchanged. It never loads or calls into the original exe. The
natives table is keyed to the original exe's function addresses ("VA-backed"),
recovered with IDA and Ghidra. MIT licensed. One developer, started 2026-08-25,
seven commits landed in large pushes, heavily AI-assisted (tooling notes mention a
128 KB editor buffer cap and Cursor rules). About 77,000 lines of C++.

**Layout and sizes.**

| Component | Lines | Notes |
|---|---:|---|
| JVM (`engine/Core/Jvm`) | 9,488 | tree-walking interpreter; opcode case map (73 entries) and tree flag table copied from exe VAs 0x4239B0 and 0x5F0914 |
| D3D9 renderer | 6,515 | fixed-function; D3DX loaded at runtime from `d3dx9_43.dll` and older |
| DirectInput input | 1,284 | mirrors stock FFB slots and emulated-strength smoothing |
| DirectSound audio | 1,264 | |
| DirectShow FMV | 834 | |
| RPK loader | 629 | portable |
| Render natives | 1,082 | |

**Native coverage by name** (from upstream `PROGRESS.md` files): Audio 10/10,
Cars 4/8, IO 35/37, Parts 72/72, Chassis 23/23, Render 34/34, Resources 104/105,
System 52/71. Unimplemented: `Vehicle`, `Navigator`, `Thread`, `Object`, `MouseCursor`.

**Fidelity markers in the code.** "PE" = reproduces original exe behaviour (3,675
mentions), "Soft" = approximate stand-in that does not match original memory layout
(1,501), "stand-in" (307), "OOS" = out of scope for now (402).

**What works.** Boots the script VM, loads RPKs, uploads meshes with bones and
textures, draws OSD text. `--game --no-wait` is a smoke test that auto-creates a
profile and runs a capped number of frames. Flags: `--boot --game --window --no-wait
--auto-new`. It cannot drive a car: rigid bodies, force accumulation and the wheel
tick are out of scope, aero writes side data with no velocity change, map collision
is partial.

**No source compiler.** The host loads only precompiled "TUFA" `.class` files. To
change a script today: edit the `.java`, run the stock exe once to compile it, then
run the host. Upstream's load-path notes give the compiler addresses:
`System.compileAll` 0x47C080, `JVM_compileAllDir` 0x418EB0, `JVM_compileSource`
0x4165A0, `JVM_compileHook` 0x412B10, `JVM_getClass` 0x410890,
`JVM_loadClassByFqn` 0x410630, `JVM_addClass_fromChunks` 0x411B20.

**Debugging aids:** essentially none (three "script error" sites, no stack traces,
line numbers, breakpoints or hot reload).

**Game-derived files in the repo:** `engine/data/System.class`,
`engine/data/System.cons.bin`, `engine/assets/StreetLegal_Redline_exe/*` (cursors,
icon). Needed at runtime by `class_index_path()`; do not delete, plan to read from the
player's install instead.

**Build facts.** MSVC 2022, CMake, Win32 target. Links `d3d9 dinput8 dxguid dsound
winmm user32 gdi32 windowscodecs ole32 oleaut32 strmiids`. No DirectX SDK needed.
Headers used: `d3d9.h dinput.h dsound.h dshow.h wincodec.h`, all of which mingw-w64
ships, and the non-MSVC CMake branch has no link libraries, so a MinGW cross-build
from Linux is plausible with a small CMake change (untested).

## 4. Routes considered

| Route | Verdict |
|---|---|
| Hook the stock exe (ASI/DLL, Frida) | Good for FFB, input, QoL and crash fixes. Dead end for new physics: fights the native integrator, collision and part-mass code; breaks on silent exe updates. |
| Export built cars to Assetto Corsa / BeamNG | Loses the garage and part assembly, which is the point of SLRR. |
| Faithful host (this fork of slrr-reverse) | Every existing mod keeps working. Inherits 2003 architecture and Windows/D3D9 for now. Physics is the least finished part, which is exactly where new work goes. |
| Clean asset port (new engine, e.g. Godot 4 + Jolt) | Full freedom and Linux-native, but Java-based mods do not run and garage/career rules must be rebuilt. |

**Decision:** fork slrr-reverse, build the physics and FFB as a standalone module
behind the existing native contract. The module is engine-agnostic so it also serves
an asset port later if that route ever becomes preferable.

## 5. Design notes

### Physics module
- Rigid body and collision through Jolt (MIT). Chassis as one body; wheels via
  suspension rays or shape casts against collision built from the map's ground data.
- Suspension per wheel from `WheelRef` params: stiffness, damping, rest/min/max
  length, max load, load smoothing, arm, hub, instant centre, bearing, opposite wheel.
- Tire: Pacejka Magic Formula or a brush model, fed from `setPacejka`, friction,
  sliction, rolling resistance, contact patch, radius, width. Which Magic Formula
  coefficients the game's parameter set maps to is a reversing task (`docs/units.md`).
- Drivetrain: engine torque arrives via `Chassis_setTorque`; wheel drive/brake/
  handbrake per tick via `WheelRef` setters. The engine model itself is script-side.
- Mass, centre of mass and inertia come from the part tree through
  `Chassis_forceUpdate` and must be recomputed when parts change in the garage.
- Fixed step at 500 Hz or higher, decoupled from the render rate.
- Read-back must match script expectations: Y-up, yaw/pitch/roll via `getYpr`.

### Force feedback
- Rack torque = tire self-aligning moment + lateral force x mechanical trail, summed
  over steered wheels, through a low-pass filter, with a clipping indicator.
- One DirectInput constant-force effect updated every physics tick, replacing the
  stock emulated-strength smoothing. The host already has the effect slots
  (`ffb_fx[3]`, `g_input_force_feedback_enabled`, `Input_DiDevice_setFeedback`
  0x558260, `Input_ffbStrengthEmulated` 0x61AB14).
- Optional extras: suspension-load texture, kerb and slip vibration.
- Steering must bypass any script-side steering assist when a wheel is present
  (`Controller_user_SetAxisSmooth / SetAxisSpeed / SetAxisForce` natives exist).
- Under Wine, DirectInput constant force maps to evdev `FF_CONSTANT`.
- Later: SDL3 haptics as the cross-platform backend.

### Renderer and platform (later milestone)
- Scripts never touch Direct3D. They call about 90 render natives: `GfxEngine`
  (viewports, cameras, video modes, present, global envmap), `Viewport`, `Camera`
  (fog), `Text` (OSD), `RenderRef` (scene nodes, bones, lights, flares, lines, routes,
  colour, matrices, LOD detail), `Animation`, `ParticleSystem`. Replace the backend
  behind these and the scripts never know.
- Candidate backends: SDL3 GPU or bgfx (Vulkan on Linux, D3D12 on Windows). SDL3 for
  window, input, audio; ffmpeg for FMV or drop the intro videos.
- Achievable with the existing assets: HDR and tonemapping, bloom, TAA/FSR, cascaded
  shadow maps, ambient occlusion, reflection probes and a clearcoat paint shader
  (envmap flag identifies paint and chrome), dynamic time of day, ultrawide, triples,
  VR via OpenXR.
- Content limits: no normal or roughness maps (use material-class heuristics or
  generated maps), low-resolution textures (upscale), 2003 polygon counts (build a
  mod override path for HQ replacement packs early), UI laid out in scripts with 4:3
  coordinates (fix in scripts, not the renderer).

### Game logic and the VM
Three layers, each changed in its own place:

| Layer | What lives there | How to change it |
|---|---|---|
| Scripts | game rules, UI, career, cameras, engine sound selection | edit the `.java` files |
| Natives | engine services the scripts call | add or implement in the fork |
| VM | language, loader, limits, debugging | rewrite freely in the fork |

Rule: keep the native contract additive. New natives and new language features keep
existing mods working; changing an existing native's meaning breaks them. Owning the
VM also allows removing the build-number class lock, fixing dialect bugs, adding
stack traces and hot reload, and later a source compiler or precompilation.

## 6. Linux notes (for the eventual cross-platform build)

- The stock game runs under Proton with DXVK translating D3D9 to Vulkan; this helps
  performance and compatibility, not visuals.
- Kernel 6.15 and later ship `hid-universal-pidff` (Moza, Cammus, VRS, FFBeast).
  Logitech G920/G923 use the in-kernel HID++ driver since 6.3; older Logitech wheels
  get full effect support from the out-of-tree `new-lg4ff`. Fanatec needs out-of-tree
  `hid-fanatecff`; Simagic on firmware after v159 needs out-of-tree `simagic-ff`.
- A MinGW cross-build of the host looks feasible (section 3). D3DX is resolved at
  runtime, so Wine's own `d3dx9_43` implementation would serve it.

## 7. Legal and hygiene

- Never redistribute game assets, compiled scripts or the exe. Read everything from
  the player's Steam install, as OpenMW does with Morrowind.
- Modified scripts are Invictus / ImageCode copyright. ImageCode has said the Java
  files are released and the community redistributes modified scripts openly, but
  patches applied to the player's install are the safe distribution form.
- Strip the game-derived files listed in section 3 from any release build.

## 8. Sources

- Upstream repo: https://github.com/EliasFD3S/slrr-reverse
- Silent's 2018 analysis: https://silentsblog.com/2018/06/15/slrr-proof-of-fix/
- SLRR-Proofix: https://github.com/CookiePLMonster/SLRR-Proofix
- SLRRMultiplayer: https://github.com/nedobylskiy/SLRRMultiplayer
- Steam thread on the source code (2019): https://steamcommunity.com/app/497180/discussions/0/1770385542777386134/
- Java source pack notes: https://slmodders.com/download/street-legal-racing-redline-sl2-java-source-pack-v1-1/
- RPK decoding guide: http://archive.vstanced.com/page.php?al=rpk_basics
- Adding parts guide: https://www.scribd.com/document/684572041/How-to-Add-New-Parts-to-SLRR
- MethanolFuel LE2MWM repo: https://github.com/MethanolFuel/SLRR-LE2MWM-MethanolFuel-Edition
- Street Tuning Evolution on Steam: https://store.steampowered.com/app/905240/Street_Tuning_Evolution/
- Kicktraq STE status: https://www.kicktraq.com/projects/473961744/street-tuning-evolution/
- Linux universal-pidff: https://lwn.net/Articles/1003941/ and https://cateee.net/lkddb/web-lkddb/HID_UNIVERSAL_PIDFF.html
- new-lg4ff: https://copr.fedorainfracloud.org/coprs/lnvso/new-lg4ff
- hid-fanatecff: https://github.com/gotzl/hid-fanatecff
