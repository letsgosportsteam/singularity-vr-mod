# Builds the D3D9 -> D3D11 -> OpenXR chain probe as x86 (matching the 32-bit game),
# linking the prebuilt Win32 OpenXR loader from the vendored SDK.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk  = "R:\SingularityVR-Dev\tools\openxr-sdk"

$inc    = Join-Path $sdk "include"
$libDir = Join-Path $sdk "native\Win32\release\lib"
$binDir = Join-Path $sdk "native\Win32\release\bin"
foreach ($p in @($inc, $libDir, $binDir)) {
    if (-not (Test-Path $p)) { throw "OpenXR SDK path missing: $p" }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

Push-Location $here
try {
    $cmd = "`"$vcvars`" x86 && cl /nologo /EHsc /W3 /Zi /MD /std:c++17 " +
           "/I`"$inc`" " +
           "/Fe:openxr_chain_probe.exe /Fo:openxr_chain_probe.obj openxr_chain_probe.cpp " +
           "/link /LIBPATH:`"$libDir`" openxr_loader.lib d3d9.lib d3d11.lib dxgi.lib user32.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

    # the loader dll must sit next to the exe
    Copy-Item (Join-Path $binDir "openxr_loader.dll") $here -Force
    Write-Host ""
    Write-Host "Built: $here\openxr_chain_probe.exe (+ openxr_loader.dll)"
    Write-Host "Run with the headset connected and the Oculus service running."
} finally {
    Pop-Location
}
