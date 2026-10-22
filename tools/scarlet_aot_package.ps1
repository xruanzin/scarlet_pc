param(
    [Parameter(Mandatory = $true)]
    [string]$SuyuDir,

    [string]$AotDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\aot\modules'),

    [string]$OutputDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\ScarletPC')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$suyu = (Resolve-Path $SuyuDir).Path
$aot  = (Resolve-Path $AotDir).Path

$suyuCmd = Join-Path $suyu 'suyu-cmd.exe'
if (-not (Test-Path $suyuCmd -PathType Leaf)) {
    throw "suyu-cmd.exe was not found in: $suyu"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$output = (Resolve-Path $OutputDir).Path

$expected = @(
    'recompiled_image.dll',
    'recompiled_rtld.dll',
    'recompiled_subsdk0.dll',
    'recompiled_sdk.dll'
)

foreach ($name in $expected) {
    $path = Join-Path $aot $name
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Missing AOT module: $path"
    }
}

Get-ChildItem $output -File -Filter 'recompiled_*.dll' -ErrorAction SilentlyContinue |
    Remove-Item -Force

Remove-Item (Join-Path $output 'ScarletPC.exe') -Force -ErrorAction SilentlyContinue

Copy-Item $suyuCmd (Join-Path $output 'ScarletPC.exe') -Force

Get-ChildItem $suyu -File -Filter '*.dll' |
    Copy-Item -Destination $output -Force

foreach ($name in $expected) {
    Copy-Item (Join-Path $aot $name) (Join-Path $output $name) -Force
}

$manifest = Join-Path $aot 'scarlet-aot-manifest.json'
if (Test-Path $manifest) {
    Copy-Item $manifest (Join-Path $output 'scarlet-aot-manifest.json') -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $output 'mods') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $output 'user\config') | Out-Null

@'
Scarlet PC AOT runtime package.

No game files, console keys, firmware, Nintendo assets, or generated game code
are distributed by the Scarlet PC source repository.

Runtime/recompiler foundation: suyu v14.1.0 (GPL-3.0).
Scarlet-specific integration, tooling and patches are maintained by Scarlet PC.
'@ | Set-Content (Join-Path $output 'README_NATIVE_EXPORT.txt') -Encoding UTF8

Write-Host "[Scarlet PC] Package created: $output" -ForegroundColor Green

Get-ChildItem $output -File |
    Where-Object { $_.Name -eq 'ScarletPC.exe' -or $_.Name -like 'recompiled_*.dll' } |
    Select-Object Name,@{N='MB';E={[math]::Round($_.Length / 1MB, 1)}}
