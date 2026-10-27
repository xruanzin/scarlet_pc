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

$exe = Join-Path $repoRoot 'build\native\ScarletPC.exe'
if ($Build -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'scarlet_native_build.ps1') -Jobs $Jobs
}
if (-not (Test-Path $exe -PathType Leaf)) {
    throw 'Native ScarletPC.exe is missing after build.'
}

$resolvedRoot = $GameRoot
if (Test-Path $GameRoot -PathType Container) {
    $resolvedRoot = (Resolve-Path $GameRoot).Path
}
elseif (Test-Path (Join-Path $repoRoot $GameRoot) -PathType Container) {
    $resolvedRoot = (Resolve-Path (Join-Path $repoRoot $GameRoot)).Path
}

$oldRuntime = $env:SCARLET_RUNTIME
$oldProfile = $env:SCARLET_NATIVE_PROFILE
$oldLauncher = $env:LARECOMP_NO_LAUNCHER
try {
    $env:SCARLET_RUNTIME = $Runtime
    if ($Profile) {
        $env:SCARLET_NATIVE_PROFILE = '1'
        $env:LARECOMP_HLE_PROF = '1'
    }
    if ($NoLauncher) { $env:LARECOMP_NO_LAUNCHER = '1' }

    Write-Host ("[runtime] {0} -> {1}" -f $Runtime, $exe) -ForegroundColor Cyan
    Write-Host ("[game]    {0}" -f $resolvedRoot) -ForegroundColor DarkGray

    $launch = @()
    if ($NoLauncher) { $launch += '--play' }
    $launch += $resolvedRoot

    $p = Start-Process -FilePath $exe -ArgumentList $launch -WorkingDirectory $repoRoot -PassThru -Wait
    if ($p.ExitCode -ne 0) {
        throw "Scarlet Native Runtime exited with code $($p.ExitCode)"
    }
}
finally {
    if ($null -eq $oldRuntime) { Remove-Item Env:SCARLET_RUNTIME -ErrorAction SilentlyContinue } else { $env:SCARLET_RUNTIME = $oldRuntime }
    if ($null -eq $oldProfile) { Remove-Item Env:SCARLET_NATIVE_PROFILE -ErrorAction SilentlyContinue } else { $env:SCARLET_NATIVE_PROFILE = $oldProfile }
    if ($null -eq $oldLauncher) { Remove-Item Env:LARECOMP_NO_LAUNCHER -ErrorAction SilentlyContinue } else { $env:LARECOMP_NO_LAUNCHER = $oldLauncher }
    if (-not $Profile) { Remove-Item Env:LARECOMP_HLE_PROF -ErrorAction SilentlyContinue }
}
