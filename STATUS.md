# STATUS — session handoff

Last updated **2026-08-08** (defaults audit). Read this first, then `ENGINE_NOTES.md`.

**This file is current state only.** The run-by-run log moved to `HISTORY.md` on 2026-08-04 — you
do not need it cold, but go there before building on any premise you inherited.
It also flags, with ⛔ banners, the status claims that had gone stale in place.

> ### ⭐ Run 180: UE3 yaw deltas were never wrapped, and it cost months of misattribution
>
> `camYaw - ctlYaw` on `int32_t` does **not** wrap. UE3 is 65536 units per turn, so two headings
> **0.3° apart** differ by 65590 units and read as **360.3°**. Two sites did this and both carried
> a comment saying `// wraps correctly`.
>
> Measured in one 8,476-line run: **20** false `camera handed to the GAME (360.3 deg from the
> pawn)` events, each re-anchoring `g_baseYaw` to the camera and yanking the view. Reported for
> months as phantom snap turns — there were no snap turns; the run was `TurnMode=2` and fired
> **zero**. After the fix, same conditions under heavy turning: **0 handoffs**, and the drift log
> reports `-5.0 / +89.5` instead of `±360`.
>
> **Cast the difference to `int16_t`** — 65536 units *is* 2¹⁶, so truncation gives the shortest
> signed path for free. Use `YawDelta()` / `YawDeltaDeg()`; a raw subtraction is only safe if both
> sides are provably normalised, and nothing here normalises them.
>
> ⚠️ **Several other sites still subtract yaws raw**, including one at the FRotator write path
> carrying the same `int32 wrap is correct` claim. No measured evidence yet. Audit them next.

> # ✅ The ladder is complete. Every rung, including controller input.
>
> **Full resolution parity at a locked 120 fps, inherited automatically from the headset.**
> Launch `Singularity.exe` with no arguments. That is the whole procedure — it **starts in VR**
> (`StartInVr=1`, the default since run 159). No keypress, no command line.
>
> ```
> auto-resolution: injecting -ResX=4992 -ResY=2688 -windowed (cached from the headset last run)
>     requested 4992x2688 -> got 4992x2688   *** ACCEPTED ***
>     per eye 2496x2688 against the headset's 2496x2688 -> 100% horizontal, 100% vertical
>
> fps median 119.9   our work 3.02 ms   xrWaitFrame 4.35 ms IDLE   XR submit 0.94 ms
> ```

## TL;DR — where we stand

A **working, wearable VR mod** for Singularity (2010, UE3, D3D9, x86), shipping as one `d3d9.dll`
dropped beside the game exe. No game files modified.

| rung | state |
|---|---|
| Head-tracked image, sRGB colours | ✅ |
| **6-DOF positional tracking** | ✅ camera position solved via the view matrix |
| **Head roll** | ✅ run 59 — applied in the view matrix, `NUMPAD1`/`NUMPAD2` |
| **True stereo**, real per-eye parallax | ✅ draw-call duplication; eyes align, depth correct |
| **FOV / aspect / resolution** | ✅ **100% of the headset's pixels, automatic** |
| **Performance** | ✅ **120 fps locked**, 4.35 ms idle per frame |
| **Controller input** | ✅ **working** — full Touch mapping, menus, haptics, snap turn, off-hand movement |
| **Aim decoupling** | ✅ **SOLVED via route 2** — the bullet follows the controller, the view never moves, and it costs NOTHING in culling or LOD. Aim mode 11 |
| **Gun follows the controller** | ✅ **working** — 6-DOF, arms hidden, anchor tuned. APPS toggles it |
| **Cutscene head tracking** | ✅ **working, automatic** — run 138. `SetCinematicMode` drives it; look around freely during a cutscene, normal turning restored on exit |
| **`D3D9ExMode=1` texture corruption** | ✅ **fixed** — run 139. Zero-copy is usable again; visuals match `D3D9ExMode=0` |
| **Smooth turning** | ✅ run 140 — `TurnMode=2`, degrees per second, works during cutscenes |
| **VR settings panel** | ✅ run 142 — hold Y 1.3s anywhere; live changes, saved to the ini on close |
| **The game's HUD and menus in both eyes** | ✅ run 151 — health, ammo, crosshair, prompts, pause menu. `HudStereo=1` |
| **Shipping defaults = the tested configuration** | ✅ 2026-08-08 — a fresh install with no ini now behaves like the tuned dev machine. See below |

### ⭐ The code defaults ARE the tested configuration (2026-08-08)

Every validated run in this project's history ran against the dev machine's `SingularityVR.ini`.
Nothing had ever run on the **code defaults** — the configuration a new user actually gets. A second
install exposed the gap: with no ini, the gun swivelled about the wrong point, the debug readout was
drawn, and the D3D9Ex wrapper was off so the frame took the ~9.8 ms CPU copy.

All 48 keys were diffed mechanically against the source. These defaults were promoted to the tested
values, and `SingularityVR.ini.example` — which shipped `D3D9ExMode=0` and `Debug=1` **uncommented**,
overriding whatever the code said — was corrected to match:

`GunAnchorFwd/Right/Up` 30/9/−13 · `GunSignPitch` 1 · `GunSignCombo` 2 · `D3D9ExMode` 1 ·
`Debug` 0 · `PoseLeadFrames` 1 · `AimMethod` 1 · `AimFieldSet` 1 · `TurnMode` 2

**`OcclusionQueryMode` is the one deliberate non-match** — default 2 against the dev ini's 0. Mode 0
forces every query visible and costs roughly twice the draws (1500–1693 vs 737–943 per frame); mode 2
has never been played on for a full session. Its risk is **geometry winking out** at distance or
screen edges, not frame rate. If that appears, `OcclusionQueryMode=0` is the first suspect.

⚠️ **`AutoResX`/`AutoResY` must never be shipped or hand-edited.** They are the mod's own cache of
*this* headset's size. A shipped value injects one machine's resolution into everyone's first launch.

**Two method notes this cost.** A comment describing a defect fixed a day later was quoted as current
a week on, which is why the wrapper default stayed wrong — `HISTORY.md` now carries a ⛔ banner there.
And the `GunSignPitch`/`GunSignCombo` pair was left unmatched on the argument that `GunSigns()`
resolves −1/combo 0 and 1/combo 2 to identical signs. That reading may be correct; shipping on it was
not. Every working run was on 1/2 and every broken one on −1/0, and a code argument was used to
discount a perfect empirical correlation. **Matching a tested value costs nothing when you are right.**

### If you are picking this up cold, read these three things

1. **Almost every hard problem here was a stale premise, not a hard problem.** Parity was recorded
   as "arithmetically unreachable" for a dozen runs because a 2010-era cap was measured once on a
   device that later changed. 120 fps was blocked by a wireless link nobody had timed. The frame
   rate was wrong for thirty runs because SSW was halving it. **Re-test the premise before building
   on it**, especially one you inherited from an earlier run.
2. **"our work" in the frame budget is not a measurement.** It is the residual — everything not yet
   broken out — and it will absorb any amount of someone else's latency while looking exactly like
   your own bug. `xrEndFrame` hid a **189 ms** stall in there for an entire day.
3. **The cheap control first.** Rename `d3d9.dll` to `d3d9.dll.off` and run the game unmodded at the
   same resolution. One minute, and it settles "is this us at all" before any bisection starts.

### Next session, in order

**A. ✅ Runs 181–190 are FLOWN and four fixes are verified.** Details at items 1, 3, 4 and 5.

| fix | state |
|---|---|
| Crosshair jump-spread no longer escapes the hide rule | ✅ **verified** — two frames at `r=0.167` / `r=0.151`, past the old `0.15` box, `8 seen 8 dropped`. The case was genuinely exercised |
| Gun stepping on slow movement (1.9 cm lattice) | ✅ **verified** — integer truncation of the hand offset; now milli-UU |
| Gun pose one full frame stale | ✅ **measured** 8.33 ms → **0.00 ms** with `PoseLeadFrames=1` |
| Muzzle flash + barrel smoke at screen centre | ✅ **suppressed** (`HideMuzzleFx=1`); ⚠️ the real cause is the stereo seam — see item 3 |

**Still owed from run 180, and none of these has had its condition occur yet:**

- **The trigger veto now fails CLOSED at startup.** It used to pass everything through until
  `trigger/touch` resolved, which is the load-in phantom melee and the gun attached to nothing.
  It is a **race**: measured at 2, 0 and 9 unprotected gate-windows across three runs of one
  build, so a clean load-in proves nothing. **Verify by finding a run whose log says
  `NO SENSOR YET (vetoing - startup grace)`.** Both runs since the fix resolved instantly and
  never exercised it.
- **The turn stick now has a narrow `thumbstick/touch` sensor and a per-window census.**
  Measuring only — nothing is vetoed. It was added to catch phantom snaps and instead proved
  they were not snaps at all. Left in because the stick had *no* instrument before.
- **The mesh-latch census** logs the whole foreground list at take and drop. It has not yet
  caught a drop. Needs a long session — every run that dropped the latch was 11,000+ lines,
  every run that did not was under 3,100.

**B. 📋 Audit the remaining raw yaw subtractions.** Same class as the run-180 fix, no evidence yet.
`YawDelta()` exists now. The one at the FRotator write path carries the same false
`int32 wrap is correct` comment and is the first to check.

**C. ⚠️ PARTLY FIXED (run 211) — `g_inCinematic` latched ON permanently.** `SetCinematicMode` is
only ever *observed* with `arg raw 0x00000001`: the run-95 script hook sees native→script entry
points and the cutscene **exit is not one of them**, so a `0` never arrives. Everything gated on
the flag then leaked out of cutscenes, which is why the two cutscene comfort settings were seen
affecting normal gameplay. Both settings gate on it *correctly*; the flag was the bug.

Fixed from the other end rather than by finding the exit event: `RefreshCameraPosition` clears the
flag when the camera **instance** changes, which is an unambiguous level transition. Verified —
the flag now alternates (29 OFF / 11 ON / 51 OFF / 2 ON / 28 OFF in one run) instead of sticking,
all `view drift` lines read `cine off`, and `engine-vs-asked residual` is back to 0.10–1.03° from
a 3.5° mean with a standing 21.5° drift. User-confirmed: turning behaves after a reload.

**⚠️ Still not a true fix.** A cutscene that ends *without* a level change keeps the flag set
until the next transition. The exit event remains invisible.

The tabled **menu rotation bug** is still open and was NOT this — repro: free/follows → enter game
→ pause → free/locked → exit to main menu → start game → menu is rotated. Do not confuse it with
the run-211 menu *double vision*, which was a different bug and is fixed (see D).

**D. ✅ FIXED and VERIFIED (run 211) — menu double vision after returning from gameplay.**
Readability is not liveness. A level transition leaves the previous level's `RvPlayerCamera` in
`GObjects` with its memory mapped and its class pointer intact, so both tests `FindCamera` applied
— `Readable` plus a class compare — passed on a **dead** object; the walk took the first
non-archetype match in `GObjects` order, so the stale camera could win and then be revalidated
forever.

Diagnosed entirely from `view_matrix.log`, no new instrument needed: the post-transition main menu
reported `identification: 36 of 120 frames lost the matrix entirely; best rejected dotFwd +0.6964
(gate 0.80)`. dotFwd 0.696 is 45.9°, and the camera being read reported yaw **+44.5°** while the
matrix carried ~0° — two different cameras. Every rejection invalidates the register slot, so the
menu ran at `split 0, mono-fallback 399`: every draw unsplit across the full side-by-side frame,
i.e. one image with each eye seeing a different half. The boot menu was fine only because just one
camera existed at that point, which is what made this look like a regression.

`FindCamera` now **prefers a live instance and falls back to the first match**, so it can never
return less than before — a false negative from `IsLive` costs a periodic re-walk, not the camera.
The liveness recheck is throttled to every 15 frames (~0.125 s), because the condition only arises
at a transition. After: `split 334 (334 with parallax), mono-fallback 0` at 120 fps in the same
menu, and no window in the run carries the broken `peak live in a frame 0` signature except four
splash-screen windows that read `registers tracked: none` — before any camera exists.

**Known residue:** three windows still show heavy mono-fallback (549, 1340, 4386) at 38/29/37 fps,
all landing exactly on load/transition moments while the camera is being replaced. Expected, but
recorded rather than filtered out, because a long enough transient reads as a visible flash.

0. **📋 The BACKSPACE keyboard exemption can now go** — `VR MODE` is on the panel as of run 159, so the last reason to keep it live under `Debug=0` is gone.
   Run 157's `[Render] Debug` makes every keyboard key inert except **BACKSPACE** (VR mode on/off)
   and **PAUSE** (hold to quit, which is the `WM_CLOSE` path that lets the engine save — and every
   copy of this game shares one save profile). Both are marked at their call sites as the only
   places a raw `GetAsyncKeyState` is legitimate. A panel row retires the BACKSPACE one; PAUSE
   probably has to stay, since quitting cannot depend on the thing you are quitting.

1. **✅ FIXED and VERIFIED (run 181) — the crosshair's JUMP SPREAD escaped the hide rule.**
   Confirmed by measurement, not by looking: `8 stride-8 elements, 8 dropped` throughout, with two
   frames reaching `r=0.167` and `r=0.151` — both **past** the old `0.15` box, so the spread really did
   exceed it and the case really was exercised. Not a false pass.
   Reported: with the crosshair hidden, **two** crosshair lines come back while the player is
   jumping. Spread pushes the four ticks outward and the test was a **square** box — `|cx| < 0.15`
   and `|cy| < 0.15` — in a space where x and y are not the same scale.
   - Clip x spans the frame's full **width** and clip y its full **height**, so one tick at a given
     *pixel* radius reads 16/9 = **1.8× larger in y**. That ratio was already sitting in the
     measurement and was read straight past: `0.045 / 0.025 = 1.8`. The vertical pair therefore
     leaves a square box **first and alone** — two lines, which is the report down to the count.
   - The fix uses the part of the measurement the box discarded: **each tick sits ON a centre
     axis.** `LooksLikeCrosshair` now tests "on an axis, within reach of centre" (`kXhairAxis`
     0.006, `kXhairReach` 0.60) — *tighter* across the axis than the old box, looser along it,
     which is the only direction spread can move a tick.
   - **Verify with the census, not by looking.** `crosshair census: N stride-8 element(s) this
     frame, M dropped, furthest at r=…` prints from Present on change. **Resting truth is 8 seen,
     8 dropped** (four ticks, each drawn twice). *Seen above dropped is the bug*, in whatever form
     it returns. **The false pass: not jumping hard enough to spread the reticle at all** — a run
     with no line above `r=0.045` never exercised this.
   - **⚠️ STATUS carried a stale version of this item for twenty-odd runs**, and so did the comment
     at `g_hideCrosshair`: both said mode 1 *flickered* because `g_handHeld[1]` is a capacitive
     sensor, and that `HideCrosshair=2` was "the setting that behaves". **Run 158 replaced that
     signal with AIM METHOD** — deterministic, no flicker — and neither note was updated. Modes
     0, 1 and 2 are all sound. Both texts are corrected; the code comment keeps the warning.
   - `g_handHeld` is still not to be trusted if anything else ever wants it. Run 155: `g_handHeld[0]`
     read `on desk` through a whole session including ninety seconds of deliberately holding both
     controllers, one sample moving at **1331 µm/frame**. Unreliable in *both* directions, and it
     has no per-frame consumers left.
   - **The crosshair signature, measured** (bare view, nothing else on screen): four ticks around
     centre, each drawn twice, `stride=8` at `(±0.025, 0)` and `(0, ±0.045)`. **Stride 8 is unique
     to it** — everything else is stride 4, 12 or 20, and centred *prompts* are stride 20 at
     `y = -0.138`. Deliberately NOT keyed on primitive count: 10 is what these ticks happen to be,
     and another weapon's reticle will differ. Re-measure if a later weapon's crosshair persists.
2. **📋 BETA: the HUD sits too close to the edge to see in the headset.** Health and ammo duplicate
   correctly per eye since run 151, but they are anchored to the corners of a 16:9 frame and a
   headset cannot see its own corners — the eye's usable area is a circle inside that rectangle.
   They need pulling inward toward the centre.
   - The mechanism is already in hand: the UI's stage-to-clip matrix is `c5`–`c8`, and
     `HudStereoPair` already rewrites it per eye. Insetting is a scale-about-centre on the same
     registers rather than a new system.
   - Wants to be a panel setting (HUD inset / safe area), not a constant — how far in is
     comfortable depends on the headset's optics and the player's IPD.
3. **⭐ The muzzle flash and barrel smoke draw on the STEREO SEAM — and it is OUR bug, not the
   engine's.** Suppressed for now: `MUZZLE FX` on the panel's DISPLAY page, `HideMuzzleFx=1`, default
   hidden. The real fix is queued and should retire that setting entirely.
   - **They are ONE particle system.** `RvGame.u` lists no smoke property at all — the barrel smoke is
     an emitter inside the muzzle-flash asset. Reported as two separate bugs for sixty runs; nulling
     `mMuzzleFlash1stPerson` plus the looping and ironsights variants kills both.
   - **The diagnosis came from the flat monitor and it is decisive.** The effect sits at the dead centre
     of the *monitor*, on the seam between the two eye images. World geometry goes through `StereoPair`
     and is drawn **twice**, once per scissored half — so a merely mis-placed effect would appear
     **twice**, once near each gun. Seeing **one**, straddling the middle, means its clip position is
     `(0,0)` in the *full side-by-side frame*: each eye's scissor keeps the half that falls in its
     region, and the halves meet at the seam.
   - **Therefore the aim method cannot matter**, because the effect never receives a per-eye position at
     all. Confirmed identical in HEAD and MOTION CONTROLLER. That is why the setting is binary — an AUTO
     that cannot differ from ALWAYS is a setting lying about having a reason to exist.
   - **📋 TABLED at run 194, with the draw identified.** Four facts are nailed down and should not be
     re-derived; the remaining step is small but my instruments kept eating the runs.
     | fact | how |
     |---|---|
     | The draw is **`DrawIndexedPrimitiveUP`, stride 72**, ~16–18 prims, ~7 per shot | run 192's baseline-vs-fire census; stride 72 appears in every fire window and never in baseline |
     | It **is** already split per eye | same census — so the split works and only the transform is unremapped |
     | Its vertices are **world-space**, not pre-transformed screen space (box `x[260..860]`) | so it is not a screen-space quad, and the Bink-quad fix does not apply |
     | It has a **vertex shader**, and **`c5..c8` is its local→world matrix** (w column `(0,0,0,1)`; translation drifts frame to frame as the particle moves) | run 193's per-draw write-mask dump |
     | Pass-throughs are **104/frame in baseline and in fire alike** | firing adds no fullscreen-quad pass-through, so the quad classifier is not involved |
   - **What is still unknown:** which register carries the draw's *clip* transform. `c5..c8` is affine,
     so something else projects it. Registers written just before the draw are
     **`c5`–`c12`, `c15`–`c18`, `c20`–`c25`**. A view matrix (unit axes + translation) is visible around
     **`c26`** in the `c24..c27` dump.
   - **🛑 The stride-72 identification is UNVALIDATED and probably wrong (run 196).** Its shader's own
     bytecode says it **reads `c0`** — and `c0` was confirmed locked, valid and remapped at that very
     draw. So that draw's transform *is* eye-remapped and it cannot be what lands on the seam. Run 192
     was right that stride 72 is *shot geometry*; the error was reading "unique to firing" as "is the
     muzzle flash". A tracer, a casing or an impact effect all appear only when firing and all render
     correctly.
   - **🛑 CONFIRMED WRONG (run 197). `MuzzleFxDropStride=72` with the flash visible: the artefact
     REMAINED.** Stride 72 was never the muzzle flash — it is some other shot geometry (tracer, casing
     or impact). **Runs 193–196 measured the wrong draw from beginning to end.** The `c0` reading, the
     write masks, the block dumps and the bytecode all describe a draw that was never the artefact.
   - **▶ NEXT STEP if resumed: identify by SHADER, not by primitive count.** The flash is on an
     **indexed** path (`DrawIdxPrim` rose ~1600 during fire — the observation dismissed in run 192 as
     confounded, and the only surviving lead). Indexed draws cannot be told apart by primitive count —
     that is exactly what saturated run 192's table with twenty rows of zeros — so the handle has to be
     something else, and the shader pointer is the natural one:
     1. Record the **set of vertex shaders** used in a baseline window and in a fire window. The ones
        unique to firing are the shot's own. `GetVertexShader` returns a comparable pointer, and run 196
        already built a bytecode reader for whatever it finds.
     2. Add an ini key to **drop draws using shader N**, then walk N until the artefact disappears.
        That is identification *by construction* rather than by inference — one build, then cheap
        relaunches, instead of a run per instrument.
   - **Do not skip step 2 for step 1's convenience.** That is precisely the mistake of runs 193–196:
     a plausible label taken as identified, then three instruments built on it.
   - **⛔ Confirm what a thing IS before measuring its properties.** Three instruments (runs 193–196)
     were built on an identification nothing had tested. The bytecode dump was the first measurement
     capable of contradicting it, and did — after three runs. The one-line drop test was available the
     whole time.
   - **⚠️ A cheaper hypothesis to check FIRST, and it costs one log line:** the flash may ride the
     scene's own `c0` and simply arrive when **no `g_camMats` slot is valid** — `g_camMatrixBound` is
     cleared whenever a shadow or post pass takes the tracked register, and `InvalidateCamMats()` fires
     on partial overlaps. A duplicated draw with nothing to remap lands identically in both halves,
     which is exactly this artefact. **Log how many slots are valid at a stride-72 draw before hunting
     any further for a register.**
   - **⛔ Four runs were lost to my own instruments, not to the problem.** Recorded because the shape
     repeats: run 183's owner cap filled with archetypes before reaching the live weapon; run 192's
     table saturated on indexed draws; run 193 `break`'d after the first constant block; run 194 added a
     4-alignment filter that **excluded `c5`, the block already confirmed**. Every one was a bound
     chosen for tidiness that silently excluded the answer. **Shader constants have no alignment
     requirement — a compiler packs them wherever they fit.** When the question is "which of these is
     it", do not cap the list at one.
   - ⛔ **Runs 182–188 chased a phantom.** They assumed the effect was correctly drawn at a wrong
     *engine* position and went after moving the weapon mesh in the engine. The engine position may have
     been right all along. What survives is in `ENGINE_NOTES.md` and is genuinely useful: the UObject
     property-offset layout, and the measured negative that writing a component `Translation` does
     nothing without a reattach.
   - ⛔ **An earlier premise named the wrong effect**, and this item chased *"find what the smoke rides
     on that the flash does not"* for sixty-odd runs. The aligned smoke was the **impact** smoke off the
     wall: spawned in world space at the trace hit point, which route 2 already rotates, so it is right
     for free. Two grey puffs appear near your aim within a second of each other — one on the wall, one
     on the gun. Easy to conflate in a headset, expensive to write down, because it then reads as a
     measurement.

4. **✅ FIXED (run 190) — the gun no longer moves in STEPS. The cause was integer truncation.**
   Reported first as jitter, then **redescribed as "moving in steps, like a deadzone — if I move the
   controller very slowly I can see it jumping."** That redescription is what solved it: latency shifts
   smooth motion in time and **cannot** turn it into stairs.
   - `g_handOffX/Y/Z` held the hand-minus-head offset in **whole unreal units**. Eye separation is a
     measured 3.32 UU = 6.3 cm, so **1 UU ≈ 1.9 cm** — the gun could only ever sit on a 1.9 cm lattice.
     `(LONG)` also **truncates toward zero**, so the step was asymmetric about the origin.
   - Now milli-UU with `lroundf`: **19 µm**, far below the controller's own noise floor. Both read sites
     divide by the same constant, including the on-screen anchor marker — run 174's trap was a readout
     in different units from the thing it described.
   - **The rotation was never involved and was deliberately left alone.** `g_handDev*UU` are engine
     rotation units at 65536/turn = 0.0055° per step. Fixing both would have hidden which one mattered.
   - **Method note, and it is the useful part.** A *confirmed measurement pointed the wrong way*.
     `pose lead` correctly read 8.33 ms of staleness; acting on that alone would have produced a small
     improvement and a false "fixed". **The redescription ruled out the entire latency class by
     itself.** When a symptom is described as *stepping*, suspect quantisation before timing, whatever
     the instruments say.

5. **✅ The gun's pose was ALSO a full frame stale — fixed separately (`PoseLeadFrames=1`).**
   Measured **8.33 ms**, exactly one display period at 120 Hz, then **0.00 ms** with the fix.
   - **Why the head hides it:** the head is submitted as the projection layer's pose and the compositor
     **reprojects** the image at scan-out, correcting late error in hardware. The gun is baked into
     **geometry** by `BuildGunC`, and reprojection cannot move a vertex. D3D9 renders *then* Presents,
     so the pose sampled at Present N is consumed by frame N+1's draws — one period late.
   - The fix is one term: locate the hand spaces at `predictedDisplayTime + predictedDisplayPeriod`. The
     period comes from `xrWaitFrame` rather than a hardcoded 8.33 ms, so it is correct at 72 and 90 Hz.
   - ⚠️ **The head's layer pose is deliberately NOT shifted.** It is already correct, and moving it would
     hand the compositor a pose for a frame it is not displaying.
   - ⚠️ **`0.00 ms` is bookkeeping, not tracking quality.** With pacing locked the metric is
     arithmetically exact and is measuring its own arithmetic. It proves the timestamp we ask for matches
     the frame the geometry lands in; it says nothing about whether the runtime's *prediction* at that
     timestamp is good. The remaining risk is **overshoot on sharp reversals**, since prediction error
     grows with lead time — judged in the headset, not in the log.
6. **✅ WORLD SCALE is adjustable (run 201).** Panel → `DISPLAY` → `WORLD SCALE`, live, 5% steps,
   50–200%, saved as `WorldScalePct`. Above 100% the world feels **bigger**.
   - **One constant governs scale and that is why one knob is correct**: `kMetresToUU = 52.5`, which is
     not a guess — UE3's documented 16 units per foot gives 52.5 UU/m exactly. Everything real-world
     flows through `MetresToUU()`: the **stereo baseline** (apparent size), the **6-DOF translation**
     (distance travelled per step), and the **hand position** the gun rides.
   - ⚠️ **All three must scale together.** Scaling the IPD alone changes how big things look but not how
     far you walk, which reads as the world resizing *as you move* — the exact complaint the slider
     exists to fix. Scaling head but not hand slides the gun around your body.
   - ⚠️ **It cannot change EYE HEIGHT** — the game owns that. A player who feels the wrong height is
     describing a different problem from one who feels the wrong scale.

7. **✅ FIXED (run 201) — the gun was anchored to the ENGINE's eye, not to yours.** Reported as *"when I
   recentered, the gun came closer to me."*
   - 6-DOF folds `g_hmdOffset` into the **camera matrix**, so you view from `engine eye + g_hmdOffset`.
     But `BuildGunC` applied `H = (hand − head)` relative to the origin of translated-world space — the
     **engine's** eye — which never received that displacement. The gun's apparent position was therefore
     wrong by however far you had drifted from the recentre point, and recentring zeroed the drift so it
     jumped. Direction depended on which way you had moved, which is why it was not obviously diagnostic.
   - Fixed by adding `g_hmdOffset` to `H`. Applied unconditionally, including with position-following
     off: the gun should stay glued to the view exactly as the flat game's does.
   - `GunFollowsHeadOffset=0` reverts without a rebuild — the *sign* of a position fix is something this
     project has had to flip more than once.
   - **Method note: no instrument in the session would have caught this.** The eye-height probe read
     `+0.0 UU` right after recentring and looked like a clean pass. A symptom that only exists when two
     things which should share a reference frame don't, needs someone wearing the headset.

8. **📋 Eye height: measured, and probably NOT a problem.** `Pawn.BaseEyeHeight = 70.0` (stable),
   `EyeHeight` 67.4–71.4 with bob and crouch. Offsets in `ENGINE_NOTES.md`, and both are writable if a
   knob is ever wanted.
   - ⚠️ **`BaseEyeHeight` is measured from the actor's Location — the CENTRE of the collision cylinder,
     not the floor.** So 70 UU is not eye height above ground and converting it to 133 cm invites the
     wrong conclusion that the character is child-sized. Log `CollisionHeight` before comparing to a
     human.
   - **Recentre is confirmed working from seated** (offset `+0.0 UU` immediately after). Sitting versus
     standing is handled by recentring, not by a setting: **hold MENU ~1 s**, which re-zeroes yaw, lean
     and height together. If the view ever feels too low or high mid-session, hold MENU before assuming
     a bug.

9. **Shadows are broken when High Quality Decals is ON, and run 139 found a new clue.** With that
   option on, **the hidden arms turn BLACK instead of staying invisible.** So whatever hides the
   first-person meshes is not reaching the pass that HQ Decals enables — the arms are still being
   drawn there, at their original position. That is a much sharper lead than "decals are broken":
   it says the mesh-hiding rides on the locked view-projection register and this pass does not.
   Connects directly to the unidentified **third scene-sized surface** and to `ExtraCamReg` (`vs
   c13`), both of which are passes we do not remap. Workaround remains free: leave High Quality
   Decals off.

> ## ✅ FIXED (run 139): the texture wrapper. `D3D9ExMode=1` is now on par with `0`.
>
> One defect, three symptoms — the soldier's colours, the black dock floor and the vanishing fire.
> `UpdateTexture` copies the source's **dirty regions**, not the source; ours did not cover what
> had been written, so it copied too little and returned `S_OK`. Marking the shadow fully dirty at
> every unlock (`AddDirtyRect(NULL)`, per-face for cubes, `AddDirtyBox` for volumes) fixes it.
>
> ⚠️ **The fix is confirmed. The mechanism is NOT, and the first explanation was wrong.** The
> diagnosis was `D3DLOCK_NO_DIRTY_UPDATE`; the counter added to prove it measured **0 of 6716
> locks**. The engine never passes that flag. Do not repeat that story — it is written up here
> only so nobody re-derives it.
>
> Still true and still worth suspecting if this resurfaces:
> - `AddDirtyRect` was never hooked and never called anywhere in the file.
> - **`e->dirty` is one flag for the whole texture while locks are per-LEVEL**, and `FlushShadow`
>   clears it after any copy — so a texture locked on several levels before any unlock copies once
>   and silently skips the rest. A real defect on inspection, whether or not it was the one biting.
> - `UpdateTexture` clears the source's dirty regions as it copies.
>
> **Cost not yet measured.** `AddDirtyRect(NULL)` marks the whole level, so streaming copies more
> than it needs to. The locked rect is already recorded if it wants narrowing.

> **Why it took five runs, kept because the reasoning error is the useful part.**
> - **Why it looked like culling.** UE3 selects texture mips by projected screen size in *pixels*,
>   so which mips get requested moves with FOV, with resolution and with distance — all three of
>   the levers that looked like rendering knobs. Every one of those uploads goes through the
>   wrapper.
> - **Why the symptoms differ by surface.** Fire is **additively blended**, so an unwritten upload
>   contributes nothing and the effect is *invisible* rather than miscoloured. The deck is opaque,
>   so the same failure renders **black**.
> - **The dismissal that cost the most.** The wrapper was ruled out in run 135 because *"the effect
>   is FOV-dependent and a dropped upload wouldn't care about FOV"*. Mip selection **is**
>   FOV-dependent, so the one fact that felt exculpatory was the one that most implicated it — and
>   the wrapper had already been proven defective on the soldier by then. **An independently
>   confirmed defect should not be dismissed on a one-line argument.**

10. **✅ FIXED (run 199) — the "reload arms" were never reload geometry.** `HideExtraArms=1`. Reload now
   looks natural.
   - ⛔ **Carried as a reload/latching problem from run 122 to run 198, and it was neither.** Reported
     correctly at last: *"they're actually always there during gameplay but hidden under where the player
     could normally see... and they move with the gun, so when I move the controller up they're pretty
     visible. And no, they don't disappear when I reload."*
   - **It is the 390-vertex mesh.** The baseline set is `[5799 gun, 3314 arms, 390]` and run 181's rider
     census reads `vertex counts seen riding: 390`. So it is **in** the baseline — which is why run 198's
     "hide what was not in the baseline" rule could not reach it — and it **rides the gun transform**,
     which is why it tracks the controller.
   - **A defect the mod created**, which is why sixty runs never saw it: the fragment sits below the
     bottom of a 16:9 frame, and VR's much taller field of view plus a gun that follows your hand brings
     it into view.
   - Hidden by **role** — "the baseline mesh that is neither the gun nor the arms" — not as the literal
     390, so another weapon's fragment is covered. ⚠️ It rides the gun, so if a weapon's third baseline
     mesh turns out to be a sight or magazine this would remove it; `HideExtraArms=0` reverts.
   - **Method note:** run 198 fixed a problem nobody had (stray animation meshes) because the label on
     the report was taken as the diagnosis. The user's description contained the answer — *always
     present*, *moves with the gun* — and both facts were already in the logs.

10b. **✅ FIXED and VERIFIED (run 212) — the run-207 role shift came back past `MinFgMeshes`.**
   `VetoQuietGun=1`, `RelatchWrongRoles=1`.
   - **Verified, and the case was genuinely exercised** — this is not a clean-run false pass. In a
     161,376-line playthrough from the beginning, `view_matrix.log` line 17471 logs the veto firing on
     `fg census (at the REFUSED take): 3 entries: [0]=3314 [1]=390 [2]=662` — **the identical signature
     to the Aug 7 failure**, including the same 662 mesh. The pistol then latched correctly at line
     35663, `[5799 3314 390]`. User-confirmed in play.
   - **Reported:** playing **from the very beginning** to the pistol, the arms are visible (low, where
     the flat game never shows them) and **the pistol is invisible**. Loading the save just before the
     pistol works. The differentiator was the diagnosis: the two runs traverse different states.
   - **Cause.** A stray 662-vertex mesh joined the weaponless pass, `n` reached 3, `MinFgMeshes` passed,
     and the arms took the gun role. `MinFgMeshes` is a **floor** — an extra mesh satisfies it as well
     as a weapon does. Named in `view_matrix_2026-08-07_23-11-44.log` line 37863.
   - ⚠️ **662 appears in exactly one census line of an 88,575-line log.** The gate is evaluated only on
     the frame the settle completes, so one frame of a stray prop poisons the session.
   - **Fix:** the gun cannot be a mesh that is drawn when you have no gun. The weaponless pass is
     already detected by the `MinFgMeshes` branch, so record it and refuse the gun role to anything in
     it. Plus a backstop that drops a latch whose gun role is contradicted — see `HISTORY.md`.
   - **Verified against data, not reasoning:** 71 takes across every log kept, **0 of 54 good latches
     affected**, and the one bad take blocked. Also confirmed a no-op on the working save-load path,
     which already records the weaponless set before latching 5799.
   - ⚠️ The run-158d **menu** latch (`[4 251 745 …]`, 8 meshes) is **not** covered and is still open.
   - **✅ Weapon swapping is now measured, and works.** The same run cycled **four** weapons — `5799`,
     `8297`, `11387`, `11389` — through **21 drop/re-take pairs**, every one re-taking the correct gun.
     The old "is 11387 a second weapon or one mesh in two states" question is settled by
     `[0]=11387 … [3]=9` and `[0]=8297 …` coexisting in one session as distinct weapons.
   - **The `RelatchWrongRoles` backstop never fired**, so the `bigger` misfire I flagged as possible did
     not occur across 161k lines and four weapons. It also cannot pre-empt a swap: the normal drop needs
     240 frames and the backstop 600, so the normal drop always wins that race. Confirmed — 21 swaps,
     zero backstop firings.
   - ⚠️ **The ~2 s of hidden gun after a swap is still neither confirmed nor refuted.** The mechanism is
     unchanged and still there in the code (the new weapon is outside the old baseline, so
     `HideStrayArms` hides it until the 240-frame drop), but the drop line marks the *end* of that
     window so the log cannot size it, and the user did not report seeing it across 21 swaps. Open.
   - ⚠️ **NEW, and latent rather than visible: the ARMS role can be misassigned.** Line 156769 latched
     `gun=11389, arms=97` from `[0]=11389 [1]=97 [2]=3314 [3]=390` — slot [1] was a 97-vertex sub-mesh,
     not the real 3314 arms. **No visible defect, by luck**: with `HideExtraArms=1` the real arms are
     hidden anyway as "a baseline mesh that is neither gun nor arms". With `HideExtraArms=0` they would
     be visible. The role assignment "slot [1] is the arms" is the weak part — `3314` is invariant
     across every weapon in every log and is the better identifier. Not fixed; recorded.

11. **✅ The arms reappear for the health injector animation (run 198–199).** `HealArmsMs=2150`,
   `HealArmsDelayMs=100`.
   - The arms are shown **and the gun is returned to the engine's position** for the window — moving the
     gun onto the controller while the arms animate at the engine position tears one animation in half.
   - **Gated on the heal being able to do anything.** Reads `Pawn.Health` / `Pawn.HealthMax` (see
     `ENGINE_NOTES.md`) at the press, so at full health the arms never appear.
   - ⚠️ **Do NOT "verify" by waiting for health to rise.** Health is applied by the animation, possibly
     at its end, so the confirmation arrives *late* and a window opened then runs on past the animation
     it was meant to cover. Asking "could this heal do anything" *before* the press has no timing
     dependency.
   - Both edges are knobs measured from the press, because they were reported separately and move
     independently: shortening the duration to fix a late finish would also have made it start early.
   - **✅ The no-items case is closed too (run 200).** Also reads `RvGameSharedHUD.mHealthPackCount`, so
     the window is skipped when the player has no packs. Both reasons a heal does nothing are now checked.
   - ⚠️ **Chose the HUD's count over `RvGameSharedInventoryManager.mCounts` deliberately.** `mCounts` is
     an array indexed by an `ERvCountItem` enum, so using it means guessing which slot is the health pack.
     `mHealthPackCount` is a single int whose name states what it holds, and a frame of HUD lag cannot
     matter for a check made once at a button press.
   - ⚠️ **Both checks fail OPEN.** If either property cannot be read the window opens anyway and the log
     says so — a *missing* animation is a worse failure than a spurious one.
12. **The TMD**, when the game reaches it. Left-hand device, so it needs the same treatment driven
   by `g_handPoseValid[0]` instead of `[1]`. Everything generalises; it is unverifiable until that
   point in the game, and `Singularity.exe <map>` makes reaching one practical.
13. **Decals — leave alone unless it bothers you.** Five theories are dead (shadows, `G16R16`,
   `ScreenPositionScaleBias`, `vs c13`, unhooked user-pointer draws) and the workaround is free:
   turn **High Quality Decals** off in the in-game video options.

> **RETRACTED (run 136).** This list used to end with *"one object still culls early at distance —
> PAGE DOWN fixes it, which points at the run-21 mechanism: FOV inflation makes objects project
> 3.4× smaller than the engine expects."* The run-21 mechanism is **not** the cause. Two
> measurements kill it: sweeping the engine's asked FOV over 0.35×–6× changed nothing while the
> engine demonstrably took the request, and the draw count is identical at two resolutions that
> look completely different. PAGE DOWN "fixing" it was the coincidence that kept this alive —
> PAGE DOWN changes the forced matrix as well as the ask, and it is the forced matrix that matters.

**Watch this one:** MinHook now initialises in `DllMain` (run 59, for the command-line detour)
rather than at first `Present`. `InstallViewHook` tolerates `MH_ERROR_ALREADY_INITIALIZED`
accordingly — treating it as a failure would silently kill head tracking. It is the newest
structural change in the build and the first thing to suspect if tracking ever misbehaves.

## Measured, so it does not have to be re-derived

| Quantity | Value |
|---|---|
| View matrix | `c0`, **ROW** storage, **translated-world** space |
| Eye separation | 3.32 UU = 6.3 cm (correct IPD), stable |
| Headset wants | **2496×2688 per eye** → 4992 wide for side-by-side |
| Per eye at 2560×1440 | 1280×1440 — 51% of the panel |
| Per eye at 3840×2160 | 1920×2160 — 77% of the panel |
| Resolution cap | **RETRACTED — there isn't one at these sizes.** 4096 ✅ · 4608 ✅ · **4992 ✅** (run 58), with `D3D9ExMode=1`; the scene targets follow every time. Run 33 measured its refusal on the *plain* D3D9 device, before the Ex upgrade |
| Per eye at 4992×2688 | **2496×2688 — 100% of what the headset asks for.** Parity reached in side-by-side; no per-eye targets needed |
| Frame aspect | Keep per-eye ≈ **0.93** (2496/2688). A taller frame at fixed width narrows the horizontal field — 4096×2688 renders 82.5° vs 95° at 4096×2160 — and the shortfall shows as **black bars at the sides** |
| Frame rate | **120.0 fps LOCKED at 4992×2688 (full parity)** — median 8.34 ms, `our work` 3.02 ms, **4.35 ms idle**. Needed both zero-copy (run 39) and a **dedicated router** (run 56). SSW **off**, 120 Hz. Every figure before run 32 was SSW halving the rate and is void |
| Frame copy | **0.00 ms** under zero-copy. Was ~9.8 ms of a ~16 ms frame at 4K on the CPU path |
| XR submit (`xrBeginFrame`+`xrEndFrame`) | **0.08–1.28 ms** on a dedicated router. Was **20.94 ms mean / 189 ms worst** on shared WiFi — and it was pooled into "our work", where it looked like a game-side stall for a whole day (run 56) |
| Engine FOV | horizontal at 16:9, dips to its 65° default ~7% of samples |
| Scene targets | 2× scene-sized (A8R8G8B8 + A16B16G16R16F), shadow map 512×512 R32F |

## Quick start

**The live build is `spikes/view_matrix/`** (spike 9). **`spikes/vr_render/`** (spike 8) is the
known-good fallback if spike 9 misbehaves. Earlier spikes are kept deliberately — several
document approaches that did *not* work, which is most of their value.

```powershell
.\spikes\view_matrix\build.ps1 -Install     # builds x86, copies into the dev game copy
```

Connect **Virtual Desktop** first (VDXR is the only usable 32-bit OpenXR runtime — Meta's own
crashes in `xrCreateSession`, SteamVR has no 32-bit runtime at all), then launch:

```
R:\SingularityVR-Dev\Singularity\Binaries\Singularity.exe
```

**No arguments needed** since run 59 — the resolution is inherited from the headset. Pass `-ResX`
only to override it deliberately.

> ### 🛜 Virtual Desktop needs a dedicated router, and this is not optional
>
> On a shared WiFi network `xrEndFrame` blocked for up to **189 ms**, which read as seconds-long
> stalls and cost an entire session of misdiagnosis. With a dedicated router and the PC on
> **ethernet** to it, XR submit is **0.08–1.28 ms** and the frame locks to 120 fps.
>
> The `XR submit` figure in the frame budget is the check. If it is anything but small, the link is
> the problem and **nothing in the render code will help** — see run 56.

**The cold start is VR** since run 159 — `StartInVr=1` is the default, applied at the first Present
and gated on an OpenXR session existing, so a machine with no headset still comes up flat rather
than frame-split. `VR MODE` is the first panel row and it saves.

**BACKSPACE is now only a toggle**, for peeking at the flat game — it deliberately does *not*
save, because a key you press to look at something should not change how the game launches
tomorrow. Toggling off drops the forced projection with it; a mono image stretched to 98° looks
broken rather than standard, which is expected.

> Run 37 reverted run 36's default-on because it rendered the untested main menu frame-split on
> every launch. Run 159 reinstated it on a checkable change, not a change of mind: **run 151 put
> the HUD and menus into both eyes**, so the thing run 37 objected to is the thing run 151 built.
> `StartInVr=0` restores the flat cold start with no rebuild.

Individual toggles, unchanged: **F9** head tracking · **F10** 6-DOF head position · **F1** true
stereo (draw duplication) · **F12** alternate-eye stereo ·
**F6** VR-correct projection · **F8** recentre · **F5/F4** flip yaw/pitch sign ·
**F7** scan for the view matrix · **F3** constant offset probe · **F11** offset amount ·
**INSERT** blank one eye half (mapping test) · **DELETE** swap eye/half assignment ·
**PAGE UP** cycle culling headroom (now runs *down* from 1.0) · **PAGE DOWN** cycle how much of
the headset's FOV we render · **HOME** scissor off (drawn-then-clipped vs never-drawn) ·
**END** clip out everything driven by the tracked register (remapped vs unremapped) ·
**NUMPAD1** head roll on/off · **NUMPAD2** flip the roll sign ·
**NUMPAD3** gamepad emulation (on by default since run 61) · **NUMPAD4** walk-forward test ·
**NUMPAD5** stick-look test.

Controller settings live in an `[Input]` section of `Binaries\SingularityVR.ini`: `Gamepad`,
`TurnMode` (0 smooth / 1 snap), `TurnAngle`, `TurnSign`, and a 360 button mask per action
(`Jump`, `Crouch`, `Reload`, `Use`, `Menu`, `Sprint`, `LeftX`, `LeftY`, `Melee`). Raven's own
console layout is not documented anywhere, so the masks are a first guess kept editable rather
than a rebuild.

**Do not press F12 after F1.** F1 clears alternate-eye, but F12 does *not* clear duplication, so
that order leaves both stereo modes running at once. F12 before F1 is harmless and pointless.

Mouse and head tracking now **add** rather than fight — turning with the mouse no longer snaps
back, and the same fix is what stick-turning will need in the shipped mod.
Log: `C:\Users\<you>\AppData\Local\SingularityVR\view_matrix.log` — **read it directly from disk**;
it does not need pasting into chat, and it must never be pasted into this file. Truncated at each
attach and stamped with the run's date and time on line 1, so a stale file is obvious at a glance.

**Every run is kept, named for when it started.** The live run is always `view_matrix.log`; at
the next attach it is renamed to `view_matrix_YYYY-MM-DD_HH-MM-SS.log`, the stamp read back out
of its own line-1 header. Newest archive = most recent finished run. Nothing is ever evicted, so
**a reference to a specific log stays valid permanently** — cite them by name in notes.

The old `view_matrix.prevN.log` files are the previous scheme and are left alone; `prevN` named a
*position*, so those names meant something different after every launch. Pruning is manual and
deliberate — nothing deletes logs automatically, including the 24-line ones, which are the
evidence when the question is "why did it die at startup". Uninstall = delete `d3d9.dll`.

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

## VR-correct projection — what it does

Scale could not be judged in the headset because the projection layer was claiming the
**headset's** FOV for an image the game rendered at a different one — a uniform stretch that
made head movement, world scale and object size all wrong together, so none could be measured
against another. Now the submitted frustum is *derived from the matrix* (read, not assumed) and
the game is asked for a vertical FOV matching the headset so that truth also fills the view.
Toggle with **F6**.

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
