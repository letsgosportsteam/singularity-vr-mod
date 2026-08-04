<#
    Singularity VR mod - PhysX setup

    Singularity delay-loads the 32-bit PhysXLoader.dll. If it is missing the game dies instantly
    with no message (0xc06d007e, then a NULL deref at +0x007cdde6) - and because the VR mod is the
    only unusual file in the folder, the mod gets blamed. The UNMODDED game crashes identically;
    this was verified on a clean Steam copy at the same fault offset.

    Raven never shipped a local copy of that DLL, so Singularity depends on whatever PhysX is
    installed system-wide. That makes it fragile: installing almost any other older game can run a
    LEGACY PhysX installer which, when a newer PhysX System Software is registered, runs in REMOVE
    mode and deletes the loader this game needs. That is not hypothetical - it is exactly how it
    broke here, and it was reproduced deliberately afterwards.

    This script makes Singularity self-contained the way Mirror's Edge already is: it puts a copy
    of PhysXLoader.dll in the game's Binaries folder. Windows searches the application directory
    first, so the local copy wins and the game stops caring about system state. Verified: with both
    copies present, the game loads the local one.

    It redistributes nothing. The DLL comes from your own machine.

    USAGE - from the folder containing the game's Binaries directory:

        powershell -ExecutionPolicy Bypass -File setup_physx.ps1

    Optionally point it at a specific install:

        powershell -ExecutionPolicy Bypass -File setup_physx.ps1 -GamePath "C:\Games\Singularity"
#>

[CmdletBinding()]
param(
    [string] $GamePath
)

$ErrorActionPreference = 'Stop'
function Ok   ($m) { Write-Host "  [ok]   $m"   -ForegroundColor Green }
function Info ($m) { Write-Host "  [info] $m"   -ForegroundColor Gray  }
function Warn ($m) { Write-Host "  [warn] $m"   -ForegroundColor Yellow }
function Fail ($m) { Write-Host "  [FAIL] $m"   -ForegroundColor Red   }

Write-Host ""
Write-Host "Singularity VR - PhysX setup" -ForegroundColor Cyan
Write-Host "----------------------------"

# ---- 1. locate the game -------------------------------------------------------------------
# Accept either the game root or the Binaries folder itself, since people run scripts from
# wherever they happen to be.
if (-not $GamePath) { $GamePath = $PSScriptRoot; if (-not $GamePath) { $GamePath = (Get-Location).Path } }

$binaries = $null
foreach ($c in @((Join-Path $GamePath 'Binaries'), $GamePath, (Split-Path $GamePath -Parent))) {
    if ($c -and (Test-Path (Join-Path $c 'Singularity.exe'))) { $binaries = $c; break }
}
if (-not $binaries) {
    Fail "Could not find Singularity.exe."
    Info "Run this from your Singularity folder, or pass -GamePath ""C:\path\to\Singularity""."
    exit 1
}
Ok "Game found: $binaries"

$localDll = Join-Path $binaries 'PhysXLoader.dll'
if (Test-Path $localDll) {
    Ok "PhysXLoader.dll is already in the game folder. Nothing to do."
    Info "The game is independent of system PhysX. Other games' installers cannot break it."
    exit 0
}

# ---- 2. find a source copy on this machine ------------------------------------------------
# 32-bit only. PhysXLoader64.dll is a different file and will NOT work - Singularity is x86.
$sources = @(
    "${env:ProgramFiles(x86)}\NVIDIA Corporation\PhysX\Common\PhysXLoader.dll",
    "$env:ProgramFiles\NVIDIA Corporation\PhysX\Common\PhysXLoader.dll",
    "$env:SystemRoot\SysWOW64\PhysXLoader.dll",
    "$env:SystemRoot\System32\PhysXLoader.dll"
)
$source = $sources | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($source) {
    Copy-Item -LiteralPath $source -Destination $localDll -Force
    Ok "Copied PhysXLoader.dll into the game folder."
    Info "From: $source"
    Info "The game now uses its own copy and is independent of system PhysX."
    exit 0
}

# ---- 3. nothing to copy: PhysX is not installed at all ------------------------------------
Warn "PhysXLoader.dll (32-bit) was not found anywhere on this PC."
Write-Host ""

# The remove-mode trap. A legacy installer run while a NEWER version is registered uninstalls
# instead of installing - it took PhysXLoader64.dll and the physxcudart files with it here, and
# left the registration behind so it looked like it had succeeded.
$newer = Get-ChildItem 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                       'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
         ForEach-Object { Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue } |
         Where-Object { $_.DisplayName -match 'PhysX System Software' } |
         Select-Object -First 1

if ($newer) {
    Warn "A newer PhysX is registered: $($newer.DisplayName)"
    Warn "UNINSTALL IT FIRST, from Settings > Apps."
    Warn "With a newer version registered, the legacy 9.09 installer runs in REMOVE mode and"
    Warn "deletes files instead of installing them. That is how this breaks."
    Write-Host ""
}

$redist = Join-Path (Split-Path $binaries -Parent) 'redist\PhysX_9.09.1112_SystemSoftware.exe'
if (Test-Path $redist) {
    Info "Your copy ships the installer. Run this AS ADMINISTRATOR:"
    Write-Host ""
    Write-Host "    $redist" -ForegroundColor White
    Write-Host ""
    Info "Then run this script again to make the game self-contained."
} else {
    Info "This copy ships no redist folder - GOG packages of Singularity do not include one."
    Info "Install NVIDIA's legacy PhysX System Software (9.09) from nvidia.com, then run this"
    Info "script again to make the game self-contained."
}
exit 2
