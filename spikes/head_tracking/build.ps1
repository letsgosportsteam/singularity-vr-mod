# Builds the head-tracking shim as x86, linking the vendored 32-bit OpenXR loader.
# -Install also copies it (and the loader) into the disposable dev copy of the game.

param([switch]$Install)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk  = "R:\SingularityVR-Dev\tools\openxr-sdk"
$devGameBin = "R:\SingularityVR-Dev\Singularity\Binaries"

$inc    = Join-Path $sdk "include"
$libDir = Join-Path $sdk "native\Win32\release\lib"
$binDir = Join-Path $sdk "native\Win32\release\bin"
foreach ($p in @($inc, $libDir, $binDir)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

Push-Location $here
try {
    $cmd = "`"$vcvars`" x86 && cl /nologo /LD /EHsc /W3 /Zi /MD /std:c++17 " +
           "/I`"$inc`" /Fe:d3d9.dll /Fo:d3d9.obj d3d9.cpp " +
           "/link /DEF:d3d9.def /LIBPATH:`"$libDir`" openxr_loader.lib dxgi.lib user32.lib shell32.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "Built: $here\d3d9.dll"

    if ($Install) {
        if (-not (Test-Path $devGameBin)) { throw "dev game folder not found: $devGameBin" }
        Copy-Item (Join-Path $here "d3d9.dll") $devGameBin -Force
        # the OpenXR loader must sit beside the game exe so our dll can find it
        Copy-Item (Join-Path $binDir "openxr_loader.dll") $devGameBin -Force
        Write-Host "Installed d3d9.dll + openxr_loader.dll to: $devGameBin"
        Write-Host "Uninstall by deleting those two files."
    }
} finally {
    Pop-Location
}
