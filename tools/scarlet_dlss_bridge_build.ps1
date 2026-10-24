param(
    [string]$SdkRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\NVIDIA-DLSS'),
    [string]$RuntimeSource = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\scarlet-runtime-src'),
    [string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-dlss-bridge')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
& (Join-Path $PSScriptRoot 'scarlet_fetch_dlss.ps1') -Destination $SdkRoot

$vulkanInclude = Join-Path $RuntimeSource 'externals\Vulkan-Headers\include'
if (-not (Test-Path (Join-Path $vulkanInclude 'vulkan\vulkan.h') -PathType Leaf)) {
    throw 'Vulkan-Headers were not found in the Scarlet runtime source. Build/fetch the runtime first.'
}

$source = Join-Path $repoRoot 'runtime\nvidia'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeHelp = (& cmake --help | Out-String)
if ($cmakeHelp -match 'Visual Studio 18 2026') {
    $generator = @('-G', 'Visual Studio 18 2026', '-A', 'x64')
} elseif ($cmakeHelp -match 'Visual Studio 17 2022') {
    $generator = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
} else {
    throw 'Visual Studio 2022/2026 is required to build the NVIDIA NGX bridge.'
}

Write-Host '[Scarlet PC] Configuring optional NVIDIA DLSS bridge...' -ForegroundColor Cyan
$configure = @('-S', $source, '-B', $BuildDir) + $generator + @(
    "-DDLSS_SDK_ROOT=$($SdkRoot.Replace('\','/'))",
    "-DVULKAN_INCLUDE=$($vulkanInclude.Replace('\','/'))"
)
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'Scarlet DLSS bridge CMake configure failed.' }

& cmake --build $BuildDir --config Release --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Scarlet DLSS bridge build failed.' }

$stage = Join-Path $BuildDir 'stage'
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$bridge = Get-ChildItem $BuildDir -Recurse -File -Filter 'ScarletDlssBridge.dll' |
    Where-Object { $_.DirectoryName -ne $stage } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bridge) { throw 'DLSS bridge build succeeded but ScarletDlssBridge.dll was not found outside the stage directory.' }

$stageBridge = Join-Path $stage 'ScarletDlssBridge.dll'
if (-not [string]::Equals(
        [IO.Path]::GetFullPath($bridge.FullName),
        [IO.Path]::GetFullPath($stageBridge),
        [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item $bridge.FullName $stageBridge -Force
}
Copy-Item (Join-Path $SdkRoot 'lib\Windows_x86_64\rel\nvngx_dlss.dll') (Join-Path $stage 'nvngx_dlss.dll') -Force
Copy-Item (Join-Path $SdkRoot 'LICENSE.txt') (Join-Path $stage 'NVIDIA-DLSS-LICENSE.txt') -Force

Write-Host "[Scarlet PC] DLSS bridge ready: $stage" -ForegroundColor Green
