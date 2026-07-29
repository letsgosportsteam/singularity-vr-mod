# Builds the D3D9->D3D11 interop probe as x86, to match the 32-bit game.
# CMake is not installed on this machine; we call cl.exe directly via vcvarsall.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No VS install with the C++ toolset found" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars" }

Write-Host "VS:  $vsPath"
Write-Host "out: $here\d3d9_interop_probe.exe"
Write-Host ""

Push-Location $here
try {
    # x86 target on purpose - Singularity.exe is 32-bit, and D3D9Ex/D3D11 interop
    # behaviour is what we want to observe in the same bitness the mod will run in.
    $cmd = "`"$vcvars`" x86 && cl /nologo /EHsc /W3 /Zi /MD " +
           "/Fe:d3d9_interop_probe.exe /Fo:d3d9_interop_probe.obj " +
           "d3d9_interop_probe.cpp /link d3d9.lib d3d11.lib user32.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "Build OK. Run with: .\d3d9_interop_probe.exe"
} finally {
    Pop-Location
}
