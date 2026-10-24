param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\FidelityFX-SDK')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git'
# Same FidelityFX SDK revision used by Zelda-on-PC for its Vulkan FSR 3 frame-generation path.
$revision = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    throw 'git.exe is required to fetch the FidelityFX SDK.'
}

$gitMarker = Join-Path $Destination '.git'
if (-not (Test-Path $gitMarker)) {
    if (Test-Path $Destination) {
        $items = @(Get-ChildItem $Destination -Force -ErrorAction SilentlyContinue)
        if ($items.Count -gt 0) {
            throw "FidelityFX destination exists but is not a Git checkout: $Destination"
        }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
    Write-Host '[Scarlet PC] Fetching pinned AMD FidelityFX SDK...' -ForegroundColor Cyan
    & git clone --filter=blob:none --no-checkout $repo $Destination
    if ($LASTEXITCODE -ne 0) { throw 'Could not clone FidelityFX SDK.' }
}
else {
    Write-Host '[Scarlet PC] Using existing FidelityFX Git checkout/submodule.' -ForegroundColor DarkGray
}

Push-Location $Destination
try {
    & git sparse-checkout init --cone
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize FidelityFX sparse checkout.' }

    & git sparse-checkout set ffx-api PrebuiltSignedDLL
    if ($LASTEXITCODE -ne 0) { throw 'Could not configure FidelityFX sparse checkout.' }

    & git fetch origin $revision --depth 1
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch pinned FidelityFX SDK revision.' }

    & git checkout --detach $revision
    if ($LASTEXITCODE -ne 0) { throw 'Could not checkout pinned FidelityFX SDK revision.' }

    $actual = (& git rev-parse HEAD).Trim()
    if ($actual -ne $revision) {
        throw "FidelityFX SDK revision mismatch: $actual"
    }

    $headers = Join-Path $Destination 'ffx-api\include\ffx_api\ffx_framegeneration.h'
    $runtime = Join-Path $Destination 'PrebuiltSignedDLL\amd_fidelityfx_vk.dll'
    if (-not (Test-Path $headers -PathType Leaf)) {
        throw "Pinned FidelityFX SDK is missing expected headers: $headers"
    }
    if (-not (Test-Path $runtime -PathType Leaf)) {
        throw "Pinned FidelityFX SDK is missing amd_fidelityfx_vk.dll: $runtime"
    }

    $license = Join-Path $Destination 'LICENSE.txt'
    if (-not (Test-Path $license -PathType Leaf)) {
        $licenseSpec = $revision + ':LICENSE.txt'
        & git show $licenseSpec | Set-Content $license -Encoding UTF8
        if ($LASTEXITCODE -ne 0) { throw 'Could not materialize FidelityFX SDK license.' }
    }
}
finally {
    Pop-Location
}

Write-Host "[Scarlet PC] FidelityFX SDK ready: $Destination" -ForegroundColor Green
Write-Host "[Scarlet PC] Revision: $revision" -ForegroundColor DarkGray
