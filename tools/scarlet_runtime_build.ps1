param(
    [string]$SourceDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\scarlet-runtime-src'),
    [string]$BuildDir  = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-runtime'),
    [ValidateRange(1, 4)]
    [int]$Jobs = 1,
    [switch]$RefreshSource
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$upstream = 'https://github.com/suyu-emu/suyu-main.git'
$revision = '347fcf91d95cd39cb5c3db50f1b6d827ad699311'

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

Require-Command git
Require-Command cmake

if ($RefreshSource -and (Test-Path $SourceDir)) {
    Remove-Item $SourceDir -Recurse -Force
}

if (-not (Test-Path (Join-Path $SourceDir '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $SourceDir -Parent) | Out-Null
    Write-Host "[Scarlet PC] Fetching pinned runtime source..." -ForegroundColor Cyan
    & git clone --filter=blob:none --recurse-submodules $upstream $SourceDir
    if ($LASTEXITCODE -ne 0) { throw "Runtime source clone failed." }
}

Push-Location $SourceDir
try {
    & git fetch origin $revision --depth 1
    if ($LASTEXITCODE -ne 0) { throw "Could not fetch pinned runtime revision." }

    & git checkout --detach $revision
    if ($LASTEXITCODE -ne 0) { throw "Could not checkout pinned runtime revision." }

    & git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "Runtime submodule update failed." }

    $actual = (& git rev-parse HEAD).Trim()
    if ($actual -ne $revision) {
        throw "Runtime source revision mismatch: $actual"
    }
}
finally {
    Pop-Location
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeHelp = (& cmake --help | Out-String)
$generatorArgs = @()
if ($cmakeHelp -match 'Visual Studio 18 2026') {
    $generatorArgs = @('-G', 'Visual Studio 18 2026', '-A', 'x64')
}
elseif ($cmakeHelp -match 'Visual Studio 17 2022') {
    $generatorArgs = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
}
elseif (Get-Command ninja -ErrorAction SilentlyContinue) {
    $generatorArgs = @('-G', 'Ninja')
}
else {
    throw "No supported Visual Studio or Ninja CMake generator was found."
}

Write-Host "[Scarlet PC] Configuring self-built runtime." -ForegroundColor Cyan
Write-Host "[Scarlet PC] Runtime revision: $revision" -ForegroundColor Cyan
Write-Host "[Scarlet PC] Dynarmic/JIT: disabled" -ForegroundColor Cyan

$configure = @(
    '-S', $SourceDir,
    '-B', $BuildDir
) + $generatorArgs + @(
    '-DENABLE_QT=OFF',
    '-DYUZU_CMD=ON',
    '-DSUYU_NO_JIT=ON',
    '-DENABLE_OPENGL=OFF',
    '-DENABLE_WEB_SERVICE=OFF',
    '-DENABLE_UPDATE_CHECKER=OFF',
    '-DUSE_DISCORD_PRESENCE=OFF',
    '-DYUZU_ROOM=OFF',
    '-DYUZU_ROOM_STANDALONE=OFF',
    '-DBUILD_TESTING=OFF',
    '-DYUZU_TESTS=OFF'
)

& cmake @configure
if ($LASTEXITCODE -ne 0) {
    throw "Scarlet runtime CMake configure failed."
}

& cmake --build $BuildDir --config Release --target suyu-cmd --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    throw "Scarlet runtime build failed."
}

$host = Get-ChildItem $BuildDir -Recurse -File -Filter 'suyu-cmd.exe' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $host) {
    throw "Runtime build succeeded but suyu-cmd.exe was not found."
}

$stage = Join-Path $BuildDir 'scarlet-stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $host.FullName (Join-Path $stage 'ScarletPC.exe') -Force

Get-ChildItem $BuildDir -Recurse -File -Filter '*.dll' |
    Where-Object { $_.FullName -notlike "$stage*" } |
    Sort-Object Name,LastWriteTime -Descending |
    Group-Object Name |
    ForEach-Object {
        Copy-Item $_.Group[0].FullName (Join-Path $stage $_.Name) -Force
    }

[ordered]@{
    project          = 'Scarlet PC'
    runtime_revision = $revision
    source_origin    = $upstream
    no_jit           = $true
    host_source      = $host.FullName
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json | Set-Content (Join-Path $stage 'runtime-build.json') -Encoding UTF8

Write-Host ""
Write-Host "[Scarlet PC] Self-built runtime host ready:" -ForegroundColor Green
Write-Host "  $stage\ScarletPC.exe" -ForegroundColor Green
