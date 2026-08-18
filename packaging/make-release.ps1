<#
    make-release.ps1 - build the mod and stage a release archive.

    Mirror's Edge had no script here and its zip was assembled by hand. That is very
    likely how its first release ended up with the download as a link in the notes
    instead of an attached asset. This exists so the archive, its checksums and the
    upload command all come out of one run.

        .\packaging\make-release.ps1 -Version 0.1.0-alpha

    Output lands in packaging\dist\ (gitignored):

        SingularityVR-<version>.zip  <- attach this to the GitHub release AS AN ASSET
                                         d3d9.dll + openxr_loader.dll + SingularityVR.ini -
                                         everything a player pastes into Binaries, one zip
        d3d9-<version>.pdb           <- attach this too; it decodes crash addresses
        SHA256SUMS.txt               <- paste into the release notes

    Nothing here touches the remote. It prints the gh command and stops.
#>

param(
    [Parameter(Mandatory = $true)][string] $Version,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$root = Resolve-Path (Join-Path $here '..')
$src  = Join-Path $root 'spikes\view_matrix'

# Same SDK the build uses. Overridable so this is not welded to one machine.
if ($env:OPENXR_SDK) { $sdk = $env:OPENXR_SDK } else { $sdk = 'R:\SingularityVR-Dev\tools\openxr-sdk' }
$xrBin = Join-Path $sdk 'native\Win32\release\bin'

$stage = Join-Path $here 'staging'
$dist  = Join-Path $here 'dist'

function Step ($m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "    [ok]   $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "    [warn] $m" -ForegroundColor Yellow }

# ---- 1. build -------------------------------------------------------------------------
if (-not $SkipBuild) {
    Step "Building d3d9.dll (x86)"
    & (Join-Path $src 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} else {
    Warn "-SkipBuild: using whatever d3d9.dll is already in $src"
}

$dll = Join-Path $src 'd3d9.dll'
$pdb = Join-Path $src 'd3d9.pdb'
if (-not (Test-Path $dll)) { throw "no d3d9.dll at $dll" }

# ---- 2. verify the DLL is actually x86 -------------------------------------------------
#
# The game is a 32-bit process. An x64 d3d9.dll does not fail loudly - the game simply
# refuses to start with no message, which is indistinguishable from the PhysX failure in
# section 7 of the README. Reading the PE machine type costs nothing and removes the
# ambiguity from every bug report that would otherwise follow.
Step "Verifying architecture"
$fs = [System.IO.File]::OpenRead($dll)
try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Position = 0x3C
    $peOff = $br.ReadInt32()
    $fs.Position = $peOff
    $sig = $br.ReadUInt32()                    # 'PE\0\0'
    if ($sig -ne 0x00004550) { throw "not a PE file: $dll" }
    $machine = $br.ReadUInt16()
} finally { $fs.Close() }

if ($machine -ne 0x014C) {
    throw ("d3d9.dll is NOT x86 (PE machine 0x{0:X4}). The game is 32-bit and will not start." -f $machine)
}
Ok "PE machine 0x014C (x86)"

# ---- 3. stage -------------------------------------------------------------------------
Step "Staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

$payload = @(
    @{ From = $dll;                                        To = 'd3d9.dll' },
    @{ From = (Join-Path $xrBin 'openxr_loader.dll');       To = 'openxr_loader.dll' },
    @{ From = (Join-Path $src 'SingularityVR.ini.example'); To = 'SingularityVR.ini.example' },
    @{ From = (Join-Path $here 'README.txt');               To = 'README.txt' },
    @{ From = (Join-Path $root 'LICENSE');                  To = 'LICENSE' },
    @{ From = (Join-Path $root 'THIRD-PARTY-NOTICES.txt');  To = 'THIRD-PARTY-NOTICES.txt' },
    @{ From = (Join-Path $src 'setup_physx.ps1');           To = 'setup_physx.ps1' }
)

foreach ($p in $payload) {
    if (-not (Test-Path $p.From)) { throw "missing payload file: $($p.From)" }
    Copy-Item -LiteralPath $p.From -Destination (Join-Path $stage $p.To) -Force
    Ok $p.To
}

# The active SingularityVR.ini ships too now - it is what a player actually pastes into
# Binaries, not just the .example. It is generated rather than copied verbatim: the dev
# copy carries AutoResX/AutoResY, this machine's own cached headset resolution, and
# STATUS.md is explicit that shipping those injects one person's resolution into
# everyone's first launch. Stripping just those two lines leaves every tuned value intact
# and lets the mod's normal self-detect-on-first-run behaviour take over, same as if the
# key had never been written.
#
# Written via WriteAllLines with an un-BOM'd UTF8Encoding rather than Set-Content: the mod
# reads this file with the ANSI GetPrivateProfileString family, and a UTF-8 BOM ahead of
# the first section header is exactly the kind of silent corruption this project's own
# notes warn about elsewhere (STATUS.md: "the ini takes the first key of a duplicated name,
# silently"). Verified there is no BOM in the output before this shipped.
$devIniSrc  = Join-Path $src 'SingularityVR.ini'
$stagedIni  = Join-Path $stage 'SingularityVR.ini'
if (-not (Test-Path $devIniSrc)) { throw "missing payload file: $devIniSrc" }
$iniLines = Get-Content -LiteralPath $devIniSrc | Where-Object { $_ -notmatch '^\s*AutoRes[XY]\s*=' }
[System.IO.File]::WriteAllLines($stagedIni, $iniLines, (New-Object System.Text.UTF8Encoding($false)))
Ok "SingularityVR.ini (AutoResX/AutoResY stripped)"

# ---- 4. the guards that matter ---------------------------------------------------------
#
# All of these have already gone wrong once in this project's history, which is the only
# reason they are checks rather than good intentions.
Step "Checking the archive for machine-specific leakage"

# (a) Neither ini in the archive may carry an UNCOMMENTED AutoResX/AutoResY. The .example
#     ships commented out and SingularityVR.ini is generated with the lines removed above -
#     this is the check that catches it if either ever changes.
foreach ($iniName in 'SingularityVR.ini', 'SingularityVR.ini.example') {
    $iniPath = Join-Path $stage $iniName
    if (-not (Test-Path $iniPath)) { continue }
    $badRes = Select-String -Path $iniPath -Pattern '^\s*AutoRes[XY]\s*=' -ErrorAction SilentlyContinue
    if ($badRes) {
        throw ("$iniName has an active AutoResX/Y on line {0}. Strip or comment it before shipping." -f $badRes[0].LineNumber)
    }
}
Ok "no active AutoResX/AutoResY in either ini"

# (b) No home directories anywhere in the shipped text.
$leaks = Get-ChildItem $stage -File |
         Where-Object { $_.Extension -in '.txt', '.example', '.ps1', '.ini', '' } |
         Select-String -Pattern 'C:[\\/]+Users[\\/]+[A-Za-z0-9_.-]+' -ErrorAction SilentlyContinue |
         Where-Object { $_.Line -notmatch '<you>|<user>|<username>|%LOCALAPPDATA%|%USERPROFILE%|\$env:' }
if ($leaks) {
    $leaks | ForEach-Object { Warn "$($_.Filename):$($_.LineNumber)  $($_.Line.Trim())" }
    throw "a machine-specific path is in the archive"
}
Ok "no machine paths"

# ---- 5. archive + checksums ------------------------------------------------------------
Step "Packing"
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }

$zipName = "SingularityVR-$Version.zip"
$zipPath = Join-Path $dist $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal

$pdbName = "d3d9-$Version.pdb"
$pdbPath = Join-Path $dist $pdbName
if (Test-Path $pdb) {
    Copy-Item -LiteralPath $pdb -Destination $pdbPath -Force
    Ok $pdbName
} else {
    Warn "no d3d9.pdb found - crash addresses in bug reports will not be decodable"
}

$sums = Join-Path $dist 'SHA256SUMS.txt'
Get-ChildItem $dist -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } |
    Out-File -FilePath $sums -Encoding utf8

Ok ("{0} ({1:N0} KB)" -f $zipName, ((Get-Item $zipPath).Length / 1KB))

# ---- 6. what to do next ----------------------------------------------------------------
Write-Host ""
Write-Host "Contents:" -ForegroundColor Cyan
Get-ChildItem $stage -File | ForEach-Object { "    {0,-32} {1,10:N0} bytes" -f $_.Name, $_.Length }
Write-Host ""
Get-Content $sums | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

Write-Host ""
Write-Host "Next - attach BOTH files as release ASSETS, not as links in the notes:" -ForegroundColor Cyan
Write-Host ""
Write-Host "    gh release create v$Version --prerelease --title `"v$Version`" ``" -ForegroundColor White
Write-Host "        --notes-file packaging\RELEASE-$Version.md ``"               -ForegroundColor White
Write-Host "        `"$zipPath`" ``"                                             -ForegroundColor White
Write-Host "        `"$pdbPath`""                                                -ForegroundColor White
Write-Host ""
Write-Host "  Doing it in the browser instead? The files go in the" -ForegroundColor Yellow
Write-Host "  'Attach binaries by dropping them here' box at the BOTTOM of the" -ForegroundColor Yellow
Write-Host "  release editor. A markdown link in the body is not an asset." -ForegroundColor Yellow
Write-Host ""
