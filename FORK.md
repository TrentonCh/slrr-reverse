# Fork notes: physics + force feedback for slrr-reverse

This fork of [EliasFD3S/slrr-reverse](https://github.com/EliasFD3S/slrr-reverse) adds a
modern vehicle physics backend and real force feedback behind the host's existing
native contract, so stock Java scripts and existing mods keep working.

Upstream marks the physics bodies as out of scope ("OOS"), so there is nothing to
preserve there. We write new. Everything else in the host we leave alone unless a
native we need is missing.

## Remotes and branches

| Remote     | Points at                                   | Use                      |
|------------|---------------------------------------------|--------------------------|
| `upstream` | github.com/EliasFD3S/slrr-reverse           | fetch only, never push   |
| `origin`   | your fork (add after clicking Fork on GitHub)| push                     |

`main` mirrors `upstream/main` and is never committed to directly.
`dev` is the working branch. Upstream lands work in large rewrites, so rebase `dev`
onto `upstream/main` often and keep new code in new files (see "Where new code goes").

```powershell
git remote add origin https://github.com/<you>/slrr-reverse.git
git push -u origin main dev
# later, to pick up upstream changes:
git fetch upstream
git checkout main && git merge --ff-only upstream/main && git push origin main
git checkout dev && git rebase main
```

## Windows setup

1. Visual Studio 2022 with the "Desktop development with C++" workload. That includes
   CMake and the Windows SDK; nothing else is needed to compile.
2. Street Legal Racing: Redline v2.3.1 from Steam. The host runs from inside that folder
   and reads the game's RPK packs and compiled scripts. Nothing from the game is copied
   into this repo.
3. D3DX is loaded at runtime (`d3dx9_43.dll` and older, see `render_d3d9_part2.inc`).
   The Steam install normally has the DirectX June 2010 redistributable; if textures fail
   to load, install "DirectX End-User Runtime" from Microsoft.
4. Ghidra (free) for reverse engineering. Upstream's `tools/` has both Ghidra and IDA
   helpers; IDA is optional.

Build (from the README):

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S engine -B engine/build -G "Visual Studio 17 2022" -A Win32
& $cmake --build engine/build --config Release
```

Run from the game folder. `--game --no-wait` is a capped-frame smoke test; `--game --window`
runs windowed.

```powershell
cd "C:\Program Files (x86)\Steam\steamapps\common\Street Legal Racing Redline v2.3.1"
<path-to-fork>\engine\build\Release\slrr_engine.exe --game --no-wait
```

## Where new code goes

| New code                         | Location                                  |
|----------------------------------|-------------------------------------------|
| Vehicle model (rigid body, suspension, tire, drivetrain) | `engine/Runtime/Physics/` (new) |
| FFB computation (rack torque, filters, clipping) | `engine/Runtime/Physics/ffb_*` |
| Standalone test harness          | `engine/tools_physics/` (new, own CMake target) |

Touch upstream files only at the native dispatch points:
`Runtime/Parts/WheelRef.cpp`, `Runtime/Parts/Body/Chassis.cpp`,
`Runtime/Resources/PhysicsRef.cpp`, `Runtime/Cars/Cars.cpp`,
`Core/Platform/input_win32.cpp` (FFB output).

## The native contract we must honour

Everything the scripts hand to native code per wheel and per car. Names are the
host's native symbols; the script side is `java.game.parts.WheelRef`,
`java.game.parts.bodypart.Chassis`, `java.util.resource.PhysicsRef`, `java.game.Vehicle`.

- Suspension: `setStiffness setDamping setDamping_1 setRestLen setMinLen setMaxLen
  setMaxLoad setLoadSmooth setArm setHub setInstantCenter setBearing setOppWheel`
- Tire: `setPacejka setFriction setFrictn_x setSliction setRollRes setCPatch
  setRadius setWidth`
- Per-tick inputs: `setSteer setDrive setBrake setHBrake setForce`,
  `Chassis_setTorque setAckermann setSteerWheel setSteerWheelRadius setBuck`
- Read-back: `getPos getYpr getRadius getSteer getDrive getBrake getHBrake`,
  `Chassis_getMass getCM getTorque getWheel getWheels getWheelPos getWheelDamage`,
  `PhysicsRef_getPos getOri getVel getAngVel setMatrix setStatic createBox createSphere`,
  `Vehicle_getSpeedSquare`
- Rebuild: `Chassis_forceUpdate` re-ingests the part tree after a garage change
  (`forceUpdate_ingestPart`, `forceUpdate_cloneHdr`, `forceUpdate_tail`).

Rule: additive only. New natives are fine. Changing what an existing native means breaks
every mod that calls it.

## Roadmap

0. Build on Windows, run the smoke test, confirm the host boots the stock scripts.
1. Read the Java side for units and conventions: `Chassis.java`, the suspension and
   tire part scripts, `Engine.java`. Record findings in `docs/units.md`.
2. Ghidra on the few things scripts don't tell us. Bookmarks from upstream comments
   (addresses are for the exe build upstream analysed; verify against yours):
   - `Chassis_physWheelTick` 0x455BA8..0x455F15, aero 0x456A6B..0x456D2D
   - `Chassis_forceUpdate*` (mass / CM / inertia aggregation), `Chassis_allocCamBlob` 0x44A250
   - `Part_buildPhysSlotTable` 0x46EAE0, `Part_allocPhysBlob` 0x46E930
   - `Physics_createPrimitive` 0x49AB60, `Phys_allocBodyFromPrimitive` 0x49B300
   - `Input_DiDevice_setFeedback` 0x558260, `Input_ffbStrengthEmulated` 0x61AB14
   - `JVM_compileSource` 0x4165A0, `JVM_compileAllDir` 0x418EB0 (for the compiler later)
3. Standalone physics module with its own harness: Jolt for rigid body and collision,
   per-wheel spring/damper from the WheelRef params, Pacejka or brush tire model,
   drivetrain fed from `Chassis_setTorque`, fixed step at 500 Hz or higher.
4. FFB: one constant-force DirectInput effect updated every physics tick from
   steering-rack torque (tire self-aligning moment + lateral force x trail), with
   filtering and a clipping meter. The host already has the DirectInput effect slots
   (`ffb_fx[3]`, `g_input_force_feedback_enabled`); replace the stock "emulated strength"
   axis smoothing with real output. Under Wine this maps to evdev constant force.
5. Bind the natives above to the module. Getters must return what scripts expect
   (Y-up, yaw/pitch/roll via `getYpr`).
6. Later milestones, independent of each other: a `.java` source compiler (upstream
   loads only precompiled TUFA `.class` files today), SDL3 platform port and a modern
   renderer behind the `GfxEngine`/`RenderRef` natives, script debugging aids.

## Hygiene before any release

- `engine/data/System.class`, `engine/data/System.cons.bin` and
  `engine/assets/StreetLegal_Redline_exe/*` are derived from the game. The host needs
  them at runtime today (`class_index_path()` in `main_part1.inc`). Plan: read them from
  the player's game install instead, and never ship them in a release.
- Modified scripts are Invictus / ImageCode copyright. Distribute script changes as
  patches applied to the player's own install.
