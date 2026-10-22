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

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Ninja build requires the MSVC environment, but vswhere.exe was not found.'
    }

    $vsRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($vsRoot)) {
        throw 'Visual Studio C++ Build Tools were not found.'
    }

    $vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd)) {
        throw "VsDevCmd.bat not found at $vsDevCmd"
    }

    Write-Host '[Scarlet PC] Loading MSVC x64 environment for Ninja...' -ForegroundColor DarkGray
    $lines = & cmd.exe /s /c "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $lines) {
        $p = $line.IndexOf('=')
        if ($p -gt 0) {
            $name = $line.Substring(0, $p)
            $value = $line.Substring($p + 1)
            Set-Item -Path "Env:$name" -Value $value
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'MSVC environment was loaded but cl.exe is still unavailable.'
    }
}

function Get-ScarletGlslangValidator {
    $existing = Get-Command glslangValidator.exe -ErrorAction SilentlyContinue
    if ($existing) {
        $repoRoot = Split-Path $PSScriptRoot -Parent
        $stableDir = Join-Path $repoRoot 'build\tools\glslang-bin'
        New-Item -ItemType Directory -Force -Path $stableDir | Out-Null
        $stableExe = Join-Path $stableDir 'glslangValidator.exe'
        Copy-Item $existing.Source $stableExe -Force
        return ($stableExe -replace '\\','/')
    }

    $version = '16.6.0'
    $revision = 'e1b562a8bed273a02f30b59b66a5d499793cede5'
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $toolsRoot = Join-Path $repoRoot 'third_party\tools'
    $source = Join-Path $toolsRoot "glslang-$version-src"
    $build = Join-Path $repoRoot "build\tools\glslang-$version"

    $exe = Get-ChildItem $build -Recurse -File -Filter 'glslangValidator.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $exe) {
        $exe = Get-ChildItem $build -Recurse -File -Filter 'glslang.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1
    }
    if ($exe) {
        $stableDir = Join-Path $repoRoot 'build\tools\glslang-bin'
        New-Item -ItemType Directory -Force -Path $stableDir | Out-Null
        $stableExe = Join-Path $stableDir 'glslangValidator.exe'
        Copy-Item $exe.FullName $stableExe -Force
        return ($stableExe -replace '\\','/')
    }

    New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null

    if (-not (Test-Path (Join-Path $source '.git'))) {
        Write-Host "[Scarlet PC] Fetching pinned glslang source $version..." -ForegroundColor Cyan
        & git clone --filter=blob:none --branch $version --depth 1 https://github.com/KhronosGroup/glslang.git $source
        if ($LASTEXITCODE -ne 0) {
            throw "Could not clone glslang source."
        }
    }

    Push-Location $source
    try {
        & git fetch origin $revision --depth 1
        if ($LASTEXITCODE -ne 0) { throw "Could not fetch pinned glslang revision." }

        & git checkout --detach $revision
        if ($LASTEXITCODE -ne 0) { throw "Could not checkout pinned glslang revision." }

        $actual = (& git rev-parse HEAD).Trim()
        if ($actual -ne $revision) {
            throw "glslang revision mismatch: $actual"
        }
    }
    finally {
        Pop-Location
    }

    $cmakeHelp = (& cmake --help | Out-String)
$generatorArgs = @()
$usingNinja = $false

# Upstream Windows builds use Ninja. It also avoids MSBuild custom-command
# quoting failures when invoking glslang for the host SPIR-V shaders.
if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    Import-MsvcEnvironment
    $generatorArgs = @('-G', 'Ninja')
    $usingNinja = $true
}
elseif ($cmakeHelp -match 'Visual Studio 18 2026') {
    $generatorArgs = @('-G', 'Visual Studio 18 2026', '-A', 'x64')
}
elseif ($cmakeHelp -match 'Visual Studio 17 2022') {
    $generatorArgs = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
}
else {
    throw 'No supported Visual Studio or Ninja CMake generator was found.'
}

$cache = Join-Path $BuildDir 'CMakeCache.txt'
if ($usingNinja -and (Test-Path $cache)) {
    $cachedGenerator = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' | Select-Object -First 1
    if ($cachedGenerator -and $cachedGenerator.Matches[0].Groups[1].Value -ne 'Ninja') {
        Write-Host '[Scarlet PC] Switching runtime build from MSBuild to Ninja; clearing only the runtime build directory.' -ForegroundColor Yellow
        Remove-Item $BuildDir -Recurse -Force
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    }
}
Write-Host "[Scarlet PC] Configuring self-built runtime." -ForegroundColor Cyan
Write-Host "[Scarlet PC] Runtime revision: $revision" -ForegroundColor Cyan
Write-Host "[Scarlet PC] Dynarmic/JIT: disabled" -ForegroundColor Cyan

$configure = @(
    '-S', $SourceDir,
    '-B', $BuildDir
) + $generatorArgs + @(
    $(if ($usingNinja) { '-DCMAKE_BUILD_TYPE=Release' } else { $null }),
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
    '-DYUZU_TESTS=OFF',
    "-DGLSLANGVALIDATOR:FILEPATH=$glslangValidator"
)

$configure = @($configure | Where-Object { $null -ne $_ -and $_ -ne '' })
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
 | Select-Object -First 1
    if ($cachedGenerator -and $cachedGenerator.Matches[0].Groups[1].Value -ne 'Ninja') {
        Write-Host '[Scarlet PC] Switching runtime build from MSBuild to Ninja; clearing only the runtime build directory.' -ForegroundColor Yellow
        Remove-Item $BuildDir -Recurse -Force
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    }
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
    '-DYUZU_TESTS=OFF',
    "-DGLSLANGVALIDATOR:FILEPATH=$glslangValidator"
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
