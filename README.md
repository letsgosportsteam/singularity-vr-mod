# Singularity VR

A VR mod for _Singularity_ (2010, Raven Software, Unreal Engine 3.584). It is a `d3d9.dll`
proxy: it forwards every Direct3D 9 export to the real system library, and along the way renders
the game in stereo to an OpenXR headset with 6-DOF head tracking, motion-controller input, and a
weapon that follows your hand.

**No game files are modified.** One DLL goes in beside the game exe. Uninstalling is deleting it.

> **This is an alpha.** It has run on very few machines so far.

**[Download the latest release →](https://github.com/letsgosportsteam/singularity-vr-mod/releases)**

---

## Install — unzip into one folder

Unzip the release into the game's `Binaries` folder, beside `Singularity.exe`:

```
d3d9.dll
openxr_loader.dll
SingularityVR.ini
```

`SingularityVR.ini` ships pre-configured. The in-headset settings menu changes it live and saves
automatically, so you rarely need to touch it by hand.

Then connect Virtual Desktop and launch `Singularity.exe` **with no arguments**. It starts in VR.

**The first run will be at the wrong resolution.** That is expected. The mod must inject
`-ResX`/`-ResY` before the engine reads the command line — which is before it can ask the headset
anything — so it queries the headset during the run and uses the answer on the *next* launch.
Launch once, quit, launch again.

## Requirements

| | |
|---|---|
| Game | _Singularity_ (2010) for PC — Steam or GOG |
| Headset | **Virtual Desktop** with **VDXR** as the OpenXR runtime — all testing has been on a **Quest 3S** |
| Runtime | **Visual C++ 2015–2022 Redistributable (x86)** — [installer](https://aka.ms/vs/17/release/vc_redist.x86.exe), needed to run the game with the mod installed, not just to build it |
| OS | Windows 10 or 11 |
| Network | Highly recommended: a dedicated router (PC on ethernet), **or** Virtual Desktop's wired USB connection (currently in beta) |

This build only supports Virtual Desktop with VDXR. Nothing else has been tried against
Singularity yet — see `STATUS.md` if SteamVR or OpenVR is what you're curious about.

**The network matters more than your GPU.** On shared WiFi, frame submission was measured blocking
for up to **189 ms**, which reads as seconds-long stalls and cannot be fixed in the render code.
Wired to a dedicated router — or over Virtual Desktop's wired USB connection, currently in beta —
the same build locks to 120 fps with submission under a millisecond. If it stutters, suspect the
link first.

## What works today

| | |
|---|---|
| Native stereo, real per-eye parallax | ✅ draw-call duplication; eyes align, depth correct |
| 6-DOF head tracking | ✅ position and rotation, sampled inside the frame it is drawn for |
| Head roll | ✅ applied in the view matrix |
| Resolution | ✅ **100% of the headset's pixels**, inherited automatically |
| Performance | ✅ **120 fps** on the development machine, with occasional dips or stutters |
| Motion controllers | ✅ full Touch mapping, menus, haptics, snap or smooth turn |
| Aim decoupling | ✅ the shot follows the controller; the view never moves |
| Weapon follows the controller | ✅ 6-DOF, arms hidden, anchor tuned |
| Laser pointer | ✅ toggleable aiming aid |
| Weapon alignment | ✅ position and rotation tunable live, in the settings menu |
| Head-tracking aim option | ✅ aim with your head instead of a motion controller — works with mouse/keyboard or a gamepad |
| HUD and menus in both eyes | ✅ health, ammo, crosshair, prompts, pause menu |
| Sniper scope in both eyes | ✅ whole scope per eye, world drawn once |
| Head tracking during cutscenes | ✅ automatic — look around freely, turning restored on exit |
| VR settings menu | ✅ live changes, in the headset, saved automatically |

## Controls

Touch controllers act as a full gamepad, with haptics — this is every physical input on both
controllers and what it does.

**Left controller**

| | |
|---|---|
| Thumbstick | Move / strafe |
| Thumbstick click | Dash |
| Trigger | Aim |
| Grip | Impulse |
| X | Age / renovate (TMD) |
| Y | Flashlight — **hold ~1 s** to open the VR settings menu instead |
| Menu | Pause menu — **hold ~1 s** to recentre (re-zeroes your view and seating) |

**Right controller**

| | |
|---|---|
| Thumbstick | Turn (snap or smooth, set in the settings menu) — tilt up/down cycles weapons or uses a health item |
| Thumbstick click | Anti-gravity |
| Trigger | Fire |
| Grip | Use / reload |
| A | Jump — **hold ~1 s** to also toggle the laser sight (you'll jump once on the way, since jump has to stay instant) |
| B | Crouch |

Every button beyond the two long presses is Raven's own binding, passed straight through.

The F-key bindings described in `STATUS.md` are development keys. They are **inert** unless
`Debug=1`.

## Configuration

Settings live in `SingularityVR.ini` beside the DLL, and the in-headset panel is the intended way
to change them. `SingularityVR.ini.example` documents every key but is **not loaded** — the
compiled-in defaults are the tested configuration, and the panel writes its own ini when you change
something.

Two traps if you hand-edit:

- **The ini takes the first key of a duplicated name, silently.** A second copy further down does
  nothing.
- **`AutoResX`/`AutoResY` are the mod's own cache of _your_ headset's size.** Never copy them from
  someone else's ini.

## Known issues

- This has only been tested on one machine, so your experience may vary.
- Missing or culled geometry is mostly resolved outside of in-engine cutscenes, where it isn't
  fixable yet. If you notice geometry not rendering elsewhere, set **OCCLUSION** to **DRAW ALL**
  on the ADVANCED page of the settings menu.
- A menu rotation bug: enter a game, pause, exit to the main menu, and the menu can be rotated.
- If the game dies instantly with no message, that is usually a missing 32-bit `PhysXLoader.dll` —
  and the **unmodded** game does it too. Run [`setup_physx.ps1`](spikes/view_matrix/setup_physx.ps1),
  which makes the game self-contained from a copy already on your machine.

`STATUS.md` is the live state of all of these and is always more current than this list.

## Planned

Next up: more testing, general bug fixes, and performance improvements.

Further out: SteamVR/OpenVR support, and Xbox controller support for the head-aiming mode.

Longer shots, and both will be tough: two-hand tracking, and maybe manual reloading.

## Reporting a bug

[Open an issue](https://github.com/letsgosportsteam/singularity-vr-mod/issues) and attach the log
from `%LOCALAPPDATA%\SingularityVR\`. `view_matrix.log` is always the run that just happened;
older runs sit beside it stamped with their start time.

Say which headset, which OpenXR runtime, Steam or GOG, and whether you had already relaunched once.

**One control worth a minute of your time:** rename `d3d9.dll` to `d3d9.dll.off` and run the game
again at the same resolution. If the problem is still there, it is not this mod. Please say whether
you tried it.

## Building

```powershell
.\spikes\view_matrix\build.ps1        # builds x86
.\packaging\make-release.ps1 -Version 0.1.0-alpha
```

Needs Visual Studio 2022 with the C++ x86 toolset, and the OpenXR SDK. Point at your own SDK with
`$env:OPENXR_SDK`; the build also runs an `/analyze` pass first that fails on format-string defects,
which is there because one such mistake cost four debugging sessions.

**On the layout:** `spikes/view_matrix/` is the live build — a single large `d3d9.cpp`.
`spikes/vr_render/` is the known-good fallback. The earlier spikes are kept deliberately: several
record approaches that did *not* work, which is most of their value.

## The development notes

Unusually for a mod, the working notes are published as-is:

| | |
|---|---|
| [`STATUS.md`](STATUS.md) | Current state, and what to do next |
| [`ENGINE_NOTES.md`](ENGINE_NOTES.md) | Engine internals, addresses, structures, Ghidra findings |
| [`HISTORY.md`](HISTORY.md) | The run log, newest first — search it, don't read it |
| [`FEASIBILITY.md`](FEASIBILITY.md) | The original assessment, kept for the record |

They include the wrong turns, and sections marked ⛔ **SUPERSEDED** where a claim went stale in
place. That is on purpose. The most expensive defects in this project were not hard problems — they
were stale premises inherited from an earlier run and never re-tested. A yaw delta that was never
wrapped cost months of misattribution; SSW silently halved every frame-rate number for thirty runs.
Recording that is more useful to the next person than a clean narrative would be.

## Licence

MIT — see [`LICENSE`](LICENSE), which notes one GPL-licensed development tool that is not part of
the mod and ships in no release.

Third-party components and their required notices are in
[`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt).

Not affiliated with or endorsed by Activision or Raven Software. _Singularity_ is their trademark.
This mod contains no game code or assets, redistributes no game content, and modifies no game
files.
