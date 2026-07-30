# STATUS — session handoff

Last updated **2026-07-29**. Read this first, then `ENGINE_NOTES.md`.

## Ladder status (against the original plan)

| Rung | State |
|---|---|
| Head-tracked image in the headset | ✅ done |
| Colours — sRGB | ✅ done |
| **6-DOF positional tracking** | ✅ **done** — camera position solved via the view matrix |
| **Stereo — per-eye offset for real depth** | 🔧 **working, artifacts remaining** — true native stereo by draw-call duplication; one-eye ground textures fixed run 12, texture flicker open |
| FOV / aspect match | 🔧 projection is VR-correct and forced; **render resolution not yet matched** to the headset |
| Performance — CPU round-trip → D3D9Ex + MANAGED wrapper | ⬜ not started, still planned |
| Controller input, menus, aim decoupling | ⬜ not started (mouse/stick coexistence fixed run 12 as a prerequisite) |

So: **6-DOF is solved**, and stereo is *working* rather than done — the mechanism is right and the
remaining issues are graphical.

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
**F7** scan for the view matrix · **F3** constant offset probe · **F11** offset amount ·
**INSERT** blank one eye half (mapping test) · **DELETE** swap eye/half assignment ·
**PAGE UP** cycle culling headroom · **PAGE DOWN** cycle how much of the headset's FOV we render.

Mouse and head tracking now **add** rather than fight — turning with the mouse no longer snaps
back, and the same fix is what stick-turning will need in the shipped mod.
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

**Correct route:** change resolution in the game's **video options**, which makes the engine
allocate scene targets at the new size and keeps backbuffer and scene consistent. There is no game
ini to edit — Singularity keeps video settings in its binary `.ue3profile`.

The override has been removed rather than left as a disabled option, since it cannot work. The ini
stays for the mode selection the shipped mod needs.

**Guard added:** if duplication is on and *nothing* was split, the log now says so loudly. Stereo
silently doing nothing cost a whole session here; it should never be inferred from a census again.

### ⚠️ Still open: texture flicker while walking

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

1. **Raise resolution in the game's video options** (not the ini), to 3840×2160 if offered, with
   render FOV back at **100%** (PAGE DOWN twice from 70%). Prediction: per-eye becomes 1920×2160 =
   21.0 px/deg, nearly the 20.5 that felt good at 70%, so shimmer at *full* FOV should look about
   like 70% did. Quantitative, so it either lands or the sampling theory is wrong too.
2. **Then the D3D9Ex zero-copy path.** Unambiguously the gating item now: matching the headset means
   ~49 MB per frame each way over the CPU. Everything else waits on it.
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

- **Performance — now the gating item, not a later nicety.** The frame copy is a full CPU
  round-trip (~14 MB each way at 2560×1440). The fast path needs the game's device upgraded to
  D3D9Ex, which needs a `D3DPOOL_MANAGED` → `DEFAULT` wrapper (**10,454** allocations measured in
  spike 2). Two measurements promote it: the run-12 fps distribution is **bimodal — median ~70,
  floor ~13** across 66 samples, which is the signature of a CPU-bound stall rather than GPU load
  (draw-call duplication doubled GPU work and barely moved the median); and matching the headset's
  resolution multiplies the copy cost by ~3.6×, so it blocks that too.
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
