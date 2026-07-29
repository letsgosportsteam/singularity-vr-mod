# Builds the observation-only d3d9.dll shim as x86 (the game is 32-bit).
# -Install also copies it into the DISPOSABLE dev copy of the game.

param([switch]$Install)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$devGameBin = "R:\SingularityVR-Dev\Singularity\Binaries"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No VS install with the C++ toolset found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

Push-Location $here
try {
    $cmd = "`"$vcvars`" x86 && cl /nologo /LD /EHsc /W3 /Zi /MD /std:c++17 " +
           "/Fe:d3d9.dll /Fo:d3d9.obj d3d9.cpp " +
           "/link /DEF:d3d9.def user32.lib shell32.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "Built: $here\d3d9.dll"

    if ($Install) {
        if (-not (Test-Path $devGameBin)) { throw "dev game folder not found: $devGameBin" }
        # Refuse to clobber a real d3d9 that isn't ours (there shouldn't be one).
        $target = Join-Path $devGameBin "d3d9.dll"
        Copy-Item (Join-Path $here "d3d9.dll") $target -Force
        Write-Host "Installed to: $target"
        Write-Host "Uninstall by deleting that single file."
    } else {
        Write-Host ""
        Write-Host "Not installed. Re-run with -Install to copy into the dev game folder:"
        Write-Host "  $devGameBin"
    }
} finally {
    Pop-Location
}
