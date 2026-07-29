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

Fixed by adding an independent second test, now in this build: for a genuine world→clip matrix
the xyz of the w term **is the camera's forward axis**, and the camera's real facing is known
from its `FRotator`. Requiring agreement (`dotFwd >= 0.99`) rejects the unrelated matrices *and*
resolves the storage convention, since ROW and COL read the forward vector from different
components. Injection now targets only windows passing both tests, instead of all 18.

**Known artefact: a black band at the frame edge.** The game culls geometry on the CPU against
its *original* frustum, so anything newly visible after the offset was never submitted to the
GPU. Moving the matrix cannot conjure draw calls that were never issued. Standard VR-injection
problem with a standard fix — render wider than you display (`mCurrentPOV` FOV at `+0x0438` is
proven writable). The proof used 300 UU deliberately; a real IPD is a few UU, so F11 now steps
the offset down through 300 / 100 / 30 / 10 / 3 to find where it stops mattering.

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
| **F3** | Offset the camera along its own right axis through what was found |
| **F11** | Cycle the offset: 300 / 100 / 30 / 10 / 3 UU |
| F2 | Constant-pitch test (proves the rotation detour still works) |
| F9 / F8 / F5 / F4 | Head tracking · recentre · flip yaw sign · flip pitch sign |

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

## Next

1. **F11 down the offset** and find where the culling band stops mattering — that sizes the
   FOV-widening job, or removes it
2. **Widen the render FOV** via `mCurrentPOV +0x0438` so the margin covers the offset
3. **Two images per frame**, the remaining piece for real stereo
4. **6-DOF**, now mostly free — feed the HMD's positional delta in as the offset instead of a
   constant

## What this build drops

The ruled-out position experiments from spike 8 — the `FUN_0104e420` detour, the all-fields
write test, and the hardware-breakpoint machinery — are gone from this source. They are
preserved in `spikes/vr_render/` along with the notes explaining why each failed.
