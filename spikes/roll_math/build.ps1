# Builds and runs the head-roll matrix test. Host architecture, no game and no headset needed -
# this is pure arithmetic, which is the point: the sign and the aspect correction in ApplyRoll are
# exactly the kind of thing that otherwise costs a headset run to discover.
#
# ApplyRoll is duplicated here rather than #included. d3d9.cpp pulls in the whole D3D9/OpenXR
# surface and cannot be compiled standalone, so the alternative to a copy is no test at all.
# If ApplyRoll changes, change it in both places - the test asserts the properties, so a drifted
# copy shows up as a pass here and a tilted horizon in the headset.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vcvars = "R:\VStudio\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) { throw "missing: $vcvars - see ENVIRONMENT.md" }

# Absolute paths throughout, and /Fo: names the .obj FILE rather than a directory. Two traps here,
# both silent: an argument ending in a backslash immediately before the closing quote reads to cmd
# as an escaped quote (the compiler then reports no source file), and vcvarsall.bat changes the
# current directory, so a bare `rolltest.exe` is not found even after Push-Location.
$src = Join-Path $here "rolltest.cpp"
$exe = Join-Path $here "rolltest.exe"
$obj = Join-Path $here "rolltest.obj"
cmd /c "`"$vcvars`" x86 >nul && cl /nologo /EHsc /W4 /Fe:`"$exe`" /Fo:`"$obj`" `"$src`" >nul && `"$exe`""
if ($LASTEXITCODE -ne 0) { throw "roll math test FAILED" }
