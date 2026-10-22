param(
    [string]$GamePath,

    [string]$PackageDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\ScarletPC')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $PackageDir 'ScarletPC.exe'

if (-not (Test-Path $exe -PathType Leaf)) {
    throw "ScarletPC.exe was not found. Run tools\scarlet_aot_setup.ps1 first."
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    $localConfig = Join-Path $repoRoot 'build\scarlet-local.json'
    if (Test-Path $localConfig -PathType Leaf) {
        $cfg = Get-Content $localConfig -Raw | ConvertFrom-Json
        $GamePath = $cfg.game_path
    }
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    throw "Game path is not configured. Pass -GamePath or run scarlet_aot_setup.ps1."
}

$game = (Resolve-Path $GamePath).Path

Write-Host "[Scarlet PC] Starting: $game" -ForegroundColor Cyan

Push-Location $PackageDir
try {
    & $exe -g $game
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
