param(
    [int]$Jobs = 4,
    [int]$LtoJobs = 4
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $RepoRoot "build"
$Builder = Join-Path $PSScriptRoot "scarlet_build_windows.ps1"

Write-Host "[scarlet-restore] Recriando a lasgame.dll com o ultimo perfil conhecido por bootar..." -ForegroundColor Cyan
Write-Host "[scarlet-restore] Baseline funcional compacta: -O3 + x86-64-v3 + sem ThinLTO" -ForegroundColor Cyan

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Builder -GameDllOnly -Jobs $Jobs -LtoJobs $LtoJobs
if ($LASTEXITCODE -ne 0) {
    throw "Falha ao reconstruir a lasgame.dll conhecida."
}

$Dll = Join-Path $Build "lasgame.dll"
$Id = Join-Path $Build "lasgame.dll.id"
if (-not (Test-Path -LiteralPath $Dll -PathType Leaf) -or -not (Test-Path -LiteralPath $Id -PathType Leaf)) {
    throw "Build terminou sem lasgame.dll/lasgame.dll.id."
}

$Backup = Join-Path $Build "gamedll-backups"
New-Item -ItemType Directory -Force -Path $Backup | Out-Null
Copy-Item -LiteralPath $Dll -Destination (Join-Path $Backup "lasgame-known-good.dll") -Force
Copy-Item -LiteralPath $Id -Destination (Join-Path $Backup "lasgame-known-good.dll.id") -Force

$Hash = (Get-FileHash -LiteralPath $Dll -Algorithm SHA256).Hash
$SizeMB = [Math]::Round((Get-Item -LiteralPath $Dll).Length / 1MB, 2)

Write-Host ""
Write-Host "[scarlet-restore] DLL restaurada e congelada." -ForegroundColor Green
Write-Host "[scarlet-restore] SizeMB: $SizeMB"
Write-Host "[scarlet-restore] SHA256: $Hash"
Write-Host "[scarlet-restore] Backup: $Backup\lasgame-known-good.dll"
Write-Host "[scarlet-restore] Referencia historica: a DLL funcional compacta tinha ~348.43 MB; pequenas diferencas de tamanho podem ocorrer por toolchain/linker."
