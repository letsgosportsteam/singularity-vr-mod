# HISTORY — the run log, newest first

Split out of `STATUS.md` on 2026-08-04. **Everything here was moved verbatim; nothing was
deleted.** `STATUS.md` holds current state and is the file to read first.

## How to read this file

**You do not need to read this cold.** It exists to be searched, for one job: when you are about
to build on a premise you inherited, come here and check what actually measured it. That is the
single most expensive failure mode this project has — see *Method notes* in `STATUS.md`.

Two kinds of content live here, and they carry different risk:

- **Run narrative** — every section is stamped with its run number, so its age is visible and you
  can weigh it yourself. Safe to read as "what was true at run N".
- **Superseded status snapshots** — sections that were written as *current state* and never
  updated, so they read as present-tense fact while being wrong. Each now carries a
  ⛔ **SUPERSEDED** banner naming what overturned it. **Do not quote these as current.** They are
  kept because the runs that produced them are kept, and a claim is easier to weigh next to the
  work that made it.

A grep that lands in this file may well have landed on a retracted conclusion. Check for a
SUPERSEDED banner above your hit, and check the run number, before you act on it.

---

## ⭐ Run 216: the scope reorders the first-person pass, and the roles were positional

**Reported:** after using the sniper scope, the arms are on the controller and the gun is invisible,
and it survives a weapon switch. **Fixed and confirmed.**

The run-207 role shift again, by a route neither run-212 defence covered.

### The scope draws the arms FIRST

Both orders were recorded in the same run-213 census, and read past at the time:

```
normal   [0]=11389 [1]=97   [2]=3314 [3]=390
scoped   [0]=3314  [1]=390  [2]=11389 [3]=97
```

So a re-latch while scoped takes slot[0]=3314 — the arms — as the gun.

### Why it could not recover, which is specific to this route

A latch taken from a scoped pass captures a baseline containing the **real gun**. So `FgIsExtraArm`
hides it — that is the invisible gun — and run 212's recovery, which required a larger mesh **outside
the baseline**, never saw a contradiction. The drop rule meanwhile watches `3314`, which is always
drawn. Permanently stuck, which is what "persisted across weapon switches" means.

### The fix: size, not position

**The gun is the largest mesh in the first-person pass; the arms are the second largest.** True of
every census on disk — `5799/3314/390`, `11387/3314/390/9`, `8297/3314/390`, `11389/97/3314/390`, and
the scoped order above. **Size is invariant under draw order; position is not.** It also closes run
213's `arms=97` misassignment for free.

Plus two supporting changes: no latch is taken from a scoped pass at all (`ScopeActive`'s own signal,
at 500 ms), and the recovery's baseline-membership requirement is dropped — the invariant that matters
is simply that the gun is the largest mesh, which cannot hold for a latch that put the arms in the gun
role, and requiring "outside the baseline" is exactly why it stayed silent.

### Verified, and the case was genuinely exercised

Two takes from the **arms-first** list, both correct:

```
5391  fg census: [0]=3314 [1]=390 [2]=11387 [3]=9    -> gun=11387 arms=3314
9613  fg census: [0]=3314 [1]=390 [2]=11389 [3]=97   -> gun=11389 arms=3314
```

Under the old positional rule both would have been the bug. No `ROLES ARE WRONG` firing was needed —
prevention held. The named false-pass mode (a clean run that never re-latches while scoped) did not
occur.

### Method note

Run 213 recorded the weak rule, named the right replacement, and left it because the symptom was
invisible. Three runs later it produced a symptom that was not. **The census had both draw orders in
it the whole time.**

---

## ⭐ Run 215: the mesh matchers were frame-wide, and a floor plane collided with them

**Reported:** a hole in the floor — water and the basement under the next room visible through it,
including a chest down there. **Fixed and confirmed:** floor intact, fragment still hidden.

### Bisected on ini keys alone — no rebuild until the cause was named

| test | result | rules out |
|---|---|---|
| `d3d9.dll.off` | floor fine | **it IS us** |
| AIM METHOD → HEAD | floor fine | **the MESH path** (`hideMeshIdx = 0`) |
| `OcclusionQueryMode=0` | no effect | culling |
| `D3D9ExMode=0` | no effect | the run-139 texture wrapper |
| `HideStrayArms=0` | no effect | the stray rule |
| `HideExtraArms=0` | **fixed it** | → `FgIsExtraArm` |

### The bug

The matchers key on vertex count with **no positional gate that actually bites**. `fgDrawCount >
kFgSanePass` looks like one — but outside the first-person pass `g_fgDrawCount` is **0**, so the test
reads `0 > 64`, false, and the match proceeds. They matched **anywhere in the frame**.

The weapon's baseline set was `[11387, 3314, 390, 9]`, and `FgIsExtraArm` hides any baseline count that
is neither gun nor arms — so **every 390-vertex and every 9-vertex draw in the whole frame** was
hidden. A large flat floor plane has very few vertices.

**Measured after the fix: ~4 refusals per frame, across the whole run.** This was not a rare
coincidence in one room; it was removing small world meshes constantly, and only became *visible*
where the collided mesh was a floor you could fall through.

### ⚠️ Gating on the foreground pass alone would have been wrong

The obvious fix, and it trades this bug for run 120's. The depth-prime copy of the arms is drawn at
**draws 2–3 at the top of the frame**, and hiding only inside the pass left the prime alive to write a
black silhouette. So the window is *"inside the first-person pass, **or** within the first few draws of
the frame"* — both needs met.

Applied to the **move** path too: a world mesh colliding with the gun's count is transformed onto the
controller, which removes it from where it belongs just as surely as hiding it does.

Checked only on an actual match, so a world draw matching nothing costs exactly what it did before.

### Method notes

- **The cheap control settled it in one step and should have been first.** `CLAUDE.md` says so
  explicitly. Two ini theories were tried ahead of it and both were wrong — `OcclusionQueryMode` and
  `D3D9ExMode` cost a run each. "Is this us at all" is one minute and it collapses the search space.
- **A guard that reads correctly can still never fire.** `fgDrawCount > kFgSanePass` was written as a
  sanity bound and reviewed as one; it is inert precisely where it was needed, because the counter it
  reads is zero outside the pass. **Check what a guard evaluates to in the case you are not thinking
  about**, not only in the one that motivated it.
- **A symptom noticed late is usually a condition met late.** The collision was pervasive from the
  start; the room only made it visible.

---

## Run 214: the melee swing was invisible for the same reason the heal was

**Reported and fixed, confirmed in play.** `MeleeArmsMs=700`, `MeleeArmsDelayMs=0`.

The animation is performed by the arms this mod hides, so hiding them left the player watching
nothing happen — run 198's finding, arriving on a second animation.

Deliberately the **same mechanism**, not a variation: the window shows the arms **and returns the gun
to the engine's position** for its duration. Run 198 established that moving the gun onto the
controller while the arms animate at the engine position tears one animation in half, and a swing has
exactly that problem too.

The three sites that gated on `HealWindowActive()` now ask `ArmsAnimActive()`, so a third animation is
one function to extend rather than three sites to remember. Both windows keep the run-203 cheap-gate
shape — two plain aligned 32-bit reads in the common case, no 64-bit interlocked value on the hot
path, which run 203 measured as a two-thread fight over one cache line.

Armed on the **rising edge** of the button the mod injects itself, as the heal is: no inference to get
wrong, and a held button cannot re-arm every frame. The button row is resolved **by name** once at ini
load rather than matched by mask per frame, because the mask is rebindable from that same loop.

Verified in the log rather than by eye: **12 windows, one per press**, none double-armed.

### ⚠️ The impulse weapon shares this button, and that is deliberately not guessed at

Once that weapon exists the same press fires a weapon rather than swinging a fist, and the window
becomes a misfire. Rather than invent a discriminator for a weapon nobody has seen — the failure mode
that cost run 213 four fixes — **every window logs the latched weapon with it**:

```
melee window: arms shown from +0 ms to +700 ms after the press (gun=11387)
```

So when the impulse weapon does exist, one ordinary run says whether the two cases can be told apart
at all, instead of that being its own diagnostic run. `MeleeArmsMs=0` disables it without a rebuild.

✅ **700 ms is CONFIRMED — user-reported as "the timing was perfect", first flight, no tuning.** It was
chosen as a guess and is recorded here as measured. `HealArmsMs` needed a round of tuning after its
first flight and this did not, so the two are not the same kind of number any more. It stays an ini
value because a different weapon's swing may not be 700 ms, not because this one is unverified.

---

## ⭐ Run 213: the sniper scope — four wrong fixes, and the shader had the answer all along

**Reported:** the sniper scope is not shown in each eye. It comes up on the left trigger (alt-fire).
**Fixed and confirmed in play.** Whole scope in each eye, world once, circle round, no streaking.

### What it was

The overlay is `DrawPrimitive, triangle strip, 2 primitives` — a fullscreen quad from a bound vertex
buffer. `StereoPair` duplicates it and scissors each copy to a half, then remaps the **camera
registers** — and this quad is already in clip space and reads no camera register, so the remap is a
no-op on it. Both copies landed at identical full-frame coordinates and each was clipped to its own
half: one circle across the seam. It also explains why the HUD was always fine — `HudStereoPair`
transforms `c5`–`c8`, which the HUD shader does read.

**The fix is three parts, and all three were needed:**

| part | what it does |
|---|---|
| per-eye **viewport** | NDC maps onto the viewport rect, so pointing it at one half puts the whole quad inside that half. No vertex data needed — which matters, because this draw has no vertex pointer to rewrite |
| `c1` **ScreenPositionScaleBias**, X only | the scene read derives from the projected position, not the geometry, so it kept spanning both eyes until the UV was mapped into a half |
| `c0` **ScreenAspectRatio**, halved | each eye's half is 1280×1440, not 2560×1440, so the roundness correction has to halve with it |

### 💥 Four candidate fixes were written on inference. All four were wrong.

1. **stride 16, 2 prims, spans NDC.** Appeared in every scope window and no baseline, so it was picked
   on correlation alone. Drawing it per eye fired **120 times a second — one per frame, the gate was
   right** — and the picture did not change. *A per-frame draw that only happens while scoped is not
   necessarily the thing you can see.*
2. **the `PASSED THROUGH BUT DOES NOT SPAN NDC` verdict.** Written for the muzzle flash, a small
   centred sprite. It flagged two stride-12 rows with degenerate boxes and stayed silent on the real
   overlay, which is passed through *because* it spans NDC. *A verdict line is only as good as the case
   it was written for.*
3. **"new since the baseline".** 32 rows, every one a count of **one**, none of them the scope — and
   its baseline was taken with a **different weapon** (`11387` against `11389`). Novelty was the wrong
   filter; **persistence** is what defines an overlay, and the count column was already in the table.
4. **scaling `c1.x` and `c1.z`.** Right register, wrong component — see below.

### ⭐ The one that actually paid: read the shader

The pixel shader is 600 DWORDs and carries its own **CTAB symbol table**:

```
c0 ScreenAspectRatio   c1 ScreenPositionScaleBias   c3 ScreenResolution
s0 BlurredSceneColorTexture   s1 SceneColorTexture   s2-s5 Texture2D_0..3
dcl_texcoord0 v0    dcl_texcoord5 v1    (no vPos anywhere)
20  mad   r1.xy, r1.yzzw, c1, c1.wzzw
32  texld r3, v0, s3        <- the scope ART reads TEXCOORD0
```

Two facts, neither obtainable any other way. The scene reads derive from `v1` (projected position)
while the art reads `v0`, so **they are separable** — the run-213f worry that the aliased
`TEXCOORD0`–`3` made this unfixable was unfounded. And the bias swizzle is `.wzzw`:

```
u = (v1.x/w) * c1.x + c1.w      <- X bias is .w
v = (v1.y/w) * c1.y + c1.z      <- Y bias is .z
```

Run 213g scaled `c1.x` (right) and biased `c1.z`, the **Y** bias. That one mistake produced **both**
reported symptoms at once: X kept its untouched `0.5002` bias against a halved `0.25` scale and so
sampled `[0.2502, 0.7502]` — the middle band, still straddling the seam, which is why the doubling
never went away — while the mangled Y bias smeared the image **vertically**, which was the new
streaking.

### Method notes this run earned

- **When a fix depends on how something computes a value, read the thing that computes it.** The
  shader was one `GetFunction` call away for the whole investigation and it had a symbol table.
- **The drop test is not optional.** `MuzzleFxDropStride` was proposed, then skipped as "a whole run
  to confirm one bit" — and that bit was the one worth buying. It could not reach `DrawPrimitive`
  anyway (it keys on a vertex stride and that path has none), which is *why* the candidate hid there.
  Building `DropPrimCount`/`DropPrimType` first, and only then the fix, is what turned a fourth guess
  into an answer.
- **A census that lists only some draw paths will name the wrong candidate rather than none.** Three
  censuses listed only the user-pointer hooks; of the handful of things they could see, stride 16 won
  by default rather than on merit.
- **A slow-motion lead closed negatively, and that was worth knowing.** Eight dumps spanning both
  states were byte-identical — same shader, same constants, same textures — so whatever reads as
  different in slow motion is not this draw, and no constant-level fix could be keyed to it.

### Also shipped: the shot follows the head while scoped

`ScopeHeadAim=1`. Aim mode 11 rotates the fire trace by the hand-minus-head deviation, which is right
when you point a gun with your hand and wrong when you are looking through a reticle drawn to the
view. Gated on **the scope overlay actually being drawn** rather than on the alt trigger — the trigger
is alt-fire generally and means something different on every other weapon, whereas the overlay quad is
the scope by measurement. Applied at **both** trace sites.

✅ **Confirmed in play:** *"the aiming is accurate with the scope on, based on reticle rather than
controller pose."* Both trace sites agree, which is what the "apply it at both or neither" note was
protecting — a shot decided by the other line check would have followed the hand and read as
intermittent inaccuracy rather than a clean miss.

---

## ⭐ Run 212: the run-207 role shift came back, past the gate that was built to stop it

**Reported, and the differentiator was the whole diagnosis:** playing **from the very beginning** to
the pistol, the arms are visible (low, where the flat game would never show them) and **the pistol is
invisible**. Loading the save immediately before the pistol and picking it up works correctly — no
arms, gun on the controller.

### The bug is run 207's, and `MinFgMeshes` did not close it

Diagnosed entirely from a log already on disk, `view_matrix_2026-08-07_23-11-44.log` (88,575 lines,
the from-the-beginning run):

```
line 16550  mesh latch: the list has settled on 3314/390 but the pass holds only 2 mesh(es) ... not latching
line 21546  mesh latch: gun=11387 verts, arms=3314 verts, baseline set of 4
line 27730  mesh latch: gun=11387 has not been drawn for 240 frames - dropping the latch
line 27731  fg census (at the DROP): 2 entries: [0]=3314 [1]=390          <- weaponless
line 37863  mesh latch: gun=3314 verts, arms=390 verts, baseline set of 3
line 37864  fg census (at the TAKE): 3 entries: [0]=3314 [1]=390 [2]=662  <- a stray third mesh
```

A **662-vertex mesh** joined the weaponless pass. `n` reached 3, `MinFgMeshes` passed, and the roles
shifted by one exactly as in run 207: the **arms** took the gun role and were moved onto the
controller, the fragment took the arms role, and the baseline set described a weaponless pass — so
when the pistol arrived, `5799` was not in it and `HideStrayArms` **hid the weapon**.

**`MinFgMeshes` is a FLOOR.** The run-209 note argued a wrong count "fails SAFELY"; that is true of a
count set too high and false of one defeated from below. An extra mesh satisfies it as well as a
weapon does.

⚠️ **662 appears in exactly one census line in the whole 9 MB log.** The gate is evaluated only on the
frame `steady` reaches `kLatchSteady`, so a **single frame** of a stray prop poisons the session.

### Why the save-load run was fine

Loading straight into the save before the pistol never presents a 3-mesh weaponless pass, so the gate
holds and the first latch happens with the pistol genuinely in hand. Playing from the start walks
through whatever draws the 662 mesh. **The two runs differ in the states traversed, not in the code.**

### Sticky, but not permanent — and the distinction was measured

The drop rule watches the latched gun, which in the shifted state is the arms, and the arms are drawn
whenever there is a first-person pass. It clears only where the pass is **non-empty and lacks them**:
`view_matrix_2026-08-04_21-45-47.log` recovered that way at its line 16303, via menu geometry. Four
level transitions in the Aug 7 run did **not** clear it, because empty frames are not counted as
misses (`n == 0` returns early). So in practice it lasted the rest of the session.

### The fix: the gun cannot be a mesh that is drawn when you have no gun

`VetoQuietGun=1`. The weaponless pass is already detected and logged by the `MinFgMeshes` branch, so
record what it holds and refuse the **gun role** to anything in it. Replaced wholesale, never
accumulated — an accumulating set would carry menu geometry into gameplay forever, and a count wrongly
in it is a weapon that can never latch.

**Checked against every latch in every log kept — 71 takes:**

| census at the take | takes | verdict |
|---|---|---|
| `[5799 3314 390]` | 52 | 5799 not in the set → unaffected |
| `[11387 3314 390 9]` | 2 | 11387 not in the set → unaffected |
| `[3314 390]` | 15 | blocked (`MinFgMeshes` already blocks these) |
| `[3314 390 662]` | 1 | **blocked — this is the reported bug** |
| `[4 251 745 …]` | 1 | menu geometry; 4 is not in the set → **still not caught** |

**0 false positives on 54 good latches.** The last row is run 158d's menu latch — a separate, still-open
hole, and this does not claim to close it.

### And a backstop, because the veto has a precondition

`RelatchWrongRoles=1`. The veto needs a weaponless pass to have settled first; a session poisoned
before one does gets the old behaviour. So the latch also drops on either of two contradictions, after
`kWrongRoleFrames` (600 — 5 s at 120 fps, deliberately far longer than a ~2 s reload or the 2.15 s
heal window, because "a mesh outside the baseline" is also what a reload looks like):

- **quiet** — the gun role is held by a mesh seen drawn with no weapon in hand. Cannot be true of a
  good latch. Needs the weaponless observation.
- **bigger** — a non-baseline mesh is larger than the latched gun. In all 54 good censuses the weapon
  is the largest mesh in the pass (`5799 > 3314 > 390`, `11387 > 3314 > 390 > 9`). Needs nothing
  observed in advance, which is the point.

⚠️ **`bigger` can in principle fire on a good latch** — a scope, the TMD, a large first-person prop
that outlasts the window. The misfire is **benign and self-healing**: the drop is followed immediately
by a re-take whose first list entry is the gun, so it costs ~0.25 s of the gun at the engine position
against a session that otherwise stays broken. `RelatchWrongRoles=0` reverts.

### ✅ VERIFIED, and the case was genuinely exercised

161,376 lines, played from the very beginning. `view_matrix.log` line 17471:

```
*** mesh latch: REFUSING this latch. The list settled on 3314/390 across 3 mesh(es), but 3314
was drawn in the weaponless pass - so it is the ARMS, not a gun ... ***
fg census (at the REFUSED take): 3 entries: [0]=3314 [1]=390 [2]=662
```

**The identical signature to the Aug 7 failure, down to the same 662 mesh.** The pistol then latched
correctly at line 35663 as `[5799 3314 390]`. This was the outcome I said was *less* likely than a
clean-but-inconclusive run — the false-pass mode was named in advance and did not occur.

### ✅ And weapon swapping is measured now — it works

The same run cycled **four** weapons — `5799`, `8297`, `11387`, `11389` — through **21 drop/re-take
pairs**, every one re-taking the correct gun. So:

- The "is 11387 a second weapon, or one mesh in two states?" question from run 158d's diagnostic is
  **settled by ordinary play**: `8297` and `11387` are distinct weapons appearing in one session.
  `CheckFgCoexist` never fired and never could — only the weapon in your hands is drawn.
- **The `RelatchWrongRoles` backstop never fired.** The `bigger` misfire flagged as possible did not
  occur. It also cannot pre-empt a swap by construction: the normal drop needs 240 frames and the
  backstop 600, so the normal drop always wins that race — 21 swaps, zero backstop firings.

### Still open, and not papered over

- **The ~2 s of hidden gun after a swap is neither confirmed nor refuted.** The mechanism is still in
  the code — the new weapon is outside the old baseline, so `HideStrayArms` hides it until the
  240-frame drop — but the drop line marks the **end** of that window, so the log cannot size it, and
  it was not reported across 21 swaps. Do not record this as "fine"; it is unmeasured.
- **⛔ SUPERSEDED by run 216 — and the reason it was left alone is the lesson.** This recorded
  `gun=11389, arms=97` as latent, named the cause correctly ("slot [1] is the arms" is the weak rule),
  identified the right replacement (`3314` is invariant across every weapon in every log) — and then
  declined to fix it, because it changed nothing visible in the shipping configuration.
  It changed something visible three runs later: the same positional rule put the **arms** in the gun
  role after scope use, and that one was neither latent nor recoverable. **A known-weak rule left alone
  because its current symptom is invisible is a bug waiting for a louder symptom.**
- The fix remains designed against **one** logged instance of the original failure, now confirmed by a
  second. Both knobs stay ini toggles.
- The run-158d **menu** latch (`[4 251 745 …]`) is untouched and still open.

---

## ⭐ Run 211: menu double vision and broken controls were one bug — a dead camera

Reported after a quit-to-menu-and-reload: **double vision in the main menu**, making it unusable,
then **controls and culling wrong** back in the same save. The user explicitly separated the first
from the long-tabled *menu rotation* bug, and that mattered — rotation and double vision are
different mechanisms, and treating them as one would have sent this at the rotation repro.

**Diagnosed with no new instrument.** Everything needed was already in `view_matrix.log`:

| | working boot menu | post-gameplay menu |
|---|---|---|
| split / mono | `split 332 (332 with parallax), mono-fallback 0` | `split 0, mono-fallback 399` |
| register slot | `c0 (peak live in a frame 1)` | `c0(stale) (peak live in a frame 0)` |

100% mono fallback means every draw goes out **unsplit across the full side-by-side frame**, so
each eye sees a different half. That is double vision — and note it presents as *wrong* content,
not missing content, which is why it did not look like a stereo failure.

The cause was named by a line that was already being logged: `identification: 36 of 120 frames lost
the matrix entirely; best rejected dotFwd +0.6964 (gate 0.80)`. dotFwd 0.696 is 45.9°, and the
camera being read reported yaw **+44.5°** while the matrix carried ~0°. Two different cameras.

**Readability is not liveness.** A level transition leaves the previous level's `RvPlayerCamera` in
`GObjects` with its memory mapped and its class pointer intact, so both tests `FindCamera` applied
passed on a dead object, and the walk took the first non-archetype match in `GObjects` order. The
boot menu worked only because a single camera existed then — which is exactly what made this look
like a regression from the day's other work, and it was not.

Two fixes, both the same root condition:

- `FindCamera` **prefers a live instance, falling back to the first match**, so it can never return
  less than before; an `IsLive` false negative costs a re-walk, not the camera. Liveness rechecked
  every 15 frames (~0.125 s) since the condition only arises at a transition.
- `RefreshCameraPosition` clears `g_inCinematic` when the camera **instance** changes. This was the
  approved "option 1" for a separate open bug: `SetCinematicMode` is only ever observed with
  `arg raw 0x00000001`, because the run-95 script hook sees native→script entry points and the
  cutscene **exit is not one of them**. The flag latched ON and leaked cutscene configuration into
  gameplay — measured as `view drift -21.5 deg ... cine ON` every frame, the engine refusing the
  yaw we wrote. A level transition is a signal we can actually see, unlike the exit event.

**Verified, user-confirmed on all three symptoms.** Menu: `split 334 (334 with parallax),
mono-fallback 0` at 120 fps. Flag: alternates 29 OFF / 11 ON / 51 OFF / 2 ON / 28 OFF instead of
sticking; the detector logged all 5 transitions and caught the stuck flag twice. Controls: all 21
`view drift` lines read `cine off`, `engine-vs-asked residual` back to 0.10–1.03° from a 3.5° mean.
The `identification:` line vanished from the log entirely — it only prints when frames are lost.

**Still open / residue, recorded rather than filtered:**
- The cutscene **exit event remains invisible**. A cutscene ending without a level change keeps the
  flag set until the next transition.
- Three windows still show heavy mono-fallback (549, 1340, 4386) at 38/29/37 fps, all on
  load/transition moments while the camera is replaced. A long enough transient reads as a flash.
- The **menu rotation** bug is untouched and still has its own repro.

### Method note

Two of this project's standing lessons paid out here, and one nearly cost the run:

- **The user's symptom distinction was load-bearing.** "This is not the same menu bug as earlier"
  is what kept this off the rotation repro. Extends the existing note that his *conditions* are as
  reliable as his commit attributions.
- **A stale-premise check, not a hard problem.** The instinct was that the day's feature work had
  regressed stereo. The log said the classifier was innocent — passthrough quad counts were
  *identical* between the working and broken menus (2,239 vs 2,215) — and the first hypothesis
  (the fullscreen-quad classifier) died on that one line. Check the control before bisecting.
- **`Readable` is not a liveness test, anywhere.** This is the second time a cached engine pointer
  survived readability after the object died. `ENGINE_NOTES.md` now records which objects `IsLive`
  applies to (camera: yes) and which it must not (HUD: no).

---

## ⭐ Run 201: world scale, and the gun was anchored to the wrong eye

Asked how world scaling is done and whether it could be adjustable. The answer turned out to be tidy,
and asking the question surfaced a real bug that no instrument here would have found.

### ✅ WORLD SCALE, on the DISPLAY page

Scale is governed by **one constant** — `kMetresToUU = 52.5`, which is UE3's documented 16 units per foot
exactly, not a tuned guess. That is why it looked right without anyone adjusting it. All three real-world
quantities flow through it: the stereo baseline (apparent size), the 6-DOF translation (distance travelled
per step), and the hand position the gun rides.

Those three **must** move together. Scaling the IPD alone changes how big things look but not how far you
walk, which reads as the world resizing *as you move* — the exact complaint a world-scale slider exists to
fix. One conversion, three users, one knob.

### 💥 The gun was anchored to the ENGINE's eye, not to the player's

Reported in passing: *"when I recentered, the gun came closer to me."*

6-DOF folds `g_hmdOffset` into the **camera matrix**, so the view is at `engine eye + g_hmdOffset`. But
`BuildGunC` applied `H = (hand − head)` relative to the origin of translated-world space — the **engine's**
eye — which never received that displacement. So the gun's apparent position was off by however far the
player had drifted from the recentre point, and recentring zeroed the drift, moving the gun.

The direction depended on which way you had drifted, which is exactly why it never looked like a
systematic fault. Fixed by adding `g_hmdOffset` to `H`.

> **No instrument in this session would have caught it.** The eye-height probe read `+0.0 UU` immediately
> after recentring and looked like a clean pass — which it was, for the question it was asked. A symptom
> that only exists when two things which *should* share a reference frame don't, needs someone wearing the
> headset to notice.

### 📋 Eye height: measured, and a conversion that would have misled

`Pawn.BaseEyeHeight = 70.0` (stable), `EyeHeight` 67.4–71.4 with bob and crouch. Both writable if a knob
is ever wanted.

> ⚠️ **`BaseEyeHeight` is measured from the actor's `Location` — the CENTRE of the collision cylinder, not
> the floor.** Reporting it as "133 cm" invited the wrong conclusion that the character is child-sized. The
> real eye height above ground is roughly `CollisionHeight + BaseEyeHeight`, and `CollisionHeight` has not
> been read. **A unit conversion is not a measurement if the datum it starts from is not what you think.**

Also confirmed: **recentre works from seated**, and it re-zeroes yaw, lean and height together because
`g_centrePos` is captured in the same block gated by `g_haveCentre`. Sitting versus standing is therefore
handled by holding MENU, not by a setting — worth knowing before reaching for a height knob.

---

## ⭐ Runs 198–199: the "reload arms" were never reload geometry, and the heal window

Two visual fixes before a full playthrough, and the first one had been misdiagnosed in this file since
run 122.

### ✅ The extra arms are the 390-vertex mesh, not reload geometry

Carried for seventy-odd runs as *"extra meshes that exist only in the reload animation, so they are
absent from the list when the counts are latched"*. Run 198 implemented exactly that — latch the whole
baseline set, hide anything not in it — and it changed nothing, because the premise was wrong.

The report that solved it: **always present during gameplay, hidden below where a monitor player would
see, and they MOVE WITH THE GUN.** Both facts were already in the logs and neither had been joined up:

| evidence | already recorded |
|---|---|
| `[0]=5799 [1]=3314 [2]=390` | the baseline set — so 390 is *in* it, and "hide what is not in the baseline" could never reach it |
| `vertex counts seen riding: 390` | run 181's rider census — so it follows the gun transform |

**It is a defect the mod created.** The fragment sits below the bottom of a 16:9 frame; VR's much taller
field of view plus a gun that now follows your hand brings it into view. That is why sixty runs of flat
comparison never saw it.

Fixed by hiding the baseline mesh that is neither gun nor arms — by ROLE, so another weapon's fragment is
covered too.

> **The lesson is not "measure more".** Run 198 measured carefully and built a working mechanism for a
> problem nobody had, because it took the LABEL on the report ("reload arms") as the diagnosis. The two
> facts that identified it were a user sentence and a log line from eighteen runs earlier.

### ✅ The arms come back for the health injector, and only when it does something

The animation is performed by the arms we hide, so hiding them left the player watching nothing. The
window shows the arms **and returns the gun to the engine's position** — moving the gun onto the
controller while the arms animate at the engine position tears one animation in half.

Gated on `Pawn.Health < Pawn.HealthMax`, read at the press, so a heal at full health shows nothing.

> ### ⚠️ Why "wait for health to rise" is the wrong shape
>
> The obvious verification is a trap: health is applied BY the animation, possibly at its end, so the
> confirmation arrives late and a window opened at that moment runs on past the animation it was supposed
> to cover. Asking *"could this heal do anything"* before the press has no timing dependency at all.

Both edges are separate knobs measured from the press (`HealArmsDelayMs`, `HealArmsMs`), because they
were reported independently — shortening the duration to fix a late finish would also have made it start
early.

### 💥 Run 200: matching `"HUD"` as a substring found `RvCE_HUDMessage`

The heal gate's second half — skip when the player has no health packs — needed the HUD. The finder
matched class names containing `"HUD"` and picked **`RvCE_HUDMessage`**, a combat-event object with none
of these properties. Both checks then degraded to *"assume the heal works"*, the window opened anyway,
and the wrong object was CACHED — so every retry re-found it and logged again, hundreds of identical
lines burying the one line that named the object it had picked.

**Same mistake as run 197's `stride 72`: a name that looks right is not an identification.** Third time
in this stretch that a plausible label was taken as a fact.

Fixed by identifying with the thing actually needed: resolve `RvGameSharedHUD` — the class that
*declares* `mHealthPackCount` — and accept only instances whose `SuperField` chain reaches it. A
coincidence cannot satisfy that. The live instance is `RvHUD`; the property is at `+0x588`, depth 1.

Also: `IsLive` deliberately does **not** gate the HUD. A UE3 HUD hangs off the PlayerController rather
than `PersistentLevel`, so the liveness rule that correctly separates pawn instances from archetypes
rejects every real HUD — reusing it without checking would have been a silent no-op.

### 💥 A fifth silent cap: an 8-level class walk hid `Pawn.Health`

`PropOffsetInChain` walked eight super-classes. `Pawn.Health` is at **depth 10** from `RvPlayerPawnSP`,
so it reported "not found" and the heal window fell back to the button alone.

That is **owner cap (183), signature table (192), first-block break (193), 4-alignment filter (194), and
class-chain depth (199)** — five times, one shape: *a bound chosen because it looked generous, silently
excluding the answer.*

Now it walks to `SuperField == 0`, keeps a 64-level guard purely as a corrupt-pointer backstop, **reports
when the guard is what stopped it**, and logs the depth at which each property was found so a future
`-1` can be told apart from a truncated search. The one thing that worked correctly on the failing run
was the fallback saying `health could NOT be read` out loud instead of silently doing nothing.

---

## ⭐ Runs 182–190: the gun's feel fixed twice, and a nine-run hunt aimed at the wrong thing

Two wins on how the mod *feels*, one workaround, and an expensive lesson about what a measurement
does and does not license.

### ✅ The gun moved in STEPS, and it was integer truncation

Reported first as "jitter, like it's not tracking at the full framerate", then **redescribed as
"moving in steps, almost like there's a deadzone — if I move the controller very slowly I can see it
jumping."** The redescription is what solved it, and it did so by ruling out an entire class:
**latency shifts smooth motion in time; it cannot turn smooth motion into stairs.**

`g_handOffX/Y/Z` held the hand-minus-head offset in **whole unreal units**. Eye separation is a
measured 3.32 UU = 6.3 cm, so 1 UU ≈ **1.9 cm** — the gun could only ever sit on a 1.9 cm lattice.
`(LONG)` also truncates *toward zero*, so the step was asymmetric about the origin. Now milli-UU with
`lroundf`: 19 µm, below the controller's noise floor.

The **rotation** was never involved — `g_handDev*UU` are engine rotation units at 65536/turn, 0.0055°
per step. Fixing both would have worked and taught nothing about which mattered.

> ### ⚠️ The method lesson, and it is the uncomfortable kind
>
> A **confirmed measurement pointed the wrong way.** The `pose lead` instrument was built for this
> report, worked correctly, and read **8.33 ms** of pose staleness — exactly one display period, five
> windows running. Acting on that alone would have produced a small real improvement and a confident
> "fixed", with the stepping still there.
>
> **The instrument was right, the number was right, and it was not the answer to the question asked.**
> What settled it was a user redescription, not a measurement. When a symptom is described as
> *stepping*, suspect quantisation before timing, whatever the instruments say.

### ✅ The gun's pose was also a full frame stale — a separate, additive fix

Same runs, different defect, deliberately tested in its own run. D3D9 renders and *then* Presents, so
the pose sampled at Present N is consumed by frame N+1's draws — one display period late.

**Why the head hides it:** the head goes to the compositor as the projection layer's pose and gets
**reprojected** at scan-out, so late error is corrected in hardware. The gun is baked into *geometry*
by `BuildGunC`, and reprojection cannot move a vertex. Hence gun-only lag next to a smooth head.

Fix: locate the hand spaces at `predictedDisplayTime + predictedDisplayPeriod` (`PoseLeadFrames=1`).
Measured 8.33 ms → **0.00 ms**. The period comes from `xrWaitFrame`, not a hardcoded 8.33 ms.

> ⚠️ **`0.00 ms` is bookkeeping, not tracking quality.** With pacing locked the metric becomes
> arithmetically exact and is measuring its own arithmetic. It proves the timestamp we ask for matches
> the frame the geometry lands in; it says nothing about the runtime's *prediction* at that timestamp.
> The open risk is overshoot on sharp reversals, since prediction error grows with lead time.

### ⛔ Nine runs chased the muzzle FX through the engine. The seam was the answer.

The flash and barrel smoke were assumed to be correctly drawn at a *wrong engine position*, so runs
182–188 went after moving the first-person weapon in the engine's object model. What ended it was one
observation from the **flat monitor**, not the headset:

> the effect sits at the dead centre of the **monitor** — on the seam, between the two eye images.

World geometry goes through `StereoPair` and is drawn **twice**, once per scissored half. A merely
mis-placed effect would appear **twice**, once near each gun. Seeing **one**, straddling the middle,
means its clip position is `(0,0)` in the *full side-by-side frame*. **It is our own per-eye remap
missing this draw — not Raven's placement.** And it predicts, correctly, that the aim method cannot
matter, because the effect never receives a per-eye position at all.

Also established: the flash and the smoke are **one particle system**. `RvGame.u` lists no smoke
property; the smoke is an emitter inside the muzzle-flash asset. Two bugs reported for sixty runs,
one asset.

Workaround shipped — `HideMuzzleFx=1` nulls the first-person templates on the live `RvWeaponFX`, which
kills both. The real fix is a per-eye remap and should retire the setting.

**What the nine runs did produce, and it is in `ENGINE_NOTES.md`:** the UObject property-offset layout
(`Children +0x4C`, `Next +0x40`, `Offset +0x64`), solved by requiring it to reproduce
`Actor.Location=+0x054` *and* `Actor.Rotation=+0x060`; and the measured negative that writing a
component's `Translation` moves the derived transform by **+0.0 UU** — UE3 will not recompose without
a reattach.

### 📋 Runs 191–194: the flash draw IDENTIFIED, the register not, and tabled

Four solid facts, none of which should be re-derived: the draw is **`DrawIndexedPrimitiveUP` at stride
72** (unique to firing, ~7 per shot, found by a baseline-vs-fire census); it **is** already split per
eye; its vertices are **world-space** (so not a screen-space quad — the Bink fix does not apply); and it
has a vertex shader whose **`c5..c8` is its local→world matrix**, confirmed by a `w` column of
`(0,0,0,1)` and a translation that drifts frame to frame as the particle moves.

Pass-through counts were **104/frame in baseline and fire alike**, which killed the quad-classifier
theory outright — and only a baseline could have shown that. A fire-window number on its own had
nothing to compare against.

**Not found:** the register carrying the *clip* transform. Still open, with the shortlist recorded in
STATUS, plus a cheaper hypothesis to test first — that the flash rides `c0` and merely arrives when no
`g_camMats` slot is valid, which would make a duplicated draw land identically in both halves.

> ### 💥 Run 196: three instruments were built on an identification nothing had tested
>
> Run 192's census diffed a fire window against a baseline and found **stride 72** unique to firing. That
> part was sound. **The error was reading "unique to firing" as "is the muzzle flash"** — a tracer, an
> ejected casing and an impact effect all appear only when you shoot, and all render correctly.
>
> Runs 193, 194, 195 and 196 then hunted that draw's transform: a per-draw write mask, a block dump, an
> alignment fix, and finally the shader bytecode. The bytecode was **the first measurement capable of
> contradicting the premise**, and it did — the shader reads `c0`, which is locked, valid and remapped at
> that draw, so its transform cannot be what lands on the seam.
>
> The test that would have caught it was one line, available from the start, and is the same
> discriminator that settled the foreground-pass question in a single run: **drop the draw and see
> whether the artefact goes.** It now exists as `MuzzleFxDropStride`.
>
> **Run 197 ran it: the artefact REMAINED.** So stride 72 was never the flash, and runs 193–196 measured
> the wrong draw from beginning to end — four runs of increasingly careful instrumentation, every one of
> them pointed at geometry that was already being remapped correctly. The one-line test cost nothing and
> would have redirected all four.
>
> **Confirm what a thing IS before measuring its properties.** Every later instrument inherited the
> unvalidated label, and being more rigorous about *alignment*, *caps* and *bytecode* could not rescue a
> chain whose first link was a guess.

> ### 💥 Run 191's scan was the wrong instrument, and it looked like evidence
>
> The matrix scan tests whether the **camera** transforms to `w≈0`, which is only true of a full
> world→clip matrix in translated-world space. A local→clip matrix for an effect fails that test while
> still being the transform. So its silence was **structural blindness, not a negative** — and it was
> read as a negative for a run. Before trusting an instrument's "not found", check that the thing being
> looked for is inside the class it can detect.

### 💥 Four more runs lost to bounds I chose for tidiness

The recurring shape, worth stating as a rule: **a cap chosen to keep output readable silently excluded
the answer.** Run 183's owner cap filled with archetypes before reaching the live weapon. Run 192's
signature table saturated on indexed draws that carry no vertex data. Run 193 `break`'d after the first
constant block — and the first block was the *world* matrix, not the screen transform. Run 194 then
added a 4-alignment filter that **excluded `c5`, the very block the previous run had confirmed.**

**Shader constants have no alignment requirement** — a compiler packs them wherever they fit. The view
matrix visible in the same dump was not 4-aligned either.

When the question is *"which of these is it"*, do not cap the list at one.

### 💥 Five instruments in a row could not answer, and all five failures were mine

Worth recording as a set, because they were not five different mistakes:

| run | the instrument's defect | shape |
|---|---|---|
| 183 | ownership cap filled with `RvCE_WeaponFire` and archetypes before reaching the live weapon | a cap that silently truncates the answer |
| 183 | `NameOf` per object across ~36,000 objects, re-walking on every miss | **string compare where an index compare would do** |
| 184 | `Readable(obj, 0x1000)` all-or-nothing silently disabled the control | a control that quietly declines to participate |
| 185 | NaN passed every test — `err > maxErr` is false for NaN, so NaN scored a perfect zero | a filter applied only at seed time |
| 186 | 2,223-combination search with a syscall in the innermost loop → seconds-long load-in freeze | **string compare where an index compare would do** |

Three of those are one recurring shape — comparing interned UE3 names as **strings** instead of
comparing the `FName` index at `+0x2C`. It is the default failure mode of this API and it is now
written down in `ENGINE_NOTES.md`.

The habit that saved the run: **the scan carried a positive control** — the camera actor, where
`+0x0054` is known to hold the world position. When it read `ABSENT`, the run was thrown out instead of
its negative being believed. Two of the five defects were caught by that control and by nothing else.

And one instrument earned its keep by being *wrong usefully*: run 185's delta scan found the wrong
field to **write** (`LocalToWorld`, a derived output) and exactly the right field to **watch** — which
is what made run 188's component-write test self-verifying without needing anyone's eyes.

---

## ⭐ Run 181: two visual artefacts, and both were the same reading error

Reported together: the pistol's **barrel smoke sits at the centre of the screen** instead of at the
muzzle, and **two crosshair lines appear while jumping** with the crosshair hidden. They turned out
to share a shape — in each case the measurement that would have prevented the bug was already in the
file and had been read past.

**Flown.** ✅ The crosshair fix is verified. 🔬 The smoke fix engaged and did not reach the smoke.

### The results

| | outcome |
|---|---|
| crosshair on jump | ✅ **fixed.** `8 seen, 8 dropped`, with two frames at `r=0.167` and `r=0.151` — both past the old `0.15` box, so the spread really did exceed it and the case really was exercised |
| barrel smoke | ❌ still at screen centre |
| impact smoke | ✅ correctly did **not** move — the negative control passed |
| `weapon fx` peak | `0 → 2 → 3 → 4` — **the mechanism engaged**; there are draws beyond the gun mesh in the pass and they did ride |

The smoke result is the informative one: the transform reached extra geometry and the smoke was not
among it. That rules out "in the pass, transform wrong" — that would have moved it *somewhere*. So
the smoke is not in the foreground pass, and `WEAPON FX = HIDE PASS (TEST)` is the next step because
it answers *where* by construction rather than by inference: what vanishes is in the pass.

**⚠️ A print-once diagnostic hid the one thing worth knowing about it.** `weapon fx: STOPPED RIDING`
fired once at load-in — the run-115 depth-clear boundary tripping early, before the gun was latched,
which is the benign case. But the guard was `static bool said`, so the log **could not say whether it
recurs during gameplay** — and the gun transform rests on that same boundary. A diagnostic that
reports an event exists and then goes quiet is the shape of half the instruments this project has had
to rebuild. Now: first occurrence in full, then a running count on a slow tick.

### The smoke: a transform whose reach was narrower than its description

`FgMoved` matched exactly **one** vertex count, the latched gun mesh. Route 2 deliberately never
moves the engine's own weapon — that is the whole reason culling and LOD cost nothing — so a muzzle
effect spawns at the *engine's* muzzle, which is the centre of the view, while the mesh it is
supposed to be leaving is over on the controller. Nothing was wrong with the transform; it simply
never reached the effects.

The rule that generalises was established back in run 114 and not used: **the foreground pass IS the
first-person weapon pass.** The engine already separated that geometry for us. So `FgRidesGun` moves
every draw in the pass except the arms, which stay hidden — and the **non-indexed** draw hook, which
could not move anything at all before, now can.

Shipped as `WEAPON FX` on the panel's ADVANCED page because it is a **discriminator, not timidity**:
if the smoke follows the gun at `RIDE GUN` and returns to centre at `STAY`, the effects are in the
pass and this is the fix; if there is no difference either way, they are drawn elsewhere, no
transform on this path can reach them, and the next seam is `SetFlashLocation` — which run 95's
census already measured at 88.9% enrichment under fire. `weapon fx: N … (new peak)` in the log
separates "engaged and found nothing" from "never engaged", which look identical in a headset.

> ### ⛔ The premise this replaces named the wrong effect, and the observation itself was correct
>
> The run-112 known-issues list below says the flash and smoke are *"still tied to screen centre"*
> in one bullet and that *"barrel smoke **IS** correctly aligned"* in the **very next one**, and
> STATUS carried the second forward for sixty-odd runs as the lead to chase: *find what the smoke
> rides on that the flash does not.*
>
> **The aligned smoke was the IMPACT smoke — the puff off the wall where the bullet lands. The
> barrel smoke was never aligned.** Established by the user in run 181, and it makes the whole thing
> coherent:
>
> | effect | space | why it lands where it does |
> |---|---|---|
> | impact smoke | **world** — spawned at the trace hit point | route 2 rotates the trace, so the hit point is correct, so the puff is correct |
> | muzzle flash, barrel smoke | **view** — attached to the first-person weapon | the engine still has that weapon at the camera, and route 2 never moves it |
>
> So there **is** a real asymmetry, and it is between world-space and view-space effects. It is just
> not the asymmetry the note described: the flash and the barrel smoke are one system and always
> were, so *"what does the smoke ride on that the flash does not"* had no answer to find.
>
> **The failure mode is a mislabelled SUBJECT, not a stale measurement.** The observation was
> accurate — something was correctly aligned — and the name attached to it was wrong. Two grey puffs
> appear near where you are aiming within a second of each other; one is on the wall and one is on
> the gun. That is an easy conflation to make in a headset and an expensive one to write down,
> because the note reads as a measurement and cannot be checked without re-running the shot.
>
> Worth carrying forward: **the mod's own effects split cleanly on world-space vs view-space**, and
> that is the first question to ask about any future misplaced effect. World-space ones come out
> right for free.

### The crosshair: an aspect ratio hiding in plain sight

`LooksLikeCrosshair` tested a **square** box — `|cx| < 0.15` and `|cy| < 0.15` — in clip space, where
x spans the frame's full **width** and y its full **height**. One tick at a given *pixel* radius
therefore reads 16/9 = **1.8× larger in y**.

That ratio was sitting in run 153's own measurement, four lines above the test: `0.045 / 0.025 = 1.8`.
The four ticks are equidistant in pixels and the numbers differ only because the units do. Jump
spread pushes them outward, the vertical pair crosses 0.15 **first and alone**, and two lines come
back — the report, down to the count.

The fix uses the part of the measurement the box discarded: **each tick sits ON a centre axis**
(`cy = +0.000` for left/right, `cx = -0.000` for up/down). The rule is now "on an axis, within reach
of centre", which is *tighter* across the axis than the old box and looser along it — the only
direction spread can move a tick.

`crosshair census` prints seen/dropped/furthest-radius from Present on change, because **"it looks
fixed" is not a measurement** and a rule that counts only its own hits cannot report a miss. Resting
truth is **8 seen, 8 dropped**: four ticks, each drawn twice.

### Two stale notes corrected on the way past

Both said mode 1 of `HideCrosshair` *flickers* because `g_handHeld[1]` is a capacitive sensor, and
that `HideCrosshair=2` was "the setting that behaves". **Run 158 replaced that signal with AIM
METHOD** — deterministic, no flicker — and updated the panel row while leaving the declaration
comment and the STATUS item describing a sensor nothing reads. The ini log line carried the same
stale text. All three are fixed; the code comment keeps the warning rather than deleting it.

---

## ⭐ Run 180: the yaw delta that never wrapped, and three misattributions it caused

**The bug.** `camYaw - ctlYaw` on `int32_t` does not wrap. UE3 rotation is 65536 units per turn,
so two headings **0.3° apart** can differ by 65590 units, which reads as **360.3°**. Two sites did
this — the camera-ownership test and the drift log — and **both carried a comment claiming
`// wraps correctly`**. Both were introduced in `559653f`, during the menu-rotation investigation.

**Measured, one 8,476-line run:**

```
camera handed to the GAME (360.3 deg from the pawn)     x20
view drift  +358.4 / +359.9 / +360.0 / -359.8 / -360.3 deg
```

Every one of those is ~0° misread as a full turn. The ownership threshold is 15°, so each false
reading handed the camera to the game and re-anchored `g_baseYaw` to it — yanking the view
somewhere the player was not looking.

**After the fix, comparable conditions, heavy turning** (peak stick `1.000`, windows at 120/120
frames of full deflection): **0 handoffs**, drift reporting `-5.0 / -5.4 / +89.5 / -89.9`.

The fix is to cast the difference to `int16_t` — 65536 units *is* 2¹⁶, so truncation lands the
result in `[-32768, 32767]`, the shortest signed path, for free. Behind `YawDelta()` /
`YawDeltaDeg()` so a third site cannot repeat it.

### What it had been blamed on

- **"Phantom snap turns."** There were none. The run was `TurnMode=2` — smooth — so the snap
  branch never executes, and the instrument added this run recorded **zero snaps fired** while the
  symptom was being reported.
- **A set-down controller inventing input.** Ruled out by the user's own observation that it only
  happened with fingers on the sticks and while moving one. It fires when a turn carries the yaw
  across the wrap boundary, which is the opposite condition.
- **Last night's left-handed / settings work.** The user suspected the changes made while chasing
  the menu-rotation bug and was right about the commit, wrong about which part: the drift log is
  `Log()`-only and cannot change behaviour; the ownership test in the same commit can.

### The lesson is the comments

Two load-bearing comments asserted the exact property that was false, and they were believed for
every run since. `// wraps correctly` is not a note, it is a claim — and an unverified claim in a
comment is indistinguishable from a verified one when you read it six runs later.

### Also this run

- **The trigger veto failed OPEN at startup.** `trigger/touch` does not resolve immediately, and
  until it did, the run-156 veto passed every value through — the load-in phantom melee, and the
  gun attached to nothing. A **race**: 2, 0 and 9 unprotected windows across three runs of one
  build, which is why it read as "this was fine yesterday". Now fails closed, bounded by a ~30 s
  grace so a runtime genuinely lacking the sensor still keeps its triggers. **Not yet verified** —
  both runs since resolved instantly.
- **`MenuCameraFollow` (run 176) removed entirely.** It could never A/B what it was built for:
  menus report `CINE YES`, so `g_inCinematic` short-circuits the condition before the switch is
  reached, and its own log line said so. It was also a live uncontrolled variable, found at 0 in
  a run while the ini on disk read 1.
- **The turn stick had no instrument at all** while phantom snaps were being reported. It has one
  now — narrow `thumbstick/touch` per hand, plus a per-window census. Measuring only.
- **Diagnostic honesty, twice.** The stick census was briefly called broken on the strength of its
  first 8 windows, which were startup; across all 122 it read correctly. And installing the
  pre-change backup DLL for an A/B reinstated the old positional `prevN` rotation, which destroyed
  the 08-03 night logs over six relaunches.
- **Not the mod at all:** a day of "the game will not start" was `PhysXLoader.dll` missing
  system-wide — `0xc06d007e` delay-load failure, then a NULL deref at `+0x007cdde6`, identically
  on the Steam copy. This is the crash `ENGINE_NOTES.md` predicted and said was never identified.

## ✅ Runs 170–175: left-handed mode, and a settings bug that hid behind a comment

### Handedness, as TWO independent settings

Split deliberately, copying the *fixed* version of Half-Life: Alyx rather than the shipped one —
Valve welded handedness to stick assignment, players said it forced them to aim or move with the
wrong hand, and Update 1.2 separated them.

- **`LeftHanded`** — gun, aim, triggers, grips and face buttons mirror. **Sticks do not.**
  Ini-backed with a panel row that edits a *pending* value and shows `*RESTART*`: OpenXR binding
  suggestions are frozen at attach, so a live toggle would move the gun and leave the buttons.
- **`SwapSticks`** — movement and turning trade sticks, nothing else. Live, four floats at read time.

**The clamp depends on TURN MODE, not aim method.** Mouse and `TurnMode 0` both go through the
engine's look axis and inherit the matinee's limits; `TurnMode 1/2` are mod-side and do not.

⚠️ **`menu/click` is LEFT-hand only and `system/click` is runtime-reserved** — confirmed against the
Oculus Touch interaction profile, not taken from this file's own note. So MENU stays on the left
thumb in both modes, and the startup log says so rather than leaving a left-handed player to hunt.

### ⚠️ Three bugs, all the same shape: a literal where an index belonged

`AimHand()` / `OffHand()` now exist because fixing "the four aim sites" missed everything else:

| symptom | cause |
|---|---|
| no buttons worked at all | the mirror swapped the **hand** but not the **component** — `a`/`b` are right-only and `x`/`y` left-only, so `/user/hand/left/input/a/click` does not exist. `xrSuggestInteractionProfileBindings` is **all-or-nothing**: one bad path killed all 31 |
| left trigger fired only while the *right* trigger was touched | `g_trigTouch[]` is indexed by physical hand; fire was hardcoded to index 1 |
| gun rotated by the left hand, positioned at the right | `g_handLastPos[1]` hardcoded |

The mapping `x`↔`a`, `y`↔`b` had been **written down correctly and then not implemented**. Because
the suggest call names no culprit, left-handed mode now logs every mirrored binding at startup.

### 🛑 The gun anchor does NOT mirror — two attempts, both removed

A sign flip, then a second tunable value. Both assumed the anchor is a property of the *hand*. It
is not: it was tuned with the gun **attached to the arms**, so it describes where the game's own gun
mesh sits relative to the eye — and the game has no left-handed arms.

The evidence agreed once the right question was asked: with position frozen (**TAB**) the gun swung
through an **arc**, the documented symptom of a pivot in the wrong place, and ±8 is a pivot 16 units
off the grip.

⚠️ Kept as a caution: the sign flip sat **downstream of the readout and the arrow keys**, so the
panel showed one number while another was applied and pressing "right" moved the gun left. Any
per-handedness value must go through the same accessor as its readout and its tuning.

### ⚠️ `PanelSave` had its own list, and it fell out of step

`CUTSCENE CAM` was reported reverting every launch. `CUTSCENE TURN` had the identical bug and had
not been noticed. A comment saying "remember to add it here" was already present and had already
failed.

**Now `PanelSave` walks `kPageRows` — the same tables that build the UI** — so the set that is saved
is by construction the set that is shown. A row with no save entry hits the default case and *says
so in the log on the first save*.

### ⚠️ An unsupported character renders as a SPACE, silently

Reported as "there are no \*s around restart". `GlyphIndex` returns space for anything unknown, so
`*RESTART*` drew as ` RESTART ` — **and the `>` / `<` on the page rows had been invisible since they
were added.** Glyphs added rather than reworded around, and the fallback now carries a warning.

### 🛑 Run 177: the menus report `CINE YES`, and that invalidates run 176's test

**On-screen readout in both menus: `MODE 3 AUTO · ROT MATRIX · CINE YES · FOLD OFF · HELD 0`.**

So `g_inCinematic` is TRUE in the menus. The run-176 switch reads
`if (inCine || (gameOwnsCam && followMenuCam))` — **`inCine` short-circuits it**, so
`MenuCameraFollow=0` was inert in a menu and the cutscene branch anchored the base anyway. The A/B
returned "no change" because **nothing changed**, not because the hypothesis is wrong. The switch
was left outside the cutscene path on the assumption that menus and cutscenes are different
conditions; they are not.

`HELD 0` is the other half — the engine keeps *none* of our written yaw in menus, against ~100% in
gameplay. And `YAW 89`, which is the +89.5° that has been showing up since run 167.

### 📋 The real shape, from the user's own testing

Two menus share one 3D scene. **Menu 1** is the ocean/title scene, first after the splash screens.
**Menu 2** is the options menu, set in a room with that same ocean visible *in the background*.
Exiting gameplay lands you in menu 2; you go back to reach menu 1.

| | menu 1 | menu 2 |
|---|---|---|
| first entry after splash screens | **no 6-DOF** | water correct |
| entered after exiting gameplay | **6-DOF works** | **water moves with you** |

So it is not "6-DOF is broken in menus" — it is a **trade-off between two states**, and the second
symptom is the diagnostic one: **a distant backdrop moving with head translation is a skybox
receiving a positional offset it should not get.** Background geometry belongs at infinity and must
take rotation only.

That reframes it. The question is not "why is 6-DOF off in menu 1" but "why does the same scene
sometimes get the offset and sometimes not, and why does the backdrop take it when it does".

📋 **Tabled at the user's request.** Nothing here is a guess — the states are reproducible and the
readout names them.

### 🎯 6-DOF in the main menu — a named suspect, and a switch to test it (run 176)

**The user supplied the lead by noticing a timeline I had stopped tracking:** the main menu looked
right *before* runs 167/168, which is when the base started coming from the camera.

There is a mechanism, and it is not subtle:

```c
const float baseYawRad = g_baseYaw * (2π/65536);
g_hmdOffset[0] = (bc * fwdAmt - bs * rightAmt) * kMetresToUU;
```

**The 6-DOF offset is rotated into world space by `g_baseYaw`.** In menus that is now the camera's
yaw — measured at **+89.5°** — where before it was the controller's, near zero. So leaning forward
pushes you sideways. The offset keeps full magnitude, which is exactly what the run-175 probe
measured, while the motion goes somewhere unrelated to the head. **That reads as "6-DOF doesn't
work" long before it reads as "6-DOF is rotated 90°"** — and it explains why every number in the log
looked healthy.

`[Input] MenuCameraFollow` (default 1, `MENU CAM` on the ADVANCED page) switches the run-168 follow
off so the two states can be compared in one session. The cutscene anchor is deliberately **not**
behind it — that fix is confirmed and keys on a different condition.

> ⚠️ If confirmed, **the toggle is not the fix.** The follow is what fixed the menu *rotation*, so
> rotation and 6-DOF would be broken by the same change. The real fix is for the base to feed the
> **view yaw** without also rotating the **positional offset**.

### 📋 The original measurement

The menu is a real 3D scene, renders in stereo, and does not respond to head movement. Every usual
suspect is clean: one recentre, `pos valid 1 tracked 1`, zero identification failures, **153 of 153
draws split with parallax**. And the offset *is* produced, at the same scale as gameplay:

```
menu     head-centre (-0.049, -0.004, -0.096) m -> offset ( 2.7,  4.9, -0.2) UU
gameplay head-centre (-0.371, -0.021, -0.334) m -> offset (24.0, 10.6, -1.1) UU
```

So the mod behaves identically in both places. Two possibilities remain: the effect is real but too
small to perceive at that scene's distances, or the menu's content is not drawn through the
view-projection we inject into.

⚠️ **F3 cannot be used to tell them apart while 6-DOF is on.** `posOffset` wins over `g_injectOn` —
*"6-DOF wins when both are on; the F3 constant is only a probe"* — so F3 does nothing in either
place. A valid test must turn 6-DOF off (F10) first. One run was wasted on this.

## 🚧 Runs 164–169: panel pages, cutscene comfort, and TWO OPEN BUGS

### ✅ Landed

- **Panel pages.** Root (VR MODE, AIM METHOD) → `CONTROLLER`, `COMFORT`, `DISPLAY`, `ADVANCED`.
  DISPLAY holds the crosshair and is deliberately a page of one — HUD inset belongs there next.
- **`CutsceneStickTurn`** (renamed from `CutsceneLockTurn`, which was a double negative and got
  misread twice in conversation). `OFF` / `GAME` / `FREE`, default **FREE**. `GAME` hands the stick
  to the engine's look axis for the scene, so it inherits the matinee's clamp exactly like the
  mouse — and overrides `TurnMode` for the duration, a combination not reachable before.
- **`CutsceneLockCamera`**, default off. Freezes the base at the scene's opening heading so the
  scene rotates around you instead of carrying you. The tradeoff in the user's words: *"if you lock
  the camera the helicopter will move around you"* — relief for an intense scene, not a default.
- **Head roll off the panel.** It is correctness, not preference: tilt your head and the view must
  tilt with it. Was only ever a row because it happened to be a per-frame global; it began as a
  run-59 diagnostic. Ini key stays as an escape hatch.
- **Panel sizing.** Width is a per-page floor (`kPageCols`) rather than measured, so toggling a
  value no longer resizes the box; `px` derives from a fixed row count rather than the current
  page's, so sub-pages have the same text size instead of doubling it.
- **Camera-ownership follow.** The base now tracks the camera whenever the *game* owns it, not just
  in cutscenes — see below.

### ⚠️ The clamp depends on TURN MODE, not on aim method

A correction worth keeping, because the first framing was wrong and plausible:

| | path | clamped? |
|---|---|---|
| mouse | engine look axis | **yes** — inherits the matinee's limits |
| stick, `TurnMode 0` | engine look axis | **yes** |
| stick, `TurnMode 1/2` | our own base | **no** — mod-side, outside the engine |

So a 360 pad in head aim behaves like the mouse *on GAME TURN*, and doesn't on snap or smooth. The
earlier "head aim clamped, controller aim free" was an accident of testing head aim with a mouse.

### 🐛 OPEN: the menu rotation bug

Entering the main menu leaves the view rotated, and it survives into a new game. **Five attempts,
all failed.** What is known:

- `g_baseYaw` set from the camera at the recentre **works and then decays** — measured capturing
  `16289 (+89.5)` correctly and reading `17` a moment later. The yaw fold-in is anchored to the
  **controller** and subtracts the difference back out, so a base set once cannot survive.
- Camera and controller **diverge hugely in menus and agree in gameplay** — `16289` vs `461` (87°)
  against `18046` vs `18053` (0.04°). That is now the signal for "the game owns the camera", with
  10-frame hysteresis, and the handover fires and releases correctly in the log.
- **It still rotates.** So something beyond yaw is involved, and the remaining lead is that the view
  comes from the mod's forced matrix rather than from the camera we are now tracking.
- ⚠️ **`FindCamera` and `FindController` both CACHE** and both defeated an attempt. Any future fix
  that keys on a pointer changing is already known not to work.
- ⚠️ A base of exactly `16384` was read as a smoking gun and was **a coincidence** — 90° is an
  ordinary world heading for an axis-aligned level. The menu is diegetic; a non-zero base is normal.
  What must be true is `our view yaw == the camera's yaw`, nothing about the base alone.

The `view drift` log line measures exactly that and is left in: it says nothing on a clean run, so
any line at all is the bug.

### 🐛 OPEN: the video classifier misfires in both directions

`VideoStereo` corrects splash screens and the opening cutscene. It also **doubles the main menu's
background**, which is a genuine Bink video under hundreds of draws of interface.

**Four conditions were tried; the fourth was reverted** because it fixed the menu and broke real
video (a sequence before the main cutscene started doubling). Current state is three conditions:
first draw of the frame, 2-primitive NDC quad, `D3DFMT_L8`.

> **Do not add a fifth condition.** Every geometric property of a video quad is shared with
> something else on screen, which is why each attempt at describing its shape found a new false
> positive or negative. This is run 45's recorded mistake — *"a filter that encoded its own
> answer"*. It needs a real signal: hook Bink's presentation, or identify the video by its render
> target. `VideoStereo=0` restores the original all-video-spans-the-seam behaviour.

The per-second misfire counter is what caught every one of these and should stay.

### ⚠️ Method notes

- **A setting that cannot express what is wanted gets misread.** `CutsceneLockTurn=0` parses as both
  "no lock" and "turning locked off", and both readings appeared in one conversation. The rename to
  `CutsceneStickTurn` made the ambiguity vanish without changing any behaviour.
- **Name the thing measured, not the category.** "Are we in a menu" is not readable — run 141 records
  that the main menu reports `PAUSED NO`. "Does our view yaw match the camera's" is directly
  measurable and covers menus, cutscenes and anything scripted with one rule.
- **An estimate is not a measurement, including mine.** The per-frame cost of the new camera reads
  was quoted from general knowledge (~6 µs/frame, ~0.07% of frame time), never timed on this
  machine. The `xrinput cost` line exists because the same question was answered properly once.

## ✅ Runs 161–163: starting a cutscene rotated ~90° — four attempts, and what each one taught

**Symptom:** starting a new game sometimes put you ~90° off once the in-engine cutscene began. Head
look worked; the scripted camera panned correctly; only the heading was wrong.

**Cause:** the view is built from the mod's **forced matrix**, and `g_wantYaw` is an **absolute**
heading — `g_baseYaw + headDelta` — not a delta on top of the engine's rotation. `g_baseYaw` was
captured at the main menu (which is diegetic, so it captures the *menu camera's* yaw) and the
fold-in that would correct it is deliberately off during cutscenes. Measured: **base −1.8° against a
scripted camera at +99.1°.** Head look worked because the delta was live; the base was simply wrong.

**Fix:** during a cutscene, anchor `g_baseYaw` to the camera's `mCurrentPOV` yaw every frame. View
becomes scripted heading + head look, and the camera's pan (99.1 → 98.6 → 98.0 → 97.4, tracking the
helicopter) is followed rather than fought.

> ⚠️ **This is only safe because run 162 stopped writing the controller yaw during cutscenes.** The
> camera normally follows the controller, so anchoring to it while also writing it is a feedback
> loop — our own output read back as input. **The two changes are load-bearing together and neither
> should be reverted alone.**

### ⚠️ Method: three misses, and every one was a mode's NAME mistaken for its behaviour

| attempt | why it failed |
|---|---|
| recentre when the `PlayerController` pointer changes | `FindController` **caches** — the pointer never moved. And it would not have worked anyway: `yaw held` is 100%, so reading the controller returns *our own* value |
| suppress the engine yaw write during cutscenes | the yaw was not in that field; the deltas collapsed and nothing visible changed |
| assume `ROT MATRIX` carries head rotation | **`matrix carries +0.0 deg` on every logged line.** With the deviation clamp at `maxDev = 0` the split is inert — the code's own note says so: *"at maxDev = 0 the matrix rotation is identically zero"* |

**The generalisable one:** `ROT MATRIX` describes a mode's *intent*, not what it does at the current
settings. Two of the three attempts were built on the name. **Read what the split actually carries,
not what the mode is called.**

**And the instrument failed twice in the same way it did in run 154.** The first yaw probe capped at
60 events and all 60 landed *before* the cutscene — it answered a question about the wrong sixty
frames. The fix both times was the same: key the instrument on the state you care about, not on a
global budget.

**What finally settled it was one bit**, asked directly rather than inferred: *does head look still
work during the cutscene while we are not writing the engine yaw?* Yes → the view comes from the
matrix, and the base is the only thing left that could be wrong.

### `CutsceneLockTurn` — stick turning locked during cutscenes

`[Input] CutsceneLockTurn=1`. Vanilla does not let you turn during a scripted scene, and the game
frames its cutscenes assuming you are looking where it put you. **Head look is unaffected.**
Suppressed at the *accumulator* and cleared, not at the point it is applied — blocking only the
application would let deflection pile up and dump as one jump on exit. Not an early return either:
buttons must still reach the game during a cutscene, one of them being the one that skips it.

📋 Wanted: a toggle for the scene rotating you to track its subject (the helicopter). Kept for now.

## ✅ Runs 159–160: start in VR, and Bink video in both eyes

**`StartInVr=1` is the default** and `VR MODE` is the first panel row. This is run 36 again, which
run 37 reverted because default-on meant *the main menu was rendered frame-split for the first time
ever, untested, on every launch*. What changed is checkable rather than a change of mind: **run 151
put the HUD and menus into both eyes**, so the thing run 37 objected to is the thing run 151 built.
Confirmed working. `StartInVr=0` restores run 37's cold start with no rebuild.

Applied at the first Present and gated on an **OpenXR session existing**, not at device creation —
with no headset there is no session, and splitting the frame anyway would be the run-36 failure on a
machine where nothing can fix it. The panel row saves; BACKSPACE deliberately does not, because a
key you press to peek at the flat game should not change how it launches tomorrow.

### ⚠️ The centre was captured before the pose converged — and `POSITION_VALID` was set

Reported straight after `StartInVr` landed: the menu sat too low and the game loaded in at the
ceiling, and toggling VR off and on fixed it. The log had it in numbers:

```
startup : head yaw 0.0,  head pos (0.000, -1.223, 0.000)
later   : head yaw -3.5, head pos (0.033, -0.040, 0.227)
```

A centre 1.18 m low puts you 1.18 m high for the session. x, z and yaw were **exactly** zero with
only y populated — a synthetic identity pose, not a measurement.

**The obvious fix would not have worked.** `POSITION_VALID` *was* set on that frame. Valid means
"this number is usable", not "this number is real yet". It now waits for `POSITION_TRACKED` **and**
`ORIENTATION_TRACKED` to hold for 10 consecutive frames, bounded at 300 so a 3DOF headset still
centres. The `recentred:` line prints both flags, because that bad capture looked entirely normal on
the old one.

BACKSPACE always worked because by the time anyone pressed it the pose had converged. `StartInVr`
fires as early as it possibly can, which is exactly when it has not.

### ✅ Bink video: two reports, one bug

The splash screens showed double vision and the opening cutscene was "way too zoomed in". Same
artifact. Fullscreen quads are passed through untouched — correct for UE3's post-process quads,
whose source *already* holds the side-by-side frame. A Bink frame is a single image, so passed
through it covers both halves with one copy: each eye gets a different half, stretched.

Measured before classifying. Every video frame read identically:

```
DrawPrimUP type 5 prims 2 stride 24 v0 (-0.957,+1.000) tex 1280x720 fmt 50 rt 4992x2688
... 1 draw(s) this frame
```

`fmt 50` is `D3DFMT_L8` — Bink's Y/U/V planes as luminance textures, combined in a pixel shader.

**Three conditions, all required.** "Texture smaller than the render target" alone is wrong — it also
matches every bloom and DOF downsample pass. **First draw of the frame** is what excludes those.

**Both axes are halved, and `y` is not optional**: each half is 2496×2688, nearly portrait, so
halving `x` alone would squash 16:9 to 8:9. Halving both gives 2389×1344 — exactly 16:9, centred,
black above and below.

The detection line logs once; the **per-second count** is the real instrument, and a nonzero count
during gameplay is a misclassification and says so. `VideoStereo=0` disables it without a rebuild.

### 📋 Known, not chased

- **`thread-cpu: more than 96 threads`** — the CPU census under-counts on this build. Harmless while
  nothing depends on it, but that instrument would lie if it were leaned on again.

## ✅ Runs 157–158: the settings panel grew up — AIM METHOD, and three silent no-ops

`AIM METHOD` (HEAD / MOTION CONTROLLER) is now the one control the rest follow, and it **persists** —
`g_aimMode` was never read from the ini, so every session started at 0 and had to be walked back to
11 by hand. It drives aim mode, the gun and arms, the rot mode, and presets `MOVE DIRECTION`.

| | HEAD | MOTION CONTROLLER |
|---|---|---|
| aim source | engine (`g_aimMode 0`) | trace rotation (`g_aimMode 11`) |
| gun | in front of your face | follows the controller |
| arms | visible | hidden |
| crosshair `AUTO` | shown | hidden |

Also on the panel: `OCCLUSION` (modes 0–2; 3 crashes and stays ini-only) and `DEBUGGING`
(`[Render] Debug` — hides the readout and makes every keyboard key inert bar BACKSPACE and PAUSE).
Panel scale now derives from a target *height*, so adding rows shrinks the text instead of growing
the box, and the width is measured from the widest row rather than a hardcoded 30.

**It is not an input-device switch.** Both methods take keyboard, mouse, a real 360 pad, or Touch
acting as one. Three things that look like they belong here already fall out for free and were left
alone: mouse pitch does nothing in either method (`g_wantPitch` is rewritten from the headset every
frame, and there is no pitch fold-in the way there is for yaw); `TurnMode` only ever affected
controller play; and snap turn works in HEAD too.

### ⚠️ Method note: **a mode is not a value — it is a value plus whatever had to be installed for it**

Three separate settings in two runs were set correctly and did nothing, and **all three reported
themselves as applied**:

| what was set | what was missing | how it presented |
|---|---|---|
| `OcclusionQueryMode` from the panel | the GetData vtable patch was gated on the mode at *first query creation* | changing it later did nothing, forever |
| `g_hideMeshIdx = 3` | the gun/arms vertex latch only ran when the APPS cycle passed through mode 1 | arms visible, gun unmoved, panel said MOTION CONTROLLER |
| `g_aimMode = 11` | five hooks the NUMPAD-dot path installed for any mode ≥ 6 | mode selected and inert; the bullet followed the view |

**Anything moved off a keypress onto a setting has to bring its setup with it.** The keypress path
was never just a value assignment, and reading only the assignment is what produced all three.

### ⚠️ And the sharper one: a latch that succeeds with garbage silences its own retry

The mesh latch was fixed with a retry keyed on the counts being *zero*. But `g_fgStableVerts` is the
**previous frame's** foreground list, replaced wholesale each frame — at the main menu it holds two
entries that are not the gun and arms. The latch took them, returned true, and the retry never fired
again. Toggling the method re-latched from gameplay, which is why it "worked the second time".

This is run 118's defect exactly: *"the hide indices then pointed at things that were not the gun.
The report that came back was accurate; the labels on it were fiction."*

The latch is now continuous and self-correcting on the only evidence that means anything — **is the
latched mesh still being drawn?** Not latched: take the top two once they have held 30 frames.
Latched and seen: leave it (run 119's protection — muzzle flash shifts the list while firing).
Latched and unseen for 240 frames: drop it and re-take. An **empty** list is neither hit nor miss,
because cutscenes and menus have no first-person pass and counting those as misses would throw away
a good latch and re-take it from menu geometry — the same bug from the other side.

Confirmed in the log: `mesh latch: gun=5799 verts, arms=3314 verts` — run 119's numbers, first try,
no drop-and-retake.

### BACKSPACE hands the game back completely

`AimMethod` is the setting; whether it *applies* is gated on VR mode, and `ApplyAimMethod` is called
from `SetVrMode` in **both** directions so the restore falls out of the same path that applies it.
Before this, flat mode ran with missing arms, a gun transformed onto a controller possibly on a
desk, and shots that missed the crosshair — silently.

## ✅ Runs 154–156: the judder is SOLVED — a set-down controller reports a phantom trigger pull

**This closes runs 67–75.** That investigation ran nine runs, proposed five mechanisms, buried all
five, and shipped a workaround built on a correlation nobody could explain. The mechanism was
never inside the engine's input plumbing, which is why nothing found there ever held up.

### What is actually happening

A Touch controller that has been set down and left to rest reports a **sustained partial trigger
pull** — measured at left 0.34–0.43 and right 0.23–0.28, held for seconds, with `isActive` true on
all 120 frames of every sample. It passes the 0.10 deadzone and reaches the game as LT ≈ 97 /
RT ≈ 64, and [Raven's own 360 layout](#-run-61b-ravens-360-layout-read-out-of-the-games-own-config)
maps those to **AIM** and **FIRE**.

So the gun fires and aims by itself, and the engine spends the frame doing precisely what it was
told. **Nothing was ever wrong with the engine, the pad, or the input code.**

The cost, from a session where the states separate cleanly:

| trigger state | fps |
|---|---|
| neither | **120** |
| right only (fire) | **98** |
| both (fire + aim) | **77** |

Over the whole session the correlation is total: every second with a nonzero trigger is 68–98 fps,
every second without one is 109–120. The per-thread CPU census agrees — on slow intervals
`Singularity.exe+0x0116FA6F` goes **3.218 → 7.463 ms/frame**, a game thread more than doubling its
work, which is what firing a weapon continuously looks like.

**This is why the run-75 table was an interaction.** The pad mattered because it is how the phantom
reaches the game. The controllers mattered because setting them down is what invents the value.
Neither alone does anything, and no one-factor test could ever have separated them.

### The fix: `TriggerTouchGate`

Pulling a trigger requires touching it. A new action bound to `trigger/touch` **and nothing else**,
per hand, vetoes any analog value with no finger behind it. Where the binding does not resolve the
value passes through unchanged, so a runtime without the sensor keeps the old behaviour.

- **A bigger deadzone was considered and rejected.** Real pulls read a hard 1.00 — nobody lingers at
  partial deflection — so a threshold near 0.6 would work *today*. But the phantom has been seen at
  0.83, and it is not noise: it is a plausible number with no finger behind it. Only the sensor
  separates those. A deadzone would also put a floor under real input for no gain.
- **It is a separate action from `grasp` on purpose.** Grasp is deliberately broad (run 82 widened
  it to every capacitive surface) because it answers "is anyone holding this at all". A thumb on the
  stick must never count as evidence that an index finger is pulling the trigger.
- **The veto count is logged every second.** A run where the judder is gone and the count reads 0
  means something else fixed it. In the confirming run the phantom still occurred on **120/120
  frames** and was suppressed on every one — so the good result is not "it failed to reproduce".

### 🗑️ `AutoPad` is obsolete and now defaults to 0

It worked for the reason now obvious: unplugging the pad is one way to stop the phantom reaching the
game. It cost **1–2 seconds of dead controls on every genuine pick-up** while UE3 re-enumerated its
ports round-robin, and that latency is what reopened this. It is now gone. The code stays in the
build for runtimes where `trigger/touch` does not resolve; `AutoPad=1` restores it.

### ✅ SOLVED (run 175): the runtime switches to HAND TRACKING

**Confirmed by observation:** set the controllers down and the Quest runtime hands input over to
hand tracking. The gun visibly jumps to the user's actual hand. Everything measured then follows:

- The runtime synthesises controller input from hand poses, so **a curled finger reads as a partial
  trigger pull** — which is why the values were *sustained* (0.34–0.43 and 0.23–0.28) rather than
  noisy, and why they differed per hand.
- `isActive` was true on every frame because the runtime genuinely was delivering input.
- The value was an exact `0.00` while held, because that is a real controller reporting a real zero.

> ⚠️ The earlier guess in this section — that the controller sleeping costs the runtime its
> zero-point calibration — **was wrong** and is replaced. It would not have explained the gun
> jumping to a bare hand.

### 🛑 Runs 178–179: `xrGetCurrentInteractionProfile` CANNOT detect it. Built, tested, reverted.

The obvious fix — ask the runtime whether each hand is still bound to the controller profile — **does
not work on this runtime**, and the result is clean enough to be worth never repeating:

```
line  43: left hand is now NOT a controller  - (none - runtime bound nothing)
line 491: left hand is now A CONTROLLER      - /interaction_profiles/oculus/touch_controller
```

**Four transitions, all at startup and pickup.** Setting the controllers down mid-session produced
**none** — first with the `INTERACTION_PROFILE_CHANGED` event, then again with a 500 ms poll to rule
out a missing event. The profile stays `touch_controller` throughout hand tracking; `XR_NULL_PATH`
only appears before controllers have ever been seen.

So the profile is not a usable signal here, and the code was reverted rather than left in place
doing nothing.

### ⚠️ And the scope was wrong, which is the more useful lesson

The user asked, correctly, *"what are you trying to do here? we have a fix already that blocks the
triggers and prevents the framerate tanks."*

Two motivations, neither of which justified three test runs:

1. **Replacing a working heuristic with a more principled one.** The trigger-touch veto is an
   inference, and this file is full of notes preferring real signals — but that instinct was applied
   without asking whether it bought anything. It does not. The veto has worked since run 156.
2. **Stopping the gun snapping to a bare hand.** Cosmetic, visible only when the controllers are
   already down and nobody is playing. The user had already said *"I guess that is ok?"* — which was
   the answer, not a problem statement.

**The framerate tank was already solved.** A solved problem was turned into three more headset runs.
Worth remembering next to the fact that the same session's genuine wins — the video probe, the drift
detector — came from measuring a symptom someone actually reported.

### 📋 What is left, and why it is probably not worth doing

**The trigger-touch veto treats a symptom, and that is fine.** It zeroes trigger *values* only, so
under hand tracking the poses keep arriving and the gun keeps tracking a bare hand. The veto does
not fix that and cannot.

But the remaining symptom is **cosmetic** — the gun follows your hand while the controllers are down
and nobody is playing — and the user's own verdict was *"I guess that is ok?"*. The framerate tank,
which is what actually made the mod unpleasant, has been solved since run 156.

The obvious signal for a proper fix is **dead**: `xrGetCurrentInteractionProfile` cannot see the
hand-over on this runtime (runs 178–179 above, with the evidence). What is left is speculative:

- **`XR_EXT_hand_tracking`** — if the runtime exposes it, actively-tracked hand joints would be a
  direct answer. Unverified, and it needs an extension this mod does not currently request.
- Anything else would be another inference, which is what the veto already is.

⚠️ **The bar for reopening this is high.** Three headset runs went into the profile approach for a
cosmetic gain on an already-solved problem. If it is picked up again, it should be because the gun
snap is genuinely bothering someone in play — not because the veto is a heuristic.

### ⚠️ Method note: the instrument said "no effect" while the user watched the effect happen

Run 154 built a per-thread CPU census bucketed by controller state and reported the two buckets
within 0.88 ms/frame of each other — *on a run where the drop was reproduced, watched on Virtual
Desktop's own overlay, and visible in the per-second log as clean 120.0 and clean 67–70 stretches.*

The label was the broken part. `g_handHeld[0]` read `on desk` for essentially the whole session,
including ninety seconds of deliberately holding both controllers; one sample has a hand moving at
**1331 µm/frame and still reporting `on desk`**. Both buckets contained both states and the
difference averaged to nothing.

**That is the third instrument in this one investigation to hide the answer rather than miss it, and
the second to do it by mislabelling rather than by failing** — run 68's `ORIENTATION_TRACKED` read
backwards, run 74's motion threshold was 10× too low. The shape is identical every time: *a state
label derived from a sensor nobody validated against the state it names.*

The fix generalises. **Do not bucket by what you think is happening; bucket by what you are
measuring.** The census now keys intervals on their own frame rate — FAST ≥ 105 fps, SLOW ≤ 90,
anything between dropped as mixed and counted — and no sensor can corrupt a row because none is
consulted.

Second note, cheaper but real: **script calls per frame nearly doubled in the bad state (57 → 110)
and it meant nothing.** Calls per *second* rose only 11%, and 14.46/8.31 = 1.74 is almost exactly
the calls/frame ratio — script work proportional to elapsed time, i.e. a consequence of slow frames.
Chasing it would have been a sixth dead mechanism.

## Runs 144–151: the HUD in both eyes

**Fixed.** Health, ammo, crosshair, pickup prompts, message text and borders, the pause menu and
the quit confirmation are all drawn into each eye. `HudStereo=1` by default.

### How the UI was found, in four narrowing steps

Each step killed an assumption rather than confirming one:

1. **The draws are `DrawPrimUP`, stride 20, with a vertex shader** — not fixed function. Positions
   like `(5534, 141)` on a 2560-wide target, so neither NDC nor pixels: GFx's own stage space.
2. **The transform is NOT in `c0`–`c3`.** Two HUD draws in the same frame reported different `c0`,
   and a stage-to-screen mapping cannot change between them. Those were scene camera residue.
3. **It is in `c5`–`c8`**, established by watching the game *set* them — `SetVertexShaderConstantF`
   is already hooked, so the registers written between one HUD draw and the next name the shader's
   footprint directly instead of being deduced from what did not change.
4. **The convention is ROW**, settled by offering both as a key cycle. ROW put the message text
   correctly in both eyes; COL produced a white diagonal streak across the frame.

The fix is `ApplyEyeRemap` — the same clip-space squash scene geometry gets — applied to `c5`–`c8`,
with the draw issued once per eye.

### ⚠️ Two guards it must have, both learned by breaking things

- **Exclude fullscreen quads.** Keying on "did the game write `c5`–`c8`" arms for the scene's
  fullscreen composite too, because scene constant uploads overlap that range. Remapping *that*
  takes the already side-by-side frame and puts a copy in each half: **four panels**. The existing
  `UserPtrQuad` classifier separates them; it just has to run FIRST.
- **Exclude offscreen render targets.** Singularity's main menu is **diegetic** — GFx renders it
  into a texture and the game maps that onto a monitor in the scene. Squashing the offscreen render
  put two copies inside the texture, and the monitor faithfully displayed a doubled menu.
  `StereoPair` has guarded this since the shadow-map work (`if (!g_rtIsScene) return draw()`);
  `HudStereoPair` did not inherit it.

**The evidence for the second one was in a screenshot for three exchanges before I read it:** with
the remap OFF the main menu rendered *correctly*. That means it never needed remapping, not that
the remap was broken there. Two hypotheses were spent guessing (a stale viewport, double
processing) before the draw COUNT settled it — 29 remapped draws at the menu against 31 for a
popup that renders fine, so nothing was being processed twice and the difference had to be
elsewhere. **Ask for the number before the third guess, not after it.**

Also dead: an "only orthographic UI may be squashed" guard, added on the theory that the diegetic
menu carried a perspective transform. It has **never fired**. The menu's transform is orthographic
like every other UI element; the render target was the whole difference. Kept as a correctness
guard, annotated so nobody mistakes it for load-bearing.

### Cost, measured

12–16 UI draws a frame in gameplay, 23–31 in menus, against `DRAWS` of 700–1800. Roughly 30 extra
draw calls at worst — noise. That answers whether per-eye UI is affordable, which nobody had asked.

### `HudStereo` raises `HookUserPointerDraws` itself

The UI only reaches us through the user-pointer draws, so with those off the setting would do
nothing, silently — the failure mode that cost runs 133, 137 and 138. It raises the level to 3 and
logs that it did.

> ### ⛔ RETRACTED: run 55's "level 3 is not a win" was measured on the shared link
>
> Run 55 reverted `HookUserPointerDraws` to 0 on frame-time grounds. **The dedicated router landed
> in run 56 — the very next run.** So that decision was taken during the session where `xrEndFrame`
> was hiding a **189 ms** stall inside "our work", against a wireless link that measured 4% and 84%
> of windows over 15 ms within one evening.
>
> This is the fourth conclusion to fall to that link, and the method note two sections up predicted
> it: *"Three separate conclusions were 'confirmed' by A/Bs taken across it and all three had to be
> retracted."*
>
> Measured today at level 3, with ~197 user-pointer draws a frame:
>
> ```
> perf: 120.0 fps (8.33 ms/frame)  user-ptr 197
> perf: 120.0 fps (8.33 ms/frame)  user-ptr 196
> ```
>
> Locked at 120. **Level 3 costs nothing measurable post-router**, and there is no frame-time case
> against `HudStereo`. Found by asking whether run 55 predated the router rather than by re-running
> anything — the dates were already in this file.

### ⭐ Run 152: engine occlusion culling is back on, and it halves the draw calls

`OcclusionQueryMode` now defaults to **2**. Since run 30 the mod forced every occlusion query to
report VISIBLE, because in stereo the boxes were drawn once at full-frame coordinates and the pixel
count was meaningless. That cost half the frame's draw calls for over a hundred runs.

**Nothing in the occlusion code changed.** What changed is that the boxes are now split per eye,
as a side effect of the HUD work:

- run 52 measured them as user-pointer draws — 14,478 of 14,478 query windows held exactly one;
- `UserPtrQuad` classifies a 12-primitive world-space cube as real geometry, so it reaches
  `StereoPair`;
- `HookUserPointerDraws=3` is on by default from run 151, because `HudStereo` needs it.

The override was propping up a problem that had already been fixed elsewhere. Measured, same scene:

| | draws/frame |
|---|---|
| mode 0 (override, everything visible) | 1500–1693 |
| **mode 2 (engine culling live)** | **737–943** |

Lands squarely on run 30's own 50–70% figure. Frame rate is 120.0 locked either way at 1440p —
this is headroom, and it will matter at 4992×2688.

Also retracts run 55's *"auto occlusion mode made it actively worse"*, the third conclusion from
that day to fall.

> ⚠️ **Only half the discriminator is confirmed.** Run 53 wrote it two-sided and both halves must
> hold: *draws/frame falls a lot* ✅ measured above; *nothing VANISHES* is a visual judgement over a
> real play session, not a couple of minutes. Vanishing geometry is intermittent and
> view-dependent, which is exactly why it cost run 30 a whole session. **If objects wink out at
> distance or near the edges of view, `OcclusionQueryMode=0` restores the run-30 behaviour
> exactly.** Shipped on the user's call with that risk stated.

### Audit of the contaminated window (07-31 09:00 → 19:19)

The link was diagnosed at **07-31 19:19** (`86c6cbd`). Every decision taken that day before then was
measured across it. Two were performance-gated; the rest was feature and diagnostic work.

| decision | commit | verdict |
|---|---|---|
| `HookUserPointerDraws` → 0 | `fa02fb1` 09:39 | ⛔ **RETRACTED.** Cited "mean 104 → 94 fps, floor 27 → 17". An fps *floor* of 17 is the link's signature, not a draw-call cost. 120.0 locked today at the same setting |
| **Stage 4B rejected** | `cfff0b5` 10:44 | ⚠️ **Probably holds — but it is the only other one.** See below |

**Stage 4B is better protected than it looks**, which is why it is not retracted here. Its
conclusion is a *differential* with an A→B→A control that agrees to 0.6%:

```
off (control)          8.70 ms/frame   work  5.54   idle 3.09
L2 bind + 1x1 Clear   10.71            work 10.49   idle 0.17
off (control, after)   8.65
```

For the link to have produced that, it would have had to degrade during L2 and recover for the
after-control. The bracket makes that unlikely.

⚠️ **The one caveat:** "work" was a *residual* at that moment — XR submit was not broken out until
`86c6cbd` that same evening — so the +4.95 ms attributed to render-target switching could in
principle contain link latency. Re-pricing it means rebuilding the probe: **`NUMPAD3` has since
been reassigned to pad emulation**, so the Stage 4B instrument no longer exists.

**What did NOT get thrown away.** The day's *code* survived — `UserPtrMinPrims`, the primitive
histogram, and the vertex-range quad classifier are all still in the build, and the HUD fix in
run 151 is built directly on them. Only the *setting* was reverted, and only that setting. The one
`revert:` commit in the window (`71f4212`) is a genuine bug fix, a probe allocating 70 MB
unconditionally, not a performance retreat.

## Runs 139–143: smooth turning, a settings panel, and why it is not in the game's menu

### Smooth turning — `TurnMode=2`

Mode 0 was always labelled "smooth" but it only hands the stick to the engine's look axis, so the
rate is Raven's sensitivity curve — which this mod cannot see or set, making "turn speed" an
unanswerable setting. Mode 2 steps `g_baseYaw` continuously, exactly as snap steps it in jumps:

- the rate is `TurnSpeed` degrees per second regardless of the game's sensitivity,
- frame-rate independent (real elapsed time, clamped so a load hitch cannot fling the view),
- **it keeps working during cutscenes** — `g_baseYaw` never passes through the engine, unlike
  modes 0 and 1.

Deflection is squared for fine control near centre. `TurnSpeed` defaults to 120 (90 was tried and
reported too slow) and is clamped 10–360 — it multiplies elapsed time, so a typo'd 9000 is not a
bad setting, it is an experience the player cannot stop by letting go.

### The VR settings panel

**Hold Y for 1.3 s** — *(shortened to 1 s in run 219; the rest of this entry stands)* — anywhere:
main menu, pause menu, or mid-game. Left stick navigates, values
apply live, the ini is written on close (so a value you corrected never reaches the file).
`WritePrivateProfileString` edits in place and preserves comments.

Only settings consulted **every frame from a global** are in it. Anything read once at device
creation — `D3D9ExMode`, `ZeroCopy`, resolution — would appear to change and silently do nothing
until a relaunch, which is worse than not offering it.

Two things learned the hard way in the same session:

- **The panel drew twice in mono.** The backbuffer is only side-by-side while duplication is on;
  with it off — the state at the main menu, and the most likely place to open the panel — two
  half-width copies read as two panels. Copy count and width now both follow `g_dupDraws`. **The
  state readout had the identical bug** and nobody had noticed, because it is only ever read with
  VR mode on.
- **The `TURNING` row wrapped 2 → 0.** One press past SMOOTH landed in the engine's own turning,
  which was reported as *"I could not move left or right after loading my save"* and looked like a
  broken save. It clamps now, matching the numeric rows, which always did.

### Why the settings are NOT in the game's menu — the Scaleform spike

Time-boxed and stopped deliberately. What it established:

- Singularity uses the **old, thin** UE3 GFx bridge: `GFxMovie` with `Invoke` and `SetVariable*`.
  **No `GFxClikWidget`, no `DataProvider`, no `CreateArray`** — a 2010 build predating Epic's CLIK
  layer.
- So **there is no data-driven option list to append to.** "Add data, not widgets", which would
  have given native-looking rows for free, is dead. The rows exist only in ActionScript.
- The only runtime way in is `Invoke`, which needs Raven's ActionScript API, which means reading
  the menu SWF. `UI_*_SF.xxx` are **chunk-compressed** and `uedecompress` handles fully-compressed
  packages only — `GFxUI.xxx` opened, `UI_SinglePlayer_SF.xxx` did not.

⚠️ **Not established: that the menu SWF lacks an addable row.** Only that reaching it needs a
decompressor for chunked packages, then SWF extraction, then ActionScript decompilation — three
tools before the first line of the feature. That distinction is what the time-box was for.

Reusable from it: `GFxUI.u` extracts cleanly, and `GFxMovie.Invoke`/`SetVariable` are confirmed
reachable if anyone revisits this.

### Pause detection, and why the panel does not use it

Of six candidates (`ShowMenu`, `OpenMenu`, `OpenMenuDelayed`, `IsAnyMenuOpen`, `IsMenuOpen`,
`SetPause`) **only `SetPause` ever fires** — the rest are script→script and invisible to a hook
that only sees native→script entry points. Its bool argument reads cleanly, so `PAUSED` is a
trustworthy state.

**It is not used for the panel.** The main menu reports `PAUSED NO` — correctly, there is no game
running to pause — and the main menu is exactly where someone sets comfort options before playing.
Gating on pause would have locked them out of it. The hold (1.3 s then, 1 s since run 219) needs no
state at all.

Kept anyway: it is cheap, and it tells us whether the world is running.

### Movement direction — head-relative is now the default

`WalkDirection=0`, and it is also the cheaper path: it skips the rotation entirely, because the
engine's movement axes are already relative to `PlayerController.Rotation` and head tracking drives
that. Off-hand (`=1`) is the *added* transform. Exposed in the panel as `MOVE DIRECTION HEAD/HAND`,
which says `HAND (NO TRACKING)` when the left controller is not tracked — selecting it without a
pose would otherwise silently do nothing.

## Runs 133–136: three defects, and one instrument that had been lying

### 1. Cutscene head yaw — cause found, fix known, detection blocked

**Symptom.** In the opening cutscene, looking left and right does nothing. Pitch, roll and leaning
all work.

**Cause.** Head yaw does not reach the renderer through the matrix — it is written into the
PlayerController's `AActor::Rotation` every `Present`, and a matinee-driven camera never reads that.
But the yaw **fold-in** is what actually kills it. That loop exists so mouse and stick turning can
coexist with head tracking: it treats any engine-side change to the controller's yaw as player
input and folds it into `g_baseYaw`. A scripted camera holding yaw at a fixed value is
indistinguishable from that, so every frame it books the head's own contribution as "somebody else
turned" and cancels it. Position and roll survive because they are applied to the matrix directly.

**The 2×2 that proved it** (NUMPAD8 cycles all four; the mode number is derived from the flags, not
stored, so it cannot disagree with them):

| mode | ROT | YAW FOLD | cutscene yaw |
|---|---|---|---|
| 1 | ENGINE | ON | dead |
| 2 | MATRIX | ON | dead |
| 3 | MATRIX | OFF | **works** |
| 4 | ENGINE | OFF | dead |

Mode 4 failing is the important one: the engine does not honour our yaw write during a cutscene at
all, so both halves of the fix are needed.

**Why it cannot ship as a default.** The fold-in is exactly the mechanism the planned
keyboard/mouse and Xbox control schemes will need. Turning it off globally breaks turning for them
before they are written. **Touch stick turning is unaffected either way** — snap turn writes
`g_baseYaw` directly and never passes through the engine.

**Detection — what is dead and what is untested.**

- ❌ **`SetCinematicMode` via the ProcessEvent hook.** All 18 script names resolved (`18 of 18
  against 49615 GNames entries`) and it never fired once through the whole opening. Run 94 already
  explains why: that hook only sees native→script **entry points**. `SetViewTarget` and
  `ClientSetCameraMode` were silent for the same reason. This route is structurally dead, not
  merely untried.
- ❌ **Anything derived from camera yaw vs controller yaw vs written yaw.** The fold-in drags all
  three into agreement, so during a cutscene they are as self-consistent as during play.
> ## ✅ DONE (run 138). Confirmed end to end on a clean launch at `D3D9ExMode=1`: `CINE` reads
> `YES` through the intro, head yaw works, and it drops to `NO` when gameplay starts.
>
> ```
> line  112: script hook: 0 calls named this window   <-- ZERO: the watch is DEAD, not quiet
> line  461: script hook: 5992 calls named this window (0 unnamed), CINE OFF
> line  807: cutscene STARTED - switching to mode 3. Was mode 1, restored on exit.
> line 3537: cutscene ENDED - restoring mode 1.
> ```
>
> **It took three failed runs, and all three failed the same way** — a diagnostic whose "off" state
> is indistinguishable from its negative result:
>
> 1. Run 133: the name table was never resolved, so the watch had nothing to compare against.
> 2. Run 137: the names resolved but the **script hook was never installed** — `RepointScriptHook`
>    only ran on the aim-mode key.
> 3. Run 138: hook installed, names resolved, address identical to the working run — and `fnIdx`
>    was still `-1`, because it is only computed when the census is armed or aim mode ≥ 6. Run 136
>    "worked" solely because the tester had cycled to aim mode 11. **Everything the log reported
>    was true and the feature could not work.**
>
> The heartbeat (`script hook: N calls named this window`) exists so this class cannot recur. Note
> in the trace above that the first named window reports `521 calls (301 unnamed)` — the FFrame
> node-offset vote is still converging there, so **the watch is unreliable in its first window**
> and a cutscene starting that early could still be missed.

- ✅ **`SetCinematicMode` WORKS — the hook was simply never installed.** Run 136 saw it fire cleanly
  (`arg raw 0x00000001` entering a cutscene, `0x00000000` leaving), which is exactly the check the
  frame-layout note asked for. It had failed earlier because the ProcessEvent hook is **not
  installed at startup** — the log says so on line 25 of every run, and `RepointScriptHook()` only
  runs when aim mode 6+ is selected. Run 136 only worked because the tester happened to press
  NUMPAD-dot first. Run 137 installs it automatically once the names resolve, so CINE is live from
  launch, and wires it to switch to mode 3 on the cutscene edge and restore the previous mode on
  the way out. ⚠️ **This is the newest structural change in the build** — a detour on
  `ProcessInternal` from startup rather than from a keypress, in the code run 102 crashed in.
  `CutsceneAuto=0` reverts it with no rebuild, and it is the first thing to suspect if a launch
  starts misbehaving.
- ❓ **`HELD`** (on the readout, and logged as `yaw held:`) — no longer needed as a detector now
  that `SetCinematicMode` works, but kept because it is a free measure of whether the engine is
  honouring our yaw write. The percentage of frames where the
  engine kept the yaw we wrote. Detects the cutscene by its *effect* rather than its name. Near 100
  standing still in play, expected near 0 in a cutscene. **The open question is whether ordinary
  mouse/stick turning drags it low enough to overlap.** Measured only; nothing acts on it.

### 2. `D3D9ExMode=1` corrupts character textures

⛔ **SUPERSEDED — fixed in run 139.** The shadow texture was never marked dirty, so `UpdateTexture`
copied too little and returned `S_OK`. `D3D9ExMode=1` now matches `D3D9ExMode=0` visually; see
STATUS.md. **Do not quote this section as a reason to keep `D3D9ExMode` off** — it was read as
current a week after the fix and used to justify leaving the default at 0, which costs every player
the ~9.8 ms CPU frame copy that zero-copy exists to remove.

The soldier in the opening helicopter renders flat, red and washed out. `D3D9ExMode=0` fixes him
completely. This is not the FOV bug — it does not respond to FOV or resolution.

⚠️ **The wrapper reports itself healthy while doing it**: `shadow failures 0, create failures 0,
GetSurfaceLevel-on-shadowed 0`, no `UpdateTexture failed`. So the instrumentation is watching paths
that are not the broken one. Whoever picks this up should log every lock landing on a shadowed
texture with its format, level and rect, and find which one never reaches the DEFAULT copy.

Not free to work around: `D3D9ExMode=0` also disables zero-copy, which costs ~3.4 ms/frame at
2560×1440 and considerably more at full resolution.

### 3. The vanishing fire and black floor — every culling theory is dead

**Symptom.** At a burning dock, the fire effects vanish and the deck they light renders black.
Worse with wider FOV, worse at lower resolution, worse with distance.

**Eliminated, with the measurement that did it:**

| theory | killed by |
|---|---|
| stereo draw split | black with `STEREO OFF` |
| engine frustum / screen-size culling | `CULL` swept 0.35×–6×; log confirms the engine took the ask (112.5° asked → 108.5° read back), nothing changed |
| hardware occlusion queries | `OcclusionQueryMode=1` no effect — and mode 0 had **already** been forcing visible in every prior test (`overridden to FULLY VISIBLE (15255 so far)`) |
| the texture wrapper | it is FOV-dependent; a dropped upload would not be |
| shadows | a shadow failure cannot delete a particle system, and the fire goes with the floor |
| **the run-21 FOV-inflation mechanism** | see the retraction above |

**The decisive measurement.** View frozen with `]` to the engine's own camera, same save, matched
angles (yaw 88.7° vs 88.8°, pitch −6.2° vs −5.4°), `FOV 100 CULL 100`, TRUE STEREO ON in both:

| | draws/frame | forced frustum | engine FOV | inflation |
|---|---|---|---|---|
| 4992×2688 | **2502**, constant every window | 93.8 × 98.0° | 124.5° | ×2.00 |
| 2560×1440 | **2490–2493** | 91.3 × 98.0° | 122.7° | ×1.97 |

0.4% apart, and the aspect difference (1.86 vs 1.78) accounts for that on its own. Same render
targets in both (HDR `A16B16G16R16F` + LDR `A8R8G8B8`, both split). **The engine submits identical
work at both resolutions and one of them looks broken.**

⚠️ **The loophole in that test, stated so nobody has to rediscover it:** a draw call is issued
whether or not it covers any pixels. An emitter LOD'd to zero particles still counts. So this rules
out primitive *culling*; it does not prove the fire was submitted with fire in it.

> ## ✅ ANSWERED (run 137): it was the texture wrapper all along.
>
> `D3D9ExMode=0` removes the black floor, the vanishing fire **and** the soldier's colours. One
> defect, three symptoms, and the elimination table above was a two-day tour of the wrong subsystem.
>
> **The reasoning error worth keeping.** The wrapper was dropped as a suspect in run 135 on the
> grounds that *"the effect is FOV-dependent and a dropped texture upload wouldn't care about
> FOV"*. That is false: **UE3 selects mips by projected screen size in pixels**, so FOV, resolution
> and distance all change which uploads happen. The one fact that felt like it ruled the wrapper
> out was the fact that most strongly implicated it. It had already been confirmed as a real defect
> on the soldier at that point — an independently-proven bug should not be dismissed on a
> one-line argument.
>
> The identical-draw-count measurement was still right and still useful: the geometry IS submitted
> at both resolutions. It just means the texture is broken, not that the draw is lost.

**The theory that was wrong, kept because the arithmetic that killed it is reusable.** A
screen-space pass reconstructing position from the depth buffer.
`ApplyProjection` forces the frustum by rescaling the matrix's x and y columns and leaving z alone,
so any shader reconstructing view-space position from depth plus screen position is using constants
the engine derived for *its* projection. One mechanism covers everything: soft particles fading to
fully transparent (fire gone), the lighting pass leaving surfaces unlit (floor black),
proportionality to how far we scale x/y (FOV dependence), `ScreenPositionScaleBias` moving with the
buffer (resolution dependence), depth error growing with range (distance dependence), and complete
immunity to every culling knob.

It is **dead**, killed by two sums on numbers already in the logs rather than by another run. The
mismatch between what we force and what the engine's matrix assumes is `0.5 / headroom` — so it
does not vary with PAGE DOWN and does vary with PAGE UP, which is precisely backwards from what was
measured. And it comes out at 0.405 (full res) against 0.419 (1440p), 3% apart, so it predicts no
resolution dependence at all. **Check a theory against the logs you already have before spending a
run on it** — this one cost nothing because the numbers were sitting in `view_matrix.log`.

### The instrument that had been lying, and the ones added because of it

**The on-screen readout was silently truncating.** `TextRects` drops rectangles once its cap is
reached, and four lines across two eyes overran a 1200-rect budget partway through the right eye's
last line. That is why the anchor line read `ANCHOR (F27) R9 U-13` in the left eye and `AN` in the
right — reported as a text-clipping quirk for a whole run when the readout had simply run out of
rectangles. The cap is now a computed bound (21 rects per 5×7 glyph is the true worst case) and it
**logs** if it is ever hit.

That failure has a shape worth recognising, because three separate instruments had it this session:
**an instrument whose not-working state is indistinguishable from its negative result.**

- The readout dropped text instead of saying it was out of room.
- `CINE` read `NO` through an entire cutscene because the name table was never resolved — the
  resolve only ran on the aim-mode or census key, and neither is pressed before the opening
  cutscene. It now auto-resolves once a camera exists, and says so in the log.
- Seven screenshots were taken "at different PAGE DOWN levels" and not one recorded its level, so
  the set could not be read as a comparison at all.

Added in response, all on the readout: `MODE`/`ROT`/`PROJ`, `FOLD`, `CINE`, `FOV`, `CULL`, `HELD`,
`EYE` (per-eye resolution), `DRAWS`, `FREEZE`, `YAW`.

**`]` freezes the view** to the *engine's* camera — no head rotation, no roll, no 6-DOF offset,
stereo separation kept. Pinning to the engine's camera rather than the head pose is what makes two
launches comparable, because the engine's camera comes from the save and a head pose does not. The
frozen yaw **and pitch** are logged, and that immediately earned its keep: the first attempt at the
resolution A/B was 13.7° out in pitch and would have compared two different views.

**`kLogKeep` raised 3 → 12.** Three spares is under an hour of testing. The full-resolution half of
the first resolution A/B had already rotated out by the time the pair was compared, leaving four
logs that were all 1440p.

### ⚠️ Method notes that cost the most to learn

- **Validate the environment before bisecting yourself.** A full day went into bisecting commits
  while the wireless link — an invisible, wildly varying background load — moved underneath every
  comparison. Three separate conclusions were "confirmed" by A/Bs taken across it and all three had
  to be retracted. The identical binary ran at 4% of windows over 15 ms and at 84% within the same
  evening.
- **Measure the black box; do not redesign around it.** The instrument that solved the performance
  mystery took ten minutes and should have been written first.
- **Gating a diagnostic's WORK is not gating its COST.** The Stage 4B probe allocated ~70 MB of
  scene-sized surfaces in `EnsureCopyResources` behind a hotkey nobody had pressed.
- **A cost measured while its benefit is disabled is not a verdict.** Run 48 priced
  `HookUserPointerDraws=3` at 104 → 94 fps and called it worthless, while the occlusion override
  was actively suppressing what it unlocked.
- **Build discriminators, not hypotheses.** A test that separates two whole classes retires more in
  one session than three rounds of argument.
- **Check whether the instrument can report anything at all.** Eight tautological diagnostics have
  been found here — counters that could only ever report the good case.

## ✅ Run 60: the game has a native gamepad path, and it accepts a synthetic pad

**`Singularity.exe` statically imports `XINPUT1_3.dll`, ordinals 2 and 3** — `XInputGetState` and
`XInputSetState`. It is a 2010 console port, so Raven's entire 360 path is already compiled in:
analog move, analog look, buttons, rumble, and **menu navigation**.

So the last rung is not "teach UE3 about controllers". It is **impersonate one Xbox 360 pad**, and
let Raven's own input code supply the dead zones, the acceleration curves and the menu focus
handling — none of which then have to be written or tuned here. Confirmed live.

### The mechanism, from the call-rate log

| State | What the log shows |
|---|---|
| Before a pad is reported | **2 calls/sec, round-robin across all four ports** — that is device *enumeration*, not input |
| After port 0 answers `ERROR_SUCCESS` | **port 0 alone jumps to ~122/sec** (the frame rate); ports 1–3 stay on the slow scan |

The engine **promotes a port to per-frame polling on first successful read**. Two consequences:

1. **Detection costs up to ~4 seconds** (four ports, ~1/sec). A pad that only appears on a keypress
   is therefore absent for the whole main menu — the one place it is most needed. Emulation
   defaults **ON** as of run 61 (`[Input] Gamepad=0` in the ini turns it off).
2. **The per-port call rate is a free, non-tautological instrument.** `port0` at frame rate means
   the engine is reading us as a real input device — and it genuinely reported the *bad* case for
   the first two seconds of the probe run, which is the test eight earlier diagnostics here failed.

### Two traps that were avoided, both silent

- **The exe imports these BY ORDINAL.** There is no `XInputGetState` string in its import table, so
  an IAT patch keyed on the name would have found nothing to patch and reported success. MinHook
  detours the function body inside `XINPUT1_3.dll`, which catches the caller however it resolved
  the address. `GetProcAddress` by name is still the way to *find* it.
- **`dwPacketNumber` must advance only on change.** One that increments every call defeats the
  game's own skip-unchanged optimisation; one that never increments can make the game ignore us
  entirely. Both fail quietly.

**It costs nothing in the frame.** While emulating, the real `XInputGetState` is never called —
polling an absent device is the expensive case and the game was already paying it every frame.
Measured after: **120.0 fps, our work 1.08 ms, XR submit 1.31 ms.** No regression.

### ✅ And engine-driven turning composes with head tracking

The run-12 external-delta fold-in was written for the mouse. Run 60's NUMPAD5 test proved it covers
the **thumbstick** too: with the look stick held over, `head yaw` stayed within ±2° while `want yaw`
swept 152058 → 174083 → 16333, wrapping cleanly through the 65536 space. Body-turn and head-turn
**add**, exactly as VR wants. That was the open question gating the whole input design and it is
answered.

## ✅ Runs 114–132: THE GUN FOLLOWS THE CONTROLLER

**Done and confirmed in play.** Bullet and gun both follow the right controller in 6-DOF, the arms
are hidden, the view never moves, and it costs nothing in culling or LOD. Anchor tuned to
`F30 R9 U-13` and confirmed.

### ✅ Aim decoupling is DONE and confirmed in play

Aim mode 11. Bullet follows the controller in both axes, view perfectly still, culling headroom
stays at 1.0. **The tracer is visible and correct** — the earlier "I can't see the bullet" was the
mirrored yaw sending it somewhere there was no reason to look, and it resolved itself once
`g_yawSign` was applied. Nothing further needed here.

### The first-person pass, and how it was found

UE3 draws the weapon and arms in a **foreground depth-priority group**. The boundary is a
**depth+stencil `Clear` late in the frame**, after which come exactly six draws:

```
Clear ZBUFFER|STENCIL          <- boundary
  #1 5799 verts   #2 3314   #3 390
SetViewport
  #4 5799         #5 3314   #6 390     <- same three meshes, second pass
Clear TARGET                   <- post / HUD
```

| Mesh | Verts | Is |
|---|---|---|
| 1 | **5799** | **the gun** |
| 2 | **3314** | **the arms** |
| 3 | 390 | no visible contribution |

The pass also renders into **its own target** (`0x2B480760`) and is composited, which is why
*hiding* a mesh leaves a black silhouette rather than transparency — the mask has nothing to show.
Moving it has no such problem, because moved geometry is still drawn. Two of the meshes are also
drawn at **draws 2–3** under `SetViewport MinZ 0 MaxZ 0` — a near-plane depth prime — and that copy
must be transformed along with the foreground one or it leaves a hole where the gun was.

### The transform

`VP' = C · VP` substituted per draw, after `ApplyEyeRemap` so stereo survives. `C` is
`v' = (v − G)·R + G + (H − G)`: rotate about the gun's anchor `G`, translate it to the hand `H`.

- **ROW**: registers are rows of M, `clip = v·M`, so `M' = C·M`
- **COL**: `clip.k = dot(reg[k], v)`, so `reg'[k].i = dot(reg[k], C_i)`

`R = Rz(−t)·Rx(roll)·Ry(pitch)·Rz(t + yaw)` — pitch and roll are **conjugated by the view yaw `t`**
because they belong in the view's frame, not the world's.

### ⚠️ Four traps, each of which cost a run

- **The pivot lives in TRANSLATED-WORLD space.** ENGINE_NOTES already recorded that UE3 of this
  vintage pre-subtracts the view origin, so **the camera is at the origin**. Pivoting about
  `g_camPos` flung the gun twenty thousand units away. A pure translation is identical in both
  spaces, which is why the fixed-offset test passed and hid this until a pivot was introduced.
- **A mesh index is not a handle.** The foreground list is rebuilt every frame, and muzzle-flash
  geometry joins it while firing — so index `[1]` stopped being the gun and the gun flashed back to
  normal mid-shot. Meshes are now **latched by vertex count**.
- **The signature table must be double-buffered.** Session-wide, it filled with menu geometry;
  reset per frame, it was empty when the depth-prime draws arrived at the start of the frame and
  hiding left a silhouette. Learn into one list, **match against a copy promoted at end of frame**.
- **A 360° period is a frame error.** "Fine facing forward, droops as I spin, correct after a full
  circle" named its own cause: pitch applied about world Y instead of the view's right axis.

### 🚦 In-headset tooling added, and why it was overdue

Every setting went to a log file, so state could only be read by taking the headset off. That gap
was named early and left alone, and it **cost a run**: "combo 2" meant different signs in two
sessions and nothing on screen could have said so.

- **On-screen readout** — real text (5×7 bitmap, drawn as `Clear` rectangle runs; the SDK ships no
  D3DX). Shows the combo, **the derived signs**, the aim mode, and the anchor with the selected
  axis marked.
- **CAPS LOCK** — cycles 16 states: bits 0–2 flip yaw/pitch/roll, **bit 3 turns the view-frame
  conjugation off**. Signs are found by moving your hand; the frame bit is found by spinning.
- **Arrow keys** — LEFT/RIGHT pick an anchor axis, UP/DOWN move it one engine unit.
- **APPS** — 0 normal · 1 gun moved · 2 arms hidden · 3 both.
- **Hold PAUSE ~1s** — quit. `WM_CLOSE`, not `ExitProcess`, because every copy of the game shares
  one save profile. A hold, because SCROLL LOCK is adjacent and arms the census.
- **Launch straight into gameplay**: `Singularity.exe SP_RL_p` (also `SP_RT_p`, `SP_SL_p`,
  `SP_VIL_p`, `SP_WL_p`). The mod **appends** its resolution args, so parity survives. A fresh map
  may spawn you unarmed — `RvCheatManager` exists if so.

### Current calibration

`GunSignCombo=2` → yaw **+**, pitch **−**, roll **+**, view-frame **ON**. Note this is a *different*
effective pitch sign from the "combo 2" of two runs earlier — set via a **starting combo** key
rather than by changing sign defaults, because changing those renumbers the combos.

### 📋 Known issues, deliberately not chased yet — these are now the top of the list

- ⛔ **SUPERSEDED (run 181) — the second bullet below names the wrong effect. Do not quote it.**
  The smoke that was correctly aligned is the **impact** smoke off the wall, which is spawned in
  world space at the trace hit point and comes out right for free because route 2 rotates the trace.
  The **barrel** smoke was never aligned: it is attached to the first-person weapon, which the engine
  still has at the camera. So the flash and the barrel smoke are one system, they were both
  centre-anchored, and *"what does the smoke ride on that the flash does not"* — which STATUS chased
  for sixty-odd runs — had no answer to find. See the run-181 section.
- **Muzzle flash / smoke still tied to screen centre.** Barely visible in the headset, obvious on
  the monitor. A separate effect that does not follow the gun.
- **Barrel smoke IS correctly aligned** — so the effects are not one system, and whatever anchors
  the flash is not what anchors the smoke.
- **Leftover arm/hand pieces appear during reload.** Extra meshes present only in the reload
  animation, so they are not in the list when the counts are latched.
- **The TMD** (left-hand device, later in the game) will need the same treatment driven by the
  **left** controller. Unverifiable until that point in the game is reached.

## ✅ Runs 103–112: ROUTE 2 WORKS — rotate the trace, not the rotation

**The bullet follows the controller and the camera does not move.** Aim mode 11. The rung that has
been open since run 64.

### The mechanism, and why it could not have been reached by any earlier attempt

`UWorld::SingleLineCheck` at **`0x00A108B0`** — `__thiscall(GWorld, FCheckResult&, AActor*, End,
Start, flags, Extent, +1)`. During a shot, `End` is rotated about `Start` by the hand-minus-head
deviation. **No rotation field is written**, so the view is immune *structurally* rather than by a
fix layered on top — which is exactly what runs 91–102 proved could not be bought by swapping
fields at any timescale.

| Step | How it was found |
|---|---|
| `Actor.Trace`'s native at `0x00AC9E60` | `UFunction::Func` at `+0xAC` — the object model, no disassembly |
| It calls only three functions | byte-scan for `CALL rel32`, printed rather than acted on |
| `0x00BD79D0` is the **result processor**, not the trace | a constant 25° deflection moved nothing, and its struct is zeros and NaNs around an impact point |
| `0x00A108B0` **is** the trace | the call site pushes 7 args with `this = GWorld`; the epilogue is `ret 0x1C` = 7. **Two independent confirmations** |
| It reaches the bullet | mode 12's fixed 25° deflected the shot — the hand taken out of the experiment entirely |

### ⚠️ The hand deviation was biased for fifty runs and mode 1 hid it

Mode 11 first derived the deviation by differencing engine-space values (`g_aimYawUU`, built from
`g_baseYaw` + recentre offset + `g_centreYaw`, against `g_wantYaw`) and got **121° of yaw with
pitch pinned at the ±16000 clamp**. The recentre offsets are only captured when the right
controller is tracked at the instant MENU is held, so a session without a clean recentre carries an
arbitrary constant.

**Aim mode 1 clamps the deviation to 30°, so a biased hand direction still looks like it follows
the hand.** That is why this survived fifty runs. It is now measured directly — hand yaw/pitch
minus head yaw/pitch in the same XR frame, zero by construction when they agree.

### ✅ Confirmed working (run 113)

**Aim mode 11. Bullet follows the controller in both axes, view completely still, culling headroom
can stay at 1.0** — route 2 costs nothing in LOD or draw distance, which was the entire point.

The mirrored yaw was **`g_yawSign`**, not the ini. It is `-1` (XR is right-handed Y-up, UE3 is
left-handed Z-up) and `g_pitchSign` is `+1`, so a deviation published without them comes out
mirrored in yaw and correct in pitch — which is precisely what was reported, and the pairing is
what identified it. Every other conversion in the file already applied them; the new one did not.
**Deriving a quantity without the conversion the rest of the file already uses** is the same class
of error as re-deriving an offset that was already known.

`F5`/`F4` still flip those signs at runtime and the bullet deflection now follows them.

### Still open

- **The tracer.** The impact moves but the visible round does not appear. Cosmetic; `SetFlashLocation`
  is a native→script entry point the hook already sees, so its parameter is reachable.
- **The gun model.** Unchanged and still the separate mechanism — `SetAim` is script→script and
  unreachable, so it needs a direct GObjects write to the `RvAnimNodeAimOffset2D` in
  `ch_inview_arms_animtree`. Unproven, with an ordering risk.

## ⛔ Runs 91–102: route 2's field-swapping is DEAD, and this time it was measured

**The null window settled it.** Mode 10 runs the identical fire window — same functions, same hook,
same nesting, same seqlock, same logging — and writes nothing. The view is **steady** in mode 10 and
**cycles** in mode 9. So the view movement is our write, not recoil and not a race.

**Therefore: every field that carries the aim also feeds the view.** That is run 80's and run 89's
conclusion re-confirmed at microsecond granularity. No window around a shared rotation field can
separate aim from view, whichever field is chosen. Narrowing `AimFieldSet` further is wasted runs.

### What was genuinely won, and it is not nothing

| Finding | How |
|---|---|
| **`ProcessInternal` is at `0x004B7E40`**, `UFunction::Func` at **`+0xAC`** | read out of the object model — every script UFunction holds the same code pointer. No disassembly |
| `0x01308A10` was never it | `Tick` seen **0** times in 48,811 calls through it |
| **`FFrame::Node` is `+0x10`**, `Locals` `+0x1C` | voted over 300 real frames, 100% |
| **The script seam cannot reach the aim.** Only 87 functions cross `ProcessInternal`, all native→script entry points | `GetAdjustedAim`/`SetAim` absent from a census with 0% miss rate |
| **The fire path, named**: `FireAmmunition` 88.9%, `NotifyWeaponFired` 88.9%, `BeginFire` 86.5%, `StartFire` 84.2% under fire vs a 2.9% baseline | the census that finally worked |
| **Seven fields track the view**: ctl `+0x0060`, `+0x05D4`; cam `+0x0060`, `+0x0338`, `+0x035C`, `+0x042C`, `+0x0464` | matched across 4 head orientations ≥5° apart |
| **The aim is in a CAMERA field, not a controller one** | `AimFieldSet=0` (controller only) lost pitch tracking — retroactively vindicating run 80 on `+0x05D4` |
| **Pitch decoupling was observed working** | run 99, all seven fields written |

`RvGame.xxx` is the game's `.u` script package. `tools/uedecompress` + `tools/uepkg` read it, and
that is how the fire path was named rather than guessed. Both validate their own layout.

### ⚠️ Method notes, and they are the unflattering kind

- **Five of these runs were lost to my own defects, not to the problem.** A guessed offset adopted
  from six samples during level load; a code-range filter that spanned `.rdata` so a shared vtable
  outvoted the real answer at 94% vs 55%; an argument count (`ret 0xC` vs `ret 8`) that crashed the
  game twice; a literal `%` in a `Log()` string; and a hotkey installer that was not idempotent and
  nulled a live trampoline on the second press.
- **Three builds chased "the camera moves when I shoot" without ever running the control.** The
  fix for it (a seqlock) was correct on its own terms and aimed at the wrong mechanism. The control
  cost one mode and settled it immediately. **This is the run-56 lesson for the third time.**
- **Read the epilogue, not the decompiler.** `ret 8` vs `ret 0xC` was checkable at a desk in one
  minute and instead cost two crashes.
- **A filter wide enough to admit the wrong KIND of thing gets won by the wrong thing** — and it
  presents as a *stronger* result. 94% agreement looked like confirmation and was a vtable.

## ✅ Run 91 (desk, no headset): route 2 is NOT dead — the game ships its own UnrealScript

**`RvGame\CookedPC\*.xxx` are the `.u` script packages.** They are whole-file LZO-compressed UE3
packages, and nothing in this project had ever opened one. `tools/uedecompress` unpacks them;
`tools/uepkg` reads the name / import / export tables. Both are host tools, zero headset cost.

### The answer route 2 spent six runs probing for

```
Function  RvWeaponShared.GetAdjustedAim                  super=Engine.Weapon.GetAdjustedAim
Function  RvGameSharedPlayerController.GetAdjustedAimFor super=Engine.PlayerController.GetAdjustedAimFor
Function  RvTacticalPawn.GetBaseAimRotation              super=Engine.Pawn.GetBaseAimRotation
Function  RvWeaponShared.CalcWeaponFire                  super=Engine.Weapon.CalcWeaponFire
Function  RvWeaponShared.InstantFire                     super=Engine.Weapon.InstantFire
```

The stock UE3 aim chain, every link overridden by Raven and every one a **script** function with
real bytecode — so all of them pass through the interpreter, which is the seam this mod already
has a hook on. `GetAdjustedAim` is the one that returns the rotator the fire trace uses.

### Why this reopens route 2 rather than just informing it

`GetAdjustedAim` **returns** the aim. So the hook does not have to swap `Rotation` and race the
camera at all — it calls the original and overwrites the returned `FRotator` with the hand's. The
separation is exact by construction: culling, LOD, movement basis and the view never observe the
hand for even an instant. The run 76–89 finding that killed the time split — *"there is no instant
where the hand can sit in that field without the view taking it too"* — is true and **does not
apply**, because this touches a return buffer, not the field.

### ⚠️ The last blocker is an unapplied one-line fix, not an unknown

The census at `Hook_ProcessEvent` still buckets names on `Function + OBJ_NAME`, where `Function`
is arg1 — which **run 86 proved is an `FFrame`, not a `UFunction`**. That is why run 84's names
came back as heap pointers. Run 87 found the right source and wrote it down four lines away in the
same file: `+0x14 -> 'SeeMonster'`, i.e. `FFrame::Node`. The fix was found and never applied.

Also: arg2 (`Parms` in the current signature) is the **Result** pointer, not a parms block —
`ProcessInternal(FFrame&, RESULT_DECL)` under `__thiscall` puts them in that order. Confirm before
relying on it.

### Method: three checks that would have caught a wrong parse

Both tools validate their own layout instead of trusting it, because a package parser that cannot
tell "layout right" from "layout wrong" prints fiction with total confidence — the same defect
class as the eight tautological diagnostics already found here.

| Check | Result on RvGame.u |
|---|---|
| Name table must end **exactly** at `importOffset` | exact |
| Every name / outer / serial index in range | 0 bad of 36,873 |
| Header slack must equal `4 × exportCount` (empty depends map) | exact — pins the export struct size |
| Serial regions must **tile** the file | 0 gaps, 0 overlaps |

The tiling check is the one that earns its keep: wrong `SerialSize`/`SerialOffset` fields still
look plausible one at a time and cannot possibly tile 36,873 regions.

For the decompressor the equivalent is the **size**: LZO desynchronises within a few hundred bytes
if a shift is wrong, so 214 consecutive chunks each landing on its declared uncompressed size is
proof, not encouragement. `Core.xxx` -> 198,749 bytes, which is `Core.u`'s size, is the control.

**Do not commit `uscript/`** — it is decompiled game content and already in `.gitignore`.

## ✅ Run 90: aim decoupling WORKS via route 1 — culling headroom

**Aim mode 1 (engine follows the hand, 30° clamp) + `PAGE UP` to 4× culling headroom.** Reported:
4× "pretty good", 6× maybe better, LOD pop-out visible but "worth the trade off".

So the rung is reachable. `askDeg` governs culling only — `ApplyProjection` forces the rendered
frustum separately — so widening it buys coverage without touching the image.

`PAGE UP` now runs **upwards** first (1.0 → 1.30 → 1.75 → 2.5 → **4.0** → **6.0**, then the
sub-1.0 values), because reaching 4× used to take eight presses in a headset.

### Why the LOD cost cannot simply be switched off

It is not a side effect, it is **the same act**. UE3 sizes objects on screen from the projection it
derives from `mCurrentPOV.FOV` — the value we inflate. A wider FOV makes everything compute as
smaller, which both drops meshes to lower LODs *and* brings their cull distance nearer. Seeing more
of the world and shrinking apparent object size are one operation, so there is no setting that
keeps one and drops the other.

Three ways to address it, ranked by cost:

1. **Pay less of it — free, no code.** The headroom needed scales with the deviation clamp. At 30°
   it takes 4–6×; at 20° much less. `NUMPAD9` cycles the clamp, `PAGE UP` the headroom. Tuning the
   pair is the cheapest available win and needs no new mechanism.
2. **Compensate the screen-size computation — bounded RE.** UE3 computes bounds screen size from
   its CPU-side projection matrix. Find that and multiply the result back by the headroom factor,
   and LOD and cull distances are exactly restored while the frustum stays wide. This is the
   *correct* fix, and unlike `ProcessEvent` it is a bounded target — one function, one multiply,
   non-virtual. Still RE with no guarantee.
3. **Config knobs — checked, they do not exist.** Only `DecalCullDistanceScale`,
   `FractureCullDistanceScale`, `SkeletalMeshLODBias`, `ParticleLODBias`,
   `FoliageDrawRadiusMultiplier`. No global draw-distance or LOD-distance scale.

⚠️ **"Pops out" and "loses detail" are different symptoms with different fixes.** The first is
distance culling, the second is LOD selection. `SkeletalMeshLODBias` could address the second and
only for characters, not static meshes. Worth identifying which one is actually being seen before
spending anything on it.

## ❌ Runs 76–89: aim decoupling — route 2 is dead, and route 1 is still untested

**Goal:** `PlayerController.Rotation` drives the view, the culling frustum, LOD, the movement basis
*and* the weapon's fire trace. Aim decoupling needs the last of those to follow the hand while the
rest follow the head.

### What was established, and it is worth keeping

| Finding | How |
|---|---|
| **There is no second rotation field.** `+0x05D4` deflects nothing | run 80, constant −25°/+25° written to it, shot went straight |
| UE3 agrees: a player's aim is `Pawn.GetBaseAimRotation()`, which returns that same `Rotation` | — |
| **The fire path DOES go through UnrealScript** — 3 functions at 77–89% enriched under fire vs a 28% baseline | run 84 census |
| The bytecode **interpreter** is at `0x012A72C0` (15,209 bytes, `CallFunction` inlined) | the `"Error: CallFunction - '%s' is not a function"` string |
| `0x01308A10` is `ProcessInternal` or an op handler — it takes an **`FFrame`**, not a `UFunction` | its arg is a stack address with an `FOutputDevice` vtable |
| **`ProcessEvent` was never found.** Four passes | it is virtual, so no static call graph reaches it |

### ⛔ The time split is finished, and not for want of another seam

Modes 4 and 5 wrote the hand into `AActor::Rotation` at two different instants (Present, and the
XInput poll) and let the camera update overwrite it with the head. Both failed — and the reason
came from *watching*, not from a bullet hole: **the view was driven by the head and the controller
at once, spinning wildly.**

So the camera does not sample the rotation once at a point we can write after. It consumes it at
more than one moment in the tick, and **there is no instant where the hand can sit in that field
without the view taking it too.** The premise the whole idea rested on is false, so there is no
sixth variant worth trying. Modes 4 and 5 are cut out of the `NUMPAD .` cycle (0–3 now) but left in
the source as the record.

### What remains

1. **Route 1 — widen the culling frustum. STILL UNTESTED, one run.** `askDeg` governs culling only,
   because `ApplyProjection` forces the rendered frustum separately, and `PAGE UP` now reaches 4×
   and 6×. Aim mode 1 + enough headroom either stops the ceiling going black or shows the pop-in
   cost. This is not the engine being *wrong* about where you look — it is the engine being
   *conservative*.
2. **Find the fire trace itself in Ghidra.** The honest version of route 2, and a real
   reverse-engineering job rather than a probe.
3. **Accept coupled aim.** The mod is fully playable without this rung.

### ⚠️ Method notes, and they are unflattering

- **Six runs went into hunting `ProcessEvent` and three of them were lost to my own instrument
  bugs**, not to the problem: a filter that hid the data it existed to show (printed "(nothing)"
  from a full table), a probe placed behind a switch the test was told not to press, and an
  identification over-read from `ret 0xC` — which proved the ABI but could never have separated
  `ProcessEvent` from `UFunction::Invoke`.
- **The cheap alternative went untested the whole time.** Route 1 has cost zero runs and could have
  ended this at any point. Preferring the interesting path to the cheap one is the actual error
  here, and it repeated for six sessions.
- **"Shots followed my gun" was ambiguous and I nearly built on it.** In mode 4 the view and the gun
  are the same rotation, so that phrase covers both success and failure. What settled it was the
  user reporting the *view spinning* — a symptom nobody had asked for.

## ⛔ Runs 67–75: the judder was an INTERACTION — SUPERSEDED by runs 154–156, which named the cause

> **Read runs 154–156 first.** The interaction is real and every number below still stands, but the
> mechanism it could not name is now known: a set-down controller reports a phantom trigger pull,
> the game is told to fire and aim, and it does. The pad mattered only because it is the path the
> phantom takes to the engine. **The `AutoPad` fix described below is obsolete and now defaults to
> 0.** Kept in full because the method notes at the end are the most valuable part of it.

**Symptom:** judder and heavy frame loss whenever the controllers sat on the desk; fine once they
were picked up. Nine runs, five dead theories.

**The measurement that settled it** — three buckets, cumulative over a whole session, draw counts
controlled:

| pad | controllers | fps | residual | missed | draws |
|---|---|---|---|---|---|
| off | on desk | 109.3 | 4.13 | 2.1% | 1316 |
| **ON** | **in hand** | **117.9** | **4.14** | **1.0%** | 1563 |
| **ON** | **ON DESK** | **71.9** | **10.40** | **18.7%** | **1148** |

**Neither factor is the cause alone.** A connected pad while the controllers are held is fine. The
controllers on the desk with no pad is fine. Only both together break it. The confound is
controlled the right way round for once: the bad row has the *fewest* draws and is still 40 fps
slower.

**This is why five consecutive theories died.** Every run before this tested one factor at a time,
which is a question with no answer when the effect is an interaction — so each mechanism looked
plausible in isolation and each was eliminated:

| Theory | Killed by |
|---|---|
| OpenXR input calls cost too much | all four phases timed at <0.1 ms peak |
| Sleeping controllers stall the runtime | haptics gated on tracking: no change |
| `dwPacketNumber` churn makes UE3 re-process input | deadzone cut churn 20×: no change — **and the controls still worked with the packet frozen, so the engine never reads it** |
| 900 XInput polls/sec | that was the *loading screen* — 0 draws, 27 fps. In gameplay it is 1/frame |
| `bSmoothFrameRate` governor holds the rate down | neutralising every render-thread sleep: no change |

### The fix: auto-pad

Unplug the virtual gamepad when neither controller has moved for 3 s; plug it back in the instant
one moves. `[Input] AutoPad=0`, `AutoPadStillFrames` to tune. Confirmed over a full session: no
judder.

**The mechanism is still unknown**, and that is worth stating plainly rather than dressing up. This
is a workaround built on a correlation that survived every test, not an explanation that passed
one.

- **"Asleep" was always the wrong question.** A controller can be awake, delivering input, and
  still be on a desk — proven by a trigger firing accidentally on the desk without helping. The
  distinguishing signal is **motion**: in hand measures 878–10359 µm/frame, a resting desk reads 0.
- **The timeout stays at 3 s.** The costs are asymmetric — a premature unplug costs 2–3 s of *dead
  controls* mid-combat while UE3 re-enumerates (run 60: round-robin, ~1 port/sec), which is worse
  than 3 s of judder. Both controllers must be still, so aiming with one hand steady keeps it alive.

### ⚠️ Method notes from nine runs of not converging

- **A one-factor test cannot find an interaction**, and it does not fail loudly — it produces a
  plausible mechanism every time, which is exactly what happened five times running here.
- **Two instruments were themselves broken**, and both hid the answer rather than merely missing
  it. `NUMPAD/` forced the controller-state flags to false, making the one row the experiment
  needed physically unrecordable. The motion threshold was 10× too low, so every sample read
  "in hand" and the correlation table collapsed to a single row.
- **`ORIENTATION_TRACKED` reads backwards from intuition.** A controller on a desk is in clear
  camera view and reports tracked; one in your hands sits below the camera cone and does not. A
  table was read exactly wrong because of it.
- **Fixing the mechanism you blamed and not fixing the symptom means it was not the mechanism.**
  The deadzone worked perfectly and changed nothing.

## 🚨 Run 67: the CONTROLLERS are an experimental variable, and every test in runs 64–66 ran inside it

**Reported symptom:** the image judders and the frame rate drops whenever the head moves — always,
from launch — and it **stops the moment the controllers are picked up**, returning when they are
set down and the keyboard is used instead. Touch controllers sleep after a few seconds of
stillness, so "set down" and "asleep" are the same state.

**Why this contaminates everything above it:** every hotkey in this mod is on the keyboard. Pressing
NUMPAD8, NUMPAD9 or NUMPAD\* *requires putting a controller down*. So every controller-related test
in this session was performed in the degraded state, and none of the results separate the thing
being tested from the state required to test it.

### ⚠️ Retracted pending re-test

- **Run 66's conclusion is UNSAFE.** "The split is not a no-op and the engine is smoothing our
  written rotation" was inferred from a strobe that may have had nothing to do with the split. The
  smoothing hypothesis is still plausible — `mDesiredRotationInterpRate` is real — but it is
  **not evidence** until the controller state is held fixed.
- **Run 64's "geometry disappearing behind me"** is more likely genuine, because culling is
  direction-dependent in a way judder is not. Still worth re-confirming.

This is the **run 56 lesson, word for word**: validate the environment before bisecting yourself.
Three conclusions were retracted last time for exactly this reason, and the identical binary
measured 4% and 84% of windows over 15 ms across one evening. **The instrument that settles it is
the frame budget that already exists** — `XR submit` separates the link and the compositor from our
own work, and it has been in every log since run 56.

### What was added to separate it

- **`NUMPAD /`** turns *all* of this mod's XR input work off — no `xrSyncActions`, no pose
  locations, no haptics. If the judder is unchanged either way, it is not our input code.
- **Controller state is logged on the frame-budget cadence** (`left TRACKED / asleep`, sync-fail
  frame count), so one run with the controllers picked up and set down a few times shows the
  correlation directly rather than depending on anyone remembering when they did it.
- **Haptics are now gated on `ORIENTATION_TRACKED`,** not merely `VALID`. A set-down controller
  keeps reporting a valid last-known pose long after it stops being tracked, so the valid bit alone
  cannot tell a live controller from a sleeping one. This also rules out a plausible cause on its
  own terms: the mod asks for haptics every time the game rumbles, which in combat is constantly.

## ✅ Run 61b: Raven's 360 layout, read out of the game's own config

`RvGame\CookedPC\Coalesced_INT.bin` is UE3's coalesced config archive and it carries the shipped
binding table as UTF-16 text. Extract the strings, grep for `XboxTypeS`, and the whole layout falls
out — no guessing, no probing:

| 360 | Command | 360 | Command |
|---|---|---|---|
| RT | Fire | LT | Aim |
| A | Jump | B | Crouch |
| X | Use / reload | Y | **CycleWeapon** |
| LB | AgeYoung (TMD) | RB | Impulse (TMD) |
| L-stick click | Dash | R-stick click | AntiGrav (TMD) |
| **D-pad up** | **Health** | D-pad down | Flashlight |
| START | ShowMenu | BACK | Scoreboard |

Every binding routes through `PressChainButton CB_x | OnRelease ReleaseChainButton CB_x`, so the
engine wants a **press and a release** — a one-frame pulse can land between its own input ticks.
D-pad left/right are bound to generic `CB_DPAD_LEFT/RIGHT` with no gameplay command behind them.

Two facts that changed the mapping rather than merely confirming it:

- **D-pad up is the health item.** That is what run 61's weapon-switch mapping actually hit.
- **There is no next/prev weapon, only CYCLE.** One command, and the mouse wheel is bound to it in
  *both* directions. A two-way stick gesture was wrong by construction, so the stick's other
  direction is now free and carries health.

### ⚠️ The probe could confirm a binding but could not refute one

Fourteen buttons were pressed one at a time. Four reported something — A, B, RB, START — and the
other ten read as "nothing happened". **All ten of those readings were uninformative.** Use,
CycleWeapon and the three TMD powers do nothing without a target, a second weapon or a charge, and
D-pad up read as nothing *at full health*, having demonstrably used a health item ten minutes
earlier.

So the instrument could only ever report the positive case — the ninth instance of that trap in
this project, and the first one that was not caught before the run. The config file was the better
instrument and it was sitting in the install the whole time. **Look for the game's own answer
before building something to infer it.**

### The VR mapping that follows

| VR input | 360 | Action |
|---|---|---|
| Right trigger / left trigger | RT / LT | Fire / Aim |
| Right A · B · grip · stick-click | A · B · X · R-stick | Jump · Crouch · Use · AntiGrav |
| Left X · Y · grip · stick-click | LB · D-pad down · RB · L-stick | AgeYoung · Flashlight · Impulse · Dash |
| Left menu | START | Menu |
| Look stick up / down | Y / D-pad up | Cycle weapon / Health |
| Look stick left / right | — | snap turn, ours (steps `g_baseYaw` directly) |
| Move stick | L-stick axes | move (HMD-relative by construction) |

## ✅ Resolution — inherited from the headset, automatically (design run 17, built run 59)

Manual `-ResX=`/`-ResY=` was a development crutch for forty runs. It is gone.

OpenXR's `xrEnumerateViewConfigurationViews` reports a recommended per-eye rect that **already
includes whatever render-scale is set in Virtual Desktop**, so honouring it gives exactly the
behaviour every other VR title has. The mod queries it every run, caches it to the ini, and the
next launch detours `GetCommandLineW`/`A` to append the computed size.

```
auto-resolution: injecting -ResX=4992 -ResY=2688 -windowed (cached from the headset last run)
    requested 4992x2688 -> got 4992x2688   *** ACCEPTED ***
```

**Your command line always wins.** Pass `-ResX` yourself and nothing is injected:

```
auto-resolution: not injecting (you passed -ResX yourself, which always wins)
```

**Why `DllMain`.** The DLL is a `d3d9.dll` proxy, so it is a static import of the exe: the loader
runs `DllMain` *before* the exe's entry point, and the CRT reads `GetCommandLineW` inside that entry
point to build `WinMain`'s `lpCmdLine`. Any later hook point is too late — by the time the first D3D
export is called, UE3 has already parsed its arguments.

**Why a cache, not a live query.** A temporary XR instance in `DllMain` is possible (the view-config
query needs no session or graphics device) but puts XR initialisation in the process startup path,
where a failure means *the game does not launch*. The cached size is one run stale after a
render-scale change; that is a far better failure mode. Every failure path leaves the command line
untouched.

**Why not the backbuffer** (run 14): the resolution has to reach UE3's own system settings. Forcing
the D3D9 backbuffer only desynchronises it from the engine's scene targets and breaks stereo.

**Keep the aspect right.** Per-eye should stay near the headset's `2496/2688 ≈ 0.93`. A taller frame
at a fixed width narrows the horizontal field instead — 4096×2688 renders 82.5° against 95° at
4096×2160 — and the shortfall shows as **black bars at the sides**. `ApplyVrFov` anchors on the
headset's vertical FOV and derives horizontal from the frame's aspect, so shape matters as much as
pixel count.

## ⛔ SUPERSEDED — Ladder status (against the original plan)

> **Do not read this table as current state.** Its last row says controller input is
> `⬜ not started`; controller input has been working since roughly run 114 — full Touch mapping,
> menus, haptics, snap turn, off-hand movement, and aim decoupling via route 2 (aim mode 11).
> The live version of this table is the TL;DR in `STATUS.md`.
>
> Kept because it records what the *original plan's* rungs were and which run closed each one,
> which the TL;DR no longer tracks. The per-rung run attributions are accurate; only the last
> row's status is wrong.
>
> Its **"Measured, so it does not have to be re-derived"** subsection was NOT stale and has been
> promoted into `STATUS.md` as its own section.

| Rung | State |
|---|---|
| Head-tracked image in the headset | ✅ done |
| Colours — sRGB | ✅ done |
| **6-DOF positional tracking** | ✅ **done** — camera position solved via the view matrix |
| **Stereo — per-eye offset for real depth** | ✅ **done** — true native stereo by draw-call duplication; eyes align, depth is correct, and the vanishing objects are fixed (run 30, occlusion query readback) |
| FOV / aspect match | ✅ **done (run 58)** — projection is VR-correct and forced, and `-ResX=4992 -ResY=2688` delivers **100% of the headset's pixels** at 120 fps. Run 33's "the cap is 4096" was never re-tested after the D3D9Ex device landed in run 38 |
| **Head roll** | ✅ **done (run 59)** — applied in the view matrix, not the engine's rotation; `NUMPAD1` toggles, `NUMPAD2` flips the sign. Maths verified as arithmetic in `spikes/roll_math` |
| Performance | ✅ **done (runs 38–39, 56)** — zero-copy removed the 9.8 ms CPU round-trip; a **dedicated router** removed a 189 ms `xrEndFrame` stall. **120 fps locked, 4.35 ms idle at full parity** |
| **Resolution inherited from the headset** | ✅ **done (run 59)** — `GetCommandLineW` detour, cached from the headset's own query. No command line needed |
| Controller input, menus, aim decoupling | ⬜ **not started** — the only remaining rung (mouse/stick coexistence fixed run 12 as a prerequisite) |

**Every rung is done except controller input.** Stereo, 6-DOF, roll, parity resolution and 120 fps
all hold simultaneously.

## ⛔ SUPERSEDED — Where the project is

> **Do not read this as current state.** It says **3-DOF** head tracking; 6-DOF was solved
> 2026-07-29 and has been working since. The live description is the TL;DR in `STATUS.md`.
>
> Its one live fact — that `spikes/view_matrix/` is the current build and `spikes/vr_render/` is
> the known-good fallback — has been promoted into `STATUS.md` under *Quick start*.

A **working, wearable VR mod** for Singularity (2010, Unreal Engine 3, D3D9, x86):

- The game renders into the Quest via OpenXR, with correct colours
- **3-DOF head tracking** drives the in-game view (pitch and yaw)
- Ships as one `d3d9.dll` dropped beside the game exe — no game files modified

Current build: **`spikes/view_matrix/`** (spike 9) — everything below plus the camera-position
probe. **`spikes/vr_render/`** (spike 8) is the known-good fallback if spike 9 misbehaves.
Earlier spikes are kept deliberately; several document approaches that did *not* work, which is
most of their value.

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

### ⛔ SUPERSEDED — ⏭ Next actions

> **Every item below is done or retracted**, and the numbering (1, 2, 2, 3, 4, 5, 3, 4, 5) shows
> it was appended to across several runs without ever being reconciled. Parity at 4992×2688,
> D3D9Ex zero-copy, and the resolution match all shipped. The live to-do list is
> *Next session, in order* in `STATUS.md`.

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

## ⛔ SUPERSEDED — Other known work, roughly by value

> **The most misleading section in the project, and the reason this file has banners at all.**
> Five of its six items are done, and the first is stamped `✅ RESOLVED` while being *retracted* —
> it asserts a 4096 resolution cap that makes parity impossible and caps side-by-side at 58% of
> the headset's pixels. Run 40 overturned that (`the 4096 cap is UE3's, NOT the GPU's`) and the
> mod ships **100% parity at 4992×2688**. Run 33 measured the refusal on the plain D3D9 device,
> before the Ex upgrade landed in run 38.
>
> | Item below | Reality |
> |---|---|
> | "✅ RESOLVED (run 33): the cap is 4096" | **Retracted by run 40.** 4096 ✅ 4608 ✅ 4992 ✅ |
> | D3D9Ex + MANAGED wrapper, "do it when needed" | Built, runs 38–39 |
> | FOV / aspect mismatch | Done, run 58 |
> | Head-look vs aim are coupled | Done — aim mode 11, route 2 |
> | Menus are inert | Done, run 151 |
> | GOG install crash | **Still live** — and covered in fuller detail in `ENGINE_NOTES.md` |

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

