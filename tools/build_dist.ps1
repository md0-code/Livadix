<#
.SYNOPSIS
    Build the Windows release and assemble dist/ with its archive.

.DESCRIPTION
    Builds livadix.exe in Release (SDL2 and the C runtime
    linked statically) through tools/build.ps1, then fills
    dist/ with what a user needs and nothing of Apple's:

        livadix.exe                       the emulator and its configuration window
        README.md                      the starting point, linking the guides (without
                                       the logo GitHub shows)
        docs/                          the guides, the license and the components' notices
        roms/PUT_ROMS_HERE.txt         the ROM file names Livadix looks for

    and zips it as dist/releases/Livadix-<version>-windows-x64.zip, the version being
    src/VERSION. The file list is checked before zipping: anything else (an
    Apple ROM, a disk image, a DLL) fails the build. The archive is then opened
    again and every file compared with what was staged.

    dist/ doubles as a run folder: whatever the user put in dist/roms, dist/livadix.yaml and its PRAM files in dist/nvram is never touched.

    Profile-guided optimisation links with the trained profile kept in
    work/pgo/windows-x64/Livadix.pgd; without one, the build has none. Only
    -Train trains it again (the Quadra 700 ROM and/or a private G3/OS X
    fixture at work/pgo/osx.yaml or -PgoOsxConfig) and keeps the result.
    -Test runs the tests (ctest and the full tier of tests/verify.py).

    -Arch arm64 cross-compiles for Windows on ARM64 (it cannot be run here)
    into build/dist-arm64, without profile-guided optimisation or tests,
    and adds dist/releases/Livadix-<version>-windows-arm64.zip; dist/ itself stays the
    x64 run folder. The ROM note's catalogue comes from an x64 build of the
    same version (build/dist-x64, else one built in build/host). Without
    the Visual Studio component "MSVC Build Tools for ARM64" it exits with
    code 3.

.EXAMPLE
    pwsh -File tools/build_dist.ps1             # build with the kept profile, assemble, zip
    pwsh -File tools/build_dist.ps1 -Train      # train the profile again first
    pwsh -File tools/build_dist.ps1 -Arch arm64 # the ARM64 archive
#>
[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",
    [string]$BuildDir = "",
    # Optional private Jaguar fixture; default work/pgo/osx.yaml when present.
    [string]$PgoOsxConfig = "",
    # Also the tests: ctest and verify.py --full.
    [switch]$Test,
    # Train the profile again (else the one kept in work/pgo/windows-x64 is used).
    [switch]$Train,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$name = "Livadix"
$program = "livadix.exe"
if (-not $BuildDir) { $BuildDir = Join-Path $root $(if ($Arch -eq "x64") { "build/dist-x64" } else { "build/dist-x64-$Arch" }) }
$version = (Get-Content (Join-Path $root "src/VERSION") -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "src/VERSION must hold MAJOR.MINOR.PATCH, not '$version'" }
$platform = "windows-$Arch"
Write-Host "$name $version ($platform)"

# dist/livadix.exe cannot be replaced while it runs from dist/.
$running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and (($_.Path -ieq (Join-Path $root "dist\$program")))
}
if ($running) { throw "$name is running from dist/ (process $($running.Id -join ', ')): close it and run this again." }

# --- build and test ------------------------------------------------------------------------
$buildArgs = @{ Config = "Release"; BuildDir = $BuildDir; StaticRuntime = $true }
if ($Clean) { $buildArgs.Clean = $true }
$keptProfile = Join-Path $root 'work/pgo/windows-x64/Livadix.pgd'
$exe = Join-Path $BuildDir $program
# The program that runs here for the ROM catalogue: the release itself on x64.
$lister = $exe
$romFile = Join-Path $root "roms/quadra-700.rom"
$explicitOsx = [bool]$PgoOsxConfig
if (-not $PgoOsxConfig) { $PgoOsxConfig = Join-Path $root 'work/pgo/osx.yaml' }
if ($explicitOsx -and -not (Test-Path -LiteralPath $PgoOsxConfig)) { throw "OS X PGO fixture not found: $PgoOsxConfig" }
if ($Arch -ne "x64") {
    # Cross-compiled: nothing of it runs here, so no training and no tests.
    & (Join-Path $root "tools/build.ps1") @buildArgs -Arch $Arch
    if ($LASTEXITCODE -eq 3) { Write-Host "No archive for ${platform}: the ARM64 toolset is missing." -ForegroundColor Yellow; exit 3 }
    $Test = $false
    # An x64 program of this version for the ROM catalogue.
    $lister = $null
    foreach ($candidate in @((Join-Path $root "build/dist-x64/$program"), (Join-Path $root "build/host/$program"))) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        $versionFile = Join-Path ([System.IO.Path]::GetTempPath()) "livadix_version_$PID.txt"
        try {
            $probe = Start-Process -FilePath $candidate -ArgumentList "--version" -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $versionFile
            if ($probe.ExitCode -eq 0 -and ((Get-Content -LiteralPath $versionFile -TotalCount 1) -eq "$name $version")) { $lister = $candidate; break }
        } finally { Remove-Item -LiteralPath $versionFile -Force -ErrorAction SilentlyContinue }
    }
    if (-not $lister) {
        $hostDir = Join-Path $root "build/host"
        & (Join-Path $root "tools/build.ps1") -Config Release -BuildDir $hostDir
        $lister = Join-Path $hostDir $program
    }
} elseif (-not $Train -and (Test-Path -LiteralPath $keptProfile)) {
    Write-Host 'Linking with the kept PGO profile'
    if ($Test) { $buildArgs.Test = $true }
    & (Join-Path $root 'tools/build.ps1') @buildArgs -Pgo USE -PgoDir (Split-Path -Parent $keptProfile)
} elseif ($Train -and ((Test-Path $romFile) -or (Test-Path -LiteralPath $PgoOsxConfig))) {
    & (Join-Path $root "tools/build.ps1") @buildArgs -Pgo GENERATE -PgoDir (Join-Path $BuildDir "pgo")
    $vsRoot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    $runtime = Get-ChildItem (Join-Path $vsRoot "VC\Tools\MSVC") -Recurse -Filter pgort140.dll |
        Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
    if (-not $runtime) { throw "pgort140.dll not found for profile-guided optimisation" }
    Copy-Item $runtime.FullName $BuildDir -Force
    # The training run works in work/ (git-ignored) and never writes roms/ or data/.
    $work = Join-Path $root "work/pgo"
    New-Item -ItemType Directory -Force $work | Out-Null
    $training = Join-Path $work "training.yaml"
    "model: quadra-700`nemulation:`n  rom: '$($romFile.Replace('\','/'))'`nstorage:`n  disks: []`n" |
        Set-Content -Path $training -Encoding ASCII
    $profiles = Join-Path $BuildDir "pgo"
    Get-ChildItem $BuildDir, $profiles -Filter "livadix!*.pgc" -ErrorAction SilentlyContinue | Remove-Item -Force
    $env:VCPROFILE_PATH = $profiles
    $trained = 0
    if (Test-Path $romFile) {
        & $exe --config $training --hidden --frames 600 | Out-Null
        $trained = $LASTEXITCODE
    }
    Remove-Item Env:VCPROFILE_PATH
    if ($trained -ne 0) { throw "the profile training run failed ($trained)" }
    # A reusable OS X fixture is private build input, never distribution data.
    # Reset its writable image/NVRAM from seeds when supplied. The inspected
    # startup disk must stay under work/, protecting the user's source image.
    if (Test-Path -LiteralPath $PgoOsxConfig) {
        $osxConfig = (Resolve-Path -LiteralPath $PgoOsxConfig).Path
        $workRoot = [IO.Path]::GetFullPath((Join-Path $root 'work')).TrimEnd('\') + '\'
        if (-not $osxConfig.StartsWith($workRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "The OS X PGO configuration must be a private fixture under work/."
        }
        $fixture = Split-Path -Parent $osxConfig
        $seed = Join-Path $fixture 'osx-seed.img'
        if (Test-Path -LiteralPath $seed) { Copy-Item -LiteralPath $seed -Destination (Join-Path $fixture 'osx.img') -Force }
        $nvramSeed = Join-Path $fixture 'nvram-seed'
        if (Test-Path -LiteralPath $nvramSeed) {
            $nvramCopy = Join-Path $fixture 'nvram'
            New-Item -ItemType Directory -Path $nvramCopy -Force | Out-Null
            Get-ChildItem -LiteralPath $nvramSeed -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $nvramCopy -Force }
        }
        $inspectOut = Join-Path $work 'osx-inspect.log'
        $inspectErr = Join-Path $work 'osx-inspect.err'
        $inspectArgs = @('--inspect', '--config', ('"' + $osxConfig + '"'))
        $probe = Start-Process -FilePath $exe -ArgumentList $inspectArgs -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $inspectOut -RedirectStandardError $inspectErr
        if ($probe.ExitCode -ne 0) { throw "OS X PGO fixture inspection failed: $(Get-Content $inspectErr -Raw)" }
        $description = Get-Content -LiteralPath $inspectOut
        $startup = $description | Where-Object { $_ -match '^Startup disk: (.+?);.*Mac OS X' } | Select-Object -First 1
        if (-not $startup -or $startup -notmatch '^Startup disk: (.+?);') { throw 'The PGO fixture must boot a Mac OS X image.' }
        $trainingDisk = [IO.Path]::GetFullPath($Matches[1].Replace('/', '\'))
        if (-not $trainingDisk.StartsWith($workRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'The OS X PGO disk must be a disposable copy under work/.' }
        if (-not ($description | Where-Object { $_ -match '^Model:.*PowerPC 750' })) { throw 'The OS X PGO fixture must select a G3.' }
        Write-Host 'Training PGO on G3 / Mac OS X'
        $env:VCPROFILE_PATH = $profiles
        try {
            $osxArgs = @('--hidden', '--unthrottled', '--config', ('"' + $osxConfig + '"'), '--frames', '4500', '--screenshot', ('"' + (Join-Path $work 'osx-training.bmp') + '"'))
            $trainedOsx = Start-Process -FilePath $exe -ArgumentList $osxArgs -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $work 'osx-training.log') -RedirectStandardError (Join-Path $work 'osx-training.err')
            if ($trainedOsx.ExitCode -ne 0) { throw "OS X PGO training failed (exit $($trainedOsx.ExitCode))." }
        } finally { Remove-Item Env:VCPROFILE_PATH -ErrorAction SilentlyContinue }
        # The disposable image is made again from its seed by the next build.
        if (Test-Path -LiteralPath $seed) { Remove-Item -LiteralPath (Join-Path $fixture 'osx.img') -Force -ErrorAction SilentlyContinue }
    } elseif ($explicitOsx) { throw "OS X PGO fixture not found: $PgoOsxConfig" }
    $buildArgs.Remove('Clean')
    if ($Test) { $buildArgs.Test = $true }
    & (Join-Path $root "tools/build.ps1") @buildArgs -Pgo USE -PgoDir $profiles
    # Kept for the next builds.
    New-Item -ItemType Directory -Force (Split-Path -Parent $keptProfile) | Out-Null
    Copy-Item -LiteralPath (Join-Path $profiles 'Livadix.pgd') $keptProfile -Force
} else {
    Write-Host "No PGO profile kept (-Train trains one) or no training media: building without profile-guided optimisation"
    if ($Test) { $buildArgs.Test = $true }
    & (Join-Path $root "tools/build.ps1") @buildArgs -Pgo ""
}
if (-not (Test-Path $exe)) { throw "the build produced no $exe" }
# The program is a PE image for the platform: x64 (8664) or ARM64 (AA64).
$image = [System.IO.File]::ReadAllBytes($exe)
$peAt = [BitConverter]::ToInt32($image, 0x3C)
$machine = [BitConverter]::ToUInt16($image, $peAt + 4)
if ($machine -ne $(if ($Arch -eq "x64") { 0x8664 } else { 0xAA64 })) { throw "$exe is not a $Arch program (machine 0x$('{0:X4}' -f $machine))" }
# -Test: the full tier of the gates (every check, every boot-matrix cell) on the release build.
if ($Test) {
    $env:LIVADIX_BUILD = $BuildDir
    & python (Join-Path $root "tests/verify.py") --full
    $verified = $LASTEXITCODE
    Remove-Item Env:LIVADIX_BUILD
    if ($verified -ne 0) { throw "tests/verify.py --full failed ($verified)" }
}

# --- stage ----------------------------------------------------------------------------------
$stageParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/')
$stage = [System.IO.Path]::GetFullPath((Join-Path $stageParent "livadix_stage_$PID"))
if ((Split-Path -Parent $stage) -ine $stageParent) { throw "Invalid release staging path: $stage" }
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage "docs"), (Join-Path $stage "roms") | Out-Null

Copy-Item $exe (Join-Path $stage $program)
# README.md without its first lines, the logo from assets/ that only GitHub shows.
$readme = [System.IO.File]::ReadAllText((Join-Path $root "README.md")) -replace '^!\[[^\]]*\]\(assets/[^)]*\)\r?\n(\r?\n)?', ''
[System.IO.File]::WriteAllText((Join-Path $stage "README.md"), $readme)
# The guides README.md links, the license and the components' notices.
$userDocs = @("NOTES.md", "CHANGELOG.md", "THIRD_PARTY.md")
foreach ($doc in $userDocs) { Copy-Item (Join-Path $root "docs/$doc") (Join-Path $stage "docs") }
Copy-Item (Join-Path $root "LICENSE") $stage

# The Built-in ROM and current documents are embedded in livadix.exe.

# The ROM note: the standard file names Livadix looks for in roms/ (livadix --list-roms).
$nl = "`r`n"
$roms = @("$name $version - Macintosh ROMs", "",
    "$name includes no Apple ROM. Its Built-in ROM is embedded in livadix.exe;",
    "the executable starts by itself. Copy your own Macintosh ROMs into",
    "this folder under their standard names below, or under any name: $name finds a ROM by its",
    "checksum, and emulation.rom in livadix.yaml names a file anywhere. The model: setting",
    "picks the model and so the ROM (livadix --list-models; README.md).", "")
# The known images only, not the files of any roms folder on this machine.
# Windows GUI executables can return before PowerShell captures their output.
# Wait explicitly and give the child a real redirected standard-output handle.
$romListFile = Join-Path $stageParent "livadix_roms_$PID.txt"
try {
    $romListProcess = Start-Process -FilePath $lister -ArgumentList "--list-roms" -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $romListFile
    if ($romListProcess.ExitCode -ne 0) { throw "$name --list-roms failed ($($romListProcess.ExitCode))" }
    $listing = @(Get-Content -LiteralPath $romListFile | Where-Object { $_ -notmatch '^(ROM folder |  )' })
    if ($listing.Count -lt 2 -or $listing[0] -notmatch '^standard name') { throw "$name --list-roms produced no ROM catalogue" }
} finally {
    Remove-Item -LiteralPath $romListFile -Force -ErrorAction SilentlyContinue
}
$roms += $listing | ForEach-Object { "  $_" }
$roms += @("", "The ROM's own checksum is checked at start-up.")
[System.IO.File]::WriteAllText((Join-Path $stage "roms/PUT_ROMS_HERE.txt"), ($roms -join $nl) + $nl)


# --- nothing but these ------------------------------------------------------------------------
$expected = @($program, "README.md", "LICENSE", "roms/PUT_ROMS_HERE.txt") + @($userDocs | ForEach-Object { "docs/$_" })
$expected = $expected | Sort-Object
$stageFull = (Resolve-Path $stage).Path
$actual = Get-ChildItem $stage -Recurse -File | ForEach-Object {
    $_.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
} | Sort-Object
$diff = Compare-Object $expected $actual -CaseSensitive
if ($diff) { throw "the release does not hold exactly the release files:`n$($diff | Out-String)" }

# --- the archive, checked by opening it again --------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipName = "$name-$version-$platform.zip"
$tmpZip = Join-Path ([System.IO.Path]::GetTempPath()) $zipName
if (Test-Path $tmpZip) { Remove-Item $tmpZip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageFull, $tmpZip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
$check = Join-Path $stageParent "livadix_check_$PID"
if (Test-Path $check) { Remove-Item $check -Recurse -Force }
[System.IO.Compression.ZipFile]::ExtractToDirectory($tmpZip, $check)
foreach ($f in $expected) {
    $a = (Get-FileHash (Join-Path $stage $f) -Algorithm SHA256).Hash
    $b = (Get-FileHash (Join-Path $check $f) -Algorithm SHA256).Hash
    if ($a -ne $b) { throw "the archive's $f differs from the staged one" }
}
Remove-Item -LiteralPath $check -Recurse -Force

# --- dist/: the release files over the run folder (x64), the archive -------------------------------
$distRoot = Join-Path $root "dist"
New-Item -ItemType Directory -Force $distRoot, (Join-Path $distRoot "docs"), (Join-Path $distRoot "roms") | Out-Null
# What earlier releases put in the run folder and this one does not: their notes and guides.
if ($Arch -eq "x64") {
    foreach ($old in @("data/PUT_MAC_FILES_HERE.txt", "docs/LICENSE", "THIRD_PARTY.md", "docs/NVRAM.md", "docs/ROM-HASHES.md",
            "docs/PIPPIN-COMPATIBILITY.md", "docs/POWERPC.md", "docs/POWERPC-SPEED.md",
            "docs/G3-BRINGUP.md", "docs/PHASE6-COMPLETION.md", "PLAN.md", "ISSUES.md",
            "docs/CONFIGURATION.md", "docs/ROMS.md", "docs/HARDWARE.md", "docs/LIMITATIONS.md",
            "assets/livadix-logo.png")) {
        $f = Join-Path $distRoot $old
        if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force }
    }
    foreach ($dir in @("data", "assets")) {
        $oldDir = Join-Path $distRoot $dir
        if ((Test-Path -LiteralPath $oldDir) -and -not (Get-ChildItem -LiteralPath $oldDir -Force)) { Remove-Item -LiteralPath $oldDir }
    }
}
$releases = Join-Path $distRoot "releases"
New-Item -ItemType Directory -Force $releases | Out-Null
# releases/ holds the current version only; archives of earlier layouts sat in dist/ itself.
Get-ChildItem $releases -File | Where-Object { $_.Name -like "$name-*" -and $_.Name -notlike "$name-$version-*" } | Remove-Item -Force
Get-ChildItem $distRoot -File | Where-Object { $_.Name -like "$name-*.zip" -or $_.Name -like "$name-*.tar.gz" } | Remove-Item -Force
if ($Arch -ne "x64") {
    Move-Item $tmpZip (Join-Path $releases $zipName) -Force
    Remove-Item -LiteralPath $stage -Recurse -Force
    Write-Host "OK: dist/releases/$zipName" -ForegroundColor Green
    exit 0
}
# Windows keeps an existing file's letter case when it is replaced: an older
# Livadix.exe in dist/ goes first, so the release is exactly livadix.exe.
Get-ChildItem $distRoot -File -Filter "Livadix.exe" | Where-Object { $_.Name -ceq "Livadix.exe" } | Remove-Item -Force
foreach ($f in $expected) {
    New-Item -ItemType Directory -Force (Split-Path (Join-Path $distRoot $f)) | Out-Null
    Copy-Item (Join-Path $stage $f) (Join-Path $distRoot $f) -Force
}
$names = Get-ChildItem $distRoot -File | ForEach-Object { $_.Name }
if (-not ($names -ccontains $program)) { throw "dist/ holds no file named exactly $program" }
Move-Item $tmpZip (Join-Path $releases $zipName) -Force

Write-Host "release:"
Get-ChildItem $stage -Recurse | ForEach-Object {
    $rel = $_.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
    if ($_.PSIsContainer) { "  $rel/" } else { "  {0,-40} {1,12:N0} bytes" -f $rel, $_.Length }
}
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "OK: dist/releases/$zipName" -ForegroundColor Green
