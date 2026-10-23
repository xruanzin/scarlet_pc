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

@'
Scarlet PC

Pokémon Scarlet-specific Windows recompilation project.

Player-facing layout:
  ScarletLauncher.exe        (recommended entry point)
  ScarletPC.exe              (game runtime)
  assets\scarlet_launcher_background.jpg
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
