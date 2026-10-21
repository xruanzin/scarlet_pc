param(
    [Parameter(Mandatory=$true)][string]$ExeFs,
    [string]$HotJson = "analysis\rea\hot_functions.json",
    [int]$Top = 8,
    [string]$Distro = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Exporter = Join-Path $PSScriptRoot "export_nso_image.py"
$RawDir = Join-Path $RepoRoot "analysis\rea\raw"
$OutDir = Join-Path $RepoRoot "analysis\rea\functions"
$HotPath = if ([IO.Path]::IsPathRooted($HotJson)) { $HotJson } else { Join-Path $RepoRoot $HotJson }

if (-not (Test-Path -LiteralPath $HotPath -PathType Leaf)) {
    throw "Hot JSON nao encontrado: $HotPath"
}
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "WSL nao encontrado. REA/Ghidra raw ARM64 precisa rodar no Linux/WSL."
}

$Python = Join-Path $RepoRoot ".venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    $Pc = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $Pc) { $Pc = Get-Command python -ErrorAction SilentlyContinue }
    if (-not $Pc) { throw "Python nao encontrado." }
    $Python = $Pc.Source
}

Push-Location $RepoRoot
try {
    & $Python $Exporter --exefs $ExeFs --hot-json $HotPath --outdir $RawDir --top $Top
    if ($LASTEXITCODE -ne 0) { throw "Falha exportando NSO para REA." }
}
finally { Pop-Location }

$WslPrefix = @()
if ($Distro) { $WslPrefix = @("-d", $Distro) }

function Invoke-WslText([string[]]$Args) {
    $r = & wsl.exe @WslPrefix -- @Args 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ($r -join [Environment]::NewLine)
    }
    return ($r -join [Environment]::NewLine).Trim()
}

$RepoWsl = Invoke-WslText @("wslpath", "-a", $RepoRoot)
$Probe = & wsl.exe @WslPrefix -- bash -lc "command -v rea >/dev/null 2>&1 && rea doctor --json >/dev/null 2>&1"
if ($LASTEXITCODE -ne 0) {
    throw @"
REA/Ghidra ainda nao esta pronto dentro do WSL.
No WSL, configure Ghidra/JDK e rode:
  npm install --global rea-agents
  rea doctor --json

Depois execute este script novamente.
"@
}

$ManifestPath = Join-Path $RawDir "manifest.json"
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$null = New-Item -ItemType Directory -Force -Path $OutDir

$Done = 0
foreach ($Prop in $Manifest.modules.PSObject.Properties) {
    $Module = $Prop.Name
    $Info = $Prop.Value
    $ImageRel = ([string]$Info.image).Replace("\","/")
    $SeedsRel = ([string]$Info.seeds).Replace("\","/")
    if ([IO.Path]::IsPathRooted([string]$Info.image)) {
        $ImageWsl = Invoke-WslText @("wslpath","-a",[string]$Info.image)
        $SeedsWsl = Invoke-WslText @("wslpath","-a",[string]$Info.seeds)
    } else {
        $ImageWsl = "$RepoWsl/$ImageRel"
        $SeedsWsl = "$RepoWsl/$SeedsRel"
    }
    $Base = [string]$Info.base

    foreach ($Fn in @($Info.hot_functions)) {
        $Addr = [string]$Fn.address
        $Out = Join-Path $OutDir (($Addr -replace "^0x","") + ".json")
        Write-Host "[rea] $Module $Addr" -ForegroundColor Cyan

        $Cmd = "export REA_GHIDRA_SEED_FILE='$SeedsWsl'; rea function '$ImageWsl' '$Addr' --target-format raw-binary --architecture arm64 --entry-point '$Base' --provider ghidra --json"
        $Json = & wsl.exe @WslPrefix -- bash -lc $Cmd 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[rea] falhou $Addr" -ForegroundColor Yellow
            ($Json -join [Environment]::NewLine) | Set-Content -LiteralPath ($Out + ".error.txt") -Encoding UTF8
            continue
        }
        ($Json -join [Environment]::NewLine) | Set-Content -LiteralPath $Out -Encoding UTF8
        $Done++
    }
}

Write-Host "[rea] $Done dossiers -> $OutDir" -ForegroundColor Green
