# Builds uedecompress - a HOST tool, not part of the mod.
#
# x86 only because that is the toolchain path already proven on this machine (see
# spikes/d3d9_interop/build.ps1); nothing here needs 32-bit, the files are ~28 MB.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No VS install with the C++ toolset found" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars" }

Push-Location $here
try {
    $cmd = "`"$vcvars`" x86 && cl /nologo /EHsc /W4 /O2 /MD /std:c++17 " +
           "/Fe:uedecompress.exe /Fo:uedecompress.obj uedecompress.cpp"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host ("Built: " + (Join-Path $here "uedecompress.exe"))
} finally {
    Pop-Location
}
