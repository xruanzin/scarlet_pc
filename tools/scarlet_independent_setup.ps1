param(
    [string]$ExportRoot,

    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [ValidateRange(1, 4)]
    [int]$Jobs = 1,

    [switch]$RefreshRuntimeSource,

    [switch]$RebuildAot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$game = (Resolve-Path $GamePath).Path

Write-Host '=== Scarlet PC self-built runtime bootstrap ===' -ForegroundColor Magenta

$aotDir = Join-Path $repoRoot 'build\aot\modules'
$aotModules = @(
    'recompiled_image.dll',
    'recompiled_rtld.dll',
    'recompiled_subsdk0.dll',
    'recompiled_sdk.dll'
)
$haveAot = $true
foreach ($module in $aotModules) {
    if (-not (Test-Path (Join-Path $aotDir $module) -PathType Leaf)) {
        $haveAot = $false
        break
    }
}

if ($haveAot -and -not $RebuildAot) {
    Write-Host '[Scarlet PC] Reusing existing Scarlet AOT modules.' -ForegroundColor Green
}
else {
    if ([string]::IsNullOrWhiteSpace($ExportRoot)) {
        throw 'AOT modules are missing. Pass -ExportRoot once, or use -RebuildAot with a valid export.'
    }
    & (Join-Path $PSScriptRoot 'scarlet_aot_build.ps1') -ExportRoot $ExportRoot -Jobs $Jobs
}
& (Join-Path $PSScriptRoot 'scarlet_runtime_build.ps1') -Jobs $Jobs -RefreshSource:$RefreshRuntimeSource
& (Join-Path $PSScriptRoot 'scarlet_independent_package.ps1')

$localConfig = Join-Path $repoRoot 'build\scarlet-independent-local.json'
[ordered]@{
    game_path   = $game
    title_id    = '0100A3D008C5C000'
    package_dir = (Join-Path $repoRoot 'dist\ScarletPC-Independent')
} | ConvertTo-Json | Set-Content $localConfig -Encoding UTF8

Write-Host ''
Write-Host '[Scarlet PC] Independent bootstrap complete.' -ForegroundColor Green
Write-Host '[Scarlet PC] No external suyu installation is required by the package.' -ForegroundColor Green
Write-Host '[Scarlet PC] Run with: .\tools\scarlet_independent_run.ps1' -ForegroundColor Green
