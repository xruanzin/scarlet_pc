param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\NVIDIA-DLSS')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = 'https://github.com/NVIDIA/DLSS.git'
$revision = '9981c6350dc58c0ce21f024fd65e31e851c31689'

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    throw 'git.exe is required to fetch the NVIDIA DLSS SDK.'
}

if (-not (Test-Path (Join-Path $Destination '.git') -PathType Container)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
    Write-Host '[Scarlet PC] Fetching pinned NVIDIA DLSS SDK...' -ForegroundColor Cyan
    & git clone --filter=blob:none --no-checkout $repo $Destination
    if ($LASTEXITCODE -ne 0) { throw 'Could not clone NVIDIA DLSS SDK.' }
}

Push-Location $Destination
try {
    & git sparse-checkout init --cone
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize NVIDIA DLSS sparse checkout.' }

    & git sparse-checkout set include lib/Windows_x86_64
    if ($LASTEXITCODE -ne 0) { throw 'Could not configure NVIDIA DLSS sparse checkout.' }

    & git fetch origin $revision --depth 1
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch pinned NVIDIA DLSS SDK revision.' }

    & git checkout --detach $revision
    if ($LASTEXITCODE -ne 0) { throw 'Could not checkout pinned NVIDIA DLSS SDK revision.' }

    $actual = (& git rev-parse HEAD).Trim()
    if ($actual -ne $revision) {
        throw "NVIDIA DLSS SDK revision mismatch: $actual"
    }

    $header = Join-Path $Destination 'include\nvsdk_ngx_vk.h'
    $library = Join-Path $Destination 'lib\Windows_x86_64\x64\nvsdk_ngx_s.lib'
    $runtime = Join-Path $Destination 'lib\Windows_x86_64\rel\nvngx_dlss.dll'
    if (-not (Test-Path $header -PathType Leaf) -or
        -not (Test-Path $library -PathType Leaf) -or
        -not (Test-Path $runtime -PathType Leaf)) {
        throw 'Pinned NVIDIA DLSS SDK is missing expected Windows x64 files.'
    }

    $license = Join-Path $Destination 'LICENSE.txt'
    if (-not (Test-Path $license -PathType Leaf)) {
        $licenseSpec = $revision + ':LICENSE.txt'
        & git show $licenseSpec | Set-Content $license -Encoding UTF8
        if ($LASTEXITCODE -ne 0) { throw 'Could not materialize NVIDIA DLSS SDK license.' }
    }
}
finally {
    Pop-Location
}

Write-Host "[Scarlet PC] NVIDIA DLSS SDK ready: $Destination" -ForegroundColor Green
Write-Host "[Scarlet PC] Revision: $revision" -ForegroundColor DarkGray
