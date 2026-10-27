param(
    [ValidateRange(1, 32)]
    [int]$Jobs = 4,

    [string]$AotDir,

    [switch]$Release
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $repoRoot 'build\native'
$exe = Join-Path $buildDir 'ScarletPC.exe'
$deps = Join-Path $repoRoot 'build\deps-win'

$fsrHeader = Join-Path $repoRoot 'third_party\FSR-Vulkan\include\ffx_vk_portable.h'
if (-not (Test-Path $fsrHeader -PathType Leaf)) {
    Write-Host '[native] Initializing declared FSR-Vulkan submodule (no reset/clean)...' -ForegroundColor DarkCyan
    Push-Location $repoRoot
    try {
        & git submodule update --init --recursive -- third_party/FSR-Vulkan
        if ($LASTEXITCODE -ne 0) { throw 'Could not initialize third_party/FSR-Vulkan.' }
    }
    finally { Pop-Location }
}

$make = Get-Command make -ErrorAction SilentlyContinue
if (-not $make) {
    throw 'make was not found. Use the same MSYS2/UCRT64 environment used by the existing native Makefile.'
}
$clang = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clang) {
    throw 'clang was not found in PATH.'
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$args = @(
    ('B={0}' -f ($buildDir -replace '\\','/')),
    'EXE_NAME=ScarletPC',
    ('-j{0}' -f $Jobs)
)
if (Test-Path $deps -PathType Container) {
    $args += ('DEPS={0}' -f ($deps -replace '\\','/'))
}
if ($Release) {
    $args += 'RELEASE=1'
}
$args += (($exe -replace '\\','/'))

Write-Host '=== Scarlet Native Runtime build ===' -ForegroundColor Magenta
Write-Host ('make {0}' -f ($args -join ' ')) -ForegroundColor DarkGray
Push-Location $repoRoot
try {
    & $make.Source @args
    if ($LASTEXITCODE -ne 0) {
        throw "Native runtime build failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

if (-not (Test-Path $exe -PathType Leaf)) {
    throw "Build completed without producing $exe"
}

# Reuse the already-built ArmRecomp DLLs. Never rebuild AOT just because the
# native host changed.
if ([string]::IsNullOrWhiteSpace($AotDir)) {
    foreach ($candidate in @(
        (Join-Path $repoRoot 'dist\ScarletPC-Independent\aot'),
        (Join-Path $repoRoot 'build\aot\modules'),
        (Join-Path $repoRoot 'aot')
    )) {
        if (Test-Path $candidate -PathType Container) {
            $AotDir = $candidate
            break
        }
    }
}

# Preserve an existing FSR4 v07 asset set when the legacy package already has it.
foreach ($fsr4Source in @(
    (Join-Path $repoRoot 'dist\ScarletPC-Independent\assets\fsr4-v07'),
    (Join-Path $repoRoot 'assets\fsr4-v07')
)) {
    if (Test-Path $fsr4Source -PathType Container) {
        $fsr4Dest = Join-Path $buildDir 'assets\fsr4-v07'
        New-Item -ItemType Directory -Force -Path $fsr4Dest | Out-Null
        Copy-Item (Join-Path $fsr4Source '*') $fsr4Dest -Recurse -Force
        Write-Host "[native] Reused FSR4 v07 assets from $fsr4Source" -ForegroundColor DarkGreen
        break
    }
}

if (-not [string]::IsNullOrWhiteSpace($AotDir)) {
    $resolvedAot = (Resolve-Path $AotDir).Path
    $outAot = Join-Path $buildDir 'aot'
    New-Item -ItemType Directory -Force -Path $outAot | Out-Null

    $map = [ordered]@{
        'scarlet-main.dll'       = @('scarlet-main.dll', 'recompiled_image.dll')
        'scarlet-rtld.dll'       = @('scarlet-rtld.dll', 'recompiled_rtld.dll')
        'scarlet-multimedia.dll' = @('scarlet-multimedia.dll', 'recompiled_subsdk0.dll')
        'scarlet-sdk.dll'        = @('scarlet-sdk.dll', 'recompiled_sdk.dll')
    }
    foreach ($dest in $map.Keys) {
        $src = $null
        foreach ($name in $map[$dest]) {
            $candidate = Join-Path $resolvedAot $name
            if (Test-Path $candidate -PathType Leaf) {
                $src = $candidate
                break
            }
        }
        if (-not $src) {
            throw "Missing AOT module for $dest in $resolvedAot"
        }
        Copy-Item $src (Join-Path $outAot $dest) -Force
    }
    Write-Host "[native] Reused AOT modules from $resolvedAot" -ForegroundColor Green
}
else {
    Write-Warning 'AOT directory not found. The EXE was built, but native boot needs the four existing AOT DLLs in build\native\aot.'
}

# Reuse an already-built optional NGX bridge if one exists. The native renderer
# loads it dynamically only when SCARLET_UPSCALER=dlss; AMD/non-DLSS runs never
# load NVIDIA code.
foreach ($dlssStage in @(
    (Join-Path $repoRoot 'build\scarlet-dlss-bridge\stage'),
    (Join-Path $repoRoot 'dist\ScarletPC-Independent')
)) {
    if (Test-Path (Join-Path $dlssStage 'ScarletDlssBridge.dll') -PathType Leaf) {
        foreach ($name in @('ScarletDlssBridge.dll','nvngx_dlss.dll','NVIDIA-DLSS-LICENSE.txt')) {
            $src = Join-Path $dlssStage $name
            if (Test-Path $src -PathType Leaf) {
                Copy-Item $src (Join-Path $buildDir $name) -Force
            }
        }
        Write-Host "[native] Preserved optional DLSS bridge payload from $dlssStage" -ForegroundColor DarkGreen
        break
    }
}

# The dependency prefix produced by the existing Makefile contains the runtime
# DLLs. Copy them next to ScarletPC.exe; Vulkan itself remains the system loader.
$depBin = Join-Path $deps 'bin'
if (Test-Path $depBin -PathType Container) {
    Get-ChildItem $depBin -File -Filter '*.dll' | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $buildDir $_.Name) -Force
    }
}

Write-Host "[native] Ready: $exe" -ForegroundColor Green
Write-Host '[native] AOT was not regenerated.' -ForegroundColor DarkGreen
