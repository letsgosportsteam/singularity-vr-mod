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

In gameplay: **F9** head tracking · **F8** recentre · **F5/F4** flip yaw/pitch sign ·
**F7** scan for the view matrix · **F3** offset the camera through it.
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

## The one blocking problem: camera POSITION

Unsolved, and it gates **both 6-DOF and stereo** (per-eye separation *is* a position offset).
Three approaches tried, all documented in `ENGINE_NOTES.md`:

1. Writing all five candidate position fields at once — no effect
2. Hardware write breakpoint on camera `Location` — only generic actor-move plumbing with a
   dozen callers each; no camera-specific seam
3. `FUN_0104e420` (adjacent to the rotation source) — turned out to be rotation, not position

The pattern suggests this engine simply doesn't compute camera position at one interceptable
point the way it does rotation.

### ⏭ Next action: run spike 9 — `spikes/view_matrix/`

**Built and compiling clean; needs a headset session to produce results.** It intercepts the
**view matrix on its way to the GPU** (`SetVertexShaderConstantF`), bypassing the object model
entirely — the approach established VR injectors use, and one that solves 6-DOF *and* stereo
with a single mechanism.

It finds the matrix rather than guessing at register numbers: for a world→clip matrix `clip.w`
is view-space depth, so the camera itself must transform to `w = 0`. Substituting the known
camera position and keeping the 4-register windows that cancel to near zero — from ~20,000 UU
out — is a test that is very hard to pass by accident. Both matrix storage conventions and
three probe points are covered in one scan, the third being the origin, which catches UE3's
translated-world case.

To run: `.\spikes\view_matrix\build.ps1 -Install`, reach gameplay, **stand still**, press **F7**
to scan, read the log, then **F3** to offset the camera 300 UU on world Y. If the view slides,
position is solved. Full interpretation guide in `spikes/view_matrix/RESULTS.md`.

Note stereo needs a **second** thing beyond position control — see below.

### Stereo needs a second thing beyond position control

Two images per frame. Options: alternate-eye (one frame stale per eye, cheap), engine re-entry
(proper, hard), or depth reprojection (fallback). The current architecture renders once per
frame, so this stays open even once the matrix is controllable.

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
