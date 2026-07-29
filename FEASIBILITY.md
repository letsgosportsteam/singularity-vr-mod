# Singularity (2010) VR Mod — Feasibility Assessment

Scoping pass completed 2026-07-28. No code written yet.

## Verdict

**Feasible, with two hurdles the reference projects did not face.** The architecture is
well-understood thanks to an excellent public reference (see Prior Art). The novel risk is
concentrated almost entirely in one place: getting a Direct3D 9 frame into OpenXR.

## Target identification

Established by direct inspection of `R:\SteamLibrary\steamapps\common\Singularity`:

| Property | Value | How established |
|---|---|---|
| Engine | Unreal Engine 3 | `RvGame/CookedPC/` with `Core.xxx`, `Engine.xxx`, `GameFramework.xxx`, `GFxUI.xxx` |
| Architecture | x86 32-bit | PE machine type `0x014C` |
| Build date | 2010-07-08 | PE header timestamp |
| Renderer | Direct3D 9 | imports `d3d9.dll` + `Direct3DCreate9`; **no** `d3d11.dll` |
| Physics | PhysX / Novodex | `NxCooking.dll`, `PhysXExtensions.dll` |
| UI | Scaleform GFx | `GFxUI.xxx` |
| Gamepad | XInput 1.3 | imports `XINPUT1_3.dll` |
| Content | 2036 files in CookedPC | game module is `RvGame` ("Raven Game") |

`dxgi.dll` is imported but `d3d11.dll` is not — that is DXGI display-mode enumeration, not a
D3D11 renderer. The renderer is pure D3D9.

## Prior art

No Singularity VR mod exists. Searched ModDB, Nexus, GitHub, and general web; the user separately
searched the Flat2VR Discord and found nothing. This would be the first.

UEVR does not help — it covers UE4/UE5 only, not UE3.

Two BioShock Remastered VR mods are the closest available templates:

- [biovrdev/bioshock-remastered-vr](https://github.com/biovrdev/bioshock-remastered-vr) —
  `dxgi.dll` proxy, MinHook, OpenXR. Simpler; uses hardcoded vtable slots.
- [mohamad-balouza/bioshock-vr](https://github.com/mohamad-balouza/bioshock-vr) — **the better
  template.** CMake, `xinput1_3.dll` proxy, pattern scanning, and a deliberately game-agnostic
  `core/` + per-game `adapter/` split. Its `docs/` (ARCHITECTURE, ENGINE_NOTES, ROADMAP, STATUS)
  amount to a full playbook. Actively developed; decision log dated 2026-07-23.

Both target BioShock **Remastered**, which is D3D11 — neither solves the D3D9 problem.
Both clones are in the session scratchpad for reference.

### What transfers

- **The "stereo ladder"** — the single most valuable idea. Each rung ships independently and
  de-risks the next: MonoScreen (frame on a floating quad) → MonoTracked (HMD drives camera,
  same image both eyes) → AlternateEye (proves stereo geometry) → SequentialReentry (real stereo:
  call the scene-draw twice per frame with different camera params) → DepthReproject (fallback).
- **`xinput1_3.dll` proxy injection** instead of a graphics-DLL proxy. Singularity imports
  `XINPUT1_3.dll`, so this works — and it dodges the conflict where ReShade/DXVK/SpecialK all
  fight over `d3d9.dll`.
- **Synthetic XInput for controls.** The game already reads XInput, so motion controllers can be
  composed into an `XINPUT_STATE` with zero engine hooks. Full playability early.
- MinHook, the OpenXR session/pose/seqlock plumbing, per-weapon grip offsets in config, and the
  core/adapter separation.

### What does not transfer

The entire graphics layer (D3D11 → D3D9), and all engine offsets (different engine, different
binary).

## The two hurdles unique to this project

### 1. D3D9 → OpenXR (make-or-break)

**OpenXR has no D3D9 graphics binding.** Bindings exist for D3D11, D3D12, OpenGL, and Vulkan
only. Worse, Singularity calls `Direct3DCreate9`, not `Direct3DCreate9Ex` — and shared-surface
interop requires a **D3D9Ex** device.

Planned approach, in order of preference:

1. Hook `Direct3DCreate9` and return a D3D9Ex-backed device; create a render target with a shared
   handle; open that handle as an `ID3D11Texture2D`; submit to an OpenXR swapchain bound to a
   D3D11 device. Note `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` has no D3D9 counterpart, so
   synchronization needs care.
2. `GetRenderTargetData` → system memory → D3D11 upload. Slow, but proves the pipe end to end.
3. dgVoodoo2 or DXVK to translate D3D9 wholesale, then treat the game as D3D11/Vulkan.

Nothing else in the project matters until one of these works. **This should be the first spike.**

### 2. Steam DRM encrypts the code section

`Singularity.exe` carries a Steam DRM wrapper — a `.bind` section (360 KB) and a `.text` section
with Shannon entropy of exactly **8.000**, i.e. fully encrypted. Section entropy:

```
BINK      5.859  plaintext
.text     8.000  ENCRYPTED
.rdata    5.092  plaintext
.data     4.987  plaintext
.reloc    6.572  plaintext
.bind     7.993  ENCRYPTED  (the DRM stub itself)
```

This is why symbol names were readable (they live in plaintext `.rdata`) while the code is not.
**Ghidra static analysis is impossible on the shipped Steam binary.**

Confirmed 2026-07-28: Singularity is sold DRM-free on GOG.com. User bought it; installed at
`R:\GOG\Singularity`. Direct comparison against the Steam binary confirms it is the same compiled
build, not just similar code:

| | Steam exe | GOG exe |
|---|---|---|
| PE timestamp | 2010-07-08 14:50:05 | 2010-07-08 14:50:05 (identical) |
| `.text` size | 20,447,514 / 20,447,744 | identical |
| `.rdata` / `.data` / `.reloc` size | — | identical |
| `.text` entropy | 8.000 (encrypted) | 6.526 (plaintext) |
| Sections | 8 (has `.bind`) | 7 (no `.bind`) |
| File size | 27,444,736 | 27,084,288 (diff = exactly the `.bind` size) |

Steam's copy is this exact binary with the SteamStub DRM stub appended and `.text` encrypted in
place — nothing else differs. **Static analysis (Ghidra, pattern extraction) targets the GOG exe
exclusively from here on**; no unpacking step needed. Function addresses/patterns found there
should resolve identically in a running Steam process (code is decrypted in memory at runtime
regardless of source), which is a cheap live-process check but not a real open question anymore.

The pattern scanner still searches by byte signature rather than fixed offset as a matter of
general robustness (surviving patches, ASLR, ini-driven build variance), not because GOG/Steam
parity is in doubt.

## Camera hook target

UE3 does **not** have the `PlayerCalcView` that both BioShock mods hook — that is UE2.5-era, and
it is confirmed absent (0 occurrences). The UE3 equivalents are present as UTF-16 symbols:

| Symbol | Hits |
|---|---|
| `PlayerCamera` | 25 |
| `RvPlayerCamera` | 23 |
| `ViewTarget` | 15 |
| `CalcCamera` | 6 |
| `GetPlayerViewPoint` | 4 |
| `FOVAngle` | 3 |
| `UpdateViewTarget` | 2 |
| `ProcessViewRotation` | 1 |
| `RvWeapon` | 144 |
| `RvPawn` | 8 |

`RvPlayerCamera` is Raven's own camera subclass and is the likely hook site. Resolving these to
addresses requires the decrypted `.text` first.

Caveat learned from the reference project: decoupling aim from view was harder than expected
there — no single view-point function existed, and each fire path queried its own trace origin.
Expect similar archaeology in `RvWeapon`.

## Recommended sequence

1. **Spike the D3D9→OpenXR bridge.** Highest risk, gates everything. Do not build scaffolding
   around an unproven pipe.
2. **Verify a 32-bit OpenXR session** against the Meta runtime over Link. The game is x86, so the
   loader must be too. Fallback if refused: a 64-bit companion compositor fed via shared handles.
3. **Decrypt `.text`** and stand up pattern scanning.
4. **Scaffold** the CMake/Win32-x86 project with the `xinput1_3` proxy and MinHook + OpenXR SDK.
5. Then climb the stereo ladder, rung by rung.

Steps 1–3 are independent and each answers a question that could change the plan. None of them
require writing the mod itself.

## Ground rules

Adopted from the reference project, and correct on the merits:

- Never commit game-derived content — no decompiled UnrealScript, no extracted assets, no
  RenderDoc captures. Findings get summarized in notes, not pasted.
- No code from UEVR (all rights reserved). Concepts only. REFramework is MIT and may be adapted
  with attribution.
- 32-bit (Win32) only.
- Engine addresses and signatures live in exactly one per-game file, each documented with how it
  was derived.
- Do not distribute the DRM-stripped executable.

## Realistic scope

Multi-week to multi-month even with heavy AI assistance. The reference project has been running
for many sessions with detailed handoff notes and is still working through aim decoupling.
The interactive reverse-engineering steps — attaching a debugger to the live game to locate the
camera — need a human at the keyboard.
