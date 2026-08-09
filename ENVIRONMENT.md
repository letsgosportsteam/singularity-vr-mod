# Development environment

Recorded 2026-07-28, paths refreshed 2026-08-08. Machine-specific; update if paths change.

## Game installs

| Purpose | Path | Notes |
|---|---|---|
| **Dev copy (disposable)** | `R:\SingularityVR-Dev\Singularity` | Work here. Verified byte-identical copy of the GOG install (7.58 GB). Safe to wedge, crash, or delete. |
| Superseded dev copy | `R:\SingularityVR-Dev-OLD\Singularity` | The previous dev copy, kept only so it can be deleted deliberately. Nothing points at it. |
| GOG (clean, DRM-free) | `R:\GOG\Singularity` | Reference / static analysis. Leave untouched. |
| Steam (DRM-wrapped) | `R:\SteamLibrary\steamapps\common\Singularity` | User asked this stay untouched. Use only for final Steam-compat verification. |

`Singularity.exe` SHA256:

- GOG **and** dev copy: `8D438B6C0BD51AB8F24A6A2B6AC569DED6DEF84B20DC407357C4F78E97C0BE1E`
- Steam: `BD459ACD87D4B82693928DEE4ED437C086D5928A8E0428F3AD54B6FC114C0F51`

Hashes differ only because Steam's copy carries the SteamStub `.bind` wrapper; the underlying
compiled code is the same build (see FEASIBILITY.md).

The dev copy lives on `R:` deliberately — **not** under `C:\Users\...\OneDrive\`, so 7.58 GB does
not get synced.

### `R:\SingularityVR-Dev\` holds two different kinds of thing

```
R:\SingularityVR-Dev\
    Singularity\        the disposable game copy  - delete and re-copy from R:\GOG freely
    tools\              openxr-sdk, ghidra, jdk21 - NOT disposable, the build needs them
    ghidra_projects\    accumulated analysis      - NOT disposable
```

`build.ps1` reads the OpenXR SDK from `tools\openxr-sdk` and installs to `Singularity\Binaries`,
both as absolute paths. So renaming or replacing this folder moves **both** — a rename that carries
the game copy but leaves `tools` behind fails the build at its own `missing:` guard, which is the
loud version and the one to want.

## Repository location

**`R:\code\claude\singularity_vr_mod_lgst`** — moved off OneDrive 2026-07-29, re-cloned 2026-08-08.

It started under `OneDrive\Desktop\code\...`, which meant every rebuild pushed a fresh
`d3d9.dll`, `.obj`, `.pdb` and `.ilk` to the cloud. `.gitignore` stops *git* tracking those but
does not stop OneDrive syncing them, and personal OneDrive has no pattern-based exclusions.
Syncing `.git` also risks corrupting the object store if a sync lands mid-operation.

**Start Claude Code sessions from this path.** Build scripts resolve their own directory and use
absolute `R:\` paths for the SDK, toolchain and game, so nothing is tied to the repo location —
verified by a clean rebuild immediately after the move.

The move gave up OneDrive's offsite backup. A private git remote is the better replacement: it
versions the source and does not sync build output.

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

⚠️ **The build is not reproducible, so a DLL hash cannot identify a build.** Two links from
identical source with a clean working tree produced `884C1E8E…` and `5AFF67AF…` — MSVC stamps a link
timestamp and a fresh PDB GUID into every link. File *size* is a weak hint at best. To find out what
a built DLL contains, check the commit it was built from or read the `ini:` lines it writes to the
log; do not diff binaries and conclude anything from it.

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
