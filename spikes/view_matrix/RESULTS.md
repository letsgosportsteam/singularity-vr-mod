# Spike 9 — the view matrix on its way to the GPU

**Status: ✅ WORKED.** Camera position is controllable. Run 2026-07-29.

Attacked the one blocking problem: camera **position**, which gates both stereo (a per-eye
offset *is* a position offset) and 6-DOF.

## Result

```
--- scan complete: 18 candidate window(s), camera (18992.2, -22941.2, 180.0)
    c0   ROW  w=-0.0020  fwdLen=1.0000  hits=50   zero at origin(translated-world)
```

`c0`, ROW storage, cancelling to two thousandths of a unit with a unit-length forward vector
and 50 uploads in the scanned frame. Pressing F3 moved the view. **Position is solved.**

Two things the run taught us beyond the headline, both fed back into this build:

**1. The engine renders in translated-world space.** Every match landed on the *origin* probe
point, never the camera's world position — UE3 pre-subtracts the view origin on the CPU to
protect float precision at large world coordinates. Probing `(0,0,0)` as a third point is the
only reason the scan found anything at all.

**2. At the origin, the zero test degenerates.** It returned 18 candidates, most spurious,
several with `w` of exactly `+1.0000`. The reason: `clip.w` at `p = (0,0,0)` collapses to
`r[3].w` under *both* storage conventions, so the test cannot tell ROW from COL and admits any
matrix with a near-zero w constant — which is most of them. The zero-cancellation argument only
carries information when the probe point is far from the origin.

Fixed by adding an independent second test: for a genuine world→clip matrix the xyz of the w
term **is the camera's forward axis**, and the camera's real facing is known from its
`FRotator`. Requiring agreement (`dotFwd >= 0.99`) rejects the unrelated matrices *and* resolves
the storage convention. Run 2 shows it settling the convention on identical bytes:

```
** c0  ROW  w=-0.0020  dotFwd=+1.0000  hits=50    <- the view matrix
   c0  COL  w=-0.0020  dotFwd=+0.0023  hits=9     <- same bytes, wrong convention
```

**3. Tolerance has to depend on which probe point matched.** Run 2 still passed three extra
windows (`c8`/`c11`/`c14` COL, `w` ≈ −10.5, 4 uploads each) beside the real one at −0.0020 with
50. Their 3-register spacing suggests a block of `float3x4` transforms the sliding window
straddles — injecting into those would corrupt something unrelated for no benefit.

The fix follows from what the tolerance is *for*: absorbing the one-frame staleness of the
camera position we substitute. That applies to the world-position probes; it does **not** apply
to the origin probe, where the test is just "is `r[3].w` near zero" — pure matrix content, no
camera reading, nothing to go stale. So the origin probe now uses a tight **1 UU** tolerance,
which separates −0.002 from −10.5 decisively, and only `c0 ROW` should survive.

**The band is measured, not eyeballed.** Every frame, the backbuffer's edge columns are scanned
for near-black and the width is logged once a second alongside the offset that produced it — so
the log correlates the two without timestamps or anyone having to remember which step they were
on. Reading the game's own frame also sidesteps a real methodological trap: the artefact lives
in the backbuffer, so judging it through the headset would mean judging it through the aspect
mismatch and the lens distortion too, neither of which will exist in the final product.

**The culling band turned out to be a non-issue.** The game culls geometry on the CPU against
its *original* frustum, so anything newly visible after the offset was never submitted to the
GPU — moving the matrix cannot conjure draw calls that were never issued. Measured:

| Offset | Band |
|---|---|
| 300 UU | 35–73 px of 2560 (1.4–2.9%), one 721 px spike |
| **100 UU** | **0 px** |
| 30 / 10 / 3 UU | 0 px |

Gone entirely at 100 UU and below, and a real IPD is a few UU — so **stereo needs no mitigation
and the FOV-widening job is dropped.** One caveat on the metric: it counts near-black columns,
so a dark scene reads as a band (a 502 px reading appeared with injection *off*). That failure
mode only inflates the number, never deflates it, so the zeros are trustworthy.

## Why this approach

Three attempts to reach position through the engine's object model all failed (spike 8,
`ENGINE_NOTES.md`):

1. Writing every candidate position field — no effect at all
2. A hardware write breakpoint on the camera's `Location` — only generic actor-move plumbing,
   shared with every actor in the game, reached from a dozen call sites
3. `FUN_0104e420`, adjacent to the rotation source — turned out to be rotation

Rotation resolved cleanly because it had **one** interceptable source. Position does not.

So stop asking the engine and intercept the *result*. UE3 is shader-based: the world→clip
matrix reaches the GPU as vertex shader constants, and moving the camera is just a change to
that matrix. This bypasses the object model entirely, is how vorpX and 3Dmigoto do stereo, and
solves stereo *and* 6-DOF with one mechanism.

## How the matrix is identified

Not by guessing register numbers. The test is geometric and self-verifying.

For a world→clip matrix, `clip.w` is the **view-space depth** of the transformed point — so
the camera itself must transform to `w = 0`. We already know the camera's world position from
the object model, so substitute it into every candidate 4-register window and keep the ones
that come out at zero.

The camera sits ~20,000 UU from the world origin. A window that cancels that to under 25 UU by
chance is vanishingly unlikely: **the near-cancellation is the identification.**

Three things make it robust:

| Guard | What it rules out |
|---|---|
| Forward-vector length must be 0.3–3.0 | Degenerate blocks whose `w` term is all zeros pass the zero test for *every* point and mean nothing |
| Both **ROW** and **COL** storage tested | D3D9 code uploads matrices either as rows or transposed (`D3DXMatrixTranspose` + `mul(M, v)`); assuming one would miss the other |
| Three probe points tested | See below |

The three probe points are the camera actor's `Location`, `mCurrentPOV`'s location, and
**the origin**. The last one matters: UE3 of this vintage often uploads a **translated-world**
to clip matrix, pre-subtracting the view origin on the CPU to protect float precision at large
world coordinates. In that space the camera sits at the origin, and a world-position test would
be blind to it. Probing `(0,0,0)` as well costs nothing and covers that case in the same scan.

The 25 UU tolerance is deliberately loose. The camera position is read at `Present`, so it is up
to one frame older than the constants being scanned, and a player moving at speed covers ~10 UU
in that frame. **Stand still while scanning** and the reported `w` will show how well it really
cancelled.

## How the camera is then moved

Pre-multiply a world translation into the matrix — `M' = T(-o) · M`:

```
ROW: row3 -= o.x*row0 + o.y*row1 + o.z*row2
COL: the same identity against the transpose
```

Exact for any offset, including forward, with no knowledge of the projection needed. A
world-space offset is the same vector in translated-world space (the translation moves the
origin, not the axes), so one injection path covers both cases.

**Injection re-runs the detection test on every call rather than trusting a register number.**
The same register slot is reused by shadow and post passes carrying completely different
matrices; scoping the edit to the block the camera actually sits on is what keeps those intact.

## Repro

```powershell
.\spikes\view_matrix\build.ps1 -Install
```

Connect Virtual Desktop first, launch the dev copy, reach gameplay.

| Key | Effect |
|---|---|
| **F7** | Scan one frame for the view matrix, then report |
| **F10** | **6-DOF** — drive the offset from the headset's real position |
| **F12** | **Alternate-eye stereo** — left/right on successive frames |
| **F6** | **VR-correct projection** (on by default) — force the frustum, widen the game FOV |
| **F3** | Constant offset along the camera's right axis (the probe, not the product) |
| **F11** | Cycle that constant: 300 / 100 / 30 / 10 / 3 UU |
| F2 | Constant-pitch test (proves the rotation detour still works) |
| F9 / F8 / F5 / F4 | Head tracking · recentre · flip yaw sign · flip pitch sign |

6-DOF wins over F3 when both are on. F10 recentres on toggle, so the offset starts at zero.

The XR→game mapping is fixed at **recentre**, not taken from where the camera points now. That
is the correctness argument: an XR position lives in the XR world frame, so turning the head
does not change it. Mapping it through the camera's *current* facing would swing a stationary
body offset around the world as the head turned.

Log: `%LOCALAPPDATA%\SingularityVR\view_matrix.log`

## Reading the report

**Stand still, press F7.** Each window is listed as

```
** c0   ROW  w=-0.0020  fwdLen=1.0000  dotFwd=+0.9998  hits=50  zero at origin(translated-world)
```

`**` marks the windows passing **both** tests — small `w` *and* a forward axis agreeing with
where the camera really points. Only those are injected into. Unmarked rows are kept in the
report deliberately, so it is visible *why* something was rejected rather than it silently
vanishing.

If nothing matches, the log prints the closest window it saw, so a failed scan still produces
evidence rather than silence. The remaining suspects would be a matrix split across two calls,
or one reaching the GPU by some path other than `SetVertexShaderConstantF`.

## Run 3 — 6-DOF works, and exposed two more things

Head position drives the view; leaning and ducking move the camera. Two findings came out of
the same log.

**Scale was unjudgeable because the projection layer was lying.** It claimed the *headset's*
FOV for an image the game rendered at a different one, which stretches everything by one
uniform factor — head movement, world scale and object size all wrong together, so none can be
measured against another. Now the frustum is **derived from the matrix** (the x and y column
lengths are the projection scales; `tan(halfFov)` is the reciprocal), and the game is asked for
a **vertical** FOV matching the headset — vertical because 16:9 is wider than a near-square eye
view, so equal vertical FOV over-covers horizontally and fills both axes. Self-diagnosing: the
log prints the FOV requested *and* the FOV the matrix reports, so a UE3 clamp is visible
immediately.

**The 6-DOF offset froze on tracking loss.** The run ended with `(29.8, -30.8, -27.7)` UU
repeated identically — ~0.9 m of permanent displacement, no way back. Positional tracking had
dropped while orientation kept working (IMU carries rotation, position needs cameras), so the
update was skipped and the last value persisted forever. Skipping looked like the safe choice
and was the worst one; the offset now decays to neutral instead and recovers by itself.

## Runs 4–5 — the projection, and why asking the engine was not enough

The wider field read as far more natural, but the image pulsed. The log diagnosed it without a
second run: against a steady **127.9°** requested, the matrix reported `125.3, 127.2, 125.1,
123.8, 128.7, … 80.7`. The write **works** and is not clamped — but the engine's camera update
interpolates back toward its own default between our Present-time writes, each step sized by
frame duration, hence the 80.7° outlier on a long frame. A rendered FOV swinging 128° → 80° is
the monitor's zoom flash; submitting that narrow frustum leaves the vertical short, which is the
bars in the headset.

Asking more politely could not fix it — the engine gets the last word before rendering, the same
seam problem that defeated seven attempts at rotation. So the frustum is now **forced in the
matrix** (rescaling the x/y columns is exactly a clip-space scale, hence exactly an FOV change),
and the engine's FOV is demoted to a culling-only hint with 15% headroom. Run 5 confirms the
flicker is gone.

Two clarifications for reading the log:

- `engine's own matrix on arrival` measures the matrix **before** our edit — it reports the
  engine's ongoing drift (80°–141°), not our output. The `after forcing` line verifies ours.
- The modification count is per **draw call** using the camera matrix, not per distinct matrix.
  50–60 a frame is normal and expected.

Also measured: rendering wider than the engine culled for produced **0 px** of culling band
throughout gameplay, so UE3's per-object culling carries real slack. The headroom stays as
insurance rather than a demonstrated necessity.

## Next

1. **Test alternate-eye stereo (F12)** — does it read as depth, and is the half-rate judder
   tolerable?
2. **Tune `kMetresToUU`** (52.5, from UE3's 16 units per foot) — the projection is finally
   correct enough to judge scale against

## What this build drops

The ruled-out position experiments from spike 8 — the `FUN_0104e420` detour, the all-fields
write test, and the hardware-breakpoint machinery — are gone from this source. They are
preserved in `spikes/vr_render/` along with the notes explaining why each failed.
