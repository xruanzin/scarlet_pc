param(
    [string]$GamePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$package = Join-Path $repoRoot 'dist\ScarletPC-Independent'
$exe = Join-Path $package 'ScarletPC.exe'

if (-not (Test-Path $exe -PathType Leaf)) {
    throw 'Independent package not found. Run tools\scarlet_independent_setup.ps1 first.'
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    $cfgPath = Join-Path $repoRoot 'build\scarlet-independent-local.json'
    if (Test-Path $cfgPath) {
        $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
        $GamePath = $cfg.game_path
    }
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    throw 'Pass -GamePath or run scarlet_independent_setup.ps1 first.'
}

$game = (Resolve-Path $GamePath).Path

$prodKeys = Join-Path $package 'user\keys\prod.keys'
if (-not (Test-Path $prodKeys -PathType Leaf)) {
    throw 'Scarlet PC cannot decrypt the game because user\keys\prod.keys is missing. Re-run scarlet_independent_setup.ps1 with -KeysPath pointing to your legally obtained prod.keys or keys folder.'
}

Push-Location $package
try {
    $env:NSWITCH_AOT_FALLBACK = '1'
    & $exe -g $game
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "ScarletPC.exe exited with code $code"
    }
}
finally {
    Remove-Item Env:NSWITCH_AOT_FALLBACK -ErrorAction SilentlyContinue
    Pop-Location
}
