param(
    [string]$RuntimeStage = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-runtime\scarlet-stage'),
    [string]$AotDir       = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\aot\modules'),
    [string]$OutputDir    = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\ScarletPC-Independent')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$host = Join-Path $RuntimeStage 'ScarletPC.exe'
if (-not (Test-Path $host -PathType Leaf)) {
    throw "Self-built Scarlet runtime host not found. Run tools\scarlet_runtime_build.ps1 first."
}

$expected = @(
    'recompiled_image.dll',
    'recompiled_rtld.dll',
    'recompiled_subsdk0.dll',
    'recompiled_sdk.dll'
)

foreach ($name in $expected) {
    if (-not (Test-Path (Join-Path $AotDir $name) -PathType Leaf)) {
        throw "Missing AOT module: $name"
    }
}

if (Test-Path $OutputDir) {
    Remove-Item $OutputDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Copy-Item $host (Join-Path $OutputDir 'ScarletPC.exe') -Force

Get-ChildItem $RuntimeStage -File -Filter '*.dll' |
    Copy-Item -Destination $OutputDir -Force

foreach ($name in $expected) {
    Copy-Item (Join-Path $AotDir $name) (Join-Path $OutputDir $name) -Force
}

$runtimeManifest = Join-Path $RuntimeStage 'runtime-build.json'
if (Test-Path $runtimeManifest) {
    Copy-Item $runtimeManifest (Join-Path $OutputDir 'runtime-build.json') -Force
}

$aotManifest = Join-Path $AotDir 'scarlet-aot-manifest.json'
if (Test-Path $aotManifest) {
    Copy-Item $aotManifest (Join-Path $OutputDir 'scarlet-aot-manifest.json') -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $OutputDir 'user\config') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutputDir 'mods') | Out-Null

@'
Scarlet PC self-built runtime bootstrap.

This package does not require a separate suyu installation.
Its runtime is built from the pinned GPL source revision documented in
runtime-build.json and THIRD_PARTY.md.

No game, keys, firmware, or Nintendo assets are included.
'@ | Set-Content (Join-Path $OutputDir 'README_RUNTIME.txt') -Encoding UTF8

Write-Host "[Scarlet PC] Independent package ready: $OutputDir" -ForegroundColor Green
