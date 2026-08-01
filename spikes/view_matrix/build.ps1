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

    # ---- static analysis pass on d3d9.cpp, before the real build ----
    #
    # Run 27 lost four runs to a printf call with nine conversions and ten arguments: %s consumed
    # an int, which is harmless while that int is 0 and an access violation inside a held critical
    # section the moment it is not. It presented as an unexplained startup hang.
    #
    # /W1../W4 do not catch it. Log() is now annotated _Printf_format_string_, and /analyze reads
    # that annotation and reports C6067. Verified: without /analyze the mistake compiles silently
    # even at /W4; with it, it is an error here.
    #
    # Analysis-only, on our source alone - MinHook's C is third-party and not our business.
    # NB no `2>&1` here. vcvarsall writes a harmless "vswhere.exe is not recognized" line to
    # stderr, and redirecting a native command's stderr inside PowerShell wraps each line in an
    # ErrorRecord - which $ErrorActionPreference = "Stop" then treats as fatal. cl writes its
    # warnings to stdout, so there is nothing to gain from the redirect anyway.
    $an = "`"$vcvars`" x86 >nul && cl /nologo /EHsc /W3 /MD /std:c++17 /analyze:only " +
          "/I`"$inc`" /I`"$mh\include`" /c d3d9.cpp"
    $anOut = cmd /c $an
    $anOut | Write-Host
    # Format-string mistakes are the ones that have actually cost time here, so they fail the
    # build. The rest of /analyze's output is advisory.
    # C6270/C6271 added run 96: a literal '%' left unescaped in a Log() string. "86-89% enriched"
    # becomes the float specifier %e with no argument, which reads garbage off the stack. Exactly
    # the run-27 defect this check exists for, and it slipped through because only some of the
    # family were listed. If a warning says the format string and the arguments disagree, it is
    # fatal - there is no benign version of that.
    $fatal = $anOut | Select-String -Pattern "warning C6067|warning C6063|warning C6064|warning C6066|warning C6270|warning C6271|warning C6272|warning C6273|warning C6328"
    if ($fatal) { throw "static analysis found a format-string defect - fix it before building" }

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
