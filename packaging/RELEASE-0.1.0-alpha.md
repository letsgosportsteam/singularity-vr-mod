**Alpha.** First public build of a VR mod for _Singularity_ (2010). This has only run on a small
number of machines so far.

It is a `d3d9.dll` proxy. **No game files are modified**, and uninstalling is deleting one file.

---

## Before you download

**This build only supports Virtual Desktop with VDXR as your OpenXR runtime.** All testing has
been done on a **Quest 3S**.

**Highly recommended: a dedicated router (PC on ethernet), or Virtual Desktop's wired USB
connection (currently in beta).** See *"The network matters more than your GPU"* below.

**You need the Visual C++ 2015–2022 Redistributable (x86).** The x86 one — the x64 you already have
will not do. Needed to *run* the game with the mod installed, not just to build it.
[Installer](https://aka.ms/vs/17/release/vc_redist.x86.exe).

Singularity (2010) for PC — Steam or GOG. Windows 10 or 11.

## Install

Unzip `SingularityVR-0.1.0-alpha.zip` into the game's `Binaries` folder, beside `Singularity.exe`
— it contains everything you need:

```
d3d9.dll
openxr_loader.dll
SingularityVR.ini
```

`SingularityVR.ini` ships pre-configured. The in-headset settings menu changes it live and saves
automatically, so you rarely need to touch it by hand.

Connect Virtual Desktop **first**, then launch `Singularity.exe` with no arguments. It starts in VR.

**The first run will be at the wrong resolution — this is expected.** The mod must inject
`-ResX`/`-ResY` before the engine reads the command line — which is before it can ask the headset
anything. So it queries the headset during the run and uses the answer next time. **Launch once,
quit, launch again.**

## What works

- **Native stereo** with real per-eye parallax — eyes align, depth is correct
- **6-DOF head tracking**, sampled inside the frame it is drawn for
- **Full resolution** — 100% of the headset's pixels, inherited automatically
- **120 fps** on the development machine, with occasional dips or stutters
- **Touch controllers** as a full gamepad, with haptics, snap or smooth turn
- **Aim decoupling** — the shot follows your controller, the view never moves
- **The weapon follows your hand**, 6-DOF
- **Laser pointer** for aiming
- **Weapon alignment** — position and rotation tunable live, in the settings menu
- **Head-tracking aim option** — aim with your head instead of a motion controller, using regular
  mouse/keyboard or a gamepad
- **HUD and menus in both eyes** — health, ammo, crosshair, prompts, pause menu
- **The sniper scope in both eyes**
- **Head tracking during cutscenes** — look around freely
- **A VR settings menu**, changes applied live and saved automatically

## Controls

Every physical input on both Touch controllers, and what it does.

**Left controller**

| | |
|---|---|
| Thumbstick | Move / strafe |
| Thumbstick click | Dash |
| Trigger | Aim |
| Grip | Impulse / melee |
| X | Age / renovate (TMD) |
| Y | Flashlight — **hold ~1 s** for the VR settings menu instead |
| Menu | Pause menu — **hold ~1 s** to recentre |

**Right controller**

| | |
|---|---|
| Thumbstick | Turn — tilt up/down cycles weapons or uses a health item |
| Thumbstick click | Anti-gravity |
| Trigger | Fire |
| Grip | Use / reload |
| A | Jump — **hold ~1 s** to also toggle the laser sight (you'll jump once on the way) |
| B | Crouch |

## Known issues

- **This has only been tested on one machine**, so your experience may vary.
- **Missing or culled geometry is mostly resolved** outside of in-engine cutscenes, where it isn't
  fixable yet. If you notice geometry not rendering elsewhere, set **OCCLUSION** to **DRAW ALL** on
  the ADVANCED page of the settings menu.
- **Menu rotation bug** — enter a game, pause, exit to the main menu, and the menu can come back
  rotated.
- **If the game dies instantly with no message**, that is usually a missing 32-bit
  `PhysXLoader.dll` — and the **unmodded** game does it too. `setup_physx.ps1` is in the repository
  and fixes it from a copy already on your machine.

## Planned

Next up: more testing, general bug fixes, and performance improvements.

Further out: SteamVR/OpenVR support, and Xbox controller support for the head-aiming mode.

Longer shots, and both will be tough: two-hand tracking, and possibly manual reloading.

## The network matters more than your GPU

On shared WiFi, frame submission to the headset was measured blocking for up to **189 ms** — that
reads as seconds-long stalls, and nothing in the render code can fix it. Wired to a dedicated
router — or over Virtual Desktop's wired USB connection, currently in beta — the same build runs
at 120 fps with only occasional dips or stutters. **If it stutters, suspect the link before
anything else.**

## Reporting a bug

[Open an issue](https://github.com/letsgosportsteam/singularity-vr-mod/issues) and attach the log
from `%LOCALAPPDATA%\SingularityVR\` — `view_matrix.log` is always the run that just happened.

Say which headset, which OpenXR runtime, Steam or GOG, and whether you had already relaunched once.

**Worth one minute before you report:** rename `d3d9.dll` to `d3d9.dll.off` and run the game again
at the same resolution. If the problem is still there, it is not this mod. Please say whether you
tried it.

---

`d3d9.dll` is unsigned, so SmartScreen and some antivirus will flag it. Verify what you downloaded:

```
97899ac56ca2a444ed3deee4e85b4e254f412fdc0620bb563aee52b55c06e248  SingularityVR-0.1.0-alpha.zip
3ca6e1e023c7b42c3a1f8711057f0fb0a3316a58583bfcbf61dcf188932e1e6b  d3d9-0.1.0-alpha.pdb
```

Full development notes, including everything that did not work, are in the repository —
`STATUS.md` for current state, `HISTORY.md` for the run log.

MIT licensed. Not affiliated with or endorsed by Activision or Raven Software.
