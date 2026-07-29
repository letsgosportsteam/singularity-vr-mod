# Spike 3 results — D3D9 → D3D11 → OpenXR, end to end

Run 2026-07-28 from a **32-bit** process. **Exit 0 — full chain proven.**

## Verdict

A frame authored by Direct3D 9 reached the VR compositor and was displayed, with no CPU
round-trip, from a process the same bitness as the game. Together with spike 1 this closes the
project's make-or-break question: **the architecture works.**

```
[1] xrCreateInstance ......... VirtualDesktopXR          (after 1.0 fallback)
[2] xrGetSystem .............. Meta Quest 3, 16384x16384 max
[3] graphics requirements .... adapter LUID 0000F3B7, FL 0xB000
[4] D3D11 device ............. NVIDIA GeForce RTX 5070 Ti
[5] xrCreateSession .......... OK
[6] swapchain ................ 2 views @ 2496x2688/eye, 3 images, format 87
[7] D3D9Ex -> D3D11 .......... shared texture opened on the XR-bound device
[8] frame submitted .......... session reached FOCUSED
```

Two details that matter for the real mod:

- **Swapchain format 87 = `DXGI_FORMAT_B8G8R8A8_UNORM`** — exactly what the D3D9 shared texture
  surfaces as. No format conversion in the hot path.
- **Per-eye 2496×2688.** The game currently renders one 2560×1440 backbuffer; stereo re-entry
  means roughly 2×(2496×2688), a substantially larger pixel budget. Relevant to the LAA / 32-bit
  address space question.

## The trap: VDXR is OpenXR 1.0 only

This cost real debugging time and will bite anyone repeating the work.

Requesting `XR_CURRENT_API_VERSION` (1.1, from SDK 1.1.61) against VDXR fails with
**`-4 XR_ERROR_INITIALIZATION_FAILED`** — *not* the correct `XR_ERROR_API_VERSION_UNSUPPORTED`.
The symptom is indistinguishable from "headset not connected", which sent the investigation down
the wrong path entirely (checking network state, VD connection, streamer processes).

**The mod must request OpenXR 1.0**, or loop over candidate API versions as this probe now does.

## Runtime landscape for a 32-bit client

| Runtime | Result |
|---|---|
| Meta / Oculus `oculus_openxr_32.json` | instance + system + graphics requirements all fine, then **access violation inside `xrCreateSession`** on a runtime worker thread (SEH around the call does not catch it). **Unusable.** |
| **VirtualDesktopXR** (bundled, VD Streamer 1.34.18) | **works fully** — with the 1.0 caveat above |
| SteamVR | **ships no 32-bit OpenXR runtime at all** — only `steamxr_win64.json` |

So the mod targets **VDXR**, and the 64-bit companion-compositor fallback is **not needed**.

Operational notes:

- Virtual Desktop **rewrites both HKLM `ActiveRuntime` keys** (32- and 64-bit) to point at itself
  when it connects. Don't assume the runtime you tested last is still active.
- VD takes **exclusive** ownership of the headset; Oculus then returns `-35`
  (`FORM_FACTOR_UNAVAILABLE`) from `xrGetSystem`. Link and VD are mutually exclusive.
- VD has no native USB streaming (platform restriction on third-party Quest apps). Wireless was
  perfectly adequate for validation; revisit for playtesting comfort.

## Repro

```powershell
cd spikes\openxr_chain
.\build.ps1
$env:XR_RUNTIME_JSON = "C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json"
.\openxr_chain_probe.exe
```

Requires VD Streamer running and the headset connected through the Virtual Desktop app.
Output is unbuffered on purpose — an earlier crash discarded a full stdio buffer and hid the
failure point.
