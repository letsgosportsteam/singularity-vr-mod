# Builds the view-rotation detour shim as x86.
# Compiles MinHook's C sources alongside the C++ shim (no separate lib step needed).

param([switch]$Install)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..\..")
$sdk  = "R:\SingularityVR-Dev\tools\openxr-sdk"
$mh   = Join-Path $root "third_party\minhook"
$devGameBin = "R:\SingularityVR-Dev\Singularity\Binaries"

$inc    = Join-Path $sdk "include"
$libDir = Join-Path $sdk "native\Win32\release\lib"
$binDir = Join-Path $sdk "native\Win32\release\bin"
foreach ($p in @($inc, $libDir, $binDir, $mh)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

Push-Location $here
try {
    $mhSrc = @(
        "`"$mh\src\buffer.c`"",
        "`"$mh\src\hook.c`"",
        "`"$mh\src\trampoline.c`"",
        "`"$mh\src\hde\hde32.c`""
    ) -join " "

    $cmd = "`"$vcvars`" x86 && cl /nologo /LD /EHsc /W3 /Zi /MD /std:c++17 " +
           "/I`"$inc`" /I`"$mh\include`" " +
           "/Fe:d3d9.dll d3d9.cpp $mhSrc " +
           "/link /DEF:d3d9.def /LIBPATH:`"$libDir`" openxr_loader.lib dxgi.lib user32.lib shell32.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "Built: $here\d3d9.dll"

    if ($Install) {
        if (-not (Test-Path $devGameBin)) { throw "dev game folder not found: $devGameBin" }
        Copy-Item (Join-Path $here "d3d9.dll") $devGameBin -Force
        Copy-Item (Join-Path $binDir "openxr_loader.dll") $devGameBin -Force
        Write-Host "Installed d3d9.dll + openxr_loader.dll to: $devGameBin"
    }
} finally {
    Pop-Location
}
