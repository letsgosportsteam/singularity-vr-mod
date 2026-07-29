# Spike 1 results — D3D9 → D3D11 interop

Run 2026-07-28, 32-bit build, on the dev machine's GPU. **13 passed, 0 failed.**

## Verdict: the bridge works, zero-copy

`ID3D11Device::OpenSharedResource` successfully opened a texture created by a D3D9Ex device,
and the pixels arrived intact — wrote `ARGB(255,64,128,191)` on the D3D9 side, read back
`B=191 G=128 R=64 A=255` on the D3D11 side. No CPU round-trip.

This was the project's largest unknown. OpenXR has no D3D9 graphics binding, so every frame
Singularity renders must reach D3D11 before the compositor can see it. It can.

Observed details worth keeping:

- Shared texture surfaces in D3D11 as `DXGI_FORMAT_B8G8R8A8_UNORM` (87) — fine for an OpenXR
  swapchain format.
- D3D11 device came up at feature level `0xB000` (11_0).
- Sharing worked from a **32-bit** process, matching the game's bitness.

## Confirmed: the game's device must be upgraded

Plain `Direct3DCreate9` — what Singularity actually calls — **rejected** a texture creation
requesting a shared handle. Sharing requires a D3D9**Ex** device. Confirmed as predicted.

The upgrade path is clean: `IDirect3D9Ex` derives from `IDirect3D9`, and the probe verified the
pointer is usable as a plain `IDirect3D9*`. So a hook on `Direct3DCreate9` can call
`Direct3DCreate9Ex` and hand the result straight back to the game, which never knows.

## The one real hazard: D3DPOOL_MANAGED

| Device | `D3DPOOL_MANAGED` accepted? |
|---|---|
| plain D3D9 | yes (what UE3 expects) |
| D3D9Ex | **no** — `hr=0x8876086C` (`D3DERR_INVALIDCALL`) |

D3D9Ex removed the MANAGED pool. If Singularity's UE3 renderer allocates anything with
`D3DPOOL_MANAGED`, a transparent device upgrade breaks it at startup.

This is now the top open question for the graphics path. Options, cheapest first:

1. **It may not matter.** UE3's D3D9 RHI implements its own resource management and device-lost
   handling, and may use `D3DPOOL_DEFAULT` throughout. If so the upgrade is free. **Unverified —
   do not assume.**
2. **Translate in the wrapper.** Intercept `CreateTexture`/`CreateVertexBuffer`/`CreateIndexBuffer`
   and rewrite `D3DPOOL_MANAGED` → `D3DPOOL_DEFAULT`, managing the backing store ourselves. This is
   what mature D3D9 wrappers (dgVoodoo2 et al.) do. Well-trodden but real work.
3. **Sysmem fallback.** Leave the game on plain D3D9; `GetRenderTargetData` the backbuffer into
   system memory each frame and upload to D3D11. Always works, costs a full CPU round-trip
   (~16 MB/frame at per-eye VR resolution). Acceptable to prove early ladder rungs, not shippable.
4. **DXVK / dgVoodoo2** to translate D3D9 wholesale.

### Settled 2026-07-28: the engine uses MANAGED heavily

Built an observation-only `d3d9.dll` shim (`spikes/d3d9_pool_probe/`) that changes nothing about
game behaviour — it forwards every call to the real system `d3d9.dll` and only tallies which pool
each resource creation requests. Installed into the disposable dev copy
(`R:\SingularityVR-Dev\Singularity`), ran a real gameplay session.

**10,454 `D3DPOOL_MANAGED` allocations** in one short session (3276 textures, 6258 vertex buffers,
891 index buffers, 26 cube textures, 3 volume textures), against only 47 `DEFAULT` allocations.
UE3's D3D9 RHI leans on MANAGED almost universally for game content — option 1 from the list above
(hope it's unused) is dead. **The wrapper must translate `D3DPOOL_MANAGED` → `D3DPOOL_DEFAULT` and
own the backing store itself** (reload-on-device-lost, the classic D3D9Ex tradeoff), or fall back
to the sysmem-copy path for the actual frame. This is real, planned-for work, not a surprise that
changes the verdict — D3D9→D3D11 sharing itself is proven; this is the cost of getting there.

Also observed in the same run: backbuffer is `2560x1440`, format 21 (`D3DFMT_X8R8G8B8`), **no
MSAA** (`msaa=0`) — one fewer complication (no resolve-before-share step needed).

Full data: `spikes/d3d9_pool_probe/`.

## Other things to watch

- **No keyed mutex on the D3D9 side.** `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` has no D3D9
  counterpart, so D3D9→D3D11 synchronisation needs explicit handling. The probe used a
  `D3DQUERYTYPE_EVENT` query + spin to flush, which worked but is a placeholder; the real mod
  needs a proper fence discipline to avoid tearing or reading a half-drawn frame.
- **MSAA.** Multisampled surfaces cannot be shared. If the game's backbuffer is multisampled it
  must be resolved into a non-MSAA shared target first.

## Repro

```powershell
.\build.ps1
.\d3d9_interop_probe.exe
```
