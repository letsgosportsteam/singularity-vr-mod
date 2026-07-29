# Spike 8 results — a head-tracked image in the headset

Run 2026-07-29. **Working**: the game is visible in the Quest, tracking head rotation, with
correct colours.

This is the ladder's **MonoTracked** rung — one image shown to both eyes, with the game camera
driven by the HMD. No depth, but the view is genuinely oriented by where the head is pointing,
which is a different thing from watching a floating screen.

## How the frame gets there

```
game backbuffer (D3D9, 2560x1440, A8R8G8B8)
   -> GetRenderTargetData -> D3D9 SYSTEMMEM surface     (GPU -> CPU)
   -> LockRect + memcpy   -> D3D11 DYNAMIC texture      (CPU -> GPU)
   -> CopyResource        -> OpenXR swapchain image
   -> xrEndFrame, projection layer, both eyes same image
```

The CPU round-trip is deliberate. Zero-copy sharing needs the game's device upgraded to D3D9Ex,
which needs the `D3DPOOL_MANAGED` wrapper (10,454 allocations measured in spike 2). This path
requires none of that and produced a wearable result the same day; the fast path replaces it
later without changing anything above the copy.

Inherent one-frame latency: the backbuffer copied was rendered with the pose submitted on the
*previous* frame.

## Two bugs worth remembering

**1. Format mismatch — `GetRenderTargetData` returned `D3DERR_INVALIDCALL` (`0x8876086C`).**
The call requires the source and destination surfaces to have identical size *and* format. The
sysmem surface was hardcoded to `D3DFMT_X8R8G8B8`; the backbuffer is actually `D3DFMT_A8R8G8B8`.
Root cause was a documentation error — spike 2 logged `fmt=21` and that was recorded as
`X8R8G8B8`, but 21 is `A8R8G8B8` (22 is X8). Fixed by reading the format from the surface
descriptor, which cannot drift.

**2. Washed-out colours — sRGB double-encoding.**
The swapchain was created as `B8G8R8A8_UNORM` (linear). The game writes sRGB-*encoded* pixels, so
the compositor treated encoded values as linear and encoded them a second time — low contrast and
too bright. Fixed by preferring `B8G8R8A8_UNORM_SRGB`, which tells the compositor the data is
already encoded. It is bit-identical to the D3D9 `A8R8G8B8` layout and shares a type group with
the linear variant, so `CopyResource` from the upload texture stays legal.

Symptom-to-cause worth keeping: **washed out = sRGB data treated as linear.**

## Known-imperfect, deliberately

- **No stereo.** Both eyes get the same image. Depth comes with per-eye offset rendering.
- **No 6-DOF.** Head *rotation* only; leaning does not move the view.
- **FOV / aspect mismatch.** The game renders 16:9; per-eye is ~2496x2688, nearly square.
- **Performance.** ~14 MB each way per frame, unmeasured so far.

## Repro

```powershell
.\spikes\vr_render\build.ps1 -Install
```
Connect Virtual Desktop first, launch the dev copy, reach gameplay, press F9.
Log: `%LOCALAPPDATA%\SingularityVR\vr_render.log`
