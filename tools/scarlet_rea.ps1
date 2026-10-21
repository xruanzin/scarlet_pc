param(
    [Parameter(Mandatory=$true)]
    [Alias("PackagePath")]
    [string]$Package,
    [string]$KeysPath = "",
    [string]$FuseNxPath = "",
    [string]$HotJson = "",
    [int]$Top = 8,
    [string]$Distro = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$MountScript = Join-Path $PSScriptRoot "scarlet_mount_package.ps1"
$ReaRunner = Join-Path $PSScriptRoot "rea\run_hot_rea.ps1"

if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) {
    throw "Pacote nao encontrado: $Package"
}
if (-not (Test-Path -LiteralPath $MountScript -PathType Leaf)) {
    throw "Montador Scarlet ausente: $MountScript"
}
if (-not (Test-Path -LiteralPath $ReaRunner -PathType Leaf)) {
    throw "Runner REA ausente: $ReaRunner"
}

if (-not $HotJson) {
    $Candidates = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot "build") -Directory -Filter "scarlet-performance-*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
    foreach ($Dir in $Candidates) {
        $P = Join-Path $Dir.FullName "hot_functions.json"
        if (Test-Path -LiteralPath $P -PathType Leaf) {
            $HotJson = $P
            break
        }
    }
}
if (-not $HotJson -or -not (Test-Path -LiteralPath $HotJson -PathType Leaf)) {
    throw "hot_functions.json nao encontrado. Rode um -Profile antes ou informe -HotJson."
}
$HotJson = (Resolve-Path -LiteralPath $HotJson).Path

$MountPid = 0
$MountPoint = ""
try {
    Write-Host "[scarlet-rea] Hotspots: $HotJson" -ForegroundColor Cyan
    Write-Host "[scarlet-rea] Montando NSP/XCI somente para leitura..." -ForegroundColor Cyan

    $MountArgs = @{ PackagePath = (Resolve-Path -LiteralPath $Package).Path }
    if ($KeysPath) { $MountArgs.KeysPath = $KeysPath }
    if ($FuseNxPath) { $MountArgs.FuseNxPath = $FuseNxPath }

    $Out = @(& $MountScript @MountArgs)
    $JsonLine = @($Out | Where-Object { $_ -is [string] -and $_.Trim().StartsWith("{") } | Select-Object -Last 1)
    if (-not $JsonLine) { throw "Montagem nao retornou os caminhos virtuais." }

    $Info = $JsonLine[0] | ConvertFrom-Json
    $MountPid = [int]$Info.ProcessId
    $MountPoint = [string]$Info.MountPoint
    $ExeFs = [string]$Info.ExeFs

    Write-Host "[scarlet-rea] ExeFS: $ExeFs" -ForegroundColor Green
    Write-Host "[scarlet-rea] Chamando REA/Ghidra nos TOP $Top hotspots..." -ForegroundColor Magenta

    $Args = @{
        ExeFs = $ExeFs
        HotJson = $HotJson
        Top = $Top
    }
    if ($Distro) { $Args.Distro = $Distro }
    & $ReaRunner @Args
    if ($LASTEXITCODE -ne 0) { throw "REA terminou com exit $LASTEXITCODE." }

    $ResultDir = Join-Path $RepoRoot "analysis\rea\functions"
    Write-Host ""
    Write-Host "[scarlet-rea] ANALISE REA CONCLUIDA" -ForegroundColor Green
    Write-Host "[scarlet-rea] Dossiers: $ResultDir" -ForegroundColor Cyan
}
finally {
    if ($MountPid -gt 0) {
        $p = Get-Process -Id $MountPid -ErrorAction SilentlyContinue
        if ($p) {
            Write-Host "[scarlet-rea] Desmontando $MountPoint ..." -ForegroundColor DarkGray
            Stop-Process -Id $MountPid -Force -ErrorAction SilentlyContinue
            for ($i=0; $i -lt 40 -and $MountPoint -and (Test-Path -LiteralPath ($MountPoint + "\")); $i++) {
                Start-Sleep -Milliseconds 100
            }
        }
    }
}
