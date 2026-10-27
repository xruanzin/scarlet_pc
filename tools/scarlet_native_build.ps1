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
