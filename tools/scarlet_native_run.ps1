param(
    [ValidateSet('native','las','legacy')]
    [string]$Runtime = 'native',

    [string]$GameRoot = 'extracted',

    [string]$GamePath,
    [string]$UpdatePath,

    [ValidateRange(1, 32)]
    [int]$Jobs = 4,

    [switch]$Build,
    [switch]$Profile,
    [switch]$NoLauncher
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent

if ($Runtime -eq 'legacy') {
    Write-Host '[runtime] legacy suyu fallback' -ForegroundColor Yellow
    $legacy = Join-Path $PSScriptRoot 'scarlet_independent_run.ps1'
    $args = @{}
    if (-not [string]::IsNullOrWhiteSpace($GamePath)) { $args.GamePath = $GamePath }
    if (-not [string]::IsNullOrWhiteSpace($UpdatePath)) { $args.UpdatePath = $UpdatePath }
    & $legacy @args
    exit $LASTEXITCODE
}

# The native loader already owns base ExeFS/RomFS, but Nintendo update NSPs
# require BKTR/patch overlay semantics that have not been migrated yet. Never
# ignore an explicitly requested update: preserve the known-working legacy
# path until the native overlay service replaces it.
if (-not [string]::IsNullOrWhiteSpace($UpdatePath)) {
    Write-Warning 'Native BKTR/update overlay is not migrated yet; preserving update support through the legacy runtime for this launch.'
    $legacy = Join-Path $PSScriptRoot 'scarlet_independent_run.ps1'
    $args = @{ UpdatePath = $UpdatePath }
    if (-not [string]::IsNullOrWhiteSpace($GamePath)) { $args.GamePath = $GamePath }
    & $legacy @args
    exit $LASTEXITCODE
}

$exe = Join-Path $repoRoot 'build\native\ScarletPC.exe'
if ($Build -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'scarlet_native_build.ps1') -Jobs $Jobs
}
if (-not (Test-Path $exe -PathType Leaf)) {
    throw 'Native ScarletPC.exe is missing after build.'
}

$resolvedRoot = $GameRoot
$packageMount = $null
$packageMountPid = $null

if (-not [string]::IsNullOrWhiteSpace($GamePath)) {
    $resolvedGame = (Resolve-Path -LiteralPath $GamePath).Path
    if (Test-Path -LiteralPath $resolvedGame -PathType Container) {
        $resolvedRoot = $resolvedGame
    }
    else {
        $ext = [IO.Path]::GetExtension($resolvedGame).ToLowerInvariant()
        if ($ext -notin @('.nsp','.xci')) {
            throw "Native runtime GamePath must be an extracted game directory, .nsp, or .xci: $resolvedGame"
        }
        Write-Host '[package] mounting base game directly (zero extraction)...' -ForegroundColor Cyan
        $mountJson = & (Join-Path $PSScriptRoot 'scarlet_mount_package.ps1') -PackagePath $resolvedGame
        if (-not $mountJson) {
            throw 'Base package mount failed.'
        }
        $packageMount = ($mountJson | Select-Object -Last 1 | ConvertFrom-Json)
        $packageMountPid = [int]$packageMount.ProcessId
        $resolvedRoot = $repoRoot
    }
}
elseif (Test-Path $GameRoot -PathType Container) {
    $resolvedRoot = (Resolve-Path $GameRoot).Path
}
elseif (Test-Path (Join-Path $repoRoot $GameRoot) -PathType Container) {
    $resolvedRoot = (Resolve-Path (Join-Path $repoRoot $GameRoot)).Path
}

$oldRuntime = $env:SCARLET_RUNTIME
$oldProfile = $env:SCARLET_NATIVE_PROFILE
$oldLauncher = $env:LARECOMP_NO_LAUNCHER
$oldHleProfile = $env:LARECOMP_HLE_PROF
$oldExeFs = $env:LARECOMP_EXEFS_DIR
$oldRomFsDir = $env:LARECOMP_ROMFS_DIR
$oldRomFsImage = $env:LARECOMP_ROMFS_IMAGE
$oldPackageStream = $env:LARECOMP_PACKAGE_STREAM
$oldSvcProfile = $env:LARECOMP_PROF_SVC
$oldVkProfile = $env:LARECOMP_VK_PROF
$oldCommandCounts = $env:LARECOMP_COMMAND_COUNTS
$oldFrameCsv = $env:LARECOMP_FRAME_CSV
try {
    $env:SCARLET_RUNTIME = $Runtime
    if ($packageMount) {
        $env:LARECOMP_EXEFS_DIR = [string]$packageMount.ExeFs
        $env:LARECOMP_ROMFS_DIR = [string]$packageMount.RomFsDir
        $env:LARECOMP_ROMFS_IMAGE = [string]$packageMount.RomFsImage
        $env:LARECOMP_PACKAGE_STREAM = '1'
        Write-Host ("[package] ExeFS: {0}" -f $env:LARECOMP_EXEFS_DIR) -ForegroundColor DarkGray
        Write-Host ("[package] RomFS: {0}" -f $env:LARECOMP_ROMFS_IMAGE) -ForegroundColor DarkGray
    }
    if ($Profile) {
        $profileDir = Join-Path $repoRoot 'build\native\profile'
        New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
        $env:SCARLET_NATIVE_PROFILE = '1'
        $env:LARECOMP_HLE_PROF = '1'
        $env:LARECOMP_PROF_SVC = '1'
        $env:LARECOMP_VK_PROF = '1'
        $env:LARECOMP_COMMAND_COUNTS = '1'
        $env:LARECOMP_FRAME_CSV = Join-Path $profileDir 'frames.csv'
        Write-Host ("[profile] frame timeline -> {0}" -f $env:LARECOMP_FRAME_CSV) -ForegroundColor DarkGray
    }
    if ($NoLauncher) { $env:LARECOMP_NO_LAUNCHER = '1' }

    Write-Host ("[runtime] {0} -> {1}" -f $Runtime, $exe) -ForegroundColor Cyan
    Write-Host ("[game]    {0}" -f $resolvedRoot) -ForegroundColor DarkGray

    $launch = @()
    if ($NoLauncher) { $launch += '--play' }
    # Start-Process joins ArgumentList with spaces, so preserve a root such as
    # C:\Users\Player\Pokemon Scarlet as one argument. Keep a drive root's
    # trailing separator by doubling trailing backslashes before the quote.
    if ($resolvedRoot.Contains('"')) { throw 'Invalid quote in game path.' }
    $quotedRoot = [regex]::Replace($resolvedRoot, '(\\+)$', '$1$1')
    $launch += ('"' + $quotedRoot + '"')

    $p = Start-Process -FilePath $exe -ArgumentList $launch -WorkingDirectory $repoRoot -PassThru -Wait
    if ($p.ExitCode -ne 0) {
        throw "Scarlet Native Runtime exited with code $($p.ExitCode)"
    }
}
finally {
    if ($null -eq $oldRuntime) { Remove-Item Env:SCARLET_RUNTIME -ErrorAction SilentlyContinue } else { $env:SCARLET_RUNTIME = $oldRuntime }
    if ($null -eq $oldProfile) { Remove-Item Env:SCARLET_NATIVE_PROFILE -ErrorAction SilentlyContinue } else { $env:SCARLET_NATIVE_PROFILE = $oldProfile }
    if ($null -eq $oldLauncher) { Remove-Item Env:LARECOMP_NO_LAUNCHER -ErrorAction SilentlyContinue } else { $env:LARECOMP_NO_LAUNCHER = $oldLauncher }
    if ($null -eq $oldExeFs) { Remove-Item Env:LARECOMP_EXEFS_DIR -ErrorAction SilentlyContinue } else { $env:LARECOMP_EXEFS_DIR = $oldExeFs }
    if ($null -eq $oldRomFsDir) { Remove-Item Env:LARECOMP_ROMFS_DIR -ErrorAction SilentlyContinue } else { $env:LARECOMP_ROMFS_DIR = $oldRomFsDir }
    if ($null -eq $oldRomFsImage) { Remove-Item Env:LARECOMP_ROMFS_IMAGE -ErrorAction SilentlyContinue } else { $env:LARECOMP_ROMFS_IMAGE = $oldRomFsImage }
    if ($null -eq $oldPackageStream) { Remove-Item Env:LARECOMP_PACKAGE_STREAM -ErrorAction SilentlyContinue } else { $env:LARECOMP_PACKAGE_STREAM = $oldPackageStream }
    if ($null -eq $oldSvcProfile) { Remove-Item Env:LARECOMP_PROF_SVC -ErrorAction SilentlyContinue } else { $env:LARECOMP_PROF_SVC = $oldSvcProfile }
    if ($null -eq $oldVkProfile) { Remove-Item Env:LARECOMP_VK_PROF -ErrorAction SilentlyContinue } else { $env:LARECOMP_VK_PROF = $oldVkProfile }
    if ($null -eq $oldCommandCounts) { Remove-Item Env:LARECOMP_COMMAND_COUNTS -ErrorAction SilentlyContinue } else { $env:LARECOMP_COMMAND_COUNTS = $oldCommandCounts }
    if ($null -eq $oldFrameCsv) { Remove-Item Env:LARECOMP_FRAME_CSV -ErrorAction SilentlyContinue } else { $env:LARECOMP_FRAME_CSV = $oldFrameCsv }
    if ($packageMountPid) {
        Stop-Process -Id $packageMountPid -Force -ErrorAction SilentlyContinue
        try { Wait-Process -Id $packageMountPid -Timeout 5 -ErrorAction SilentlyContinue } catch {}
    }
    if ($null -eq $oldHleProfile) { Remove-Item Env:LARECOMP_HLE_PROF -ErrorAction SilentlyContinue } else { $env:LARECOMP_HLE_PROF = $oldHleProfile }
}
