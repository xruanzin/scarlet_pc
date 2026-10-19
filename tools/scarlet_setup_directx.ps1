param(
    [string]$Archive = '',
    [string]$Destination = (Join-Path $PSScriptRoot 'directx\mesa')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scarlet_graphics_backend.ps1')
if (Test-Path -LiteralPath $Destination) {
    $Driver = Get-ScarletDirectXDriver -Directory $Destination
    Write-Host "[directx-setup] Mesa Dozen ja disponivel: $($Driver.Manifest)"
    exit 0
}
# Pinned publisher release; no unverified latest download and no system install.
$Version = '26.2.4'
$Expected = '351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695'
$Url = "https://github.com/pal1000/mesa-dist-win/releases/download/$Version/mesa3d-$Version-release-msvc.7z"
$Temporary = Join-Path ([IO.Path]::GetTempPath()) ('scarlet-directx-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $Temporary
try {
    if (-not $Archive) {
        $Archive = Join-Path $Temporary 'mesa.7z'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Host "[directx-setup] Baixando Mesa $Version do distribuidor..."
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Archive
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Expected) {
        throw 'Checksum Mesa incorreto. Nenhuma DLL foi instalada.'
    }
    $Extract = Join-Path $Temporary 'unpacked'
    $null = New-Item -ItemType Directory -Path $Extract
    & tar.exe -xf $Archive -C $Extract
    if ($LASTEXITCODE -ne 0) { throw 'tar.exe nao conseguiu extrair o pacote 7z. Extraia-o com 7-Zip e use -DirectXDirectory no runner.' }
    $Driver = Get-ScarletDirectXDriver -Directory $Extract
    $Parent = Split-Path -Parent ([IO.Path]::GetFullPath($Destination))
    $null = New-Item -ItemType Directory -Force -Path $Parent
    Move-Item -LiteralPath $Extract -Destination $Destination
    Write-Host '[directx-setup] Pronto. Driver portatil; registro e drivers do Windows nao foram alterados.'
} finally {
    Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction SilentlyContinue
}
