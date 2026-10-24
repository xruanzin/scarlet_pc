param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\FSR-Vulkan')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = 'https://github.com/FireBurn/FSR-Vulkan.git'
$revision = 'c64f093404125e960813f95f1996c14780f0dd69'

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    throw 'git.exe is required to fetch FSR-Vulkan.'
}

if (-not (Test-Path (Join-Path $Destination '.git') -PathType Container)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
    Write-Host '[Scarlet PC] Fetching pinned FSR-Vulkan source used by the GoW3 NVIDIA fork...' -ForegroundColor Cyan
    & git clone --filter=blob:none --no-checkout $repo $Destination
    if ($LASTEXITCODE -ne 0) { throw 'Could not clone FSR-Vulkan.' }
}

Push-Location $Destination
try {
    & git fetch origin $revision --depth 1
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch pinned FSR-Vulkan revision.' }

    & git checkout --detach $revision
    if ($LASTEXITCODE -ne 0) { throw 'Could not checkout pinned FSR-Vulkan revision.' }

    $actual = (& git rev-parse HEAD).Trim()
    if ($actual -ne $revision) {
        throw "FSR-Vulkan revision mismatch: $actual"
    }

    foreach ($required in @(
        'include\ffx_vk_portable.h',
        'include\ffx_vk_fsr3_3_1_5_bridge.h',
        'include\ffx_vk_fsr4_v07.h',
        'LICENSE.txt',
        'NOTICE.md'
    )) {
        if (-not (Test-Path (Join-Path $Destination $required) -PathType Leaf)) {
            throw "Pinned FSR-Vulkan checkout is missing $required"
        }
    }
}
finally {
    Pop-Location
}

Write-Host "[Scarlet PC] FSR-Vulkan ready: $Destination" -ForegroundColor Green
Write-Host "[Scarlet PC] Revision: $revision" -ForegroundColor DarkGray
