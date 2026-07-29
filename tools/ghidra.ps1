# Ghidra launcher for this project.
#
# Ghidra's own JAVA_HOME_OVERRIDE in support/launch.properties is NOT honoured by the
# Windows batch launchers (verified 2026-07-28), so we set JAVA_HOME per-process here.
# Nothing is written to the system environment.
#
#   .\tools\ghidra.ps1              # headless usage text (smoke test)
#   .\tools\ghidra.ps1 -Gui         # open the Ghidra GUI
#   .\tools\ghidra.ps1 -Analyze     # import + auto-analyse the GOG Singularity.exe
#   .\tools\ghidra.ps1 -Script foo.java   # run a script against the analysed project

param(
    [switch]$Gui,
    [switch]$Analyze,
    [string]$Script
)

$ErrorActionPreference = "Stop"

$GhidraRoot  = "R:\SingularityVR-Dev\tools\ghidra_12.1.2_PUBLIC"
$JdkRoot     = "R:\SingularityVR-Dev\tools\jdk21"
$ProjectDir  = "R:\SingularityVR-Dev\ghidra_projects"
$ProjectName = "SingularityVR"
# The GOG binary: DRM-free, .text is plaintext. Never point this at the Steam exe.
$TargetExe   = "R:\GOG\Singularity\Binaries\Singularity.exe"
$ScriptDir   = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "ghidra_scripts"

foreach ($p in @($GhidraRoot, $JdkRoot)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}

$env:JAVA_HOME = $JdkRoot
$env:PATH = "$JdkRoot\bin;$env:PATH"

if ($Gui) {
    Write-Host "launching Ghidra GUI (JAVA_HOME=$JdkRoot)..."
    Start-Process (Join-Path $GhidraRoot "ghidraRun.bat")
    return
}

$headless = Join-Path $GhidraRoot "support\analyzeHeadless.bat"

if ($Analyze) {
    New-Item -ItemType Directory -Force $ProjectDir | Out-Null
    if (-not (Test-Path $TargetExe)) { throw "target exe not found: $TargetExe" }
    Write-Host "importing + analysing $TargetExe"
    Write-Host "project: $ProjectDir\$ProjectName"
    Write-Host "(a 27 MB x86 binary - this takes a long time)"
    & $headless $ProjectDir $ProjectName -import $TargetExe -log (Join-Path $ProjectDir "analysis.log")
    return
}

if ($Script) {
    & $headless $ProjectDir $ProjectName -process "Singularity.exe" -noanalysis `
        -scriptPath $ScriptDir -postScript $Script
    return
}

& $headless
