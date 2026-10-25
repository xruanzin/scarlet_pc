param(
    [ValidateRange(1, 4)]
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $PSScriptRoot 'scarlet_runtime_build.ps1'
$stageDir = Join-Path $repoRoot 'build\scarlet-runtime\scarlet-stage'
$packageDir = Join-Path $repoRoot 'dist\ScarletPC-Independent'

if (-not (Test-Path $buildScript -PathType Leaf)) {
    throw "Missing runtime build script: $buildScript"
}
if (-not (Test-Path $packageDir -PathType Container)) {
    throw 'Independent package is missing. Run scarlet_independent_setup.ps1 once first.'
}

Write-Host '=== Scarlet PC quick runtime update ===' -ForegroundColor Magenta
Write-Host '[Scarlet PC] Reusing runtime source, CMake cache, AOT modules, launcher and optional graphics assets.' -ForegroundColor Green

& $buildScript -Jobs $Jobs -FastIncremental
if ($LASTEXITCODE -ne 0) {
    throw 'Fast incremental runtime build failed.'
}

$runtimeExe = Join-Path $stageDir 'ScarletPC.exe'
if (-not (Test-Path $runtimeExe -PathType Leaf)) {
    throw "Updated ScarletPC.exe was not staged: $runtimeExe"
}

Copy-Item $runtimeExe (Join-Path $packageDir 'ScarletPC.exe') -Force

Get-ChildItem $stageDir -File -Filter '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
    $target = Join-Path $packageDir $_.Name
    if (-not (Test-Path $target -PathType Leaf) -or
        (Get-Item $_.FullName).LastWriteTimeUtc -gt (Get-Item $target).LastWriteTimeUtc) {
        Copy-Item $_.FullName $target -Force
    }
}

$manifest = Join-Path $stageDir 'scarlet_runtime_manifest.json'
if (Test-Path $manifest -PathType Leaf) {
    Copy-Item $manifest (Join-Path $packageDir 'scarlet_runtime_manifest.json') -Force
}

Write-Host ''
Write-Host '[Scarlet PC] Quick runtime update complete.' -ForegroundColor Green
Write-Host "[Scarlet PC] Updated executable: $packageDir\ScarletPC.exe" -ForegroundColor Green
