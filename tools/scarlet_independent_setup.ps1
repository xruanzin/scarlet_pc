param(
    [string]$ExportRoot,

    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [string]$UpdatePath,

    [string]$KeysPath,

    [string]$IconPath,

    [ValidateRange(1, 4)]
    [int]$Jobs = 1,

    [switch]$RefreshRuntimeSource,

    [switch]$RebuildRuntime,

    [switch]$RebuildAot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$game = (Resolve-Path $GamePath).Path

$update = $null
if (-not [string]::IsNullOrWhiteSpace($UpdatePath)) {
    $update = (Resolve-Path $UpdatePath).Path
}
else {
    $gameDir = Split-Path $game -Parent
    $autoUpdate = Join-Path $gameDir 'Pokemon Scarlet v4.0.0.nsp'
    if (Test-Path $autoUpdate -PathType Leaf) {
        $update = (Resolve-Path $autoUpdate).Path
        Write-Host "[Scarlet PC] Auto-detected update 4.0.0: $update" -ForegroundColor Green
    }
}

Write-Host '=== Scarlet PC self-built runtime bootstrap ===' -ForegroundColor Magenta

$aotDir = Join-Path $repoRoot 'build\aot\modules'
$aotModules = @(
    'recompiled_image.dll',
    'recompiled_rtld.dll',
    'recompiled_subsdk0.dll',
    'recompiled_sdk.dll'
)
$haveAot = $true
foreach ($module in $aotModules) {
    if (-not (Test-Path (Join-Path $aotDir $module) -PathType Leaf)) {
        $haveAot = $false
        break
    }
}

if ($haveAot -and -not $RebuildAot) {
    Write-Host '[Scarlet PC] Reusing existing Scarlet AOT modules.' -ForegroundColor Green
}
else {
    if ([string]::IsNullOrWhiteSpace($ExportRoot)) {
        throw 'AOT modules are missing. Pass -ExportRoot once, or use -RebuildAot with a valid export.'
    }
    & (Join-Path $PSScriptRoot 'scarlet_aot_build.ps1') -ExportRoot $ExportRoot -Jobs $Jobs
}
$resolvedIcon = $null
if (-not [string]::IsNullOrWhiteSpace($IconPath)) {
    $iconSource = (Resolve-Path $IconPath).Path
    if (Test-Path $iconSource -PathType Container) {
        $candidate = Join-Path $iconSource 'icon.ico'
        if (-not (Test-Path $candidate -PathType Leaf)) {
            throw 'IconPath directory must contain icon.ico.'
        }
        $resolvedIcon = $candidate
    }
    elseif ([IO.Path]::GetExtension($iconSource).Equals('.zip', [StringComparison]::OrdinalIgnoreCase)) {
        $iconImportDir = Join-Path $repoRoot 'build\scarlet-icon-import'
        Remove-Item $iconImportDir -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $iconImportDir | Out-Null
        Expand-Archive $iconSource -DestinationPath $iconImportDir -Force
        $candidate = Join-Path $iconImportDir 'icon.ico'
        if (-not (Test-Path $candidate -PathType Leaf)) {
            $candidate = Get-ChildItem $iconImportDir -Recurse -File -Filter '*.ico' | Select-Object -First 1 | ForEach-Object FullName
        }
        if ([string]::IsNullOrWhiteSpace([string]$candidate) -or -not (Test-Path $candidate -PathType Leaf)) {
            throw 'IconPath ZIP must contain an .ico file.'
        }
        $resolvedIcon = $candidate
    }
    elseif ([IO.Path]::GetExtension($iconSource).Equals('.ico', [StringComparison]::OrdinalIgnoreCase)) {
        $resolvedIcon = $iconSource
    }
    else {
        throw 'IconPath must point to an .ico file, a ZIP containing an .ico, or a directory containing icon.ico.'
    }
}

$runtimeStage = Join-Path $repoRoot 'build\scarlet-runtime\scarlet-stage'
$runtimeExe = Join-Path $runtimeStage 'ScarletPC.exe'
$runtimeManifest = Join-Path $runtimeStage 'scarlet_runtime_manifest.json'

$runtimeInputs = [System.Collections.Generic.List[string]]::new()
foreach ($path in @(
    (Join-Path $PSScriptRoot 'scarlet_runtime_build.ps1'),
    (Join-Path $PSScriptRoot 'scarlet_runtime_overlay.ps1'),
    (Join-Path $PSScriptRoot 'scarlet_vulkan_loader_import.ps1'),
    (Join-Path $PSScriptRoot 'scarlet_fetch_fidelityfx.ps1'),
    (Join-Path $repoRoot 'licenses\third-party\runtime-provenance.json')
)) {
    if (Test-Path $path -PathType Leaf) {
        $runtimeInputs.Add($path)
    }
}

$runtimeSourceOverlayDir = Join-Path $repoRoot 'runtime\nswitch'
if (Test-Path $runtimeSourceOverlayDir -PathType Container) {
    Get-ChildItem $runtimeSourceOverlayDir -Recurse -File | ForEach-Object {
        $runtimeInputs.Add($_.FullName)
    }
}


$runtimeNeedsBuild = $RebuildRuntime -or $RefreshRuntimeSource
if (-not $runtimeNeedsBuild) {
    if (-not (Test-Path $runtimeExe -PathType Leaf) -or
        -not (Test-Path $runtimeManifest -PathType Leaf)) {
        $runtimeNeedsBuild = $true
    }
    else {
        $runtimeBuiltAt = (Get-Item $runtimeExe).LastWriteTimeUtc
        foreach ($input in $runtimeInputs) {
            if ((Get-Item $input).LastWriteTimeUtc -gt $runtimeBuiltAt) {
                $runtimeNeedsBuild = $true
                break
            }
        }
    }
}

if ($runtimeNeedsBuild) {
    $runtimeArgs = @{
        Jobs = $Jobs
        RefreshSource = $RefreshRuntimeSource
    }
    if (-not [string]::IsNullOrWhiteSpace($resolvedIcon)) {
        $runtimeArgs.IconPath = $resolvedIcon
    }
    & (Join-Path $PSScriptRoot 'scarlet_runtime_build.ps1') @runtimeArgs
}
else {
    Write-Host '[Scarlet PC] Reusing existing self-built runtime; runtime inputs are unchanged.' -ForegroundColor Green
}

# Build the optional NVIDIA NGX sidecar after runtime source exists (it reuses Vulkan-Headers).
# This does not make DLSS mandatory: unsupported GPUs/drivers simply leave the bridge inactive.
& (Join-Path $PSScriptRoot 'scarlet_dlss_bridge_build.ps1')

# FSR4 v07 uses the same external model/shader bundle as the GoW3 Vulkan reference path.
# Downloads are cached under third_party and only missing files are fetched.
& (Join-Path $PSScriptRoot 'scarlet_fetch_fsr4_v07_assets.ps1') -Tiers both

$launcherArgs = @{}
if (-not [string]::IsNullOrWhiteSpace($resolvedIcon)) {
    $launcherArgs.IconPath = $resolvedIcon
}
& (Join-Path $PSScriptRoot 'scarlet_launcher_build.ps1') @launcherArgs
& (Join-Path $PSScriptRoot 'scarlet_independent_package.ps1')

$packageDir = Join-Path $repoRoot 'dist\ScarletPC-Independent'
$packageKeys = Join-Path $packageDir 'user\keys'
New-Item -ItemType Directory -Force -Path $packageKeys | Out-Null

if (-not [string]::IsNullOrWhiteSpace($KeysPath)) {
    $resolvedKeys = (Resolve-Path $KeysPath).Path
    if (Test-Path $resolvedKeys -PathType Container) {
        foreach ($name in @('prod.keys', 'title.keys')) {
            $candidate = Join-Path $resolvedKeys $name
            if (Test-Path $candidate -PathType Leaf) {
                Copy-Item $candidate (Join-Path $packageKeys $name) -Force
            }
        }
    }
    elseif (Test-Path $resolvedKeys -PathType Leaf) {
        $leaf = Split-Path $resolvedKeys -Leaf
        if ($leaf -notin @('prod.keys', 'title.keys')) {
            throw 'KeysPath file must be prod.keys or title.keys.'
        }
        Copy-Item $resolvedKeys (Join-Path $packageKeys $leaf) -Force
    }
}

if (-not (Test-Path (Join-Path $packageKeys 'prod.keys') -PathType Leaf)) {
    Write-Warning '[Scarlet PC] prod.keys is missing. Re-run setup with -KeysPath pointing to your legally obtained prod.keys or keys folder before launching.'
}

# Seed the launcher only on first setup. Rebuilds preserve the player's launcher choices.
$launcherConfig = Join-Path $packageDir 'user\config\scarlet_launcher.ini'
if (-not (Test-Path $launcherConfig -PathType Leaf)) {
    $seedKeys = Join-Path $packageKeys 'prod.keys'
    $seedUpdate = if ($update) { $update } else { '' }
    @"
[Game]
game=$game
update=$seedUpdate
keys=$seedKeys
dlc_count=0

[Graphics]
resolution=2
display_mode=1
fps=1
upscaler=0
quality=0
frame_generation=0
async_shaders=1

[Controls]
style=0
auto_assign=1

[General]
language=0
close_on_launch=0
"@ | Set-Content $launcherConfig -Encoding Unicode
}

$localConfig = Join-Path $repoRoot 'build\scarlet-independent-local.json'
[ordered]@{
    game_path   = $game
    update_path = $update
    title_id    = '0100A3D008C5C000'
    package_dir = $packageDir
} | ConvertTo-Json | Set-Content $localConfig -Encoding UTF8

Write-Host ''
Write-Host '[Scarlet PC] Independent bootstrap complete.' -ForegroundColor Green
Write-Host '[Scarlet PC] Standalone runtime package ready.' -ForegroundColor Green
Write-Host "[Scarlet PC] Launcher: $packageDir\ScarletLauncher.exe" -ForegroundColor Green
Write-Host '[Scarlet PC] Direct runtime test: .\tools\scarlet_independent_run.ps1' -ForegroundColor DarkGray
