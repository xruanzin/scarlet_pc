param(
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildScript = Join-Path $PSScriptRoot "scarlet_build_windows.ps1"
$Makefile = Join-Path $RepoRoot "Makefile"
$GameDll = Join-Path $RepoRoot "build\lasgame.dll"

if (-not (Test-Path -LiteralPath $BuildScript -PathType Leaf)) {
    throw "Build script ausente: $BuildScript"
}
if (-not (Test-Path -LiteralPath $Makefile -PathType Leaf)) {
    throw "Makefile ausente: $Makefile"
}

$MakeText = Get-Content -LiteralPath $Makefile -Raw
if ($MakeText -notmatch '(?m)^GENFLAGS\s*:=\s*-O1(?:\s|$)') {
    throw "O Makefile nao esta no perfil original do LASRecomp (-O1). Nao vou reconstruir a DLL."
}

$Before = $null
if (Test-Path -LiteralPath $GameDll -PathType Leaf) {
    $BeforeItem = Get-Item -LiteralPath $GameDll
    $Before = [PSCustomObject]@{
        Bytes = [int64]$BeforeItem.Length
        MB = [Math]::Round($BeforeItem.Length / 1MB, 2)
        Sha256 = (Get-FileHash -LiteralPath $GameDll -Algorithm SHA256).Hash
    }
    Write-Host ("[scarlet-dll] ANTES: {0:N2} MB  SHA256={1}" -f $Before.MB, $Before.Sha256) -ForegroundColor Yellow
} else {
    Write-Host "[scarlet-dll] Nenhuma lasgame.dll anterior encontrada." -ForegroundColor DarkGray
}

Write-Host "[scarlet-dll] Rebuild upstream-style: -O1, sem ThinLTO, sem iniciar o jogo." -ForegroundColor Cyan
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -Jobs $Jobs -GameDllOnly -NoThinLTO
if ($LASTEXITCODE -ne 0) {
    throw "Rebuild de lasgame.dll falhou (exit $LASTEXITCODE). Envie scarlet_build.log."
}

if (-not (Test-Path -LiteralPath $GameDll -PathType Leaf)) {
    throw "Build terminou sem criar $GameDll"
}

$AfterItem = Get-Item -LiteralPath $GameDll
$After = [PSCustomObject]@{
    Bytes = [int64]$AfterItem.Length
    MB = [Math]::Round($AfterItem.Length / 1MB, 2)
    Sha256 = (Get-FileHash -LiteralPath $GameDll -Algorithm SHA256).Hash
}

Write-Host ""
Write-Host ("[scarlet-dll] DEPOIS: {0:N2} MB  SHA256={1}" -f $After.MB, $After.Sha256) -ForegroundColor Green

if ($Before) {
    $Saved = $Before.Bytes - $After.Bytes
    $Pct = if ($Before.Bytes -gt 0) { 100.0 * $Saved / $Before.Bytes } else { 0.0 }
    Write-Host ("[scarlet-dll] DIFERENCA: {0:N2} MB ({1:N2}%)" -f ($Saved / 1MB), $Pct) -ForegroundColor Green
}

Write-Host "[scarlet-dll] Concluido. ScarletRecomp.exe nao foi iniciado." -ForegroundColor Green
