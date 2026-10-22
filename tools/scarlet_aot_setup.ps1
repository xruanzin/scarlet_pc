param(
    [Parameter(Mandatory = $true)]
    [string]$ExportRoot,

    [Parameter(Mandatory = $true)]
    [string]$SuyuDir,

    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [ValidateRange(1, 4)]
    [int]$Jobs = 1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$game = (Resolve-Path $GamePath).Path

& (Join-Path $PSScriptRoot 'scarlet_aot_build.ps1') -ExportRoot $ExportRoot -Jobs $Jobs

& (Join-Path $PSScriptRoot 'scarlet_aot_package.ps1') -SuyuDir $SuyuDir

$localConfig = Join-Path $repoRoot 'build\scarlet-local.json'
New-Item -ItemType Directory -Force -Path (Split-Path $localConfig -Parent) | Out-Null

[ordered]@{
    game_path   = $game
    title_id    = '0100A3D008C5C000'
    package_dir = (Join-Path $repoRoot 'dist\ScarletPC')
} |
    ConvertTo-Json |
    Set-Content $localConfig -Encoding UTF8

Write-Host ""
Write-Host "[Scarlet PC] Bootstrap complete." -ForegroundColor Green
Write-Host "[Scarlet PC] Run with: .\tools\scarlet_aot_run.ps1" -ForegroundColor Green
