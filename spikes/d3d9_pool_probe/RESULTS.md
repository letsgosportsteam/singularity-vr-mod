# Spike 2 results — D3DPOOL_MANAGED usage

Run 2026-07-28 against `R:\SingularityVR-Dev\Singularity` (disposable dev copy), one session:
menu → real gameplay → quit via menu. Log: `%LOCALAPPDATA%\SingularityVR\d3d9_pool_probe.log`.

## Verdict: the engine uses MANAGED constantly

| Resource | DEFAULT | MANAGED | SYSTEMMEM | SCRATCH |
|---|---|---|---|---|
| Textures | 20 | 3276 | 0 | 0 |
| Vertex buffers | 1 | 6258 | 0 | 0 |
| Index buffers | 26 | 891 | 0 | 0 |
| Cube textures | 0 | 26 | 0 | 0 |
| Volume textures | 0 | 3 | 0 | 0 |
| **Total** | **47** | **10,454** | 0 | 0 |

This settles the open question from spike 1 (`../d3d9_interop/RESULTS.md`): D3D9Ex — required for
shared-surface interop with D3D11 — rejects `D3DPOOL_MANAGED`. The engine cannot be handed an Ex
device unmodified; it would fail almost every resource creation at startup.

**Path forward:** the wrapper intercepts `CreateTexture` / `CreateVertexBuffer` / `CreateIndexBuffer`
/ `CreateCubeTexture` / `CreateVolumeTexture`, rewrites `D3DPOOL_MANAGED` → `D3DPOOL_DEFAULT`, and
takes over what MANAGED used to give the engine for free: keeping a system-memory copy and
reloading the resource after a device-lost event. This is a known, bounded pattern (the same thing
mature D3D9 wrapping layers do), not a new unknown.

## Other findings from the same run

- Backbuffer: `2560x1440`, `D3DFMT_X8R8G8B8` (21), **no MSAA**. One less complication — a
  multisampled backbuffer would need a resolve pass before the frame could be shared with D3D11.
- `CreateDevice` flags `0x142` = `D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE |
  D3DCREATE_MULTITHREADED`.
- Two distinct texture formats recur heavily among the MANAGED allocations (raw values
  `894720068` and `827611204` — these are FOURCC-packed compressed formats, i.e. DXT1/DXT5-style
  block compression, not raw ARGB). Worth decoding precisely before the wrapper assumes anything
  about format handling.

## Incidental finding: repeated crashes during the same session

Six `0xC0000005` access violations were written to
`Documents\Singularity\RvGame\Logs\unreal-v4869-*.dmp` during this test window (19:00–19:10),
all at the identical address `0x00BE6550`. That address falls inside `Singularity.exe`'s own
image range (`0x00400000`–`0x01F53ABE`), not inside this shim's range (`0x10000000`–`0x1000C000`)
or any other loaded module — and the shim never mutates arguments it forwards, so it's an unlikely
cause. Not confirmed either way without disassembling that address (blocked on Ghidra setup,
`docs`/tasks tracked separately). Not urgent — the session that produced the pool data above ran
to a clean, intentional exit — but worth resolving before relying on repeated automated test runs.

## Repro

```powershell
cd spikes\d3d9_pool_probe
.\build.ps1 -Install    # builds x86, copies d3d9.dll into the dev copy
# launch R:\SingularityVR-Dev\Singularity\Binaries\Singularity.exe, play, quit normally
# read %LOCALAPPDATA%\SingularityVR\d3d9_pool_probe.log
```

Uninstall: delete `R:\SingularityVR-Dev\Singularity\Binaries\d3d9.dll`.
