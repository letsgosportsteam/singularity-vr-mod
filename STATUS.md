# STATUS — session handoff

Last updated **2026-07-31** (end of run 49). Read this first, then `ENGINE_NOTES.md`.

> # 🚀 ZERO-COPY LANDED (run 39): 4096×2160 runs at ~115 fps, up from ~60.
>
> The CPU round-trip is **gone** — `StretchRect` into a shared D3D9Ex surface, `CopyResource` into
> the XR swapchain, both GPU-side. Frame copy went from ~9.8 ms to **0.00 ms**, and the app is now
> **display-bound with 4+ ms of idle headroom** instead of arriving late every frame. Performance is
> no longer a project problem. See **Run 39**, and **Run 38** for the `D3DPOOL_MANAGED` wrapper that
> made it legal.

> # 🚨 THE GAME RUNS AT ~110 FPS. EVERY PERFORMANCE NUMBER BEFORE RUN 32 WAS WRONG.
>
> **The "37–40 fps" this project has recorded for thirty runs was Virtual Desktop's Synchronous
> Spacewarp**, which pins the app at *half* the headset refresh on purpose and synthesises the
> frames in between. With SSW off and the headset at 120 Hz, the same build runs at **105–118 fps
> (8.5–9.6 ms/frame)**. There was no performance problem to solve.
>
> Every historical measurement here — the bimodal distribution, the "CPU-bound stall", run 31's
> conclusions — was taken through that halving and must be re-derived before it is trusted. See
> **Run 32**.

> # ⚠️ Run 31's verdict is RETRACTED
>
> Run 31 concluded "the frame copy is 3% of the frame, cancel the D3D9Ex plan". That was measured
> under SSW: 0.88 ms of a frame padded out to 27.8 ms. **Unconfounded it is ~4.2 ms of an ~8.9 ms
> frame — closer to half.** The D3D9Ex work is back on the table. See **Run 32**.

> # ✅ THE FLICKER IS SOLVED (run 30)
>
> **It was UE3's hardware occlusion queries.** The engine draws a bounding box per primitive,
> reads the pixel count back, and skips drawing that primitive later if it came back zero. In
> true stereo that readback is wrong, and it was wrong on a huge scale: with results forced to
> "visible", **draws per frame went from ~1000 to 2100–3800** and every reported object came back
> solid. The engine was discarding **50–70% of its own draw calls.**
>
> Fixed by `OcclusionQueryMode=0` (auto), now the default. Frame rate is unchanged — 37–40 fps
> with peaks of 70 — because this build is bottlenecked on the CPU frame copy, not on draw calls.
>
> Seven mechanisms were eliminated by direct test before this one landed. See **Run 30**.

> # 🆕 Run 49: head roll landed, and Stage 4B finally has its measurement built
>
> **Roll is written.** Tilting your head now tilts the view — the last axis of the head pose that
> was never driven anywhere. It goes into the **view matrix**, not the engine's rotation, and the
> maths is verified as arithmetic before any headset run (`spikes/roll_math`, `.\build.ps1`).
> **NUMPAD1** toggles, **NUMPAD2** flips the sign.
>
> **⛔ Stage 4B is MEASURED (run 50) and it does not fit — build Stage 4A instead.** Render-target
> switching costs **~5 ms of work** for 2,822 binds a frame. Idle collapses **3.09 → 0.17 ms** and
> the app falls off the 120 Hz deadline. The **~19 ms** estimate that blocked this for eight runs
> was wrong by 4×, and the answer is *still* no — while run 40 had already removed *resolution* as
> a reason to want it, so it would buy correctness only.
>
> **Stage 4A gets the same three fixes** — seam, post-processing bleed, occlusion boxes — **for one
> patched constant**, and run 41 already found it: `ps c1 = (0.5, -0.5, 0.50023, 0.50012)`.
>
> ⚠️ **And the cheap version of that probe measured nothing.** Level 1 binds away and back with no
> draw between; D3D9 applies state lazily, so the driver collapsed it — 2,576 binds a frame for
> +0.36 ms. Level 2 forces the bind with a 1×1 `Clear`. Had level 2 not been built, this would have
> been the eighth tautological instrument in this file.

## TL;DR — where we stand

**Working and wearable.** Head tracking, 6-DOF, correct colours, a VR-correct projection, and
**true native stereo** with real per-eye parallax. The stereo mechanism is validated: the eyes line
up, depth reads correctly, and it holds ~37 fps at 2560×1440 / ~28 at 3840×2160.

**The flicker is fixed** (run 30 — occlusion query readback). Objects no longer vanish. Everything
remaining on the list is planned work rather than a bug.

### If you are picking this up cold, read these three things

1. **The seam.** In true stereo the frame is side by side, left half → left eye. Any geometry that
   reaches the frame *without* being remapped into an eye-half keeps full-frame coordinates — and
   the centre of the frame **is** the seam, so a head-on object is clipped out of *both* halves and
   disappears. Three separate causes have fed into this one mechanism.
2. **Draw-path coverage is the unsolved half.** Catching every route geometry takes to the GPU is
   what keeps failing. `DrawPrimitiveUP`/`DrawIndexedPrimitiveUP` are the currently-known gap, and
   as of run 27 they are hooked again — the thing that used to block them is fixed.
3. **Placement depends on matrix identification, and that is the fragility.** A draw we cannot
   remap gets drawn once at full-frame coordinates, which puts a centred object *on* the seam. So
   any failure to recognise the view matrix — for one draw or for a whole frame — turns straight
   into the reported artefact. Run 27 fixed several ways that failed; **Stage 4** below is the
   change that removes the dependency altogether.

### Next session, in order

With the flicker closed, the ladder's remaining rungs are all *quality* work rather than
debugging.

1. **✅ DONE — zero-copy (runs 38–39).** 4096×2160 at ~115 fps, frame copy 0.00 ms, app
   display-bound with 4+ ms idle. Turn it on with `D3D9ExMode=1` + `ZeroCopy=1`.
   **Remaining follow-ups, both small:** confirm no tearing by eye, and consider raising the 100 ms
   fence bound that fired once during a level load.
2. **⛔ Per-eye render targets (Stage 4B) — MEASURED (run 50) and it does not fit. Do Stage 4A.**
   ~5 ms of work for 2,822 render-target binds a frame; idle collapses 3.09 → 0.17 ms and the app
   falls off the 120 Hz deadline. The ~19 ms estimate that blocked this for eight runs was wrong by
   4×, but the answer is still no. And run 40 already removed *resolution* as a reason to want it,
   so it would buy correctness only.
   **Stage 4A gets the same three fixes for one patched constant** — viewport split plus per-half
   `ScreenPositionScaleBias`, and run 41 already found that constant exactly
   (`ps c1 = (0.5, -0.5, 0.50023, 0.50012)`). This is the next real piece of work.
3. **Raise the resolution by defeating UE3's own validation**, not by rebuilding the render path.
   `GetDeviceCaps` reports 16384×16384, so the hardware allows the 5376×2880 side-by-side target
   full per-eye parity needs; what refuses 4992 is the engine.
3. **`GetCommandLineW` detour** to inject `-ResX`/`-ResY` from the headset's reported size, so the
   manual command line goes away. Designed, not implemented; must fail safe or the game will not
   launch.
4. **Vertical culling shortfall** — 98° rendered against the engine's 51.1° (run 33). Revisit the
   headroom removed in run 28; **PAGE UP** walks it.
2. **Resolution.** Per-eye is currently 1280×1440 against the headset's 2496×2688. Needs the
   command-line detour (design already written up under *How resolution will work*), and probably
   per-eye render targets given the ~4096 cap.
3. **Controller input and menus.** Untouched — needs OpenXR input. Same work unlocks
   motion-controller aiming.
4. **✅ DONE — head roll (run 49).** Applied in the view matrix, on by default, **NUMPAD1** toggles
   and **NUMPAD2** flips the sign. Needs one headset run to confirm the sign and to watch for
   corner dropout — the engine still culls with an unrolled frustum.

Two loose threads worth keeping visible, neither blocking:

- **`HookUserPointerDraws` stays 0.** Level 3 routes UE3's `DrawDenormalizedQuad` — the fullscreen
  quad every post-process pass uses — through `StereoPair`, which scissors it to a half and
  overwrites `c0` underneath it. That is the "rainbow" of run 28. Fixing it needs draw
  classification, and with the flicker solved there is no longer a reason to rush it.
- **Post-processing still bleeds across the seam** (bloom and similar sample the whole target).
  Stage 4 below addresses it as a side effect.

### Method note earned the hard way this session

**Five theories about the flicker were wrong, each killed by the diagnostic built to test it.** What
finally worked was *elimination* — turning one intervention off at a time — not another mechanism.
Reach for that sooner. Equally: three of those wrong theories assumed a single-threaded draw path,
and a bisection ladder driven from the ini (no rebuild per test) settled in three launches what
guessing had not in three sessions.

## How resolution will work in the shipped mod (design, run 17)

Manual `-ResX=`/`-ResY=` is a **development crutch, not the shipping design.** Every other VR title
inherits resolution from the headset and lets the platform's own render-scale slider adjust it, and
this mod should behave the same way.

The mechanism: OpenXR's `xrEnumerateViewConfigurationViews` reports a *recommended* per-eye rect,
and that value **already includes whatever render-scale the user set in SteamVR or the Meta app**.
So honouring it gives exactly the expected behaviour, with no per-user command line. The build now
queries and logs it, along with the exact launch line it implies:

```
headset wants 2496x2688 per eye (max ...) - includes your runtime's render-scale setting
  => for side-by-side stereo, launch the game with:  -ResX=4992 -ResY=2688 -windowed
```

To remove the manual step entirely, the DLL loads **before** the game parses its command line, so it
can detour `GetCommandLineW`/`A` and append the computed `-ResX`/`-ResY`. That is the piece that
turns this from guidance into automatic behaviour — **not yet implemented**, deliberately: it must
fail safe (leave the command line untouched if the XR query fails) or the game will not launch.

Note this is also why forcing the backbuffer failed (run 14): the resolution has to reach UE3's own
system settings, which the command line does and a `CreateDevice` override does not.

## Ladder status (against the original plan)

| Rung | State |
|---|---|
| Head-tracked image in the headset | ✅ done |
| Colours — sRGB | ✅ done |
| **6-DOF positional tracking** | ✅ **done** — camera position solved via the view matrix |
| **Stereo — per-eye offset for real depth** | ✅ **done** — true native stereo by draw-call duplication; eyes align, depth is correct, and the vanishing objects are fixed (run 30, occlusion query readback) |
| FOV / aspect match | 🔧 projection is VR-correct and forced; **resolution cannot be matched in the current design** — the cap is 4096 (run 33), giving 2048 per eye against 2688 wanted. Needs per-eye render targets |
| Performance — CPU round-trip → D3D9Ex zero-copy | ✅ **done (runs 38–39)** — `D3DPOOL_MANAGED` wrapper + shared-surface frame path. 4096×2160 went from ~60 to **~115 fps**, frame copy 9.8 ms → **0.00 ms**, and the app is now display-bound with idle headroom |
| Controller input, menus, aim decoupling | ⬜ not started (mouse/stick coexistence fixed run 12 as a prerequisite) |

So: **6-DOF is solved**, and stereo is *working* rather than done — the mechanism is right and the
remaining issues are draw-path coverage.

### Measured, so it does not have to be re-derived

| Quantity | Value |
|---|---|
| View matrix | `c0`, **ROW** storage, **translated-world** space |
| Eye separation | 3.32 UU = 6.3 cm (correct IPD), stable |
| Headset wants | **2496×2688 per eye** → 4992 wide for side-by-side |
| Per eye at 2560×1440 | 1280×1440 — 51% of the panel |
| Per eye at 3840×2160 | 1920×2160 — 77% of the panel |
| Resolution cap | **4096 confirmed** (run 33) — 4096×2160 accepted and the scene targets followed; 4992 refused. Classic D3D9 max texture dimension |
| Per eye at 4096×2160 | **2048×2160 — 76% linear of the 2688×2880 wanted.** Side-by-side needs 5376 wide, so parity is impossible without per-eye targets |
| Frame rate | **112–117 fps at 4096×2160** with zero-copy (run 39); 105–118 at 1440p. SSW **off**, 120 Hz. The old "~37 fps / floor ~13" figures were SSW halving the rate and are void |
| Frame copy | **0.00 ms** under zero-copy (run 39). Was ~9.8 ms of a ~16 ms frame at 4K on the CPU path |
| Engine FOV | horizontal at 16:9, dips to its 65° default ~7% of samples |
| Scene targets | 2× scene-sized (A8R8G8B8 + A16B16G16R16F), shadow map 512×512 R32F |

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

**Cold start is flat**, as always. **BACKSPACE reaches head tracking + 6-DOF + true stereo in one
press** instead of three (run 37 reverted run 36's default-on, which rendered the untested main
menu split from launch — BACKSPACE gets the same one-key convenience without changing what the
game looks like before you press anything). BACKSPACE again drops back to flat, dropping the
forced projection with it — a mono image stretched to 98° looks broken rather than standard.

Individual toggles, unchanged: **F9** head tracking · **F10** 6-DOF head position · **F1** true
stereo (draw duplication) · **F12** alternate-eye stereo ·
**F6** VR-correct projection · **F8** recentre · **F5/F4** flip yaw/pitch sign ·
**F7** scan for the view matrix · **F3** constant offset probe · **F11** offset amount ·
**INSERT** blank one eye half (mapping test) · **DELETE** swap eye/half assignment ·
**PAGE UP** cycle culling headroom (now runs *down* from 1.0) · **PAGE DOWN** cycle how much of
the headset's FOV we render · **HOME** scissor off (drawn-then-clipped vs never-drawn) ·
**END** clip out everything driven by the tracked register (remapped vs unremapped) ·
**NUMPAD1** head roll on/off · **NUMPAD2** flip the roll sign ·
**NUMPAD3** render-target switch probe (Stage 4B's cost, image unchanged).

**Do not press F12 after F1.** F1 clears alternate-eye, but F12 does *not* clear duplication, so
that order leaves both stereo modes running at once. F12 before F1 is harmless and pointless.

Mouse and head tracking now **add** rather than fight — turning with the mouse no longer snaps
back, and the same fix is what stick-turning will need in the shipped mod.
Log: `C:\Users\<you>\AppData\Local\SingularityVR\view_matrix.log` — **read it directly from disk**;
it does not need pasting into chat. Truncated at each attach and now stamped with the run's date and
time on line 1, so a stale file is obvious at a glance. Uninstall = delete `d3d9.dll`.

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

### ✅ Run 9: the both-eyes disappearances are a CULLING shortfall, not a stereo bug

Ammo, the gun and the manhole vanish from **both** eyes — and the manhole was already flickering
before duplication existed. That rules stereo out: if a draw call is never issued, our hook
never sees it and duplication cannot help.

Cause, measured. The engine's FOV is **horizontal at 16:9** — a wide, short frustum. Ours is the
headset's: narrow and **tall**. Vertical coverage is therefore the binding constraint, and the
old 1.15 headroom never met it. Asking 133.9°, the engine actually held ~124°, dipping to 105.8°:

| Engine horizontal | Its vertical culling FOV | We render | Shortfall |
|---|---|---|---|
| 124.0° | 93.2° | 98.0° | ~5° |
| 105.8° | 73.2° | 98.0° | ~25° |

Anything in that band is culled before a draw call exists. Ground objects entering the bottom
shortfall as the head tilts down is exactly the reported ammo/gun behaviour.

**Fix:** `kFovHeadroom` 1.15 → **2.5** (asks ~160°, holds ≥130° through the observed dips,
comfortably past the ~128° needed for 98° vertical). Not yet re-run.

### ❌ Run 11: the shadow-map theory is FALSIFIED — and the wall is now clear

The offscreen gate skipped only **11 draws of ~1,640**. Shadow rendering would be hundreds, so
shadow maps are not being separated by size — either this build allocates them at scene
resolution (UE3 does exactly that for whole-scene dominant shadows) or they do not pass through
`SetRenderTarget(0)` as assumed. Behaviour was unchanged, as the numbers predict.

**The reported signature is what matters:** it is not geometry going missing but *textures*,
worst **on the ground**, flickering while walking, and present in one eye only.

That is the signature of **screen-space UV sampling**. UE3 shaders that derive texture
coordinates from screen position — deferred decals (a manhole is a decal), projected ground
effects, SSAO, anything using `ScreenPositionScaleBias` — get that transform from a shader
constant computed for the **full** render target. Halving the viewport invalidates it, and the
error differs per half: the left half maps screen x∈[0,1280] as if the target were 2560 wide, the
right half maps x∈[1280,2560] into UV [0.5,1]. Different wrongness per eye is exactly "renders in
one eye only", and decal-heavy ground surfaces are the worst affected.

**This is the wall predicted at the outset, and it is broader than "post-processing".** It is
every shader that reasons in screen space, including ordinary ground decals. Viewport-splitting
cannot fix it without also finding and rewriting those constants per half — a second
matrix-hunt-scale investigation with no guarantee of a single interceptable constant.

Note the double-wide render target does **not** rescue this: the constants would then be
computed for a 5120-wide target and still be wrong for each 2560 half.

**Diagnostic added:** a render-target census logging every distinct target (size, format, draw
count) once a second, so what is actually being split is known rather than assumed.

### 🔧 The rework this motivated: clip-space remap + scissor, never the viewport

The diagnosis points at a better design rather than a workaround. **Stop changing the viewport.**
Leave it exactly as the game set it, and place each eye in its half by remapping **clip space** in
the matrix: scale x by 0.5, shift by ∓0.5, so the left eye occupies NDC x ∈ [−1,0].

Why that fixes it. A shader computing `UV = (clip.xy/clip.w) * scale + bias` with constants built
for the full target maps NDC [−1,1] → UV [0,1]. Under the remap, left-eye geometry lands in NDC
x ∈ [−1,0], the full-width viewport puts that in pixels [0, W/2], and the shader's own maths gives
UV [0, 0.5] — **the same half**. Geometry and texture lookups agree again, because the viewport
never lied to the shader.

The **scissor** rectangle takes over the clipping the halved viewport used to do. Scissor discards
pixels without altering the NDC→pixel transform, so it costs nothing in shader correctness — it
only stops draws we could not remap (transform sourced from a register we do not track) spilling
across the seam into the other eye.

Geometry checks out: the forced per-eye frustum is 91.3° × 98.0°, `tanX/tanY = 0.889` matching a
1280×1440 half, and the remap maps that frustum onto exactly half the full-width viewport.

**✅ Confirmed fixed (run 12): ground textures no longer appear in one eye only.** The
screen-space diagnosis was right and the remap is the correct mechanism.

### The render-target census (run 12) — what is actually being drawn

```
2560x1440  A8R8G8B8       ~27000 draws/s  <- SPLIT   (LDR / backbuffer path)
2560x1440  A16B16G16R16F  ~37000 draws/s  <- SPLIT   (HDR scene colour)
 512x512   R32F              ~840 draws/s  left alone (shadow depth)
```

Two scene-sized targets, both correctly split; the shadow map **is** correctly excluded — it was
simply always a small fraction (~1%), which is why excluding it changed nothing visible. Useful to
have on record rather than assumed.

### ✅ Run 13: the flicker is RESOLUTION, not LOD — theory refuted by its own test

Narrowing the render FOV to 70% cut the flicker sharply. But the engine's own FOV barely moved
between the two settings — **132.0° at 100% render, 133.4° at 70%** — and the culling headroom
(PAGE UP) made no perceptible difference at all. Since streaming and LOD key off the *engine's*
FOV, and that was constant, **the LOD/streaming theory is refuted.**

What actually changed is angular sampling density. The same 1280×1440 half squeezed into a
narrower field:

| Render FOV | Per-eye px/deg | vs headset (~25 px/deg) |
|---|---|---|
| 100% (91.3°) | 1280/91.3 = **14.0** | ~55% |
| 70% (62.5°) | 1280/62.5 = **20.5** | ~80% |

Undersampling high-frequency texture and then upscaling ~1.9× to fill the eye is exactly what
shimmers — worst on detailed interiors and small distant objects, which is precisely the reported
pattern (building interiors, and a desk that vanished at 100% but was visible at 70%).

So the answer to "should we fix LOD to keep the VR FOV" is **no** — the lever is resolution, and
raising it lets the full FOV come back.

**Test-design error worth recording:** PAGE UP was only ever pressed while at 70% render, where
the engine's culling (79.9° vertical at headroom 1.0) already comfortably covered our 68.6° render.
Headroom genuinely could not matter in that configuration, so "no difference" was not evidence
about headroom at all. The instructions caused that.

### Run 16: alignment CONFIRMED FIXED; sharpness is arithmetic, not a bug

> ⚠️ **Superseded in part by run 27.** The `906 with parallax, 0 flat` below was a tautology —
> `remapKnown` was `const bool ... = true`, so that line could not report anything else. The
> forced-projection readback and the lock behaviour in this section still stand; the parallax
> claim does not.

**The eyes line up correctly now.** The revocable lock did its job — 10 bad locks were dropped and
re-scanned, and parallax landed on essentially every split draw (`906 with parallax, 0 flat`) with
the forced projection exact (`91.3 x 98.0 (want 91.3 x 98.0)`, 321 frames). The stereo mechanism is
validated.

#### Why 4K did not look sharp — side-by-side halves the width per eye

This is arithmetic, and it is the key number for planning:

| Frame | Per eye | vs headset (2496×2688) |
|---|---|---|
| 2560×1440 | 1280×1440 | 51% × 54% |
| **3840×2160** | **1920×2160** | **77% × 80%** — still upscaled |
| 4992×2688 | 2496×2688 | **100%** — parity |

So "4K" in side-by-side stereo is really 1920 wide per eye. It is a genuine 1.5× improvement over
1440p, but still short of the panel, which is exactly why it reads as soft rather than sharp.
**`-ResX=4992 -ResY=2688` is the true VR-ready setting** — 3.6× the pixels of 1440p, and the reason
the CPU round-trip has to go.

#### ⚠️ Still open: the locked register saw 194 distinct projections

Over one session, the locked window carried projections from **36.7×21.1°** to **143.9×119.8°**.
Some of that spread is the engine's own FOV drifting frame to frame, but values that far apart are
different *passes* — the first-person weapon renders with its own narrower FOV. Forcing all of them
to the scene frustum corrupts the ones that legitimately differ.

Two fixes for it:
1. **Nothing is modified without a lock.** Previously, before a lock existed, every passing window
   was modified — so unrelated passes were being forced. Now the scan must identify the scene
   matrix first, and only that one window is ever touched.
2. **The lock is chosen by upload count, not by facing agreement.** The scene view-projection is
   uploaded once per pass and used by most of the frame's draws (50 uploads in the original scan,
   against 4–9 for impostors). Ranking by dot-product let a rare pass win whenever its number was
   marginally better, which is how a 36.7×21.1° matrix got locked. A minimum of 8 uploads is now
   required to lock at all, and scans re-arm automatically since nothing is modified without one.

### ❌ Run 15: 4K worked, but exposed the biggest fragility in the design — a non-revocable lock

`-ResX=3840 -ResY=2160 -windowed` on the command line **does** work (the engine sizes its scene
targets from it, unlike the backbuffer override). But the run was a mess, and the cause was ours:

```
split 1274 (0 with parallax, 1274 flat)          <- NOTHING got an eye offset
own matrix on arrival: 180.0 x 180.0   (x47)     <- degenerate columns
own matrix on arrival:  90.0 x  90.0   (x12)     <- a cube-map face projection
after forcing, the matrix measures 127.9 x 98.0 (want 91.3 x 98.0)   <- stale, not success
```

Register locking was added purely as an optimisation, and it quietly became the worst single point
of failure: **once set it was never re-examined.** At 4K the lock landed on something that is not
the scene matrix — a real one reads ~128×98°, these read 180×180° (x column length ≈ 0) and 90×90°
(unit columns, i.e. a 90° cube-map face). Because the modify path only ever tests the *locked*
window, recognition could never recover. So no draw was offset, no projection was forced, the
geometry kept the engine's own ~158° projection while we submitted a 91.3° frustum, and the halves
were sliced out of a mono wide image — which is exactly "wildly zoomed, eyes don't line up".

Worse, it *reported success*: the forced-projection readback kept echoing 127.9 from before F1
changed the target.

**Three fixes:**
1. **The lock is now revocable.** Verified every frame; after 60 frames without holding it is
   dropped and the next frame re-scans. A wrong lock now costs a second, not a session.
2. **Captured projections are sanity-bounded** (tangent 0.15–8.0), so 180×180 and 90×90 can no
   longer pollute the diagnostic or make a broken lock look healthy.
3. **The forced readback resets when the target changes**, so a leftover value can never again be
   mistaken for partial success.

### ❌ Run 14: forcing the BACKBUFFER does not raise UE3's render resolution

Tried, and it broke stereo outright. The census was unambiguous:

```
render-target census (scene is 3840x2160):
     2560x1440  A8R8G8B8       7103 draws  left alone
     2560x1440  A16B16G16R16F 19946 draws  left alone
```

**Zero draws split.** UE3 allocates its scene render targets from its *own* configured resolution,
not from the backbuffer. Overriding the backbuffer only desynchronised the two: the engine kept
rendering 2560×1440 into a 3840×2160 buffer, our scene test (which compares the bound target to the
backbuffer) stopped matching, and duplication silently switched itself off. Slicing a mono
2560×1440 image out of a 3840-wide buffer hands the right eye 640 px of picture and 1280 px of
black — exactly the reported symptom, and the cull band measured that black to the pixel
(`right=1280 of 3840`).

**Correct route (confirmed run 15):** UE3's own command line, which the engine reads into its
system settings so it sizes scene targets to match:

```
Singularity.exe -ResX=3840 -ResY=2160 -windowed
```

`-windowed` is needed because fullscreen clamps to real display modes — on a 1440p monitor the
video menu will not offer more. There is no game ini to edit; Singularity keeps video settings in
its binary `.ue3profile`. (Driver DSR/VSR is the alternative, exposing higher modes to the menu.)

The override has been removed rather than left as a disabled option, since it cannot work. The ini
stays for the mode selection the shipped mod needs.

**Guard added:** if duplication is on and *nothing* was split, the log now says so loudly. Stereo
silently doing nothing cost a whole session here; it should never be inferred from a census again.

## ✅ Run 49: head roll, and the instrument that decides Stage 4B

Two independent pieces. Neither has been in the headset yet — see *The test* at the end.

### Head roll — the last unwritten axis

F9 drove pitch and yaw. `r->roll` was never written, so tilting your head left the horizon glued to
the headset. It is now applied **in the view matrix**, and the choice of location is the whole
result.

**Why not the engine's rotation.** The detour owns pitch, yaw already needs a direct write to the
controller, and UE3 zeroes camera roll in several places. Three seams to fight for an angle we
already own outright, because by the time the matrix reaches the GPU it is ours.

**⚠️ And why the compositor does not already do it — this is the trap.** The submitted projection
layer has always carried the head's full orientation, roll included, so it looks like the runtime
should be rotating the image for us. It does not: a projection layer is reprojected by the **delta**
between the pose we claim and the pose the display is at. Claim a rolled pose for an unrolled render
while the head is at that same roll, the delta is zero, and the image is presented straight — which
is *exactly* the reported symptom. Baking roll into the render is what makes the claimed pose true,
and the two then agree by construction. **Nothing on the submit side needed changing**, which is
also why this looked for a while like it might already work.

**Where it goes in the pipeline**, and both constraints are satisfied by one placement — in
`Hook_SetVSConstF`, immediately after `ApplyProjection`:

| Constraint | Why |
|---|---|
| **After** the forced projection | The tangents read there are the frustum the eye really sees. Under duplication `g_targetTanX` is already the half-width per-eye value. |
| **Before** `ApplyEyeRemap` | That squashes clip x by half. Rolling afterwards would mix a halved x with a full y and tilt by the wrong angle. |

It commutes with `ApplyOffset` (a world-space pre-multiplication against a clip-space
post-multiplication), and the eye separation needs no special handling at all: `g_eyeDeltaUU` comes
from the two real eye **positions**, which already swing about the head as it tilts.

**The frustum is not square**, so this is not a plain rotation of clip x/y — `tanX ≠ tanY`, and under
duplication `tanX` is the half-width value. Rotating the raw columns would *shear as it rotates*.
The maths converts to the symmetric view-space direction, rotates, and converts back.

### The maths is verified as arithmetic, not by eye

`spikes/roll_math` — `.\build.ps1`, no game and no headset. It asserts the rotation is exact, that
the magnitude is preserved (the shear case above), that it composes with the eye remap, that zero
roll is bit-exact, and that a square frustum degenerates to a pure NDC rotation. Both storage
conventions. **All pass.**

It deliberately does **not** assert the absolute sign. That is a handedness question between
OpenXR's frame and the game's, and this project has guessed it wrong on *both* of the other two
axes — which is what F4 and F5 are for. **NUMPAD2** settles it in one keypress.

### 🎯 Stage 4B: the measurement that has been missing for eight runs

Stage 4B has been "worth one measurement before ruling out" since run 33, and **the measurement was
never taken**. Every cost figure in this file is arithmetic on an *assumed* per-switch cost —
"~3,800 switches, even at an optimistic 5 µs that is ~19 ms against an 8.3 ms budget". A guess with
a multiplication after it.

That is the same shape as three claims this project has already had to retract: the SSW frame rates,
the "3% frame copy", and the 4096 cap that run 33 blamed on the GPU and run 40 measured at 16384.

**NUMPAD3** swaps the render target and depth-stencil away to a scratch pair and straight back, once
per draw — exactly the two `SetRenderTarget` + two `SetDepthStencilSurface` Stage 4B would issue for
that draw's two eyes, at the real draw count, with the real interleaving, against the real driver.

Because it swaps **back**, every draw lands where it always did and **the image is pixel-identical**.
Any frame-time delta is switching cost and nothing else. Cost and correctness are fully separated,
which is what makes one keypress a valid A/B — and toggling live beats two launches, which is two
different scenes, positions and streaming states.

| delta | verdict |
|---|---|
| under ~1 ms | affordable — build Stage 4B |
| 1–3 ms | affordable only if the resolution work pays for itself first |
| over ~4 ms | dead in this design; run 39's 4+ ms of idle does not cover it |

**⚠️ It is a LOWER BOUND, not the price.** Stage 4B also pays for two full-size targets instead of
one — double the pixel fill and bandwidth once each eye is full resolution — plus compositing them
into the submitted frame. If the floor already blows the budget, that settles it with no further
work.

### ⚠️ Stage 4B's justification has changed, and the TL;DR above was stale

Run 33 concluded per-eye targets were **mandatory** because side-by-side could never reach parity
against a 4096 cap. **Run 40 retracted the premise** — `GetDeviceCaps` reports 16384×16384, and what
refuses 4992 is **UE3's own resolution validation**. That retraction never propagated into the
"Next session" list, which has been calling Stage 4B mandatory on resolution grounds for eight runs.
Corrected now.

**Resolution is no longer a reason to want Stage 4B.** Its remaining case is *correctness* — the
seam, post-processing bleed, occlusion-box remapping. And the cheaper route to the pixels is
defeating UE3's resolution validation, not rebuilding the render path.

### Method note

Both halves of this run are the same move: **the expensive plan deserves the cheap check first.**
Roll got a 150-line arithmetic test that runs in a second and caught nothing — but would have caught
a shear, and a shear is exactly the failure that reads as "roughly working" in a headset until you
tilt far enough. Stage 4B got a keypress instead of a rewrite. Neither needed the headset to build.

### ✅ RESULT (run 50): roll works, and Stage 4B is priced at last

**Roll: confirmed working, sign was wrong, now fixed.** 3,000–4,000 draw matrices rolled per window
at tilts up to 43°. `g_rollSign` defaulted to `+1` and tilted the horizon the wrong way; **`-1` is
now the default** and the second run needed no correction. Third axis, third time OpenXR's
handedness has come out negative against the game's — `g_yawSign` is `-1` for the same reason.

**Roll costs nothing.** The two largest tilts in run 49 (41°, 43°) sit next to the *fastest* frames
of that run (8.45 ms, 118 fps). No corner dropout was reported at 43° either, so the unrolled
culling frustum has enough slack in practice.

#### 🎯 The RT-switch measurement, and level 1 was measuring nothing

Four phases in one run, same scene, draw counts within 8% of each other (1396 / 1513 / 1488):

| phase | frame time (median) | "our work" | idle (`xrWaitFrame`) |
|---|---|---|---|
| **off** (control) | 8.70 ms | 5.54 ms | 3.09 ms |
| **L1** bind away and back | 8.65 ms | 5.90 ms | 2.65 ms |
| **L2** bind + forced 1×1 `Clear` | **10.71 ms** | **10.49 ms** | **0.17 ms** |
| off (control, after) | 8.65 ms | — | — |

**Level 1 measured nothing, exactly as predicted.** 2,576 `SetRenderTarget` + 2,576
`SetDepthStencilSurface` per frame for **+0.36 ms** — D3D9 applies render state lazily, so a bind
immediately followed by a rebind is collapsed to one bind at the next draw. Had level 2 not been
built, this run would have concluded "render-target switches are free" and it would have been the
eighth tautological instrument in this file.

**Level 2 is the real number: +4.95 ms of work** for 1,411 switch pairs/frame (2,822 `SetRT` +
2,822 `SetDS`) — about **3.5 µs per pair**.

#### ⛔ Verdict: Stage 4B does not fit, but it is not the disaster the estimate claimed

The estimate that blocked this for eight runs said **~19 ms**. It is **~5 ms**. Wrong by 4×, and
wrong in the direction that mattered — but the answer is still no:

- Frame time only rose **+2.01 ms** because **2.9 ms of existing idle absorbed the rest**. Idle went
  from 3.09 ms to **0.17 ms** — the headroom run 39 won is entirely consumed.
- Work at 10.49 ms against an **8.33 ms** display period means the app can no longer make the
  120 Hz deadline at all. ~93 fps, with every frame late.

Bracket the number honestly, because it is wrong in both directions: **L2 overstates** (Stage 4B
binds and then issues a draw it was going to issue anyway — the `Clear`, the two `SetViewport`s and
the `SetScissorRect` are the probe's own overhead), and **L2 understates** (Stage 4B also pays for
two full-size targets — double pixel fill and bandwidth at full per-eye resolution — plus the
composite). The flush is the dominant term and it is genuine either way.

#### ➡️ Recommendation: build Stage 4A instead, and the constant is already found

Stage 4B would now be spent **purely on correctness** — run 40 removed resolution as a reason to
want it. Paying a quarter of the frame budget and all of the idle headroom for the seam,
post-processing bleed and occlusion boxes is a bad trade **when Stage 4A fixes the same three things
for the price of one patched constant**:

- Place each eye with the **viewport**, which applies to every draw whether or not its matrix was
  identified — so geometry can never be lost at the seam, and the whole identification dependency
  goes away.
- Patch UE3's `ScreenPositionScaleBias` per half, which is the one thing that broke last time
  viewport splitting was tried (run 11).
- **Run 41 already found that constant exactly:** `ps c1 = (0.5, -0.5, 0.50023, 0.50012)` at
  4096×2160. It was ruled out as the *decal* cause, correctly — but it is precisely the constant
  Stage 4A needs to patch, and it is sitting in the log already identified.

Cost: a handful of `SetPixelShaderConstantF` calls per eye against 2,822 render-target binds.

#### The slowdowns reported in run 49 were environmental

Run 49 showed 23 of 72 windows over 15 ms, with spikes to 88 ms. **Not the probe** (30% hitch rate
with it off, 37% on; the worst was probe-off), **not roll** (the 68 ms frame was at 1.5° roll),
**not the copy** (0.00 ms), **not the GObjects walk** (0.04 ms), **not the GPU** (1.16 ms wait).
All of it landed in CPU-side "our work" at normal draw counts.

Run 50, same build and same ini, was clean — max 9.87 ms across 53 control windows. So run 49's
hitching was the session, not the code. The standing hypothesis remains UE3's texture streaming: the
log shows `FOV inflation vs native 65 deg: x2.01 -> objects project 3.4x smaller`, and UE3 picks
streaming mip levels from projected screen size. **PAGE UP** walks the headroom if anyone wants to
confirm it.

### ⬜ The original test procedure (kept for method)

Roll and the probe are independent; do roll first, since the probe needs a steady scene.

1. **BACKSPACE** — VR mode on.
2. Tilt your head side to side. **The horizon should stay level.** If it tilts the *wrong* way,
   press **NUMPAD2** once and tilt again.
3. With your head tilted ~30°, look around. **Watch the corners** for geometry dropping out — the
   engine still culls with an unrolled frustum, and **PAGE UP** is the headroom lever if it does.
4. **NUMPAD1** turns roll off, for a direct comparison against the old behaviour.
5. Now stand still, facing something ordinary. Wait for a `perf:` line.
6. **NUMPAD3**. Wait for the next `perf:` line.
7. Compare the two `ms/frame` figures. **That difference is Stage 4B's floor.**

The `RT-switch probe` line prints in both states, so the control condition is in the log next to
the measurement rather than remembered.

## 🛑 Run 48: user-pointer draws ruled out too. STOPPING — the workaround is the answer.

`UserPtrQuadByVertex=1` classified correctly and the counts prove it:

```
user-pointer draws this window: 3590 split per eye, 1975 passed through as fullscreen quads
```

Passed-through count barely moved (1975 vs 1989 under the count-only rule), which means **every
2-primitive draw has its first vertex in NDC** — that bucket is entirely fullscreen quads, with no
decals hiding in it. Meanwhile 3,590–7,350 draws per window *were* being split per eye.

**So the user-pointer draws are being handled correctly and the decals are still wrong.** Valid
test, negative result. That closes the last live theory.

### What is ruled out, with evidence

| theory | how it died |
|---|---|
| Shadows | there is no shadow setting; **"High Quality Decals" off** removes it (run 44) |
| `G16R16` scene-sized target | NUMPAD0 dropped every draw to it — **no change** (run 45) |
| `ScreenPositionScaleBias` (`ps c1`) | found and confirmed exactly, but a scissored fullscreen quad is already self-consistent with it (run 41) |
| Second view-projection at `vs c13` | remapping it stretched geometry and left decals untouched (run 43) |
| Unhooked user-pointer draws | hooked and split correctly, decals unchanged (run 48) |

### The answer that works

**Turn "High Quality Decals" off in the in-game video options.** Zero cost, no rebuild, removes the
only known visual defect. `HookUserPointerDraws=0` is restored as the shipped setting — level 3 cost
real performance (mean 104 → 94 fps, floor 27 → 17) and bought nothing.

### ⬜ If anyone resumes this

The remaining untried approach is to identify the decal draw **by its shader or texture** rather
than by geometry or render target — hook `SetPixelShader`/`SetTexture`, find what is bound uniquely
during a decal, and treat those draws specially. That is a bigger instrument than anything built so
far and the payoff is one cosmetic effect.

Two things worth knowing before starting: the artefact is **view-dependent** (same light, different
position each run, different eye depending on where the player stands), which still points at
screen-space reconstruction rather than object placement; and **every counter in this file reports
only the draws our hooks see**, so any new diagnostic must be checked for that failure mode first —
it has now caused seven wrong conclusions here.

## ❌ Run 45: `G16R16` is NOT the decal pass — and the real candidate was in plain sight

NUMPAD0 dropped every draw to that surface and **the artefact did not change.** So the surface that
three runs of work were aimed at is unrelated. It was never more than "the only anomalous thing in
the census", which is exactly the kind of inference that has now been wrong four times here.

### The candidate that was always there: unhooked user-pointer draws

`HookUserPointerDraws=0`, so `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` are **completely
unhooked**. Decals are dynamic, CPU-generated geometry — precisely what that path exists for. An
unhooked draw never reaches `StereoPair`, so it is rendered **once at full-frame coordinates** and
spans both eye halves.

**That is the reported symptom exactly**: part of the decal in one eye, part in the other.

### ⚠️ Why every instrument said everything was fine

```
split 367 (367 with parallax, 0 flat), offscreen 0, mono-fallback 0, user-ptr 0
```

Every one of those counters increments **inside our hooks**. A draw that bypasses the hooks is
invisible to all of them. `user-ptr 0` does not mean "there are none" — it means "we are not
looking". `split 367 … 0 flat` says every draw *we saw* was handled perfectly, and says nothing
about the ones we did not.

Fifth tautological diagnostic, after `remapKnown = true`, the perf line's swallowed argument, the
census `<- SPLIT` label, and `registers cached: 1`. **The pattern is always the same: a counter that
can only report the good case.**

Run 30 measured **245–370 user-pointer draws per frame** when these hooks were briefly on. That
volume of geometry has been rendering mono, at full-frame coordinates, for the entire project.

### ⬜ The test needs no code change at all

`HookUserPointerDraws=3` routes them through `StereoPair`. Known side effect (run 28): level 3 also
catches `DrawDenormalizedQuad`, the fullscreen post-process quad, and scissoring that produces the
"rainbow". So:

- **Decals split correctly per eye** → cause confirmed; the remaining work is draw classification,
  so decals get the treatment and fullscreen quads do not
- **Decals unchanged** → they do not come through this path either
- **Rainbow makes it unreadable** → still informative, and it confirms the path carries the
  fullscreen quads; drop to level 1 or 2 to bisect

## 🎯 Run 44: it is NOT shadows — it is DECALS. Found by the user, not the instruments.

**There is no "dynamic shadows" setting in this game. Turning off "High Quality Decals" makes the
artefact disappear.**

That reframes everything, and it fits every measurement already taken. UE3's high-quality decals
are **deferred projected decals**: they reconstruct world position from the scene depth buffer and
project a texture onto it.

| observation | explained by |
|---|---|
| Wrong position, differently per eye | reads depth rendered with our **remapped** projection, reconstructs with an **unremapped** inverse |
| Lands on the gun and arm | whatever depth is in front of the projector receives it |
| Goes wild when a monster is under the light | more surfaces receiving projections |
| ~2 fullscreen draws/frame into scene-sized `G16R16` | a deferred projection pass, not geometry |
| Looks like a shadow | a dark projected texture *is* what a decal looks like |

It also explains **why run 43 failed**: the decal pass is not transformed by a view matrix at all,
so remapping `vs c13` could never have moved it. That negative result was correct and is now
understood rather than merely recorded.

### ✅ There is a working answer today

**Turn off "High Quality Decals" in the in-game video options.** Zero cost, no rebuild, no mod
change — and it removes the only known visual defect. That is the shipping answer unless someone
later wants decals back.

### ⬜ If it is picked up again, the target is now specific

Not "find the shadow bug" but: **find the constant the decal pass uses to reconstruct world
position from depth, and remap it per eye.** Everything needed to resume:

- The pass writes to the scene-sized **`G16R16`** target, ~2 draws/frame — `SplitColourTargetsOnly=1`
  routes it down the excluded path where the existing constant dump catches it
- `ps c1` is UE3's **`ScreenPositionScaleBias`**, confirmed exactly — correct as-is for a scissored
  fullscreen quad, so *not* the one to patch
- The reconstruction constant was **not** in `ps c0..c63` or `vs c0..c95` in any obvious matrix form,
  which is itself a clue: it may be composed into the decal's own projection matrix per draw
- **Zero-cost confirmation next run:** with `SplitColourTargetsOnly=1`, compare the census with
  High Quality Decals on vs off. If the `G16R16` rows vanish with the setting off, the target is
  confirmed as the decal pass with no extra instrumentation at all.

**Method note:** four runs of instrumentation narrowed this to the right surface and the right
mechanism but never to the right *name* — the user found that by toggling a game setting. Worth
remembering that the game's own options are a diagnostic, and a cheap one.

## ❌ Run 43: remapping `vs c13` FAILED — and the shadow theory is weakened

`ExtraCamReg=13` was the first actual fix attempt. Result: **geometry stretched to the edge of the
screen, and the shadows were still wrong.**

Both halves of that matter, and they point in opposite directions:

**The stretching proves `c13` is real.** Remapping it visibly moved geometry, so it genuinely
drives on-screen vertices — it is not a dead register or a light's transform. The run-42 dump was
reading something meaningful.

**But the shadows did not change, which weakens the theory.** If `c13` were the carrier for the
mis-projected shadow geometry, remapping it should have moved the artefact even if it moved it
wrongly. It did not. So the shadow's carrier is most likely **something else**, and `c13` is simply
a second camera matrix that this mod has never remapped — a real finding, but a different one.

**Why the stretch:** the remap was applied with `conv = g_lockedConv`, assuming `c13` shares `c0`'s
storage convention, and without `ApplyProjection`. Either assumption could be wrong on its own, and
a world-space matrix may not accept `ApplyEyeRemap`'s clip-space scaling the same way a
translated-world one does. Chasing that is a project in itself.

Backed out — `ExtraCamReg=-1` is the default and the shipped setting. The switch is kept so the
result is reproducible rather than folklore.

### ⬜ Recommendation: stop here on shadows

Four runs and a fix attempt have gone into this. What is known:

- The pass writes to a scene-sized **`G16R16`** target, ~2 draws/frame, and is fullscreen
- `ps c1` is UE3's **`ScreenPositionScaleBias`**, confirmed exactly — but ruled out, since a
  scissored fullscreen quad is already self-consistent with it
- The artefact tracks **skinned** casters (monster, arm, gun) and splits across the seam
- Only **one** register is ever remapped, and remapping the obvious second one does not fix it

The next honest step would be finding which constant the shadow pass actually reconstructs world
position with — real work, on the most fragile code here, for a **cosmetic** defect on one light.
Against a mod that otherwise runs true stereo at 4096×2160 and ~115 fps with correct tracking,
colour, and no vanishing geometry, that is a poor trade. **Disabling dynamic shadows in the
in-game video options is the reasonable shipping answer** unless this becomes a priority later.

## ⚠️ Run 42: a second view-projection at `vs c13` is never remapped (NOT the shadow cause — see run 43)

> ⚠️ **Correction, recorded because it was written down wrong first.** This was initially filed as
> vindicating the run-18 "vanishing monster" theory. **It is not.** Objects vanishing — the monster
> among them — was **occlusion query readback, solved in run 30**. That symptom is closed and this
> is a different one: geometry that renders correctly but whose *shadow* is mis-projected and split
> across the seam. Conflating the two muddies a question run 30 already answered.
>
> What survives the correction is the measured evidence below, which stands on its own and never
> needed the run-18 story.

Three pieces of evidence, and they agree:

**1. Only one register is ever remapped, by construction.**
```
camera matrix registers tracked: c0 (peak live in a frame 1)
```
The modify path keys entirely off `g_lockedReg`, a *single* register — the `g_camMats[8]` array is
vestigial. The scan locks `g_hits[0]`, the single best hit, and discards the rest.

**2. There is a real second view-projection at `vs c13`.**
```
vs c0  = (-0.9166,  0.0150, -0.0137, -0.0138)     vs c13 = (-0.3227,  0.0106, -0.0137, -0.0138)
vs c3  = ( 0.8876,  1.5487,-10.8155,  -0.8263)    vs c16 = (5179.24,-1737.82,26024.01,26060.06)
```
Identical projection columns; `c0` is translated-world (small `c3`), `c13` is world-space (huge
`c16`). Same camera, different space.

**3. The user's observation named the geometry.** The artefact goes wild when a *monster* walks
under the light, and settles to "on the gun and arm only" when it does not — but stays split across
the eyes either way. Monster, arm and gun are all **skinned** geometry, and `vs c26` onward in the
dump is a long run of bone matrices. Skinned meshes ride the second matrix.

So: geometry driven by `c13` keeps full-frame coordinates, gets scissored anyway, and lands split
across the seam. More shadow casters under the light means more of it visible at once.

### Why the single-register limit went unnoticed

The perf line reported `registers cached: 1`, which reads like a measurement of how many
view-projections the engine uses. **It is a tautology** — the cache can only ever hold
`g_lockedReg`, so the number is 1 by construction regardless of what the engine does. Run 28 caught
this and annotated the line; nothing followed up. Fourth tautological diagnostic in this file, after
`remapKnown = true`, the perf line's swallowed argument, and the census `<- SPLIT` label.

(Run 28's annotation framed this around the run-18 *vanishing* theory. That framing is now moot —
vanishing objects were occlusion queries, closed in run 30 — but the point about the counter being
incapable of reporting anything but 1 stands, and is what matters here.)

The F7 scan cannot settle it either: it identifies view matrices by requiring the camera to
transform to `w ≈ 0`, and only keeps the best single hit.

### ⬜ The fix

**Lock and remap every register carrying a view-projection, not just `g_hits[0]`.** The machinery
downstream already supports it — `g_camMats` holds 8, and `StereoPair` already loops over every
valid slot applying `ApplyOffset` + `ApplyEyeRemap`. What is missing is upstream: the scan keeping
more than one hit, and the modify path in `Hook_SetVSConstF` running for each locked register
rather than one.

⚠️ **This is the most fragile code in the project** — runs 15, 19, 20 and 27 all broke something
here. It deserves its own focused change behind an ini switch, not a hasty addition. Anything that
touches the matrix path should be A/B-able in one relaunch.

Note also `ps c1` = UE3's `ScreenPositionScaleBias`, found and confirmed by exact arithmetic in
run 41 — **not** the cause here (a scissored fullscreen quad is already self-consistent with it),
but it is the constant the viewport-split design needs and probably the post-processing bleed too.

## 🔎 Run 41: the shadow surface is `G16R16`, and the census label was lying

### The mystery target has a name

```
2A39E220   4096x2160  G16R16   240 draws  <- SPLIT
```

`FmtName` had been falling through to the literal string `"other"`, hiding the identity of the one
surface under suspicion. It is **`G16R16`** — a two-channel 16-bit scene-sized target, exactly the
shape of a shadow/light attenuation accumulation buffer.

**240 draws is per SECOND, not per frame** — at ~115 fps that is **~2 draws per frame**, a
fullscreen pass. Confirmed by the counters: `SplitColourTargetsOnly=1` moved `offscreen` from 0 to
**2** and `split` from 138 to 136.

### ⚠️ The census label was reporting the RULE, not the DECISION

With the exclusion active, the census *still* printed `<- SPLIT` for `G16R16`, because `isScene`
was recomputed from size equality instead of from what actually happened. Run 41 nearly concluded
the switch had not worked from that line alone. Now fixed — it prints `<- scene-sized, EXCLUDED by
ini` when that is the truth.

That is the third instrument in this project found reporting something other than reality (after
`remapKnown` being a compile-time `true` and the perf line's `(null)`). **Check what the diagnostic
actually measures before believing it.**

### ✅ The surface IS implicated — and the artefact's behaviour says so

Excluding it moved the artefact **from one eye to both eyes at once**, which is precisely what a
fullscreen pass does when it stops being remapped: split → placed per-eye (wrong in one), unsplit →
drawn once at full-frame coordinates spanning both halves. The reported "different eye on different
runs, same light source, different spots" is consistent with a view-dependent reconstruction error
rather than a fixed per-eye bug.

### ✅ Performance did NOT regress — the perceived hit is not in the data

| run | median fps | mean | min | max |
|---|---|---|---|---|
| Run 39 (zero-copy) | 110.0 | 106.5 | 27.2 | 120.2 |
| caps run | 109.3 | 102.7 | 26.9 | 118.8 |
| Run 41 A (`Split=0`) | 111.1 | 104.5 | 12.6 | 120.2 |
| **Run 41 B (`Split=1`)** | **112.0** | 104.5 | 26.9 | 118.4 |

Medians flat across all four; the run *with* the change has the highest. What is real is the
**variance** — every run including run 39 has occasional dips to 12–27 fps during level loads and
heavy scenes. That is pre-existing and unrelated to these changes.

### ⬜ Next: dump the constants, now that the haystack is two draws

The fix for this class is known in principle (Stage 4A): find the constant mapping clip space to a
texture coordinate and patch it per half. Its signature was predicted precisely — derived from the
render-target size, so near `(0.5, −0.5, 0.5+½px, 0.5+½px)`.

With the pass narrowed to ~2 draws a frame, that is now a small search. The build dumps pixel-shader
constants `c0..c31` for the first few draws into the excluded target and flags anything with a
half-scale/half-bias shape. Read via `GetPixelShaderConstantF` — no hook needed.

**Fixing it properly also fixes the post-processing bleed**, which is the same mechanism.

## ❌ Run 40: the 4096 cap is UE3's, NOT the GPU's — run 33's conclusion is RETRACTED

```
device caps: MaxTextureWidth=16384 MaxTextureHeight=16384 MaxTextureAspectRatio=16384
  -> the hardware ALLOWS a 5376x2880 side-by-side target. The 4096 refusal in run 33 was
     therefore UE3's own limit, NOT the GPU's - per-eye render targets may be unnecessary.
```

**The GPU supports 16384×16384.** Run 33 concluded "side-by-side can never reach parity, so per-eye
render targets are mandatory" from a single observation — 4096 accepted, 4992 refused — attributed
to the classic D3D9 4096 texture limit. That attribution was a hypothesis, and it was wrong.

Nothing had ever called `GetDeviceCaps`. One call retracts the argument for a large architectural
rewrite. **Full per-eye resolution needs 5376×2880, and the hardware allows it** — what refuses is
UE3's own resolution validation, which is a far cheaper problem than rebuilding the render path.

This is the third time in this project that a confident conclusion rested on an untested premise
(after the SSW frame rates and the "3% frame copy"). The pattern is now unmistakable: **the
expensive plan deserves the cheap check first.**

### ⬜ But per-eye targets are not dead — they just have a different justification

Resolution was the wrong reason. **Correctness** may be the right one — see the shadow bug below.
Note also that the cost estimate ("~3,200 render-target switches per frame, likely unaffordable")
still stands: at ~1,900 draws that is ~3,800 switches, and even at an optimistic 5 µs each that is
~19 ms against an 8.3 ms budget. The 4+ ms of idle won in run 39 does not come close to covering it.

## 🐛 Run 40: shadows appear in ONE eye and are wrong in that eye

Reported with a screenshot: hard-edged geometric shadow shapes that do not match the scene, visible
in one eye only. The census has been flagging the likely cause for several runs, unread:

```
render-target census (scene is 4096x2160; one row per SURFACE):
  2A330540  4096x2160  A16B16G16R16F   9337 draws  <- SPLIT
  2A330240  4096x2160  A8R8G8B8         740 draws  <- SPLIT
  2A330840  4096x2160  other            240 draws  <- SPLIT
^ 3 distinct SCENE-SIZED surfaces are being split. Only the LDR and HDR scene colour targets should be
```

**The scene test is size equality alone**, so a third scene-sized surface — neither colour target —
is being split 240 times a frame.

### The mechanism this points at

A screen-space effect that **reconstructs world position from depth** does so with an
inverse-projection constant we do **not** remap. Feed it depth from geometry we **did** remap and
the reconstruction lands somewhere else entirely — and differently per eye, because each eye's
remap differs. That is precisely "the shadow appears in one eye and is wrong in that eye".

It is the same family as the already-known post-processing bleed, and the same problem Stage 4A
predicted for `ScreenPositionScaleBias`.

### Two changes, both cheap

1. **`other` now names itself.** `FmtName` fell through to the literal string "other" for anything
   unrecognised, which hid the identity of the exact surface under suspicion. It now decodes FOURCC
   and prints the numeric `D3DFORMAT` otherwise. A name you cannot read is not a diagnostic.
2. **`SplitColourTargetsOnly=1`** restricts splitting to the two known scene colour formats,
   excluding the third surface. If the shadows change, it is implicated; if they do not, the cause
   is elsewhere. One relaunch either way.

## 🚀 Run 39 RESULT: zero-copy works — 4K went from ~60 fps to ~115

```
*** ZERO-COPY ACTIVE: shared RT 4096x2160, D3D11 sees fmt=87 - the frame no longer touches the CPU ***

frame copy [ZERO-COPY]: StretchRect 0.00 + lock 0.00 + memcpy 0.00 + upload 0.00 = 0.00 ms of 8.64 (0%)
frame budget: xrWaitFrame 4.35 (idle) + copy 0.00 + GObjects walk 0.04 + our work 4.25 = 8.64 ms
perf: 115.8 fps (8.64 ms/frame) | draws/frame peak 368, split 368 (368 with parallax, 0 flat)
```

| at 4096×2160 | before | after |
|---|---|---|
| Frame copy | ~9.8 ms | **0.00 ms** |
| GPU wait (fence) | 2–4 ms | **0.5–0.9 ms** |
| `xrWaitFrame` (idle) | 0.1–0.9 ms | **4.0–5.1 ms** |
| Frame time | ~16 ms | **8.58–8.87 ms** |
| **Frame rate** | **55–63 fps** | **112–117 fps** |

**The application is now display-bound rather than work-bound**, which is the result that matters
more than the fps number. It finishes its frame in ~4.4 ms and then sits idle ~4.3 ms waiting for
the compositor — the exact inversion of every previous run, where `xrWaitFrame` was ~0 because the
app always arrived late. There is now headroom instead of a deficit.

Against an 8.33 ms display period the frame lands at 8.6–8.9 ms, so it just misses locking to a
solid 120 Hz. Closing a ~0.4 ms gap is a very different problem from closing a ~8 ms one.

Run 35 predicted a "~6.5 ms zero-copy ceiling"; the real figure is ~8.7 ms. Directionally right,
magnitude close enough, and the prediction was made from a measurement rather than a guess.

### ⚠️ The fence timed out once

```
GPU fence did not signal within 100 ms - abandoning the wait
```

The bounded spin added in run 35 fired exactly once, almost certainly during a level load where the
GPU genuinely had >100 ms of work queued. The consequence is that **one frame may have been copied
while the shared surface was still being written** — a single torn frame, cosmetically negligible
but worth knowing the mechanism exists. If tearing is ever reported, this is the first thing to
check, and the bound is the thing to raise.

Everything else is clean: no fallbacks fired, no `StretchRect` failures, pool translation identical
to run 38, `GetSurfaceLevel-on-shadowed` still 0.

### ⬜ Visual correctness still needs a human

The log cannot see tearing, a stale band, or frame-to-frame flicker — the artefacts a
synchronisation bug produces. Those need eyes in the headset.

## 🔨 Run 39: Stage 2, the zero-copy frame path — the design as built

What the whole D3D9Ex exercise was for. Requires `D3D9ExMode=1`; on a plain device a shared surface
cannot be created, so `ZeroCopy=1` alone changes nothing.

| | path | cost at 4096×2160 |
|---|---|---|
| before | `GetRenderTargetData` → `LockRect` → `memcpy` 35 MB → `Map` → `CopyResource` | **~9.8 ms** |
| after | `StretchRect` backbuffer → shared RT, then `CopyResource` → XR swapchain | **target < 1 ms** |

Both copies are GPU-side. **The frame never touches the CPU.** Spike 1 proved every link of this in
a 32-bit process — a D3D9Ex texture created with a shared handle, opened by
`ID3D11Device::OpenSharedResource`, pixels arriving intact.

### Synchronisation — the only genuinely new problem

D3D9 has no counterpart to D3D11's keyed mutex, so nothing makes the D3D11 read wait for the D3D9
write. A `D3DQUERYTYPE_EVENT` fence issued **after** the `StretchRect` and waited on before the
D3D11 copy is what spike 1 used, and what is used here.

Note the fence **moves** relative to the CPU path. There it is issued *before* the readback and
measures "GPU finished the frame". Here it must come *after* the `StretchRect` is queued, so one
wait covers both the frame being drawn and the copy into the shared target.

That wait is not overhead being added — run 35 measured it at 2–4 ms and it is genuine rendering
time; a frame cannot be shown before it is drawn. What disappears is the ~9.4 ms of transfer and
memcpy stacked on top of it.

### ⚠️ No pipelining under zero-copy, deliberately

`FrameCopyMode`'s two-slot ring exists because a sysmem readback can be deferred a frame. There is
exactly **one** shared render target, and `StretchRect` has just overwritten it with *this* frame —
so consuming an older slot's metadata would submit these pixels with the previous frame's pose.
That is the reprojection-swim failure the capture stamping exists to prevent, reached from the
opposite direction. The zero-copy path uses its own dedicated slot and always consumes immediately.

### Also inert under zero-copy: the cull-band measurement

There are no CPU-readable pixels any more, by construction. Logged once so a missing diagnostic is
never mistaken for a zero reading. `ZeroCopy=0` to measure it.

### What to read

```
*** ZERO-COPY ACTIVE: shared RT 4096x2160, D3D11 sees fmt=87 - the frame no longer touches the CPU ***
frame copy [ZERO-COPY]: StretchRect 0.30 + lock(DMA lands) 0.00 + memcpy 0.00 + upload 0.05 = 0.35 ms of 6.80 (5%)
```

Every fallback is logged loudly and drops to the CPU path rather than failing: shared-RT creation,
`GetSurfaceLevel`, `OpenSharedResource`, and a `StretchRect` that fails at runtime.

## ✅ Run 38 RESULT: the wrapper works, and the feared gap does not exist

**The game runs correctly on a D3D9Ex device with every MANAGED allocation translated.** Textures
look right. Zero failures of any kind.

```
Direct3DCreate9Ex succeeded - handing the game an Ex factory
*** D3D9Ex device created - D3DPOOL_MANAGED will be translated to DEFAULT ***
resource creation hooks installed (CreateTexture/Volume/Cube/VB/IB)
texture vtable patched (LockRect=713E5C20 UnlockRect=713E5CC0 Release=7140D240 GetSurfaceLevel=713E2920)

D3D9Ex pool translation: 1053 textures, 18 cube, 1 volume, 2810 vertex buf, 133 index buf
  | shadows 1072 (~46 MB), shadow failures 0, create failures 0, GetSurfaceLevel-on-shadowed 0
```

### The three things that mattered, all resolved

**1. `GetSurfaceLevel-on-shadowed 0` — the known gap is not real.** UE3 locks textures directly and
never goes through a surface, so the level-by-level surface pairing that would have been the next
piece of work **is not needed at all**. This was the single most likely way the design could have
been wrong.

**2. Shadow memory is ~46 MB, not the hundreds feared.** A 32-bit address space made this a genuine
risk. It is a non-issue, and the reasoning holds up: MANAGED kept a system copy too, so this
replaces that copy rather than adding one.

**3. Nothing escaped translation.** Not just because the counters say so — **an untranslated
MANAGED allocation would fail outright on an Ex device**, and the game renders correctly. The
counts sitting below spike 2's census (1,053 textures vs 3,276; 2,810 VB vs 6,258) is session
length, not leakage: spike 2 walked more of the game than a 60-second sample.

### Performance-neutral, exactly as Stage 1 intended

Both resolutions were run, which is the better test:

| | before the wrapper | with the wrapper |
|---|---|---|
| 2560×1440 | 105–118 fps (run 32) | **109–115 fps** |
| 4096×2160 | 57–66 fps (run 35/36) | **55–63 fps**, copy ~9.8 ms |

Unchanged within noise. That is the correct result: Stage 1 changes only how resources are
allocated, and the frame still goes through the CPU copy. **The wrapper is not supposed to be
faster — it is supposed to make the fast path legal.**

### ⬜ Stage 2: the actual zero-copy path

Everything blocking it is now cleared. Spike 1 already proved the interop end to end (D3D9Ex shared
texture → `ID3D11Device::OpenSharedResource` → pixels intact, 32-bit, no CPU round-trip). What
replaces the current path:

| now | Stage 2 |
|---|---|
| `GetRenderTargetData` (GPU→sysmem, blocks) | `StretchRect` backbuffer → shared RT (GPU→GPU) |
| `LockRect` (DMA lands) + `memcpy` ~14–35 MB | — gone — |
| `Map` + `CopyResource` to swapchain | `CopyResource` shared → swapchain (GPU→GPU) |

Sync needs a `D3DQUERYTYPE_EVENT` fence, since D3D9 has no keyed-mutex counterpart — spike 1 used
exactly that and it worked. Target: the ~9.8 ms copy at 4K drops to well under 1 ms, putting the
frame near the ~6.5 ms zero-copy ceiling measured in run 35.

## 🔨 Run 38: the D3DPOOL_MANAGED wrapper — the design as built

The zero-copy prerequisite, justified by run 35's measurement (~9.4 ms of a ~16 ms frame
recoverable, GPU idle at 2–4 ms). **Off by default** — it replaces the resource management of a
shipping engine, so its failure mode is "the game does not start" and it stays one relaunch from off.

### What it does

| Resource | Count (spike 2) | Treatment |
|---|---|---|
| Vertex + index buffers | **7,149** | **Pool swap only.** D3D9 permits `Lock()` on a DEFAULT buffer — it stalls, but these are locked once at load, not per frame. Two thirds of the problem needs no machinery. |
| Textures / cube / volume | **3,305** | **Pool swap + SYSTEMMEM shadow.** `LockRect` on a DEFAULT texture genuinely fails, so these need the system copy MANAGED used to keep. |

The game is handed the **real DEFAULT texture**, with `Lock`/`Unlock` redirected to a shadow and
`UpdateTexture` uploading on unlock after a write.

**What makes this safe rather than merely possible: D3D9Ex does not lose `D3DPOOL_DEFAULT`
resources on `Reset`.** On a plain device this translation would mean rebuilding every resource on
every device-lost event. That single Ex behaviour is what removes the biggest objection.

### Why vtable patching, not COM proxies

The obvious implementation is a wrapper class per resource type — which also means unwrapping at
every device entry point that accepts a resource (`SetTexture`, `SetStreamSource`, `SetIndices`,
`StretchRect`, …), where one missed site is a crash.

Patching the vtable avoids all of it: the game holds the genuine DEFAULT texture, so every device
call consuming it works untouched, and only `Lock`/`Unlock`/`Release` are intercepted. D3D9
resources of a type share one vtable, so a single patch covers every instance — the same trick
already proven here on `IDirect3DQuery9::GetData` for the run-30 occlusion fix. Cost: the patch is
global, so locks on textures we did *not* translate land in the hook too and take a side-table miss.

**All slots verified against the SDK header**, by an enumeration that reproduces every slot this
project already relies on (Present 17, SetRenderTarget 37, DrawPrimitive 81,
SetVertexShaderConstantF 94, CreateQuery 118). New: `CreateTexture` 23, `CreateVolumeTexture` 24,
`CreateCubeTexture` 25, `CreateVertexBuffer` 26, `CreateIndexBuffer` 27, `UpdateTexture` 31;
texture `LockRect` 19 / `UnlockRect` 20 / `GetSurfaceLevel` 18; buffer `Lock` 11 / `Unlock` 12.

### ⚠️ This is STAGE 1 — deliberately no zero-copy yet

It changes **only how resources are allocated**. The frame is still delivered by the old CPU copy,
so **if it works the game should look and perform exactly as before.** That is the entire point: it
separates "does the wrapper work" from "does zero-copy work". Doing both at once would mean a
failure could not be attributed to either. Stage 2 switches the frame path.

### ⬜ The known gap, instrumented rather than hoped about

Redirection covers `Lock` on the **texture**. An engine can instead call `GetSurfaceLevel` and lock
the returned surface, bypassing all of it — and locking a surface of a DEFAULT texture fails, so
the upload would silently do nothing and the texture would render as uninitialised VRAM.

Hooking surface `Lock` is not a clean fix (surfaces from textures share a vtable with render-target
and depth surfaces, and the shadow's surfaces need pairing level by level). So before building
that, **find out whether UE3 takes the path at all**: `GetSurfaceLevel-on-shadowed` counts it. Zero
means the gap is theoretical. Climbing means that is the next piece of work — and the reason any
textures look wrong.

### What to read in the log

```
D3D9Ex pool translation: 3276 textures, 26 cube, 3 volume, 6258 vertex buf, 891 index buf
  | shadows 3305 (~NNN MB), shadow failures 0, create failures 0, GetSurfaceLevel-on-shadowed 0
```

Counts should land near spike 2's census. Anything far below means allocations are escaping
translation; any failure count above zero, or `GetSurfaceLevel-on-shadowed` climbing, names the
problem directly. `~NNN MB` is worth watching in a 32-bit process — though MANAGED kept a system
copy too, so this should be roughly a wash rather than new pressure.

## ↩️ Run 37: run 36's default-on reverted — BACKSPACE stays, the cold start doesn't change

Run 36 made VR mode start active, on the reasoning that F9/F10/F1 are the mod rather than three
optional experiments. Reverted: that also meant the main menu was rendered frame-split for the
first time ever, completely untested, on every launch. **The cold start is flat again** — same as
every run before 36. **BACKSPACE still reaches the working state in one press**, which was the
part actually worth keeping; it now does that without changing what the game looks like before the
first key is pressed. `g_enabled`, `g_sixDof`, `g_dupDraws` are back to their original `0` defaults.

## ✅ Run 36: the freeze is fixed, and VR mode was briefly on by default

### The GObjects backoff worked

```
frame budget: ... + GObjects walk 0.03 + our work 5.76 = 15.09 ms
perf: 66.3 fps (15.09 ms/frame)
```

**0.03–0.04 ms in gameplay**, against the ~70 ms/frame it was contributing during the freeze. One
transition window still shows 6.99 ms with the warning firing correctly — that is the backoff doing
its job (≈4 scans/sec instead of 60) rather than the problem returning. Gameplay is now **57–66 fps**
at 4096×2160, slightly up.

### BACKSPACE: one press instead of three (default-on itself reverted in run 37)

F9 + F10 + F1 were never really three features; they are the mod. Requiring three keypresses to
reach the working state was a leftover from when each was a separate experiment, so **BACKSPACE**
now toggles all three (plus the forced projection) as one set. **Run 37 reverted making this the
cold start** — see above — but the toggle itself stands.

"Off" restores the plain game — including dropping the forced projection, which is only correct
while duplication is splitting the frame. Leaving it on would give a mono view stretched to 98°,
which looks broken rather than standard. Alternate-eye is cleared too, so the toggle can never
produce the both-stereo-modes-at-once state that F12-after-F1 does.

BACKSPACE because F1–F12 are all taken and it is large enough to find **by feel while wearing the
headset**.

## ✅ Run 35: the round-trip IS the frame, and the menu freeze is a GObjects scan

### The split, measured — and it beats the estimate

```
round-trip split: gpu wait 1.99 ms (rendering - NOT recoverable)
  | transfer 9.37 + memcpy ... = 9.37 ms RECOVERABLE
frame budget: xrWaitFrame 0.21 + copy 9.51 + our work 6.15 = 15.87 ms
```

| | run 34 estimate | **measured** |
|---|---|---|
| GPU wait (not recoverable) | ~5 ms | **2–4 ms** |
| Recoverable round-trip | ~7 ms | **~9.4 ms** |
| Zero-copy ceiling | ~100 fps | **~6.5 ms/frame, comfortably past 120 Hz** |

**The sanity check passed**: total frame time is 15.7–16.5 ms, unchanged from run 33's 15.2–17.9,
so the fence relocated the wait without distorting the frame. (Contrast `FrameCopyMode`, which
failed exactly that test.)

So at 4096×2160 the CPU round-trip is **~60% of the frame and the GPU is nearly idle** — 2–4 ms of
actual rendering. **The wrapper is justified.** This is the first time that claim has had a
measurement under it rather than an assumption.

### ⚠️ But the breakdown was mislabelled, and the correction matters

`GetRenderTargetData` timed at **0.00 ms**, which does not mean the transfer is free — the driver
**defers** it, and `LockRect` is what blocks until the DMA lands. Timing lock and memcpy as one
bucket therefore reported ~7 ms of *bus transfer* as *"memcpy"*. Both are removed by a zero-copy
path so the RECOVERABLE total stands, but the two have entirely different fixes and should never
have shared a bucket. Now split: `readback + lock(DMA lands) + memcpy + upload`.

This is the same failure mode as the pipelining lesson, one level down: **a phase timer that
attributes a cost to the wrong bucket is as misleading as one that loses it.**

### 🐛 The menu freeze: `FindCamera()` rescans all of GObjects, every frame

Reported as "froze for a few seconds" before pressing F1 and again when quitting. The log shows it
is **not** the copy — that stays normal at ~8.9 ms — it is `our work`:

```
xrWaitFrame 5.67 + copy 8.87 + our work 82.00 = 96.53 ms   (10.4 fps)
xrWaitFrame 4.36 + copy 9.04 + our work 69.39 = 82.80 ms   (12.1 fps)
```

`FindCamera()` and `FindController()` cache their object and return instantly while it is valid,
which hides how expensive the miss path is: it walks **every entry in GObjects** — tens of
thousands — calling `Readable()` two or three times per object, and `Readable()` is `VirtualQuery`,
a **kernel transition**. Hundreds of thousands of syscalls per frame.

It costs nothing while the object exists. When it does **not** exist the cache can never warm, so
the full walk repeats every single frame — and "does not exist" is exactly the **main menu, level
load, and quitting**. Precisely where the freeze was seen.

**Fixed** by arming a 250 ms backoff on a failed scan (`ScanAllowed`). Acquisition is delayed by at
most that once the object appears, which is imperceptible; the menu cost drops ~15×. The walk is
now timed and broken out of `our work`, with a warning when it exceeds 2 ms, so this cannot hide
again.

Not a regression from the fence probe — large `our work` spikes are visible in run 32 and 33 logs
too (26–35 ms). It was simply never attributed, because nothing measured it.

### Also hardened

The fence spin is now **bounded at 100 ms**. An unbounded spin on a query that never signals — lost
device, driver hiccup — would wedge the game with no way out but Task Manager, and this is a
measurement, not a feature.

## ⚠️ Run 34: the ~60 fps at 4K is real, and the "12.5 ms copy" is NOT all recoverable

Two corrections to run 33, both mine.

### 1. The SSW warning cried wolf, and the fix was already in the data

Run 33 ran with SSW **off**. The warning fired anyway (`1.91x`, `1.99x`) because it tested frame
time against the display period and nothing else — and **60 fps against 120 Hz is 2.00× by
definition.** It would flag any app honestly running at half the refresh.

The discriminator was already being collected: **`xrWaitFrame`**. Under SSW the runtime holds the
app back, so it finishes early and *blocks* — a large share of the frame is idle. An app that is
merely slow never idles; it arrives late and `xrWaitFrame` returns at once. Run 33 showed
`xrWaitFrame 0.1–0.9 ms` of a 16 ms frame: **genuinely busy, not held back.**

The check now requires ~2× period **and** >35% of the frame idle. So **~60 fps at 4096×2160 is the
true ceiling of this build**, not an artefact.

### 2. `copy` conflates GPU wait with transfer — and only one of them is recoverable

**`GetRenderTargetData` does not merely transfer, it synchronises.** It cannot return until the GPU
has finished rendering the frame. So the 12.5 ms attributed to `copy` is really:

```
(wait for the GPU to finish the frame)  +  (the actual GPU->CPU transfer)
```

**Only the second is what a zero-copy path removes.** The first is real rendering work that would
still have to be waited for — it would just happen inside the compositor rather than inside us.

This is not a footnote. The entire case for the `D3DPOOL_MANAGED` wrapper rests on which of the two
that 12.5 ms is, and **the current number cannot tell them apart**:

- mostly transfer → the frame goes 16 ms → ~4 ms, and the wrapper is transformative
- mostly GPU wait → the wrapper buys almost nothing, and run 31's cancelled verdict was
  accidentally right for the wrong reason

Back-of-envelope says it is genuinely mixed. At 4096×2160 the frame is 35.4 MB; readback over PCIe
at ~8 GB/s is ~4.4 ms and the measured memcpy rate (14 MB in 0.89 ms ≈ 15 GB/s) puts the copy at
~2.3 ms — **so roughly 7 ms recoverable and ~5 ms genuine GPU wait.** That would be ~60 → ~100 fps:
a large win, but not the 120+ claimed in run 33, and **not** a number worth starting weeks of work
on while it is still arithmetic.

**So the estimate in run 33 is withdrawn pending measurement.** `GpuFenceProbe` (new, default on)
issues an EVENT fence and blocks on it *before* the readback, paying the sync where it can be timed:

```
round-trip split: gpu wait 8.10 ms (rendering - NOT recoverable)
  | transfer 2.90 + memcpy 2.30 + upload 0.00 = 5.20 ms RECOVERABLE
  -> zero-copy ceiling 10.80 ms/frame (93 fps)
```

`RECOVERABLE` is a **ceiling**, not a promise — the compositor still has to consume the shared
surface. It adds no work, since the wait was happening anyway; it only relocates it somewhere
visible. **If total frame time moves when this is toggled, the probe is lying** — precisely the
failure that caught out `FrameCopyMode`, where a stall left the instrument without leaving the frame.

## 🧱 Run 33: 4096 works, and it proves side-by-side can NEVER reach parity

`-ResX=4096 -ResY=2160 -windowed` was **accepted**, and the engine followed it properly — the
render-target census shows the scene targets themselves at 4096×2160, not just the backbuffer:

```
render-target census (draws this second, scene is 4096x2160; one row per SURFACE):
287C5F40   4096x2160  A16B16G16R16F   21363 draws  <- SPLIT
287C6080   4096x2160  A8R8G8B8         8729 draws  <- SPLIT
```

So `-ResX` reaches UE3's system settings exactly as run 14 predicted, and **4096 is not refused**.
With 4992 refused earlier, the cap is 4096 — the classic D3D9 maximum texture dimension, landing
exactly where the leading hypothesis said it would.

### ⛔ The arithmetic that kills the current design

| | value |
|---|---|
| Frame | 4096 × 2160 (the maximum obtainable) |
| **Per eye** | **2048 × 2160** |
| Headset wants (this session) | **2688 × 2880** per eye |
| Side-by-side would need | **5376** wide — 1.3× over the cap |
| Delivered | **76% linear, 58% of the pixels** |

**That is why it is not sharp, and it is not a bug.** 76% of the panel's angular resolution is
precisely the "soft but not broken" band. Nor is it fixable by asking for more: side-by-side needs
5376 px of width and cannot have more than 4096, so **parity is arithmetically impossible in the
current one-frame design, at any setting.** Stacking vertically is worse (2688 × 5760).

**Per-eye render targets are therefore mandatory, not preferable** — Stage 4B in the list below, and
the "Remove the resolution cap" item can be closed: the cap is real, it is 4096, and the way past it
is not a bigger frame.

Note also that "headset wants" moved from 2496×2688 to **2688×2880** when the Virtual Desktop
settings changed. That is the design working as intended — the target follows the runtime's own
render-scale rather than being fixed — but it means the number must be read from the log per
session, never hardcoded.

### 🚀 And the frame copy is now 75–80% of the frame — D3D9Ex is the top item

This is the measurement run 32 could only project. At 4096×2160 the round-trip stops being
expensive and starts being the entire frame:

```
frame budget: xrWaitFrame 0.09 + copy 13.42 + our work 2.58 = 16.10 ms
frame budget: xrWaitFrame 0.08 + copy 12.82 + our work 2.41 = 15.29 ms
perf: 60.4 fps (16.57 ms/frame) | draws/frame peak 154, split 154
```

| Resolution | copy | our work | total | fps |
|---|---|---|---|---|
| 2560×1440 | 4.2 ms | 3.7 ms | 8.9 ms | ~110 |
| **4096×2160** | **~12.5 ms** | **~2.5 ms** | ~16 ms | **~60** |

**The actual rendering costs 2–4 ms.** Everything else is shuttling pixels through the CPU. The GPU
is barely working and the frame rate halved anyway.

So the zero-copy path is worth roughly **60 fps → 120+** at this resolution, not the ~1 fps run 31
claimed. Run 32 re-scoped D3D9Ex as "the resolution enabler rather than a frame-rate fix"; this
measures it, and the case is far stronger than the one it originally had. **It is now the single
highest-value item in the project.**

### ⚠️ Vertical culling shortfall, and it is not small

```
worst engine VERTICAL cull, from the ARRIVING MATRIX (ground truth): 51.1 deg vs 98.0 deg rendered
engine's own matrix on arrival: 84.4 x 51.1 deg
after forcing, the matrix measures 95.0 x 98.0 deg (want 95.0 x 98.0)
```

We render **98° vertical**; the engine culls against **51.1°**. Geometry outside its frustum is
never submitted, so the top and bottom of the view can be missing objects. Run 28 removed the
culling headroom on the grounds that the shortfall "is not happening" — against the arriving matrix
as ground truth, at this aspect ratio, it plainly is. Not necessarily the same finding run 28
refuted (that was about the *flicker*), but the headroom control was retired on this evidence and
that should be revisited. Cheap to test: **PAGE UP** walks it.

### ⬜ Minor: a third scene-sized surface is being split

```
287C6480   4096x2160  other   240 draws  <- SPLIT
^ 3 distinct SCENE-SIZED surfaces are being split. Only the LDR and HDR scene colour targets should be
```

The instrument is flagging its own suspicion. 240 draws a frame going somewhere unidentified.

## 🚨 Run 32: SSW was halving the frame rate. The real number is ~110 fps.

**The user spotted this, not the instruments.** Virtual Desktop's Synchronous Spacewarp was on for
every previous run. SSW pins the application at **half** the display refresh and generates the
intermediate frames itself — so a build capable of 110 fps reports 36, by design, and nothing in
the log looks wrong. With SSW off and the headset set to 120 Hz:

```
perf: 117.5 fps (8.51 ms/frame) | draws/frame peak 138, split 138 (138 with parallax, 0 flat)
perf: 111.3 fps (8.98 ms/frame) | draws/frame peak 444, split 444
perf: 106.1 fps (9.43 ms/frame) | draws/frame peak 891, split 883
```

**105–118 fps in true stereo at 2560×1440.** The performance problem this project has been
organising itself around for thirty runs did not exist. It was a headset setting.

### What this invalidates

Everything numeric taken before run 32, including run 31's own conclusions:

| Claim | Status |
|---|---|
| "~37 fps at 1440p, ~28 at 4K, floor ~13" | ❌ measured through SSW — re-derive |
| "fps distribution is bimodal → CPU-bound stall" | ❌ frames either side of an SSW halving look identical |
| Run 31: "frame copy is 3% → cancel D3D9Ex" | ❌ **retracted** — 0.88 ms only looked small against a 27.8 ms padded frame |
| Run 31: "4.5× fewer draw calls changed nothing" | ❌ **retracted** — both sides were pinned at the SSW cap, so the comparison could not have shown a difference |

The run-31 *method* was right and the instrument is what caught it — but an instrument that
measures the frame honestly still reports a fiction if the frame itself is being synthesised.

### ✅ The `FrameCopyMode` A/B, on clean data: pipelining LOSES

Matched by scene, so the draw counts are comparable:

| draws | pipelined (mode 0) | immediate (mode 1) |
|---|---|---|
| 367 | 107.0 fps (9.34 ms) | **109.7 fps (9.12 ms)** |
| 444 | 111.0 fps (9.01 ms) | **111.3 fps (8.98 ms)** |
| 138 | 114.5 fps (8.74 ms) | **117.5 fps (8.51 ms)** |

Immediate is equal or better every time, *and* it does not cost a frame of latency. **The default
is now `FrameCopyMode=1`.**

The phase split says why, and it is the more useful finding: **pipelining did not remove the
readback stall, it relocated it.**

```
pipelined:  copy 0.89 ms + our work 9.4 ms
immediate:  copy 4.2  ms + our work 3.7 ms      <- same total
```

Issuing `GetRenderTargetData` without locking its result lets the call return immediately, so the
phase timer sees nothing — but the driver still owes the transfer, and it surfaces later inside
work the instrument does not break out. **Moving a stall out of an instrument's view is not an
optimisation**, and it is a failure mode any future phase timing here should be read against.

### ✅ So how expensive IS the round-trip? ~4.2 ms of an ~8.9 ms frame

That is the number run 31 was trying to get and got wrong. Nearly half the frame, and it splits
into ~0.9 ms of genuine memcpy bandwidth plus ~3.3 ms of readback stall.

**This puts D3D9Ex back on the table — but with a much sharper justification than it ever had.**
It is not a general fps fix; the game already clears 120 Hz-ish at 1440p. It is specifically the
**enabler for the resolution work**, because the copy scales with pixel count:

| Resolution | pixels vs 1440p | projected copy |
|---|---|---|
| 2560×1440 (today) | 1.0× | ~4.2 ms of an 8.9 ms frame |
| 3840×2160 | 2.25× | **~9.5 ms — exceeds the entire 8.33 ms budget on its own** |
| 4992×2688 (headset native) | 3.6× | hopeless |

So the sequencing inverts. The round-trip is affordable at today's resolution and **fatal at the
one the project actually wants**. Matching the headset needs the zero-copy path; it is not
optional there, and it was never really about frame rate.

### ⚠️ Test-environment discipline, now mandatory for any measurement

SSW off, and the headset refresh recorded in the run notes. The build now logs
`predictedDisplayPeriod` on the first frame, and warns when frame time sits at ~2× it:

```
headset display period 8.33 ms (120.0 Hz)
*** frame time is 2.00x the display period. SUSPECT SSW/ASW ... Turn it OFF before trusting
    ANY number in this log. ***
```

That check would have caught this three runs ago. It is cheap and it is now permanent.

## ~~❌ Run 31 RESULT: the frame copy is 3% of the frame. The D3D9Ex plan is dead.~~ (RETRACTED — see run 32)

> **Retracted in full by run 32.** Everything below was measured with SSW halving the frame rate,
> which padded frames to 27.8 ms and made a 0.88 ms copy look like 3%. Kept, unedited, because the
> *reasoning* was sound and only the input was corrupt — and because "the instrument was honest and
> the answer was still wrong" is the most useful thing in this file.

**The project's number-one performance item for thirty runs was aimed at the wrong thing.** The
frame copy was measured for the first time, and it is not the bottleneck. It is not close.

```
frame copy [pipelined]: readback 0.00 + lock/memcpy 0.88 + upload 0.00 = 0.88 ms of 28.38 (3%)
```

Stable across the whole session — 0.87–1.21 ms against 24–44 ms frames, never above 4%.

| Phase | Cost | What it means |
|---|---|---|
| `readback` | **0.00 ms** | `GetRenderTargetData` returns immediately. The transfer is not blocking us at all. |
| `lock/memcpy` | **0.88 ms** | ~14 MB of pure CPU bandwidth. About right for the size, and D3D9Ex would not remove it anyway. |
| `upload` | **0.00 ms** | `CopyResource` is a deferred GPU command; it costs no CPU time. |

**So the D3D9Ex + `D3DPOOL_MANAGED` wrapper should not be written.** Its entire purpose is to
remove a cost that measures 0.88 ms out of 28. Best case it takes the frame from 28.4 ms to 27.5 —
**35.2 fps to 36.4 fps** — in exchange for wrapping 10,454 MANAGED allocations behind COM proxies
for five resource types, with a failure mode of "the game will not start". It is not worth it, and
it was never going to be; nobody had measured.

The one indirect signal it rested on — the bimodal fps distribution from run 12, median ~70 and
floor ~13 — is real but was over-read. A CPU-bound stall was one explanation for it. It was not
the only one, and it was never tested against another.

### ✅ And draw calls are not the bottleneck either — the run-30 sequencing worry is answered

Test C ran with `OcclusionQueryMode=2`, so the engine's own occlusion culling was live. That is
the A/B run 30 asked for, from the opposite direction:

| | draws/frame | frame rate |
|---|---|---|
| Run 30, override ON (queries forced visible) | 2272–3809 | 37–40 fps |
| **Run 31 test C, override OFF (engine culling live)** | **403–1351** | **31–38 fps** |

**Roughly 4.5× fewer draw calls, and the frame rate did not improve.** Slightly worse, if anything.

This settles the *"do NOT do this before the D3D9Ex work"* caution under run 30. The concern was
that ~7,600 draw calls a frame were only free because the frame copy hid them, and would start
costing once the copy was gone. They are not hidden by anything — they are simply free on this
hardware. The occlusion override can stay as it is, and *Better fixes* under run 30 drops from
"required after D3D9Ex" to "optional tidying, no measured payoff".

Incidentally the same run re-confirmed run 30 by reproduction: with mode 2 the vanishing objects
came back in full force, on one ini setting. The flicker now has a switch.

### ✅ Also confirmed healthy, for the first time with numbers

```
draws/frame peak 868, split 865 (865 with parallax, 0 flat), offscreen 8,
mono-fallback 0, user-ptr 0, frames with NO identification 0 [TRUE STEREO ON]
```

**Matrix identification is not failing at all** — zero frames lost it, zero mono fallbacks, and
100% of split draws got real parallax rather than being drawn flat. The whole Stage 4 rationale
was that identification failure was a live fragility. On this evidence it is not one today, which
demotes Stage 4 from "the structural fix" to a resolution/quality project, exactly as the run-27
note anticipated.

### ⬜ So where does the other 97% go? Leading hypothesis: a missed display deadline

The residual is ~27 ms and it is neither the copy nor the draw calls. The suspicious detail is
its value. Frame times cluster hard around **27.8 ms**, and **2 × 13.89 ms = 27.78 ms**, which is
exactly two frames at **72 Hz** — a common Virtual Desktop rate.

That is the signature of *just missing* the compositor's deadline and having to wait out a whole
extra display period, and it is a completely different problem from being uniformly slow:

- If it is a missed deadline, real work is somewhere between 13.9 and 27.8 ms, and **shaving 0.9 ms
  off the copy would do nothing** — but getting under 13.9 ms would **double** the frame rate to 72.
  That also explains the bimodal distribution far better than a CPU stall does: frames land either
  side of one deadline.
- If it is not, the work genuinely takes 27 ms and the question is what in it does.

`xrWaitFrame` blocks until the compositor is ready, so time spent there is the app **idle**, not
the app working — and until now it was lumped into the residual, where idling and real work are
indistinguishable and point at opposite conclusions.

**This build instruments it.** `xrWaitFrame` is timed separately, and `XrFrameState::predictedDisplayPeriod`
is logged so the pacing target is measured rather than inferred from arithmetic:

```
headset display period 13.89 ms (72.0 Hz) - frame times at 2x this are a missed deadline
frame budget: xrWaitFrame 12.40 (idle, waiting on the compositor) + copy 0.88 + our work 15.10 = 28.38 ms
```

### ⚠️ The log truncated on attach, and it cost two of the three tests

Tests A and B (`FrameCopyMode` pipelined vs immediate) **are gone** — each launch truncated the
previous run's log, so by the time the third number existed the first two did not. The failure is
silent: the file looks complete, it is just the wrong run. Every measurement this project makes is
an A/B across launches, so this was going to happen eventually.

Logs now rotate three deep: `view_matrix.log`, `view_matrix.prev1.log` … `prev3.log`.

Still unresolved as a result: whether pipelining is what made `readback` read 0.00, or whether it
always did. It does not change the D3D9Ex verdict either way — the total is 3% regardless — but it
decides whether to keep `FrameCopyMode=0` and its one frame of latency or drop back to immediate
and get that latency back for free.

## 🔬 Run 31: the frame copy, instrumented — the reasoning that led here

The performance plan of record is "upgrade the device to D3D9Ex, wrap `D3DPOOL_MANAGED`, share the
surface with D3D11, done". That is a **large** job: spike 2 measured **10,454 MANAGED allocations**
in one session, and D3D9Ex rejects the pool outright, so every one of them needs a `DEFAULT`
resource plus a staging copy we own, behind COM wrappers for five resource types, with unwrapping
at every device entry point that takes a resource. It is worth doing if the readback is the cost.

**Nothing has ever checked that it is.** "The frame copy is the bottleneck" has been repeated since
spike 2 on the strength of one indirect signal — the bimodal fps distribution — and the copy is
three costs, not one:

| Phase | What it is | Does D3D9Ex remove it? |
|---|---|---|
| `readback` | `GetRenderTargetData`, GPU → system memory | **yes** — this is the whole point |
| `lock/memcpy` | system memory → the D3D11 upload texture, ~14 MB at 1440p | no |
| `upload` | CPU → GPU again, into the XR swapchain image | no |

So this run builds the discriminator instead of the wrapper. Two changes:

**1. Phase timings in the per-second perf line.** Reports all three phases plus the **residual**
(frame time minus the copy). If the residual dominates, the round-trip was never the bottleneck and
the whole performance plan needs rethinking before a line of wrapper code is written.

```
frame copy [pipelined]: readback 0.00 + lock/memcpy 0.00 + upload 0.00 = 0.00 ms of 0.00 (0%), residual 0.00 ms
```

**2. `FrameCopyMode` — pipelined readback, and the A/B that prices it.** D3D9 has no asynchronous
readback, and the current code issues `GetRenderTargetData` and locks its result in the same
breath, at `Present`, when the GPU queue is deepest. That pays for a full pipeline drain *and* the
transfer. Mode 0 (new default) issues this frame's readback and consumes the one issued last frame,
giving the transfer a frame of GPU work to hide behind. Mode 1 restores the old ordering.

**This is the cheap half of the D3D9Ex win, testable in one run.** If pipelining collapses the
`readback` figure, the stall was the drain and a large part of the benefit is already banked
without the wrapper. If it does not move, the readback is a real transfer cost and the wrapper is
justified — with a number behind it rather than an assumption.

Also folded in: a single `memcpy` when both pitches are tight, replacing 1440 per-row calls.

### The latency, and the trap in it

Pipelining costs one frame of latency — the submitted image is a frame old. That is handled by
**capturing the eye, head pose and projection with the pixels** and submitting those, rather than
reading the live values when the frame surfaces. This is not fussiness. Handing the compositor a
*fresh* pose for a *stale* image makes it reproject towards a position the frame was never rendered
from, which reads as **swim** — an artefact that is both worse than lag and much harder to diagnose.
The submission block now reads `frame->pose` / `frame->dup` / `frame->stereo` throughout; do not
"simplify" it back to the globals.

A side benefit: toggling F1 mid-session can no longer submit a duplicated frame with mono eye rects
(or the reverse) for one frame, because the rect choice now travels with the image it describes.

### ⬜ What has to happen next, in order

1. **Read the perf line in both modes** (one launch each, `FrameCopyMode=0` then `1`).
2. **Then** decide on the wrapper. The three outcomes: readback dominates and pipelining fixes it →
   most of the win is banked, wrapper drops down the list; readback dominates and pipelining does
   not move it → write the wrapper, now justified by measurement; residual dominates → stop, the
   bottleneck is elsewhere and everything above is mis-aimed.
3. **Re-measure the occlusion override in the same session** — `OcclusionQueryMode=1` against `2`,
   comparing **residuals**. Draw submission lives in the residual, so that difference prices ~7,600
   draw calls a frame directly, without timing each one.

## ✅ Run 30: SOLVED — it was occlusion query readback

The engine issues a `D3DQUERYTYPE_OCCLUSION` query per primitive, draws its bounding box, reads
the pixel count back, and skips drawing that primitive on a later frame if it came back zero.
Overriding `GetData` to report a large count fixed every reported symptom at once, and the draw
counters quantify how much was being lost:

```
occlusion results overridden to FULLY VISIBLE (630 so far)
draws/frame peak 2272 ... 3809          <- was ~1000
perf: 70.6 fps ... 37-40 fps            <- unchanged
```

Reporting "visible" is safe in the sense that matters — it can never wrongly delete geometry, only
fail to save work. But it is **brute force**, and the section below says how to do it properly.

> ⚠️ **Correction, recorded because it was written down wrong first.** The original claim here was
> that single-view occlusion culling "is not well defined in stereo", so no correct answer exists.
> That is too strong. The engine issues one query, but we draw the box **twice**, once into each
> eye-half, so the returned pixel count is the *sum* — which is exactly the **union** of what the
> two eyes can see, and that is the correct conservative answer for stereo. Correct occlusion
> culling is achievable here. The override is a stand-in for a fix, not a substitute for one.

### ⬜ Why the root cause was never pinned down — and how to

**Occlusion boxes are drawn with colour writes disabled.** They are invisible by construction, so
HOME, END, INSERT and every other visual test in this project was *structurally blind* to them.
Whatever is happening to those boxes has never been seen. That is worth remembering before
reaching for another visual toggle.

**Leading hypothesis: the boxes go through `DrawPrimitiveUP`.** UE3 builds occlusion bounding
boxes on the CPU each frame, which is precisely what the user-pointer draw path is for. Those
hooks are off, so such draws pass through untouched at **full-frame coordinates**, while the depth
buffer they are tested against holds geometry we remapped into halves. The box is then compared
against depth from a different part of the screen entirely — which fails *systematically* rather
than marginally, matching the observed 50–70%. The magnitudes agree too: 245–370 user-pointer
draws per frame is the right order for per-primitive occlusion boxes.

This is inference, not measurement. **The cheap test:** hook `IDirect3DQuery9::Issue` (vtable slot
6) to detect `D3DISSUE_BEGIN`/`END`, and count how many draws inside each query window took the
unhooked or mono-fallback path rather than being remapped. That settles it in one run.

### ⬜ Better fixes, in increasing order of effort

1. **Selective override.** Track whether the draws inside each query window were ones we
   successfully remapped. Trust the result when they were; override only the untrustworthy ones.
   Restores most of the culling, surgical rather than global. Needs `Issue` hooked and a per-query
   flag.
2. **Fix the boxes.** Get the box draws remapped exactly like scene geometry and the query becomes
   correct on its own, with no override at all. Needs the user-pointer hooks working, which needs
   the draw classification that also fixes the run-28 rainbow — one piece of work buys both.
3. **Per-eye render targets** (Stage 4B below). No seam, no remap, boxes land correctly by
   construction, queries simply work. Also doubles per-eye resolution. Biggest job.

### ⚠️ Sequencing: do NOT do this before the D3D9Ex work

Right now the extra draws cost **nothing measurable** — frame rate was identical before and after,
because the CPU frame copy dominates everything else.

That changes once D3D9Ex lands. `split 3806` means 3,806 original draws each issued twice, so
roughly **7,600 real D3D9 draw calls per frame — about 290,000/sec at 38 fps.** That is a great
deal for D3D9, where draw submission is CPU-bound and expensive. Occlusion culling was doing real
work; its absence is currently hidden behind a bigger bottleneck.

So: **finish D3D9Ex, re-measure, then decide.** Doing it now would be optimising something the
profile says is free, with no way to tell whether it helped.



### What it cost to get here, and why

Seven mechanisms were eliminated **by direct test**, not by argument:

| Mechanism | Killed by |
|---|---|
| Shadow maps corrupted by splitting | Only 11 draws of ~1,640 were offscreen |
| Characters use a second matrix register | The END test — clip `c0` and nothing unremapped is left standing |
| FOV reset at the camera-update seam | Re-asserting there made the 10th-percentile FOV *worse* |
| Vertical culling shortfall | The engine's own arriving matrix reads 112–135° vertical vs our 98° — covered |
| Screen-size distance culling | Headroom walk to ×1.10 inflation, native LOD, centred object still gone |
| Unremapped geometry clipped at the seam | HOME (scissor off) did not bring it back |
| Draw-path coverage (`DrawPrimitiveUP`) | `mono-fallback 0`, and the artifacts are identical with the hooks off |

**Two things made the difference after six failed rounds of theorising.** First, run 27 audited the
*instruments* rather than proposing a seventh mechanism — and found three that were structurally
incapable of reporting anything (see below). Second, every step from run 28 on built a
**discriminator** rather than a hypothesis: HOME separates drawn-then-clipped from never-drawn,
END separates remapped from unremapped, the headroom walk sweeps the inflation range end to end.
Each one eliminated a whole class in a single session.

The decisive logical step was narrowing by causality rather than by symptom: the chest not being
drawn is a **CPU-side decision**, our CPU-side influences (FOV, rotation) were both ruled out by
test, and the only path by which our GPU-side changes can reach a CPU decision at all is query
readback. That named the answer before it was tested.

### ⚠️ Refusing to create the queries CRASHES this build

`D3DERR_NOTAVAILABLE` is the documented answer for an unsupported query type, and the path UE3
takes on hardware without the feature. This build dies between the splash screens and the main
menu — consistent with the unchecked-NULL-from-a-factory habit already recorded for Raven's code.
Kept as `OcclusionQueryMode=3` and documented as fatal so it is not retried. **Override the answer,
never the availability.**

## ✅ Run 27: four defects found by reading, no headset required

No new theory of the flicker — instead, a pass over the code and the last log for things that are
simply wrong. Four were, and each is capable of producing the reported symptoms on its own.

### 1. The user-pointer "hang" was a printf argument mismatch

The per-second perf line had **nine conversions and ten arguments**. `upPeak` was missing its `%d`,
so `%s` consumed an `int` and formatted it as a `char*`. The evidence was in every log from run 23
onward and went unread:

```
perf: ... offscreen 0, mono-fallback 0(null)
```

`(null)` is `upPeak == 0` printed as a string. `[TRUE STEREO ON]` never appeared once, because its
argument was being swallowed.

Harmless only while `upPeak` is zero — and `upPeak` is non-zero **exactly when
`HookUserPointerDraws >= 2`**. At that point `%s` dereferences a small integer, inside `Log()`,
inside `EnterCriticalSection`. The fault leaves the lock held, so every later `Log()` blocks
forever: a hang, not a crash, which is what was reported.

That is the whole of "level 1 reaches the menus, level 2 does not". **The run-26 conclusion that
these calls arrive on a different thread was inferred from a symptom this fully explains.** The
thread guard is kept — issuing device state changes from a thread that does not own the device is
wrong regardless — but the log now *reports* whether these calls really are off-thread, so the
question gets answered rather than assumed.

`Log()` is now annotated `_Printf_format_string_` and `build.ps1` runs an `/analyze` pass that
**fails the build** on a format defect. Both halves were needed: verified that `/W1` through `/W4`
stay silent even with the annotation, and that `/analyze` without the annotation also says nothing.

### 2. "with parallax" was a compile-time constant

`const bool remapKnown = true;`, and the offset counter was incremented under it. So
`split N (N with parallax, 0 flat)` could never read anything else. **Run 16's "906 with parallax,
0 flat → the stereo mechanism is validated" was that constant, not a measurement.** It now keys on
the eye-separation vector actually being non-zero.

### 3. Four paths left a stale view matrix being pushed over live engine state

`StereoPair` re-uploads `g_camMats[s].m` for every draw while the slot is valid. Four paths cleared
the conditions for maintaining that slot without clearing the slot:

| Path | Consequence |
|---|---|
| No valid camera position | `FindCamera` misses are retried only every **30 frames** → up to half a second in which every draw is forced through the previous frame's view-projection |
| No lock | Same, until a rescan happens to succeed |
| An upload that **overlaps** the tracked window without covering it | We re-upload our cached block over constants the engine just wrote |
| Lock revoked after 60 unverified frames | Cleared `g_haveLock` but not the slot |

A whole frame rendered from a frozen camera, then unfrozen, is indistinguishable from the flicker
being chased. Every exit now leaves the slot honest.

The camera-position gate was also simply wrong for the modify path: the winning probe is the
**origin** one, whose test is `|r[3].w| ≈ 0` — pure matrix content, nothing to go stale, no camera
reading involved. Only the *scan* needs a live camera position. That gate is now scan-only.

### 4. Identification was gated on a stale facing test, and failing it costs the whole frame

`IsViewMatrix` required `dotFwd >= 0.99` — about 8° — against `g_camFwd`, read at Present from the
camera **actor**: a frame away from the constants being tested, and a different object from the one
driving the render. At ~37 fps a 300°/s turn is ~8° per frame.

The cost of a rejection was never appreciated. `c0` carries one matrix per frame, so a rejection is
a **whole-frame** event: no forced projection and no per-eye split for any draw in it. That is a
full-frame flash keyed to head movement — and **run 23's elimination test was run with head
tracking off**, which could not have seen it.

The per-call test now uses rotation-independent structure instead:

- projection tangents in band (was only guarding the diagnostic, now a gate)
- **the basis must be orthogonal** — for `M = V·P`, column 0 is `sx·right`, column 1 is `sy·up` and
  the w column's xyz is `forward`, so normalised they are mutually perpendicular. Skinning
  palettes, UI transforms and the float3x4 blocks the sliding window straddles all fail it.

With those carrying the discrimination, the facing gate is loosened to **0.80 (~37°)** for the
per-call path — enough to reject a matrix pointing somewhere else entirely, immune to a frame of
lag. The scan keeps 0.99 for locking, where being strict is right and nothing is stale.

New counters say whether this was the flicker: `frames with NO identification`, and the **best
rejected `dotFwd`** — near 0.99 means rotation lag, far below means a genuinely different matrix.

### ⚠️ The culling number has been a proxy all along

The last log contains both, and they disagree:

```
engine FOV read back before write: mCurrentPOV 157.9
engine's own matrix on arrival:     80.7 x 51.1 deg
after forcing, the matrix measures 127.9 x 98.0 deg
```

The engine accepts 157.9 in the POV field but the matrix it actually builds carries **80.7° × 51.1°**
— and that is the frustum it rendered, so it is the one its culling came from. We render **98°
vertical against a ~51° culling frustum.** Every shortfall figure reported so far came from
`mCurrentPOV` instead. The log now prints both and flags when the proxy said "covered" and was
wrong.

This does not overturn run 22 (which ran at 40%, where the ask is much lower), but it means the
culling question was never actually measured at the operating point people played at.

### Also fixed, smaller

- The render-target census keyed on `(width, height, format)`, so **two distinct scene-sized
  surfaces read as one row**. `g_rtIsScene` splits on size equality alone, so a full-resolution
  offscreen target — a post ping-pong buffer, or the whole-scene dominant shadow map UE3 allocates
  at scene resolution — is being split as if it were the scene, and the census could never show it.
  Run 11's "shadow maps are not being separated by size" rested on that. Now one row per surface,
  with a warning if more than two scene-sized surfaces are being split.
- **F1 did not enable the position path.** The left eye is produced in the constant hook and the
  right by adding the eye delta in `StereoPair`, so with F1 alone the left eye sat at the engine's
  own camera and the right at +IPD — the pair biased half an interpupillary distance, and head
  translation did nothing unless F10 happened to be on too.
- `data` was dereferenced three lines above the null check that guards it.
- Slot exhaustion (8 rescans onto different registers) would have switched stereo off silently.
- A lock acquired on a non-origin probe point could never verify, so it would have looped
  lock→drop→rescan for ever with stereo off. Now refused explicitly, with the reason logged.

## ⬜ Stage 4: the structural fix — make placement independent of identification

Run 27 removed several ways identification can fail. It did not remove the fact that **placement
depends on it at all**, and that dependency is the whole seam class:

| Draw | What happens today |
|---|---|
| Matrix recognised | Clip remap + scissor. Correct, and screen-space sampling stays consistent. |
| Matrix not recognised | Drawn once, full-frame. A centred object lands *on* the cut — the left eye gets its right sliver at the far right edge, the right eye its left sliver at the far left. |

The mono fallback (run 23) turned "vanishes" into "misplaced", which is better but is not correct.
Three ways out, in order of preference:

**A. Viewport split + per-half `ScreenPositionScaleBias` — recommended.** Place each eye with the
**viewport**, which applies to every draw whether or not we can identify its matrix, so geometry
can never be lost at the seam. That reintroduces exactly one problem, the one that forced the move
to clip-space remapping in run 11: shaders computing `UV = ndc·scale + bias` with constants built
for the full target sample the wrong half. Fix it at the source by patching that constant per half
— with `ndc ∈ [−1,1]` wanting `uv ∈ [0,0.5]`, it is `scale·0.5` and `bias·0.5 (+0.5 for the right
eye)`. The constant is findable the same way the view matrix was, and with a far stronger
signature: it is derived from the render-target size, typically near `(0.5, −0.5, 0.5+½px,
0.5+½px)`. **This also makes post-processing per-eye-correct**, which is currently listed as
unsolved, and it composes with the D3D9Ex work rather than conflicting with it.

**B. Per-eye full-size render targets.** Semantically cleanest — no seam, no remap, no scissor, and
per-eye resolution doubles, which also sidesteps the 4096 cap and the sharpness problem. But it
needs a render-target *and* depth-stencil swap per draw call; D3D9 flushes on those, and at ~1,600
draws/frame that is ~3,200 switches. Likely unaffordable, but worth one measurement before ruling
out.

**C. Keep the clip remap and keep chasing draw paths.** This is what has been tried for six runs.

Do not start Stage 4 before the run-27 measurement: if `frames with NO identification` turns out to
be the flicker, the cheap fix already landed and Stage 4 becomes a resolution/quality project
rather than a bug fix.

### Method note

**Four defects, none of them a new theory, all found by reading the code and the last log against
each other.** Three of them had left evidence in the log for multiple sessions — `(null)` sat in
every perf line since run 23. After six wrong theories the instinct is to reach for a seventh; the
cheaper move was to check whether the instruments were telling the truth. Two of them were not.

### ⚠️ THE long-running open issue: objects and textures flickering

Distinct from the one-eye ground-texture bug (fixed run 12, still fixed). Objects pop in and out —
a chest, a locker door, a corpse, textures generally — worst when looked at directly.

**Three theories have now been wrong**, which is worth recording as a caution:

| Theory | Killed by |
|---|---|
| Shadow maps corrupted by splitting | Only 11 draws of ~1,640 were offscreen; excluding them changed nothing |
| Characters use a second matrix register | Register count measured **1, ever** |
| FOV reset happens at the camera-update seam | Re-asserting there corrected ~0.06/frame and made the 10th-percentile FOV *worse* |

The surviving theory is the **vertical culling shortfall**: the engine's FOV is horizontal at 16:9,
so its vertical culling frustum is far smaller than its headline number, and it dips to the game's
65° default (~39° vertical) while we render 98°. Geometry in that gap is culled before a draw call
exists, so nothing downstream can recover it.

#### ✅ Run 21: culling CONFIRMED — and there are TWO opposed mechanisms, not one

The 40% test worked as designed. At 55% and 40% the chest came back, and the monster stopped
vanishing. **Culling is confirmed as a real cause.**

But a residual remained — "things disappearing at a distance" — and the decisive evidence is a
control the user ran unprompted: **in the unmodified game those objects are visible from much
further away than where they vanish in the mod.** So the mod is culling early; this is not the
game's own draw distance.

That identifies a **second** mechanism, and it is driven by the same field as the first, in the
opposite direction:

| Mechanism | Caused by | Wants the engine FOV to be |
|---|---|---|
| Vertical frustum shortfall — geometry outside the engine's culling frustum | engine FOV **too low** (dips to 65°) | **wider** |
| Early distance culling — UE3 picks LOD and cull distance from projected screen size | engine FOV **too high** (we ask 157.9°) | **narrower** |

At 157.9° the inflation is **2.4× native**, which makes objects project about **8× smaller** than
the engine was tuned for — so anything with a screen-size-based draw distance culls roughly 8×
closer than vanilla. That is exactly the reported symptom.

**And headroom does not even fix the first problem**, because a dip to the 65° default is a dip
regardless of what we asked for. So high headroom buys little and costs a lot. The build now logs
the inflation factor and the implied shrink, so both sides of the trade are visible.

The consistent operating point is **render 40% + headroom 1.0**: the ask lands at ~64.6°, essentially
native, so inflation is ~1.0 (no LOD distortion) *and* the 39.2° rendered vertical is exactly
covered. Narrow, but it should be free of both mechanisms — which makes it the test that confirms
the second one.

Real fixes, both needing reverse engineering rather than tuning: find what resets the FOV to 65
(hardware write breakpoint on the field), or find UE3's draw-distance/LOD scale and compensate for
the inflation directly.

#### ❌ Run 22: at the artifact-free operating point, objects STILL disappear

The clean configuration was genuinely reached — the log confirms it, not inference:

```
FOV inflation vs native 65 deg: x1.00        (engine FOV 64.7-64.8, no LOD distortion)
worst engine VERTICAL cull: 39.2 deg vs 39.2 deg rendered -> covered
```

No inflation, no shortfall. **And distant objects still vanish.** So culling and LOD distortion,
while real, are not the whole story — there is a **third** mechanism. Four theories now dead.

One reported detail is probably *not* a bug: a close water fountain vanishing when off to the left.
At 40% the per-eye frustum is only ~35° wide (±17.6°), so a nearby off-centre object is legitimately
outside it. The 40% setting is confounded for anything off-centre; it is only trustworthy for
head-on objects.

#### ✅ Run 23: FOUND IT — splitting a draw we could not remap clips it at the seam

Elimination worked where five theories had not. With head tracking **off**, the chest was **visible
in alternate-eye mode and gone in duplication mode** — which placed the cause inside the draw hook
and ruled out rotation, FOV and culling in a single test.

The bug: **splitting was unconditional while the remap was conditional.** Splitting was made
unconditional back in run 7, to stop objects vanishing from one eye — correct at the time, because
the split then used the *viewport*. Under the clip-space scheme the two must travel together:

- Scissoring geometry we did **not** remap clips it to a half while it still holds **full-frame**
  coordinates.
- The centre of the frame **is** the seam. So a head-on object is discarded in *both* halves at once,
  while an off-centre one partly survives — exactly the reported pattern.

It was intermittent because the camera slot goes briefly invalid every time `c0` is written with
something that is not a view matrix, which happens repeatedly *within* a frame. That intermittency
is what "flickering" was.

**Fix:** when there is no live camera matrix, draw once, full frame, untouched — no scissor, no
split. That costs the object its parallax for a frame instead of its existence. The log now reports
a `mono-fallback` count so how often it happens is visible rather than inferred.

Worth recording that this also explains earlier confusion: the flicker never responded to FOV,
headroom, resolution or shadow changes because none of them touched the actual mechanism.

#### The elimination plan that found it (kept for method)

Five theories have been built and knocked down by their own diagnostics. The variable never
isolated is **which of our interventions** causes it, so test that directly rather than proposing a
sixth mechanism. Each step removes one thing:

| Test | Keys | If the flicker STOPS |
|---|---|---|
| A. No head tracking | F1 on, **F9 off** | The rotation write is the cause — likely a frame-lag mismatch where the engine culls using the rotation it had *before* our write, then renders from where we put it. Would explain "depends on where my head is pointed" and survives every FOV fix. |
| B. No duplication | **F1 off**, F9 on | Stereo/draw duplication is the cause |
| C. No projection forcing | **F6 off**, F1 off | Our matrix edits are the cause |
| D. Nothing on | all off | The mod is innocent; it is the game or the streaming link |

Run them in order and stop at the first one that clears it. That names the culprit in one session
instead of another round of theory. Note the flicker was reported on a manhole cover **before F12
was ever pressed**, which already hints the cause is not stereo — test A first.

**A decisive test now exists rather than another guess.** PAGE DOWN gained two steps (55%, **40%**).
At 40% we render 39.2° vertical — just inside the engine's *worst observed* case, so no shortfall is
possible on any frame:

- **Flicker vanishes at 40%** → culling confirmed; the fix is finding what resets the FOV (needs a
  hardware write breakpoint on the FOV field, the same tool that found the rotation source after
  seven failed guesses).
- **Flicker persists at 40%** → culling is innocent too, all four theories are dead, and the next
  move is instrumenting the draw stream directly instead of theorising.

40% is a narrow letterbox and unplayable. That is fine; it is a measurement, not a setting.

### ⚠️ Also open: texture flicker while walking

Distinct from the one-eye bug and still present. Prime suspect is now the **culling headroom
itself**: UE3 derives texture-streaming mip selection *and* mesh LOD from projected screen size, so
telling the engine the field of view is **157.9°** makes everything read as far away and can thrash
the streaming pool — which looks exactly like textures popping in and out while moving.

That headroom is a real trade, not a value to get right once: wide buys culling coverage, narrow
buys correct LOD and streaming. **PAGE UP** now cycles it (2.5 → 1.75 → 1.30 → 1.0) so the trade
can be measured. If flicker tracks the headroom, the long-term fix is widening the engine's culling
frustum *without* inflating the FOV it reports to streaming/LOD.

### FOV controls — what exists

Three separate things, worth keeping distinct:

| Control | What it does |
|---|---|
| **F6** | VR-correct projection off → the game renders its **native ~65°** and we claim the headset's FOV. This is the "standard FOV" toggle, and it has been there since run 4. Only meaningful in **mono** — with F1 on it leaves the per-eye frustum unforced and both halves come out squashed. |
| **PAGE DOWN** | *New.* Render only 100% / 85% / 70% of the headset's field. Geometrically honest: the submitted frustum follows the same numbers, so the image just subtends less of the eye. |
| **PAGE UP** | Culling headroom multiplier on top of whatever PAGE DOWN selected. |

PAGE DOWN is the more useful end of the same trade as PAGE UP. Because the engine's FOV is
horizontal at 16:9 while ours is tall and narrow, covering 98° vertical forces its reported FOV to
~128° *before* any margin — which is what drags streaming and LOD off. Narrowing the render lowers
that requirement faster than it costs immersion:

```
100% -> 98.0 deg vertical, needs the engine at ~128 deg
 85% -> 83.3 deg vertical, needs the engine at ~112 deg
 70% -> 68.6 deg vertical, needs the engine at ~96 deg
```

### Run 10: the eye mapping is CORRECT (this part stands)

The INSERT test settled the mapping: left-half-only blanked the **right** eye, right-half-only
blanked the **left**. That is correct, so the swap theory is dead and DELETE is not needed.

The decisive clue was elsewhere in the same report: with both halves drawn, distant building
textures drop out and return when looking away and back; with only **one** half drawn they are
*mostly stable*. So the damage comes from **doing two draws**, not from matrices or mapping.

Cause: the run-7 fix over-corrected. Splitting *every* draw also split draws aimed at render
targets that have nothing to do with the eyes — **shadow maps** above all. Rendering the shadow
scene twice into two halves of a shadow map leaves it holding two squashed copies, each covering
half its width. Objects then sample garbage shadow data, which reads exactly as distant surfaces
going dark and recovering when the view shifts and the cascade is rebuilt. One half is less wrong
than two, hence "mostly stable" under INSERT.

**Fix:** hook `SetRenderTarget` (vtable 37), track whether the bound target is scene-sized, and
only split when it is. Shadow maps, reflection buffers and half-res post buffers are left alone.
The perf log now reports how many draws were skipped as offscreen.

### ⚠️ Structural finding: the half-width viewport breaks unrecognized matrices

Worth recording because it changes the plan. Every game projection matrix assumes the render
target's **16:9** aspect. Rendering into a half-width viewport silently invalidates that. For the
one matrix we recognize we compensate (`ApplyProjection` forces the per-eye frustum), but for
every matrix we do **not** recognize there is no compensation — it renders with the wrong aspect
in each half. The first-person weapon is the obvious victim, and the reported "gun disappeared,
as if the in-game gun covered it" is consistent with a weapon drawn through its own separate
narrow-FOV matrix landing wrong.

**Therefore the double-wide render target is not an optimisation — it is required for
correctness.** At 5120×1440 each half is exactly 2560×1440, the size and aspect the game already
expects, so every matrix stays valid with no per-draw compensation, and per-eye resolution
returns to full. Cost: 2x backbuffer memory, 2x pixels, and a 2x frame copy (~28 MB round trip)
on a path that is already the bottleneck. Needs `D3DPRESENT_PARAMETERS` interception at
`CreateDevice` plus matching interception of UE3's own scene-sized targets.

### ⚠️ Run 9: rocks visible in one eye only — still unexplained

Rocks off to the **left** are missing from the **left** eye — the eye *nearer* to them — and stay
missing regardless of view direction. That is backwards from frustum clipping: for an object off
to the left, the **right** eye sits further away and sees it at the *larger* off-axis angle, so
the right eye should lose it first.

The eye separation itself is confirmed correct — **3.32 UU = 6.3 cm**, textbook IPD, stable all
session — so this is not a magnitude or sign error in that vector.

With the mapping now confirmed correct (run 10), a swap is ruled out — so the cause is **not**
frustum clipping and not the eye assignment. Still open. The shadow-map fix above may resolve it
as a side effect, since corrupted shadow data can black out a surface entirely; if it does not,
the next suspect is a depth-prepass/base-pass mismatch, where one pass gets the offset applied
and the other does not, so geometry fails the depth-equal test in one half only.

Diagnostics available: **INSERT** cycles both halves / left only / right only. **DELETE** swaps
the assignment (kept, though the mapping is now known correct).

### ⚠️ Correction: the "with parallax" count is optimistic

`g_camMatrixBound` is **sticky** — it reflects the last upload to the tracked register, not
whether *this* draw's transform actually comes from it. So "100% with parallax" means the camera
matrix was the last thing written there, not that every draw was genuinely offset. A draw taking
its transform from a different register counts as parallaxed while receiving no offset at all.

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

**Decision taken: continue with true native stereo** (draw-call duplication). Depth reprojection
stays documented above as the fallback if this path stalls again.

1. **Run at true parity resolution** to see the ceiling this approach can reach:
   `Singularity.exe -ResX=4992 -ResY=2688 -windowed`. Frame rate will be poor — that is the
   measurement, not a failure. Check the log for `LOCKED on cN` and a single stable value in
   `own matrix on arrival` rather than the previous spread.
2. **Then the D3D9Ex zero-copy path** — the gating item. Parity means ~49 MB per frame each way over
   the CPU; 3840×2160 already halved the frame rate (~70 → ~37 fps).
2. **Then performance**, which is now the gating item rather than a later nicety. Two reasons it
   has been promoted: the frame copy scales with pixels, so matching the headset's resolution
   multiplies its cost; and the fps distribution is bimodal (median ~70, floor ~13), which is the
   signature of a CPU-bound stall rather than GPU load. Details under *Other known work*.
3. **Then match the render resolution** to the headset — deliberately after performance, because
   per-eye 1280×1440 vs the headset's ~2496×2688 means roughly 3.6× the pixels, on exactly the
   path that is already the bottleneck.
4. **Post-processing full-screen passes remain unsolved** — they use a pass-through transform and
   sample across the whole target, so they cannot be remapped this way. Expect seam artifacts from
   bloom and similar until handled per half.
5. **Recognise more matrices** if specific objects (the weapon) stay wrong: draws whose transform
   comes from a register we do not track get no remap and are merely scissored.
3. **Depth reprojection** as the alternative approach, worth building regardless so there's a
   real comparison — and a shipped mode either way.
4. **The ini** for mode selection, plus a fallback to mono.
5. **Explain the brightness change** (run 4, deferred): most likely UE3 auto-exposure adapting to
   a much wider field, which is benign; the alternative is our injection touching a
   post-processing pass. Distinguishable with F6.

## Other known work, roughly by value

- **✅ RESOLVED (run 33): the cap is 4096, and it makes per-eye render targets mandatory.**
  `-ResX=4096 -ResY=2160` is accepted and the scene targets follow it; 4992 is refused. That is the
  classic D3D9 maximum texture dimension, exactly as hypothesised.

  The architectural consequence is now fact rather than risk: per eye tops out at **2048** against
  the **2688** the headset wants, so side-by-side delivers **76% linear / 58% of the pixels** and
  cannot be made to deliver more. Stacking vertically is worse. **Reaching parity requires per-eye
  render targets**, which is Stage 4B — promoted from optional to required, and now the fix for the
  seam, post-processing bleed and occlusion boxes as well.

- **Performance — the D3D9Ex + MANAGED wrapper. Reinstated by run 32, but re-scoped.** Not a frame
  rate fix: the game already does ~110 fps at 1440p. It is the **resolution enabler** — the CPU
  round-trip costs ~4.2 ms of an ~8.9 ms frame today and scales with pixels, so at 3840×2160 it
  projects to ~9.5 ms, more than an entire 120 Hz frame. Still 10,454 MANAGED allocations to wrap
  across five resource types. Do it when the resolution work needs it, measured at that resolution,
  not before.
- **FOV / aspect mismatch** — game renders 16:9, per-eye view is ~2496×2688.
- **Head-look vs aim are coupled** — `PlayerController.Rotation` drives both view and fire trace.
- **Menus are inert** — intended; they need OpenXR *input* (controllers, thumbsticks, laser
  pointer), which has not been touched at all. Same work unlocks motion-controller aiming.
- **GOG install crash** — a latent Raven bug (unchecked NULL from a factory) that hits on a fresh
  GOG install with missing redistributables. The install guide must warn about it.

## Method notes that earned their keep

- **Validate the test environment before trusting the instrument.** SSW halved every frame-rate
  number this project recorded for thirty runs, and nothing in the log looked wrong — the timings
  were honest, the frame itself was synthetic. Run 31 built a correct instrument, measured
  carefully, and drew a confident conclusion that was wrong at the input. **A measurement is only
  as good as the thing being measured is real.** Check pacing, refresh, reprojection and frame
  generation *first*, and record them next to the numbers.
- **Measure the thing before building what the measurement justifies.** "The frame copy is the
  bottleneck" survived thirty runs and became the top-priority work item without anyone timing it.
  That instinct was right — but see above, because the first attempt to act on it still produced a
  wrong answer, and being right about *method* did not save it.
- **A stall that leaves your instrument has not left your frame.** Pipelining the readback took the
  copy phase from 4.2 ms to 0.89 ms and moved every one of those milliseconds into unmeasured work,
  for no net gain. Any phase timing that improves while the total does not has probably relocated a
  cost rather than removed one.
- **Rotate logs that are truncated on attach.** Every measurement here is an A/B across launches,
  so a log that truncates destroys the run being compared against — silently, since the file still
  looks complete. Run 31 lost two of three tests that way.
- **Audit the instruments before proposing another mechanism.** Six theories died on their own
  diagnostics; the seventh was found only after run 27 checked whether the diagnostics could
  report anything at all. Three could not: `remapKnown` was `const bool ... = true`, the perf
  line's `%s` was eating an integer argument, and `camera matrices tracked` is capped at 1 by
  construction. Two of those had been quoted in this document as *evidence*.
- **Build discriminators, not hypotheses.** A test that separates two whole classes
  (drawn-then-clipped vs never-drawn; remapped vs unremapped) retires more in one session than
  three rounds of argument. Every step from run 28 on was one of these, and each killed a class.
- **Narrow by causality, not by symptom.** The answer was named before it was tested: the object
  not being drawn is a CPU decision, both of our CPU-side influences were already eliminated, and
  query readback is the only route by which a GPU-side change can reach a CPU decision.
- **A test that destroys its own readability is a wasted run.** Collapsing `c0` to a point piled
  every triangle onto a few pixels and let bloom smear the result over everything. Clipping the
  geometry instead — same intent, legible frame — answered it immediately.
- **Override the answer, not the availability.** Refusing to create the occlusion query crashed
  the game; patching `GetData` to lie ran the identical experiment with the engine's control flow
  untouched. Prefer the intervention that leaves the program on its normal path.
- **Check which rows of the log you are reading.** The 51.1° culling figure that briefly revived
  the shortfall theory came from a pre-gameplay frame. The instrument was right; the row was
  wrong.

- **Verify by writing, not by reading.** A struct holding plausible live values is not proof the
  renderer uses it. `mCurrentPOV` looked exactly like the camera and steers only FOV.
- **When guessing fails twice, change tools.** A hardware write breakpoint found the rotation
  source immediately after seven failed guesses.
- **Shotgun, then narrow.** Testing candidates one at a time behind a hotkey let a working case
  get cycled past unseen.
- **Doc errors become code bugs.** `ENGINE_NOTES` recorded the backbuffer as `X8R8G8B8`; it is
  `A8R8G8B8`, and that mistake propagated straight into a `D3DERR_INVALIDCALL`.
