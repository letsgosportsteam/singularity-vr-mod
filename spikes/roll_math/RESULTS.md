# Roll math — verified before the first headset run

`ApplyRoll` in `spikes/view_matrix/d3d9.cpp`, checked as arithmetic. Run `.\build.ps1`; it exits
non-zero if anything regresses.

## What it asserts, and why each one is there

| Property | Why it could have gone wrong |
|---|---|
| View-space rotation equals the requested angle | The whole point. A shear reads as "roughly working" until you tilt far. |
| View-space magnitude unchanged | **The risky one.** `tanX ≠ tanY`, and under duplication `tanX` is the *half-width* per-eye value, so rotating raw clip x/y would stretch as it rotates. |
| Composes with `ApplyEyeRemap` | Roll must land before the half-frame squash. Applied after, it would mix a halved x with a full y and tilt by the wrong angle. |
| Zero roll is bit-exact | This runs on every camera constant upload; a level head must cost nothing and change nothing. |
| Square frustum gives a pure NDC rotation | Isolates the aspect terms — with `tanX == tanY` they must vanish. |
| Both `CONV_ROW` and `CONV_COL` | The live path is ROW, but the COL branch is written and would rot silently. |

All pass, both conventions.

## What it deliberately does NOT assert

**The absolute sign.** `ApplyRoll(+θ)` rotates the image content by `+θ` in view space; whether that
is the right direction for a given head tilt is a handedness question between OpenXR's frame and
the game's, and this project has guessed that wrong on *both* of the other two axes. `g_rollSign`
and **NUMPAD2** settle it in the headset in one keypress, exactly as F4/F5 do for pitch and yaw.

## The thing worth remembering

Roll looked at first like it might already work for free. The submitted projection layer carries the
head's full orientation, roll included, so the runtime appears to have everything it needs. It does
not help: a projection layer is reprojected by the **delta** between the pose we claim and the pose
the display is at. Claim a rolled pose for an unrolled render while the head is at that same roll,
the delta is zero, and the image is presented straight — which is precisely the symptom. Baking
roll into the render is what makes the claimed pose true.
