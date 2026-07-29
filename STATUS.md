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

In gameplay: **F9** head tracking · **F10** 6-DOF head position · **F1** true stereo
(draw duplication) · **F12** alternate-eye stereo ·
**F6** VR-correct projection · **F8** recentre · **F5/F4** flip yaw/pitch sign ·
**F7** scan for the view matrix · **F3** constant offset probe · **F11** offset amount.
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

## True stereo by draw-call duplication — bug found and fixed, next run pending (F1)

Alternate-eye's flaw is **temporal** — the eyes show the world at different instants, which no
reprojection can reconcile and head rotation makes obvious (confirmed run 6: "terrible flicker").
So draw both eyes from the same instant by issuing every scene draw twice.

**The prototype needs no double-wide render target.** It splits the existing backbuffer with the
**viewport**: left half gets the left-eye matrix, right half the right, and OpenXR is handed each
half as a `subImage` rect. Half horizontal resolution per eye (1280×1440), but the technique is
complete end to end. Viewport switching rather than render-target switching is what keeps it
affordable.

Still no interpupillary distance anywhere: the eye separation vector goes through the same
XR→game mapping as everything else, and the right-eye matrix is the left one plus that offset.

Duplication forces the VR projection on, since each eye occupies half the width and without the
matching per-eye frustum both come out squashed 2x horizontally.

### Cost is not the risk — run 7 confirmed it

A 2010 game on a modern GPU has enormous headroom, and run 7 proved it: a steady **~70 fps**
throughout normal gameplay with duplication on, a real improvement over the 15–40 fps seen
before F1 existed. Throughput is not the open question here.

### ✅ Bug found (run 7) and fixed: objects vanishing from one eye

Run 7 surfaced four symptoms at once: geometry present in only one eye, an ammo pickup
flickering on/off as the head tilted, a manhole decal missing outright, and a screen-space
effect that appeared to slide with head movement. All four traced to **one mechanism**.

The original code only split a draw across both eye halves when it could positively identify
the draw's matrix as the tracked camera (small `w` at origin, forward vector matching the
camera's real facing). Any draw that failed that test — decals, per-object transforms, screen
effects — fell through to a single, un-split, full-width draw instead. Since the finished
backbuffer is sliced in half at submission (left half to the left eye, right half to the
right), a full-width draw only survives the crop in whichever half it happened to occupy. One
mechanism, four symptoms: an object present in one eye, an object whose identification
flickered pass/fail as viewing angle changed while tilting, a decal with an unrecognized matrix
gone entirely, an effect with no offset copy reading as unstable relative to a properly-doubled
scene.

**Fix:** stop gating the *split* on identification. Always duplicate into both halves when F1
is on; use identification only to decide whether the right half's copy gets the eye offset or
is an exact copy of the left. A flat, non-parallaxed duplicate in both eyes is a minor visual
compromise; a duplicate missing from one eye outright is not. The perf log now separately
reports `split` (everything, both eyes) vs `with parallax` (the subset correctly offset) vs
`flat` (duplicated but without offset) — so the extent of the remaining problem is visible
rather than hidden. **Not yet re-run** — needs the same test as before, and the manhole/ammo
symptoms specifically.

## Alternate-eye stereo — works, but flickers on head turn (F12)

The game renders once per frame and a D3D9 shim cannot make it render twice, so the cheap route
to two images is to alternate: this frame is drawn from the left eye, the next from the right,
each eye's swapchain holding the last image drawn for it, both submitted every frame.

Almost no new maths was needed — **an eye offset is a head position offset**, so each eye's pose
goes through exactly the same XR→game mapping 6-DOF already uses, and interpupillary distance
never appears as its own quantity. Consequence worth knowing: stereo shares that path, so it
brings positional head tracking with it whether or not F10 is on.

Run 6 verdict: **the depth effect reads well and scale feels right, but head rotation produces
terrible flicker.** Each eye updates at half the frame rate and the two disagree during motion.
Superseded by the duplication prototype above, but kept — it costs nothing and may still be the
best option on weak hardware.

### Shipping plan: the mode is a user choice

All three approaches share the same front half — matrix identification, per-eye offset, forced
projection — and differ only in how the second image is produced. So the shipped mod should
expose the choice (an ini beside the DLL, since hotkeys are a development affordance):

| Mode | Trade |
|---|---|
| Draw-call duplication | True stereo, same instant, 2x geometry |
| Depth reprojection | Same instant, full rate, disocclusion slivers — **not yet built** |
| Alternate-eye | Cheapest, but flickers on head rotation |
| Mono | Always works; the safe fallback |

A fallback chain matters as much as the choice: reprojection needs INTZ depth support, and
duplication may not survive every scene, so the mod should be able to drop to mono rather than
fail.

## VR-correct projection — flicker FIXED (confirmed run 5)

Run 4: the wider field read as far more natural, but the image pulsed — a zoom flash on the
monitor, black bars flicking top and bottom in the headset.

The log settled it. Against a steady **127.9°** requested, the matrix reported `125.3, 127.2,
125.1, 123.8, 128.7, … 80.7`. The write **works** and is not clamped, but the engine's camera
update interpolates back toward its own default between our Present-time writes, with each
step sized by frame duration — hence the 80.7° outlier on a long frame. A rendered FOV swinging
128° → 80° is a visible zoom, and submitting that narrow frustum leaves the vertical short,
which is the bars.

**Fix, confirmed:** force the frustum in the matrix (rescaling the x/y columns is exactly an FOV
change), and demote the engine's FOV to a **culling-only** hint asked for at 15% headroom. The
submitted OpenXR frustum is the same forced constant rather than the observed value, so the two
cannot disagree.

Run 5 confirms the flicker is gone. Two things the log clarified:

- The engine's drift **continues** (its FOV read back anywhere from 65° to 133.9°, its matrix
  arriving between 80° and 141°) and no longer matters, which is the point of forcing.
- Despite rendering wider than the engine culled for on many frames, the measured culling band
  stayed **0 px** throughout gameplay. UE3's per-object culling evidently carries enough slack.
  The 15% headroom stays as insurance rather than a proven necessity.

Read the log carefully on one point: the `engine's own matrix on arrival` figure measures the
matrix **before** our edit, so it reports the engine's drift, not our output. The separate
`after forcing` line is the one that verifies our own arithmetic. And the modification count is
per **draw call** using the camera matrix (~50–60 a frame is normal), not per distinct matrix.

## VR-correct projection — what it does

Scale could not be judged in the headset because the projection layer was claiming the
**headset's** FOV for an image the game rendered at a different one — a uniform stretch that
made head movement, world scale and object size all wrong together, so none could be measured
against another. Now the submitted frustum is *derived from the matrix* (read, not assumed) and
the game is asked for a vertical FOV matching the headset so that truth also fills the view.
Toggle with **F6**.

### ⏭ Next actions

1. **Re-test F1** after the run-7 fix — the geometry-only-in-one-eye bug, the ammo flicker, and
   the missing manhole should all be gone, since they shared one cause. Watch the new perf log
   `flat` count for how much of the scene still lacks true parallax (post-processing, decals,
   anything not sharing the tracked camera matrix) — that's the honest remaining gap, now
   visible instead of hidden.
2. **Depth reprojection** as the alternative approach, worth building regardless so there's a
   real comparison — and a shipped mode either way.
3. **The ini** for mode selection, plus a fallback to mono.
4. **Explain the brightness change** (run 4, deferred): most likely UE3 auto-exposure adapting to
   a much wider field, which is benign; the alternative is our injection touching a
   post-processing pass. Distinguishable with F6.

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
