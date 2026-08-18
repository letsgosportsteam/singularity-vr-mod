**Alpha.** First public build of a VR mod for _Singularity_ (2010). Shared to gather reports, not
because it is finished — this has run on very few machines, and there are known gaps below. Expect
crashes.

It is a `d3d9.dll` proxy. **No game files are modified**, and uninstalling is deleting one file.

---

## Before you download

**You need Virtual Desktop with VDXR as your OpenXR runtime.** This is not a preference.
Singularity is a 32-bit process and almost no modern OpenXR runtime still supports x86 — Meta's own
crashes in `xrCreateSession`, and SteamVR has no 32-bit runtime at all. If you do not have Virtual
Desktop, this build will not run for you.

**You need the Visual C++ 2015–2022 Redistributable (x86).** The x86 one — the x64 you already have
will not do. [Installer](https://aka.ms/vs/17/release/vc_redist.x86.exe).

Quest or Pico headset, Windows 10 or 11, and the game on Steam or GOG.

## Install

Copy both files into the game's `Binaries` folder, beside `Singularity.exe`:

```
d3d9.dll
openxr_loader.dll
```

Connect Virtual Desktop **first**, then launch `Singularity.exe` with no arguments. It starts in VR.

**The first run will be at the wrong resolution — this is expected.** The mod has to inject
`-ResX`/`-ResY` before the engine reads the command line, which is before it can ask the headset
anything. So it queries the headset during the run and uses the answer next time. **Launch once,
quit, launch again.**

## What works

- **Native stereo** with real per-eye parallax — eyes align, depth is correct
- **6-DOF head tracking**, sampled inside the frame it is drawn for
- **Full resolution** — 100% of the headset's pixels, inherited automatically
- **120 fps locked** on the development machine
- **Touch controllers** as a full gamepad, with haptics, snap or smooth turn
- **Aim decoupling** — the shot follows your controller, the view never moves
- **The weapon follows your hand**, 6-DOF
- **HUD and menus in both eyes** — health, ammo, crosshair, prompts, pause menu
- **The sniper scope in both eyes**
- **Head tracking during cutscenes** — look around freely
- **An in-headset settings panel**, changes applied live and saved on close

## Controls

Every controller button is already spoken for, so the extra functions are long presses.

| | |
|---|---|
| **Hold MENU** ~1 s | Recentre |
| **Tap MENU** | Pause menu |
| **Hold Y** ~1 s | VR settings panel |
| **Hold A** ~1 s | Laser sight (you will jump once on the way — A is jump) |
| **APPS** | Weapon follows the controller, on/off |
| **BACKSPACE** | Peek at the flat game. Does not save |

## Known issues

- **It crashes.** It is an alpha and very few machines have run it.
- **Geometry can wink out** at distance or near screen edges. Set `OcclusionQueryMode=0` in the ini
  — roughly twice the draw calls, but everything is forced visible.
- **Cutscene culling is not solved.** Parts of a scene may be missing during in-engine cutscenes.
- **Menu rotation bug** — enter a game, pause, exit to the main menu, and the menu can come back
  rotated.
- **If the game dies instantly with no message**, that is usually a missing 32-bit
  `PhysXLoader.dll` — and the **unmodded** game does it too. `setup_physx.ps1` is in the archive and
  fixes it from a copy already on your machine.

## The network matters more than your GPU

On shared WiFi, frame submission to the headset was measured blocking for up to **189 ms** — that
reads as seconds-long stalls, and nothing in the render code can fix it. Wired to a dedicated
router, the same build locks to 120 fps with submission under a millisecond. **If it stutters,
suspect the link before anything else.**

## Reporting a bug

[Open an issue](https://github.com/letsgosportsteam/singularity-vr-mod/issues) and attach the log
from `%LOCALAPPDATA%\SingularityVR\` — `view_matrix.log` is always the run that just happened.

Say which headset, which runtime, Steam or GOG, and whether you had relaunched once already.

**Worth one minute before you report:** rename `d3d9.dll` to `d3d9.dll.off` and run the game again
at the same resolution. If the problem is still there, it is not this mod. Please say whether you
tried it.

---

`d3d9.dll` is unsigned, so SmartScreen and some antivirus will flag it. Verify what you downloaded:

```
5c1d3f5557c0c287ad72f14f7e87e439f5813cf38510ebdd98ba6241e8a924c5  sing-vr-0.1.0-alpha.zip
3ca6e1e023c7b42c3a1f8711057f0fb0a3316a58583bfcbf61dcf188932e1e6b  d3d9-0.1.0-alpha.pdb
```

Full development notes, including everything that did not work, are in the repository —
`STATUS.md` for current state, `HISTORY.md` for the run log.

MIT licensed. Not affiliated with or endorsed by Activision or Raven Software.
