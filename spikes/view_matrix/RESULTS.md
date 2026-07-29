# Spike 9 — the view matrix on its way to the GPU

**Status: built, NOT YET RUN.** Needs a headset session to produce results.

Attacks the one blocking problem: camera **position**, which gates both stereo (a per-eye
offset *is* a position offset) and 6-DOF.

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
| **F3** | Offset the camera +300 UU on world Y through whatever was found |
| F2 | Constant-pitch test (proves the rotation detour still works) |
| F9 / F8 / F5 / F4 | Head tracking · recentre · flip yaw sign · flip pitch sign |

Log: `%LOCALAPPDATA%\SingularityVR\view_matrix.log`

## Reading the result

**Stand still, press F7.** The report lists each matching window as
`c<register> ROW|COL w=<residual> fwdLen=<length> hits=<count> zero at <probe point>`.

- `w` near zero and `fwdLen` near 1.0 → a genuine view matrix
- Which probe point matched tells us whether the engine uses **world** or **translated-world**
  space, which is worth recording either way
- Several registers matching is normal and expected — UE3 uploads the view-projection to
  different slots for different shaders

Then **press F3**. If the view slides sideways, **camera position is solved** and both stereo
and 6-DOF are unblocked.

If nothing matches, the log prints the closest window it saw, so a failed scan still produces
evidence rather than silence. The remaining suspects at that point are a matrix split across
two calls, or one reaching the GPU by some path other than `SetVertexShaderConstantF`.

## What this build drops

The ruled-out position experiments from spike 8 — the `FUN_0104e420` detour, the all-fields
write test, and the hardware-breakpoint machinery — are gone from this source. They are
preserved in `spikes/vr_render/` along with the notes explaining why each failed.
