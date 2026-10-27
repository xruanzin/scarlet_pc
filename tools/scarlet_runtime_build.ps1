param(
    [string]$SourceDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\scarlet-runtime-src'),
    [string]$BuildDir  = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-runtime'),
    [ValidateRange(1, 4)]
    [int]$Jobs = 1,
    [string]$IconPath,
    [switch]$RefreshSource,
    [switch]$FastIncremental
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$provenancePath = Join-Path $repoRoot 'licenses\third-party\runtime-provenance.json'
if (-not (Test-Path $provenancePath -PathType Leaf)) {
    throw "Missing runtime provenance file: $provenancePath"
}
$provenance = Get-Content $provenancePath -Raw | ConvertFrom-Json
$upstream = [string]$provenance.source_url
$revision = [string]$provenance.validated_revision
$legacyTarget = [string]$provenance.legacy.command_target
$legacyExeName = [string]$provenance.legacy.command_executable
$legacyNoJitOption = [string]$provenance.legacy.no_jit_option

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

Require-Command git
Require-Command cmake

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Ninja build requires MSVC Build Tools, but vswhere.exe was not found.'
    }

    $vsRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($vsRoot)) {
        throw 'Visual Studio C++ Build Tools were not found.'
    }

    $vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd)) { throw "VsDevCmd.bat not found at $vsDevCmd" }

    Write-Host '[Scarlet PC] Loading MSVC x64 environment for Ninja...' -ForegroundColor DarkGray
    $envLines = & cmd.exe /s /c "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $envLines) {
        $p = $line.IndexOf('=')
        if ($p -gt 0) {
            Set-Item -Path ("Env:" + $line.Substring(0, $p)) -Value $line.Substring($p + 1)
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'MSVC environment was loaded but cl.exe is still unavailable.'
    }
}

function Get-ScarletGlslangValidator {
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $stableDir = Join-Path $repoRoot 'build\tools\glslang-bin'
    $stableExe = Join-Path $stableDir 'glslangValidator.exe'

    if (Test-Path $stableExe -PathType Leaf) {
        return ($stableExe -replace '\\','/')
    }

    $existing = Get-Command glslangValidator.exe -ErrorAction SilentlyContinue
    if ($existing) {
        New-Item -ItemType Directory -Force -Path $stableDir | Out-Null
        Copy-Item $existing.Source $stableExe -Force
        return ($stableExe -replace '\\','/')
    }

    $version = '16.6.0'
    $glslangRevision = 'e1b562a8bed273a02f30b59b66a5d499793cede5'
    $toolsRoot = Join-Path $repoRoot 'third_party\tools'
    $source = Join-Path $toolsRoot "glslang-$version-src"
    $build = Join-Path $repoRoot "build\tools\glslang-$version"

    $cached = Get-ChildItem $build -Recurse -File -Filter 'glslangValidator.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cached) {
        $cached = Get-ChildItem $build -Recurse -File -Filter 'glslang.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if ($cached) {
        New-Item -ItemType Directory -Force -Path $stableDir | Out-Null
        Copy-Item $cached.FullName $stableExe -Force
        return ($stableExe -replace '\\','/')
    }

    New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
    if (-not (Test-Path (Join-Path $source '.git'))) {
        Write-Host "[Scarlet PC] Fetching pinned glslang source $version..." -ForegroundColor Cyan
        & git clone --filter=blob:none --branch $version --depth 1 https://github.com/KhronosGroup/glslang.git $source
        if ($LASTEXITCODE -ne 0) { throw 'Could not clone glslang source.' }
    }

    Push-Location $source
    try {
        & git fetch origin $glslangRevision --depth 1
        if ($LASTEXITCODE -ne 0) { throw 'Could not fetch pinned glslang revision.' }
        & git checkout --detach $glslangRevision
        if ($LASTEXITCODE -ne 0) { throw 'Could not checkout pinned glslang revision.' }
        $actual = (& git rev-parse HEAD).Trim()
        if ($actual -ne $glslangRevision) { throw "glslang revision mismatch: $actual" }
    } finally {
        Pop-Location
    }

    $cmakeHelp = (& cmake --help | Out-String)
    if ($cmakeHelp -match 'Visual Studio 18 2026') {
        $glslangGenerator = @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    } elseif ($cmakeHelp -match 'Visual Studio 17 2022') {
        $glslangGenerator = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    } else {
        Import-MsvcEnvironment
        $glslangGenerator = @('-G', 'Ninja')
    }

    New-Item -ItemType Directory -Force -Path $build | Out-Null
    Write-Host '[Scarlet PC] Building pinned glslangValidator locally...' -ForegroundColor Cyan
    $glslangConfigure = @('-S', $source, '-B', $build) + $glslangGenerator + @(
        '-DGLSLANG_TESTS=OFF',
        '-DGLSLANG_ENABLE_INSTALL=OFF',
        '-DENABLE_GLSLANG_BINARIES=ON',
        '-DENABLE_OPT=OFF',
        '-DBUILD_EXTERNAL=OFF',
        '-DENABLE_HLSL=OFF'
    )
    & cmake @glslangConfigure
    if ($LASTEXITCODE -ne 0) { throw 'glslang CMake configure failed.' }
    & cmake --build $build --config Release --target glslang-standalone --parallel 1
    if ($LASTEXITCODE -ne 0) { throw 'glslang build failed.' }

    $built = Get-ChildItem $build -Recurse -File -Filter 'glslangValidator.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $built) {
        $built = Get-ChildItem $build -Recurse -File -Filter 'glslang.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }
    if (-not $built) { throw 'glslang build completed but no shader compiler executable was found.' }

    New-Item -ItemType Directory -Force -Path $stableDir | Out-Null
    Copy-Item $built.FullName $stableExe -Force
    Write-Host "[Scarlet PC] Shader compiler ready: $stableExe" -ForegroundColor Green
    return ($stableExe -replace '\\','/')
}

$glslangValidator = Get-ScarletGlslangValidator

# FidelityFX SDK headers/runtime are pinned to the same revision used by Zelda-on-PC's
# Vulkan FSR 3 frame-generation integration.
if (-not $FastIncremental) {
    & (Join-Path $PSScriptRoot 'scarlet_fetch_fidelityfx.ps1')
} else {
    $fastFfxHeader = Join-Path $repoRoot 'third_party\FidelityFX-SDK\ffx-api\include\ffx_api\ffx_framegeneration.h'
    if (-not (Test-Path $fastFfxHeader -PathType Leaf)) {
        throw 'Fast incremental build needs the existing FidelityFX checkout. Run the full setup once first.'
    }
}

# The FSR4 v07 provider is a pinned submodule. Initialize it automatically for users who
# pulled the branch without --recurse-submodules.
$fsrVulkanDir = Join-Path $repoRoot 'third_party\FSR-Vulkan'
$fsrVulkanHeader = Join-Path $fsrVulkanDir 'include\ffx_vk_fsr4_v07.h'
if (-not (Test-Path $fsrVulkanHeader -PathType Leaf)) {
    Push-Location $repoRoot
    try {
        Write-Host '[Scarlet PC] Initializing pinned FSR-Vulkan provider...' -ForegroundColor Cyan
        & git submodule update --init --recursive -- third_party/FSR-Vulkan
        if ($LASTEXITCODE -ne 0) { throw 'Could not initialize third_party/FSR-Vulkan.' }
    } finally {
        Pop-Location
    }
}
if (-not (Test-Path $fsrVulkanHeader -PathType Leaf)) {
    throw "FSR-Vulkan provider header is still missing: $fsrVulkanHeader"
}

# FireBurn's provider intentionally calls the normal Vulkan loader entry points. Scarlet's
# upstream runtime normally uses only Vulkan headers, so generate a tiny import library
# locally from the system vulkan-1.dll export contract; no Vulkan SDK install is required.
$vulkanImportLib = Join-Path $repoRoot 'build\tools\vulkan-loader-import\vulkan-1.lib'
# This import library is tiny and its export list evolves with the embedded
# FidelityFX backend. Regenerate it even in fast-incremental mode so an older
# cached .lib cannot leave newly-required Vulkan entry points unresolved.
& (Join-Path $PSScriptRoot 'scarlet_vulkan_loader_import.ps1')


if ($RefreshSource -and (Test-Path $SourceDir)) { Remove-Item $SourceDir -Recurse -Force }

if (-not (Test-Path (Join-Path $SourceDir '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $SourceDir -Parent) | Out-Null
    Write-Host '[Scarlet PC] Fetching pinned runtime source...' -ForegroundColor Cyan
    & git clone --quiet --filter=blob:none --recurse-submodules $upstream $SourceDir
    if ($LASTEXITCODE -ne 0) { throw 'Runtime source clone failed.' }
}

Push-Location $SourceDir
try {
    if ($FastIncremental) {
        $actual = (& git rev-parse HEAD).Trim()
        if ($actual -ne $revision) {
            throw "Fast incremental runtime source is not at the pinned revision: $actual"
        }
        Write-Host '[Scarlet PC] Fast incremental mode: preserving patched runtime source and Ninja cache.' -ForegroundColor Green
    } else {
        & git fetch --quiet origin $revision --depth 1
        if ($LASTEXITCODE -ne 0) { throw 'Could not fetch pinned runtime revision.' }
        & git checkout --quiet --detach $revision
        if ($LASTEXITCODE -ne 0) { throw 'Could not checkout pinned runtime revision.' }

        # The Scarlet overlay intentionally edits the pinned runtime checkout.
        # Reset those generated/local edits before applying the current overlay again,
        # otherwise an interrupted or older build can leave stale branding/loader code.
        & git reset --hard --quiet $revision
        if ($LASTEXITCODE -ne 0) { throw 'Could not reset pinned runtime source.' }
        & git clean -fdq
        if ($LASTEXITCODE -ne 0) { throw 'Could not clean pinned runtime source.' }

        & git submodule update --quiet --init --recursive
        if ($LASTEXITCODE -ne 0) { throw 'Runtime submodule update failed.' }
        $actual = (& git rev-parse HEAD).Trim()
        if ($actual -ne $revision) { throw "Runtime source revision mismatch: $actual" }
    }
} finally {
    Pop-Location
}

$overlayArgs = @{
    SourceDir = $SourceDir
    FastIncremental = $FastIncremental
}
if (-not [string]::IsNullOrWhiteSpace($IconPath)) {
    $overlayArgs.IconPath = $IconPath
}
& (Join-Path $PSScriptRoot 'scarlet_runtime_overlay.ps1') @overlayArgs
& (Join-Path $PSScriptRoot 'scarlet_runtime_ports_parity.ps1') -SourceDir $SourceDir
& (Join-Path $PSScriptRoot 'scarlet_runtime_sync_fastpaths.ps1') -SourceDir $SourceDir
& (Join-Path $PSScriptRoot 'scarlet_runtime_frame_probe.ps1') -SourceDir $SourceDir
& (Join-Path $PSScriptRoot 'scarlet_runtime_hotspot_sampler.ps1') -SourceDir $SourceDir
& (Join-Path $PSScriptRoot 'scarlet_runtime_hotspot_trace.ps1') -SourceDir $SourceDir
& (Join-Path $PSScriptRoot 'scarlet_runtime_chain_time_probe.ps1') -SourceDir $SourceDir

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Windows resource compilers do not always track the referenced .ico as a Ninja dependency.
# Drop only compiled .rc resources so a changed Scarlet icon is guaranteed to be embedded.
if (-not $FastIncremental) {
    Get-ChildItem $BuildDir -Recurse -File -Filter '*.rc.res' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

# The public log format is part of Scarlet PC branding. This pinned runtime has
# two logging implementations; force both formatter objects to rebuild so an
# incremental Ninja build can never keep legacy __FILE__ paths such as suyu_cmd\suyu.cpp.
if (-not $FastIncremental) {
    foreach ($objectName in @('text_formatter.cpp.obj', 'logging.cpp.obj')) {
        Get-ChildItem $BuildDir -Recurse -File -Filter $objectName -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

$cmakeHelp = (& cmake --help | Out-String)
$generatorArgs = @()
$usingNinja = $false

if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    Import-MsvcEnvironment
    $generatorArgs = @('-G', 'Ninja')
    $usingNinja = $true
} elseif ($cmakeHelp -match 'Visual Studio 18 2026') {
    $generatorArgs = @('-G', 'Visual Studio 18 2026', '-A', 'x64')
} elseif ($cmakeHelp -match 'Visual Studio 17 2022') {
    $generatorArgs = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
} else {
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

Write-Host "[Scarlet PC] Runtime revision: $revision" -ForegroundColor Cyan
Write-Host '[Scarlet PC] AOT primary; dynamic fallback: enabled' -ForegroundColor Cyan

$buildNinja = Join-Path $BuildDir 'build.ninja'
$canSkipConfigure = $FastIncremental -and $usingNinja -and
    (Test-Path $cache -PathType Leaf) -and (Test-Path $buildNinja -PathType Leaf)

if ($canSkipConfigure) {
    Write-Host '[Scarlet PC] Fast incremental mode: reusing existing CMake/Ninja graph without reconfigure.' -ForegroundColor Green
}
else {
    Write-Host '[Scarlet PC] Configuring self-built runtime.' -ForegroundColor Cyan

    $configure = @('-S', $SourceDir, '-B', $BuildDir) + $generatorArgs
    if ($usingNinja) { $configure += '-DCMAKE_BUILD_TYPE=Release' }
    $configure += @(
        '-DENABLE_QT=OFF',
        '-DYUZU_CMD=ON',
        ("-DCMAKE_INSTALL_PREFIX={0}" -f ((Join-Path $BuildDir 'install') -replace '\\','/')),
        ("-D{0}=OFF" -f $legacyNoJitOption),
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
    if ($LASTEXITCODE -ne 0) { throw 'Scarlet runtime CMake configure failed.' }
}

$buildLog = Join-Path $BuildDir 'scarlet_runtime_build.log'
Write-Host '[Scarlet PC] Building runtime...' -ForegroundColor Cyan

# Windows PowerShell 5.1 promotes native stderr lines to ErrorRecord objects. With
# ErrorActionPreference=Stop, harmless CMake/dependency warnings can abort the wrapper
# even when cmake/ninja itself is still healthy. Let native stderr flow into the log and
# decide success strictly from the process exit code.
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & cmake --build $BuildDir --config Release --target $legacyTarget --parallel $Jobs 2>&1 |
        Tee-Object -FilePath $buildLog | Out-Null
    $buildExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousErrorActionPreference
}
if ($buildExit -ne 0) {
    Write-Host '[Scarlet PC] Runtime build failed. Last compiler messages:' -ForegroundColor Red
    Get-Content $buildLog -Tail 80
    throw 'Scarlet runtime build failed.'
}
Write-Host '[Scarlet PC] Runtime build complete.' -ForegroundColor Green

$runtimeExe = Get-ChildItem $BuildDir -Recurse -File -Filter 'ScarletPC.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $runtimeExe) { throw 'Runtime build succeeded but its host executable was not found.' }

$stage = Join-Path $BuildDir 'scarlet-stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $runtimeExe.FullName (Join-Path $stage 'ScarletPC.exe') -Force

Get-ChildItem $BuildDir -Recurse -File -Filter '*.dll' |
    Where-Object { $_.FullName -notlike "$stage*" } |
    Sort-Object Name,LastWriteTime -Descending |
    Group-Object Name |
    ForEach-Object { Copy-Item $_.Group[0].FullName (Join-Path $stage $_.Name) -Force }

[ordered]@{
    project          = 'Scarlet PC'
    runtime_revision = $revision
    no_jit           = $false
    aot_primary      = $true
    jit_fallback     = $true
    runtime_interface = 'nswitch'
    host              = 'ScarletPC.exe'
    window_title      = 'Pokémon Scarlet'
    config_file       = 'user/config/scarlet.ini'
    aot_directory     = 'aot'
    aot_modules       = @('scarlet-main.dll', 'scarlet-rtld.dll', 'scarlet-multimedia.dll', 'scarlet-sdk.dll')
    generated_at_utc  = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $stage 'scarlet_runtime_manifest.json') -Encoding UTF8

Write-Host ''
Write-Host '[Scarlet PC] Self-built runtime host ready:' -ForegroundColor Green
Write-Host "  $stage\ScarletPC.exe" -ForegroundColor Green
