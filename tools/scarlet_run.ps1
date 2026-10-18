param(
    [string]$GameRoot = "D:\Pokemon Scarlet\Extracted\Base"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $RepoRoot "build\ScarletRecomp.exe"
$GameDll = Join-Path $RepoRoot "build\lasgame.dll"
$ExeFs = Join-Path $GameRoot "exefs"
$RomFs = Join-Path $GameRoot "romfs"

foreach ($P in @($Exe, $GameDll, (Join-Path $ExeFs "main"), (Join-Path $ExeFs "main.npdm"))) {
    if (-not (Test-Path -LiteralPath $P)) {
        throw "Arquivo necessario ausente: $P"
    }
}
if (-not (Test-Path -LiteralPath $RomFs -PathType Container)) {
    throw "RomFS extraido nao encontrado: $RomFs"
}

$MsysBin = "C:\msys64\ucrt64\bin"
if (-not (Test-Path -LiteralPath $MsysBin -PathType Container)) {
    throw "Runtime DLLs do MSYS2 nao encontradas. Rode tools\scarlet_build_windows.ps1 -Setup."
}

$env:Path = "$MsysBin;C:\msys64\usr\bin;$env:Path"
$env:LARECOMP_NO_LAUNCHER = "1"

$Log = Join-Path $RepoRoot "scarlet_boot.log"
if (Test-Path -LiteralPath $Log) {
    Remove-Item -LiteralPath $Log -Force
}

Write-Host "[scarlet-run] Primeiro boot do Scarlet..." -ForegroundColor Cyan
Write-Host "[scarlet-run] Game root: $GameRoot"
Write-Host "[scarlet-run] Log: $Log"
Write-Host ""

& $Exe $GameRoot --play 2>&1 | Tee-Object -FilePath $Log
exit $LASTEXITCODE
