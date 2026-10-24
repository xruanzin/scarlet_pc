param(
    [string]$RuntimeStage  = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-runtime\scarlet-stage'),
    [string]$LauncherStage = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-launcher\stage'),
    [string]$AotDir        = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\aot\modules'),
    [string]$OutputDir     = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\ScarletPC-Independent')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$runtimeHostPath = Join-Path $RuntimeStage 'ScarletPC.exe'
if (-not (Test-Path $runtimeHostPath -PathType Leaf)) {
    throw "Self-built Scarlet runtime host not found. Run tools\scarlet_runtime_build.ps1 first."
}

$aotMap = [ordered]@{
    'recompiled_image.dll'   = 'scarlet-main.dll'
    'recompiled_rtld.dll'    = 'scarlet-rtld.dll'
    'recompiled_subsdk0.dll' = 'scarlet-multimedia.dll'
    'recompiled_sdk.dll'     = 'scarlet-sdk.dll'
}

foreach ($sourceName in $aotMap.Keys) {
    if (-not (Test-Path (Join-Path $AotDir $sourceName) -PathType Leaf)) {
        throw "Missing AOT module: $sourceName"
    }
}

$preservedUser = Join-Path $repoRoot 'build\scarlet-package-user-preserve'
if (Test-Path $preservedUser) {
    Remove-Item $preservedUser -Recurse -Force
}

# A running launcher keeps its executable mapped and older builds also kept the
# background JPG open through GDI+. Stop only the launcher that belongs to this
# package before replacing the package contents.
$packageLauncher = Join-Path $OutputDir 'ScarletLauncher.exe'
Get-Process -Name 'ScarletLauncher' -ErrorAction SilentlyContinue | ForEach-Object {
    try {
        if ($_.Path -and
            [string]::Equals([IO.Path]::GetFullPath($_.Path),
                             [IO.Path]::GetFullPath($packageLauncher),
                             [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host '[Scarlet PC] Closing the running launcher so the package can be updated.' -ForegroundColor Yellow
            Stop-Process -Id $_.Id -Force
            $_.WaitForExit()
        }
    }
    catch {
        # If Windows denies querying a different process, leave it alone.
    }
}

if (Test-Path $OutputDir) {
    $existingUser = Join-Path $OutputDir 'user'
    if (Test-Path $existingUser -PathType Container) {
        New-Item -ItemType Directory -Force -Path (Split-Path $preservedUser -Parent) | Out-Null
        Move-Item $existingUser $preservedUser -Force
    }
    Remove-Item $OutputDir -Recurse -Force
}

$aotOut = Join-Path $OutputDir 'aot'
$runtimeOut = Join-Path $OutputDir 'runtime\nswitch'
$userRoot = Join-Path $OutputDir 'user'
$userConfig = Join-Path $userRoot 'config'
$userCache = Join-Path $userRoot 'cache\scarlet'
$userKeys = Join-Path $userRoot 'keys'
$userNand = Join-Path $userRoot 'nand'
$userSdmc = Join-Path $userRoot 'sdmc'
$userLoad = Join-Path $userRoot 'load'
$userLog = Join-Path $userRoot 'log'
$userDump = Join-Path $userRoot 'dump'
$userScreenshots = Join-Path $userRoot 'screenshots'
$userPlayTime = Join-Path $userRoot 'play_time'
$userCrashDumps = Join-Path $userRoot 'crash_dumps'
$userAmiibo = Join-Path $userRoot 'amiibo'
$userTas = Join-Path $userRoot 'tas'
$userIcons = Join-Path $userRoot 'icons'
$userThemes = Join-Path $userRoot 'themes'
$modsOut = Join-Path $OutputDir 'mods'
$assetsOut = Join-Path $OutputDir 'assets'
$launcherRuntimeOut = Join-Path $OutputDir 'runtime\launcher'
$licensesOut = Join-Path $OutputDir 'licenses\third-party'

foreach ($dir in @(
    $OutputDir, $aotOut, $runtimeOut, $userRoot, $userConfig, $userCache, $userKeys,
    $userNand, $userSdmc, $userLoad, $userLog, $userDump, $userScreenshots, $userPlayTime,
    $userCrashDumps, $userAmiibo, $userTas, $userIcons, $userThemes, $modsOut, $assetsOut,
    $launcherRuntimeOut, $licensesOut
)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

if (Test-Path $preservedUser -PathType Container) {
    Copy-Item (Join-Path $preservedUser '*') $userRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $preservedUser -Recurse -Force
}

Copy-Item $runtimeHostPath (Join-Path $OutputDir 'ScarletPC.exe') -Force

$launcherExe = Join-Path $LauncherStage 'ScarletLauncher.exe'
if (Test-Path $launcherExe -PathType Leaf) {
    Copy-Item $launcherExe (Join-Path $OutputDir 'ScarletLauncher.exe') -Force

    $launcherAssets = Join-Path $LauncherStage 'assets'
    if (Test-Path $launcherAssets -PathType Container) {
        Copy-Item (Join-Path $launcherAssets '*') $assetsOut -Recurse -Force
    }

    $launcherManifest = Join-Path $LauncherStage 'scarlet_launcher_manifest.json'
    if (Test-Path $launcherManifest -PathType Leaf) {
        Copy-Item $launcherManifest (Join-Path $launcherRuntimeOut 'scarlet_launcher_manifest.json') -Force
    }
}
else {
    Write-Warning '[Scarlet PC] Launcher stage not found; packaging runtime without ScarletLauncher.exe.'
}

# Keep host dependency DLLs beside ScarletPC.exe until the planned nswitch
# component split gives them a real stable DLL ABI.
Get-ChildItem $RuntimeStage -File -Filter '*.dll' |
    Copy-Item -Destination $OutputDir -Force

foreach ($entry in $aotMap.GetEnumerator()) {
    Copy-Item (Join-Path $AotDir $entry.Key) (Join-Path $aotOut $entry.Value) -Force
}

$runtimeManifest = Join-Path $RuntimeStage 'scarlet_runtime_manifest.json'
if (Test-Path $runtimeManifest) {
    Copy-Item $runtimeManifest (Join-Path $runtimeOut 'scarlet_runtime_manifest.json') -Force
}

$aotManifest = Join-Path $AotDir 'scarlet-aot-manifest.json'
if (Test-Path $aotManifest) {
    Copy-Item $aotManifest (Join-Path $aotOut 'scarlet_aot_report.json') -Force
}

$thirdParty = Join-Path $repoRoot 'THIRD_PARTY.md'
if (Test-Path $thirdParty) {
    Copy-Item $thirdParty (Join-Path $licensesOut 'THIRD_PARTY.md') -Force
}
$runtimeProvenance = Join-Path $repoRoot 'licenses\third-party\runtime-provenance.json'
if (Test-Path $runtimeProvenance) {
    Copy-Item $runtimeProvenance (Join-Path $licensesOut 'runtime-provenance.json') -Force
}

# AMD FidelityFX SDK runtime used by the optional FSR 3 frame-generation path.
$ffxRoot = Join-Path $repoRoot 'third_party\FidelityFX-SDK'
$ffxDll = Join-Path $ffxRoot 'PrebuiltSignedDLL\amd_fidelityfx_vk.dll'
$ffxLicense = Join-Path $ffxRoot 'LICENSE.txt'
if (Test-Path $ffxDll -PathType Leaf) {
    Copy-Item $ffxDll (Join-Path $OutputDir 'amd_fidelityfx_vk.dll') -Force
}
if (Test-Path $ffxLicense -PathType Leaf) {
    Copy-Item $ffxLicense (Join-Path $licensesOut 'FidelityFX-SDK.txt') -Force
}

# Experimental FSR4 source-v07 provider/model bundle used by the GoW3 Vulkan reference path.
$fsr4ProviderRoot = Join-Path $repoRoot 'third_party\FSR-Vulkan'
$fsr4AssetsRoot = Join-Path $repoRoot 'third_party\fsr4-v07-assets'
$fsr4AssetsOut = Join-Path $assetsOut 'fsr4-v07'
if (Test-Path $fsr4AssetsRoot -PathType Container) {
    New-Item -ItemType Directory -Force -Path $fsr4AssetsOut | Out-Null
    Copy-Item (Join-Path $fsr4AssetsRoot '*') $fsr4AssetsOut -Recurse -Force
}
$fsr4ProviderLicense = Join-Path $fsr4ProviderRoot 'LICENSE.txt'
$fsr4ProviderNotice = Join-Path $fsr4ProviderRoot 'NOTICE.md'
if (Test-Path $fsr4ProviderLicense -PathType Leaf) {
    Copy-Item $fsr4ProviderLicense (Join-Path $licensesOut 'FSR-Vulkan.txt') -Force
}
if (Test-Path $fsr4ProviderNotice -PathType Leaf) {
    Copy-Item $fsr4ProviderNotice (Join-Path $licensesOut 'FSR-Vulkan-NOTICE.md') -Force
}

# Optional NVIDIA DLSS sidecar. The runtime loads this only when DLSS is selected.
$dlssStage = Join-Path $repoRoot 'build\scarlet-dlss-bridge\stage'
foreach ($name in @('ScarletDlssBridge.dll', 'nvngx_dlss.dll')) {
    $candidate = Join-Path $dlssStage $name
    if (Test-Path $candidate -PathType Leaf) {
        Copy-Item $candidate (Join-Path $OutputDir $name) -Force
    }
}
$dlssLicense = Join-Path $dlssStage 'NVIDIA-DLSS-LICENSE.txt'
if (Test-Path $dlssLicense -PathType Leaf) {
    Copy-Item $dlssLicense (Join-Path $licensesOut 'NVIDIA-DLSS.txt') -Force
}

@'
Scarlet PC

Pokémon Scarlet-specific Windows recompilation project.

Player-facing layout:
  ScarletLauncher.exe        (recommended entry point)
  ScarletPC.exe              (game runtime)
  assets\scarlet_launcher_background.jpg
  amd_fidelityfx_vk.dll        (FSR 3/3.1 + frame generation runtime)
  assets\fsr4-v07               (experimental FSR4 v07 shaders/models)
  ScarletDlssBridge.dll         (optional NVIDIA NGX bridge)
  nvngx_dlss.dll                (NVIDIA DLSS runtime, when staged)
  aot\scarlet-main.dll
  aot\scarlet-rtld.dll
  aot\scarlet-multimedia.dll
  aot\scarlet-sdk.dll
  user\config\scarlet.ini
  user\cache\scarlet
  mods
  licenses\third-party

Third-party notices and provenance are kept under licenses\third-party.
No game, keys, firmware, or Nintendo assets are included.
'@ | Set-Content (Join-Path $OutputDir 'README_RUNTIME.txt') -Encoding UTF8

@'
Scarlet PC runtime boundary

The current host is still monolithic. This directory reserves the stable
nswitch runtime boundary for the planned component split. Placeholder runtime
DLLs are intentionally not shipped.
'@ | Set-Content (Join-Path $runtimeOut 'README.txt') -Encoding UTF8

Write-Host "[Scarlet PC] Independent package ready: $OutputDir" -ForegroundColor Green
if (Test-Path (Join-Path $OutputDir 'ScarletLauncher.exe') -PathType Leaf) {
    Write-Host "[Scarlet PC] Player launcher: $OutputDir\ScarletLauncher.exe" -ForegroundColor Green
}
Write-Host '[Scarlet PC] AOT package names: scarlet-main / scarlet-rtld / scarlet-multimedia / scarlet-sdk' -ForegroundColor DarkGray
