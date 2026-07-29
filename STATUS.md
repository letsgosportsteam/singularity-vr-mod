# STATUS — session handoff

Last updated **2026-07-29**. Read this first, then `ENGINE_NOTES.md`.

## Where the project is

A **working, wearable VR mod** for Singularity (2010, Unreal Engine 3, D3D9, x86):

- The game renders into the Quest via OpenXR, with correct colours
- **3-DOF head tracking** drives the in-game view (pitch and yaw)
- Ships as one `d3d9.dll` dropped beside the game exe — no game files modified

Current build: **`spikes/view_matrix/`** (spike 9) — everything below plus the camera-position
probe. **`spikes/vr_render/`** (spike 8) is the known-good fallback if spike 9 misbehaves.
Earlier spikes are kept deliberately; several document approaches that did *not* work, which is
most of their value.

## Quick start

```powershell
.\spikes\view_matrix\build.ps1 -Install     # builds x86, copies into the dev game copy
```

Connect **Virtual Desktop** first (VDXR is the only usable 32-bit OpenXR runtime — Meta's own
crashes in `xrCreateSession`, SteamVR has no 32-bit runtime at all), then launch:

```
R:\SingularityVR-Dev\Singularity\Binaries\Singularity.exe
```

In gameplay: **F9** head tracking · **F10** 6-DOF head position · **F6** VR-correct projection ·
**F8** recentre · **F5/F4** flip yaw/pitch sign · **F7** scan for the view matrix ·
**F3** constant offset probe · **F11** offset amount.
Log: `%LOCALAPPDATA%\SingularityVR\view_matrix.log`. Uninstall = delete `d3d9.dll`.

Paths, toolchain and game copies: `ENVIRONMENT.md`. Nothing is tied to the repo location.

## What works, and how

| Piece | Mechanism |
|---|---|
| Frame into the headset | backbuffer → `GetRenderTargetData` → SYSTEMMEM → D3D11 → XR swapchain |
| View **pitch** | MinHook detour on `FUN_0104e390`, the view-rotation source |
| View **yaw** | direct write to the live controller `+0x64` (the detour's seam carries pitch only) |
| FOV | write to `mCurrentPOV` FOV, `+0x0438` |
| Finding live objects | `GObjects` walk; live actor = `Outer == PersistentLevel`, name lacks `Default__` |

## ✅ Camera POSITION — solved 2026-07-29

This was the blocker for **both 6-DOF and stereo**, and it is now controllable. Not through the
engine's object model — three attempts at that failed — but by intercepting the **world→clip
matrix** on its way to the GPU and pre-multiplying a translation into it. Confirmed live: the
view moved.

| Fact | Value |
|---|---|
| Where | `SetVertexShaderConstantF`, device vtable slot 94 |
| Register | `c0`, ROW storage (registers are matrix rows) |
| Space | **translated-world** — UE3 pre-subtracts the view origin on the CPU |
| To move by `o` | `row3 -= o.x*row0 + o.y*row1 + o.z*row2` |

Findings from the same runs, all in `ENGINE_NOTES.md`:

- **The engine renders in translated-world space.** Probing the world camera position alone
  would have found nothing; the origin probe is what made the scan work.
- **The convention was settled by a direction test**, not the position test — `c0` read as ROW
  gives `dotFwd = +1.0000`, read as COL gives `+0.0023`, from identical bytes.
- **CPU frustum culling is a non-issue at stereo scale.** Measured from the backbuffer: a
  1.4–2.9% band at 300 UU, and **0 px at 100 UU and below**. The planned FOV-widening job is
  therefore **dropped** — a real IPD is a few UU.

## ✅ 6-DOF — working, confirmed 2026-07-29

The headset's real position drives the view (F10). Leaning and ducking move the camera.

## ⚠️ VR-correct projection — works, but FLICKERS (run 4)

The wider field of view reads as far more natural in the headset, but the image pulses: a zoom
flash on the monitor, black bars flicking in and out top and bottom in the headset. Stops when
F6 turns it off.

The monitor symptom is the important one — that is the game's own backbuffer, so the **rendered**
FOV is oscillating, not just the metadata we submit. Leading hypothesis: the engine recomputes
FOV each tick and interpolates toward its own default, so our Present-time write survives some
frames and not others. A narrow 65°/16:9 frame submitted with its true (narrow) frustum would
show exactly the reported top-and-bottom bars, since the vertical deficit is far larger than the
horizontal one.

Second candidate, independent of the first: `c0` is written ~50 times a frame by different
passes, so if two carry genuinely different projections, the captured FOV depends on which
wrote last.

**Instrumented, not yet diagnosed.** The build now reads the FOV fields back *before*
overwriting them (min/max over 60 frames shows the shape of any fight) and reports the spread
of captured projections within a single frame plus the number of injected windows. That
separates the two hypotheses.

## VR-correct projection — what it does

Scale could not be judged in the headset because the projection layer was claiming the
**headset's** FOV for an image the game rendered at a different one — a uniform stretch that
made head movement, world scale and object size all wrong together, so none could be measured
against another. Now the submitted frustum is *derived from the matrix* (read, not assumed) and
the game is asked for a vertical FOV matching the headset so that truth also fills the view.
Toggle with **F6**.

### ⏭ Next actions

1. **Diagnose the FOV flicker** from the instrumented log — see the two hypotheses above.
2. **Explain the brightness change** (run 4): the image is brighter and less blue than during
   3-DOF testing, though not broken the way the sRGB double-encode was. Two candidates, and
   they are distinguishable by turning matrix injection off while leaving the projection on:
   UE3 auto-exposure adapting to a much wider field (benign), or our injection touching a
   post-processing pass that uses the camera matrix (a real bug). The per-frame injected-window
   count in the log tests the second directly.
3. **Get two images per frame** — the last piece for real stereo, and the only hard one left.
   The architecture renders once per frame, so this needs alternate-eye submission (one frame
   stale per eye, cheap), engine re-entry (proper, hard), or depth reprojection.
4. **Tune `kMetresToUU`** (currently 52.5, from UE3's 16 units per foot) once the projection is
   correct enough to judge scale against.

## Other known work, roughly by value

- **Performance** — the frame copy is a full CPU round-trip (~14 MB each way at 2560×1440).
  The fast path needs the game's device upgraded to D3D9Ex, which needs a
  `D3DPOOL_MANAGED` → `DEFAULT` wrapper (**10,454** allocations measured in spike 2).
- **FOV / aspect mismatch** — game renders 16:9, per-eye view is ~2496×2688.
- **Head-look vs aim are coupled** — `PlayerController.Rotation` drives both view and fire trace.
- **Menus are inert** — intended; they need OpenXR *input* (controllers, thumbsticks, laser
  pointer), which has not been touched at all. Same work unlocks motion-controller aiming.
- **GOG install crash** — a latent Raven bug (unchecked NULL from a factory) that hits on a fresh
  GOG install with missing redistributables. The install guide must warn about it.

## Method notes that earned their keep

- **Verify by writing, not by reading.** A struct holding plausible live values is not proof the
  renderer uses it. `mCurrentPOV` looked exactly like the camera and steers only FOV.
- **When guessing fails twice, change tools.** A hardware write breakpoint found the rotation
  source immediately after seven failed guesses.
- **Shotgun, then narrow.** Testing candidates one at a time behind a hotkey let a working case
  get cycled past unseen.
- **Doc errors become code bugs.** `ENGINE_NOTES` recorded the backbuffer as `X8R8G8B8`; it is
  `A8R8G8B8`, and that mistake propagated straight into a `D3DERR_INVALIDCALL`.
