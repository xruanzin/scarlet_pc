param(
    [int]$Jobs = 4,
    [switch]$Setup
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bash = "C:\msys64\usr\bin\bash.exe"

if ($Setup -or -not (Test-Path -LiteralPath $Bash)) {
    Write-Host "[scarlet-build] Preparando toolchain Windows/MSYS2..." -ForegroundColor Cyan
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "setup_windows.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "setup_windows.ps1 falhou (exit $LASTEXITCODE)"
    }
}

if (-not (Test-Path -LiteralPath $Bash)) {
    throw "MSYS2 nao encontrado em C:\msys64. Rode novamente com -Setup."
}

$RequiredGenerated = @(
    "generated\main\main_table.c",
    "generated\sdk\sdk_table.c",
    "generated\subsdk0\subsdk0_table.c"
)
foreach ($Rel in $RequiredGenerated) {
    $P = Join-Path $RepoRoot $Rel
    if (-not (Test-Path -LiteralPath $P -PathType Leaf)) {
        throw "Codigo gerado ausente: $P. Rode tools\scarlet_generate.ps1 primeiro."
    }
}

if ($Jobs -lt 1) { $Jobs = 1 }

$env:SCARLET_REPO = $RepoRoot
$env:SCARLET_JOBS = [string]$Jobs
$Log = Join-Path $RepoRoot "scarlet_build.log"
if (Test-Path -LiteralPath $Log) {
    Remove-Item -LiteralPath $Log -Force
}

Write-Host "[scarlet-build] Compilando runtime + 1.186 fontes geradas com -j$Jobs..." -ForegroundColor Cyan
Write-Host "[scarlet-build] Log: $Log"

$Cmd = @'
set -o pipefail
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
cd "$(cygpath -u "$SCARLET_REPO")"
make -j"$SCARLET_JOBS" EXE_NAME=ScarletRecomp build/ScarletRecomp.exe build/lasgame.dll 2>&1 | tee scarlet_build.log
'@

& $Bash -lc $Cmd
$Code = $LASTEXITCODE
if ($Code -ne 0) {
    throw "Build falhou (exit $Code). Envie as ultimas linhas de scarlet_build.log."
}

$Exe = Join-Path $RepoRoot "build\ScarletRecomp.exe"
$GameDll = Join-Path $RepoRoot "build\lasgame.dll"
$Id = Join-Path $RepoRoot "build\lasgame.dll.id"

foreach ($P in @($Exe, $GameDll, $Id)) {
    if (-not (Test-Path -LiteralPath $P -PathType Leaf)) {
        throw "Build terminou sem criar o artefato esperado: $P"
    }
}

Write-Host ""
Write-Host "[scarlet-build] BUILD CONCLUIDO" -ForegroundColor Green
Get-Item $Exe, $GameDll, $Id |
    Select-Object Name, @{N="SizeMB";E={[Math]::Round($_.Length / 1MB, 2)}}, LastWriteTime |
    Format-Table -AutoSize
