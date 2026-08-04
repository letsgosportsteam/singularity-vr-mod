# Singularity VR mod — working notes for Claude

A VR mod for Singularity (2010, Unreal Engine 3, D3D9, x86), shipping as one `d3d9.dll` dropped
beside the game exe. No game files are modified.

## Read in this order

| File | What it is | When |
|---|---|---|
| `STATUS.md` | **Current state + what to do next.** ~340 lines | Always, first |
| `ENGINE_NOTES.md` | Engine internals, addresses, structures, Ghidra findings | When touching the engine |
| `ENVIRONMENT.md` | Paths, toolchain, game copies | When a path is unclear |
| `HISTORY.md` | The run log, runs 1–179. ~3,600 lines | **Search, don't read.** See below |

**Do not read `HISTORY.md` cold** — it is roughly 55k tokens and almost none of it is current. Go
there for one job: before building on a premise you inherited, check what actually measured it.
Sections carrying a ⛔ **SUPERSEDED** banner are wrong-but-kept; never quote them as current.

## The build

```powershell
.\spikes\view_matrix\build.ps1 -Install
```

`spikes/view_matrix/` is the live build (a single ~14,500-line `d3d9.cpp`). `spikes/vr_render/`
is the known-good fallback. Earlier spikes are kept on purpose — several record approaches that
did not work.

## The log

`%LOCALAPPDATA%\SingularityVR\view_matrix.log`, rotated 12 deep (`prev1` … `prev12`, oldest
last). **Read it from disk with the Read tool.** It is truncated at each attach and stamped with
the run's date/time on line 1.

- Never paste log contents into `STATUS.md` or `HISTORY.md`. The docs stay prose; the logs stay
  on disk. (`STATUS.md` reached 240 KB without this happening — don't start.)
- Measurements here are A/B across launches, so `prev1.log` — the launch *before* this one — is
  usually the file you actually want.

## Testing happens in a VR headset

The user is wearing a headset and cannot read the screen during a test.

1. **Give numbered keypresses first.** Reasoning comes after, or not at all.
2. **State what would fool the test**, next to the steps — name the false-pass mode explicitly.
   A test that cannot fail proves nothing, and this project has lost multiple runs to instruments
   that were quietly lying.
3. One question per run where possible. A headset run is expensive; batching two uncontrolled
   changes into one wastes it.

## What this project keeps getting wrong

These are earned, not generic. The long-form versions are under *Method notes* in `STATUS.md`.

- **Almost every hard problem here was a stale premise, not a hard problem.** Re-test the premise
  before building on it, especially one you inherited from an earlier run.
- **Validate the instrument before trusting it.** SSW silently halved every frame-rate number for
  thirty runs. Three diagnostics were later found incapable of reporting anything at all.
- **"our work" in the frame budget is a residual, not a measurement.** It absorbs anyone's
  latency while looking exactly like your own bug.
- **The cheap control first.** Rename `d3d9.dll` to `d3d9.dll.off` and run unmodded at the same
  resolution — one minute, and it settles "is this us at all" before any bisection.

## Doc hygiene

`STATUS.md` is current state. When something ships, **update the existing claim rather than
appending a new one** — the four ⛔ banners in `HISTORY.md` all exist because a status section was
appended past instead of edited, and then read as fact months later. Run narrative belongs in
`HISTORY.md`, newest at the top.
