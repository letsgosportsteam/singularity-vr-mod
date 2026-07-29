# Development environment

Recorded 2026-07-28. Machine-specific; update if paths change.

## Game installs

| Purpose | Path | Notes |
|---|---|---|
| **Dev copy (disposable)** | `R:\SingularityVR-Dev\Singularity` | Work here. Verified byte-identical copy of the GOG install (2269 files, 7.58 GB). Safe to wedge, crash, or delete. |
| GOG (clean, DRM-free) | `R:\GOG\Singularity` | Reference / static analysis. Leave untouched. |
| Steam (DRM-wrapped) | `R:\SteamLibrary\steamapps\common\Singularity` | User asked this stay untouched. Use only for final Steam-compat verification. |

`Singularity.exe` SHA256:

- GOG **and** dev copy: `8D438B6C0BD51AB8F24A6A2B6AC569DED6DEF84B20DC407357C4F78E97C0BE1E`
- Steam: `BD459ACD87D4B82693928DEE4ED437C086D5928A8E0428F3AD54B6FC114C0F51`

Hashes differ only because Steam's copy carries the SteamStub `.bind` wrapper; the underlying
compiled code is the same build (see FEASIBILITY.md).

The dev copy lives on `R:` deliberately — **not** under `C:\Users\...\OneDrive\`, so 7.58 GB does
not get synced. The source tree (this repo) is inside OneDrive and should stay small.

## Toolchain

| Tool | Status |
|---|---|
| Visual Studio 2022 Community | `R:\VStudio` |
| MSVC toolset | `14.36.32532`, x86 + x64 (`bin\Hostx64\x86\cl.exe`) |
| Windows SDK | `10.0.22000.0` — includes `d3d9.h`, `d3d11.h` |
| MSBuild | `R:\VStudio\MSBuild\Current\Bin\MSBuild.exe` |
| git | `C:\Program Files\Git\cmd\git.exe` |
| dotnet | `C:\Program Files\dotnet\dotnet.exe` |
| **CMake** | **NOT INSTALLED** — not on PATH, not bundled with this VS install |
| **Ninja** | **NOT INSTALLED** |
| **OpenXR SDK** | **NOT YET VENDORED** |

Because CMake is missing, builds currently invoke `cl.exe` directly via `vcvarsall.bat x86`
(see `spikes/d3d9_interop/build.ps1` for the working pattern). If the project grows past a couple
of translation units, either install CMake or switch to `.vcxproj` + MSBuild — the latter needs no
new tooling. The reference project `mohamad-balouza/bioshock-vr` uses CMake; `biovrdev` uses
`.vcxproj`, which is the cheaper match for this machine.

**Everything builds x86/Win32.** The game is 32-bit; a 64-bit DLL cannot be injected into it.

## VR runtimes

OpenXR `ActiveRuntime` registry keys — note the split:

| Bitness | Key | Runtime |
|---|---|---|
| 64-bit | `HKLM\SOFTWARE\Khronos\OpenXR\1` | SteamVR (`steamxr_win64.json`) |
| **32-bit** | `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1` | **Oculus** (`R:\Oculus\Support\oculus-runtime\oculus_openxr_32.json`) |

The 32-bit runtime library `LibOVRRTImpl32_1.dll` exists and is PE machine `0x014C` (x86, 5.3 MB).
Since the mod is 32-bit, it will load the **Oculus** runtime — the native Quest Link path, not
SteamVR. Good for latency; means testing needs the Oculus/Meta service running.

## Save / profile data

UE3 stores this outside the install folder entirely, keyed by the game's own identity — every
copy (Steam, clean GOG, dev GOG) reads and writes the **same** files, regardless of which folder
launched the exe. Confirmed location:

```
C:\Users\<you>\OneDrive\Documents\Singularity\RvGame\SaveData\Player0.ue3profile
C:\Users\<you>\OneDrive\Documents\Singularity\RvGame\Logs\               (crash dumps go here too)
```

This is under OneDrive (auto-synced) and shared across all three installs. A backup was taken
2026-07-28 before hook-testing began, at `save_backups/Player0.ue3profile.<timestamp>.bak` in this
repo. **Re-backup before any test session that installs a hook which modifies engine behaviour**
(as opposed to the current observation-only shims, which don't touch game state) — a crash during
a write-capable hook could in principle corrupt the one save file every copy shares.

## Reference material (session scratchpad, not committed)

Two BioShock Remastered VR mods cloned for reference:

- `bioshock-ref/`  — `biovrdev/bioshock-remastered-vr` (`.vcxproj`, dxgi proxy, vtable slots)
- `bioshock-ref2/` — `mohamad-balouza/bioshock-vr` (CMake, xinput proxy, pattern scanning,
  extensive `docs/`) — the better architectural template

Both target D3D11; neither solves the D3D9 problem. Re-clone if the scratchpad is cleared.
