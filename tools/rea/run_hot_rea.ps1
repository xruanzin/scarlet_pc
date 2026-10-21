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

$ManifestPath = Join-Path $RawDir "manifest.json"
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$null = New-Item -ItemType Directory -Force -Path $OutDir
Get-ChildItem -LiteralPath $OutDir -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

# Prefer the full REA/Ghidra lane in WSL. Do not depend on an interactive
# shell profile: NVM is often unavailable to bash -lc even though REA works in
# the user's Ubuntu terminal. Locate the installed REA binary directly.
$WslPrefix = @()
if ($Distro) { $WslPrefix = @("-d", $Distro) }
$UseWslGhidra = $false
$ReaShell = ""

function Invoke-WslRaw([string]$Command) {
    $SavedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $Output = @(& wsl.exe @WslPrefix -- bash -lc $Command 2>&1)
        $Code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $SavedPreference
    }
    [PSCustomObject]@{
        Output = @($Output | ForEach-Object { "$_" })
        ExitCode = $Code
    }
}

if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
    $Probe = Invoke-WslRaw 'printf ready'
    if ($Probe.ExitCode -eq 0 -and (($Probe.Output -join "") -match "ready")) {
        # Prefer a directly installed REA under NVM. This works even when
        # ~/.bashrc is intentionally skipped by a non-interactive shell.
        $FindRea = Invoke-WslRaw 'ls -1 "$HOME"/.nvm/versions/node/*/bin/rea 2>/dev/null | sort -V | tail -n 1'
        if ($FindRea.ExitCode -eq 0) {
            $ReaPath = (($FindRea.Output -join [Environment]::NewLine).Trim())
            if ($ReaPath -and $ReaPath -notmatch '^/') {
                throw "Caminho REA invalido retornado pelo WSL: $ReaPath"
            }
            if (-not $ReaPath) {
                $NvmDiag = Invoke-WslRaw 'ls -la "$HOME/.nvm/versions/node" 2>/dev/null; ls -la "$HOME"/.nvm/versions/node/*/bin/rea 2>/dev/null'
                $NvmText = ($NvmDiag.Output -join [Environment]::NewLine)
                Write-Host "[rea] WSL ativo, mas o REA do NVM nao foi localizado:" -ForegroundColor Red
                if ($NvmText) { Write-Host $NvmText -ForegroundColor DarkYellow }
                throw "REA instalado no WSL nao foi localizado; fallback Rizin bloqueado."
            }
            if ($ReaPath) {
                $QuotedRea = "'" + ($ReaPath -replace "'","'\\''") + "'"
                # ReaPath is a POSIX path from WSL. Do not use System.IO.Path
                # here: on Windows it rewrites /home/... as \home\..., which is
                # invalid inside Bash. Strip the final /rea component manually.
                $Slash = $ReaPath.LastIndexOf("/")
                if ($Slash -le 0) { throw "Caminho REA POSIX invalido: $ReaPath" }
                $ReaBin = $ReaPath.Substring(0, $Slash)
                $QuotedBin = "'" + ($ReaBin -replace "'","'\\''") + "'"
                $NodeProbe = Invoke-WslRaw ("test -x " + $QuotedBin + "/node")
                if ($NodeProbe.ExitCode -ne 0) {
                    throw "Node do NVM nao encontrado ao lado do REA: $ReaBin/node"
                }
                # REA uses #!/usr/bin/env node. A non-interactive WSL shell
                # does not source NVM, so put the REA/Node bin directory on PATH.
                $Bootstrap = ('export PATH={0}:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; export GHIDRA_INSTALL_DIR="$HOME/ghidra_12.1.4_PUBLIC"; export JAVA_HOME="/usr/lib/jvm/java-21-openjdk-amd64"; ' -f $QuotedBin)
                Write-Host "[rea] WSL REA: $ReaPath" -ForegroundColor DarkGray
                $Doctor = Invoke-WslRaw ($Bootstrap + $QuotedRea + ' doctor --provider ghidra --json')
                $DoctorText = ($Doctor.Output -join [Environment]::NewLine)
                if ($Doctor.ExitCode -eq 0 -and $DoctorText -match '"available"\s*:\s*true') {
                    $UseWslGhidra = $true
                    $ReaShell = $QuotedRea
                    $WslReaBootstrap = $Bootstrap
                }
                else {
                    Write-Host "[rea] REA foi encontrado no WSL, mas o doctor do Ghidra falhou:" -ForegroundColor Red
                    Write-Host $DoctorText -ForegroundColor DarkYellow
                    throw "REA/Ghidra WSL encontrado, mas indisponivel. O fallback Rizin foi bloqueado para nao mascarar este erro."
                }
            }
        }
    }
}

function Invoke-WslText([string[]]$Args) {
    $SavedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $r = @(& wsl.exe @WslPrefix -- @Args 2>&1)
        $Code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $SavedPreference
    }
    if ($Code -ne 0) {
        throw (($r | ForEach-Object { "$_" }) -join [Environment]::NewLine)
    }
    return (($r | ForEach-Object { "$_" }) -join [Environment]::NewLine).Trim()
}

function Ensure-WindowsRizin {
    $ToolRoot = Join-Path $RepoRoot "tools\rea\rizin"
    $Existing = @(Get-ChildItem -LiteralPath $ToolRoot -Recurse -File -Filter "rizin.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1)
    if ($Existing.Count) { return $Existing[0].FullName }

    $null = New-Item -ItemType Directory -Force -Path $ToolRoot
    $Zip = Join-Path $ToolRoot "rizin-windows-shared64-v0.9.1.zip"
    $Url = "https://github.com/rizinorg/rizin/releases/download/v0.9.1/rizin-windows-shared64-v0.9.1.zip"
    $Expected = "45ffa004e26653eaee8d262b8e6eeae09402a7d319c1f20a2ba794bd6af1cc28"

    if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) {
        Write-Host "[rea] Baixando Rizin oficial v0.9.1 para Windows..." -ForegroundColor Cyan
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Zip
    }
    $Hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Hash -ne $Expected) {
        Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
        throw "SHA256 do Rizin portatil nao confere. Esperado $Expected; obtido $Hash"
    }

    $Extract = Join-Path $ToolRoot "v0.9.1"
    if (Test-Path -LiteralPath $Extract) { Remove-Item -LiteralPath $Extract -Recurse -Force }
    Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force
    $Found = @(Get-ChildItem -LiteralPath $Extract -Recurse -File -Filter "rizin.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1)
    if (-not $Found.Count) { throw "Rizin foi extraido, mas rizin.exe nao foi encontrado." }
    return $Found[0].FullName
}

function Resolve-WindowsRea {
    foreach ($Name in @("rea.cmd","rea.exe","rea")) {
        $Cmd = Get-Command $Name -ErrorAction SilentlyContinue
        if ($Cmd) { return [PSCustomObject]@{ Program=$Cmd.Source; Prefix=@() } }
    }
    foreach ($Name in @("npx.cmd","npx.exe","npx")) {
        $Cmd = Get-Command $Name -ErrorAction SilentlyContinue
        if ($Cmd) { return [PSCustomObject]@{ Program=$Cmd.Source; Prefix=@("-y","rea-agents@6.7.0") } }
    }
    throw "REA nao esta instalado e npx nao foi encontrado. Instale Node.js 22.19+; nao e necessario instalar WSL."
}

$Done = 0
if ($UseWslGhidra) {
    Write-Host "[rea] backend: REA 6.7.0 / Ghidra / WSL" -ForegroundColor Green
    $RepoWsl = Invoke-WslText @("wslpath", "-a", $RepoRoot)

    foreach ($Prop in $Manifest.modules.PSObject.Properties) {
        $Module = $Prop.Name
        $Info = $Prop.Value
        $ImageWsl = Invoke-WslText @("wslpath","-a",[string]$Info.image)
        $SeedsWsl = Invoke-WslText @("wslpath","-a",[string]$Info.seeds)
        $Base = [string]$Info.base

        foreach ($Fn in @($Info.hot_functions)) {
            $Addr = [string]$Fn.address
            $Out = Join-Path $OutDir (($Addr -replace "^0x","") + ".json")
            Write-Host "[rea] Ghidra $Module $Addr" -ForegroundColor Cyan
            $Cmd = $WslReaBootstrap + "export REA_GHIDRA_SEED_FILE='$SeedsWsl'; $ReaShell function '$ImageWsl' '$Addr' --target-format raw-binary --architecture arm64 --entry-point '$Base' --provider ghidra --json"
            $Run = Invoke-WslRaw $Cmd
            $Json = @($Run.Output | ForEach-Object { "$_" })
            if ($Run.ExitCode -ne 0) {
                ($Json -join [Environment]::NewLine) | Set-Content -LiteralPath ($Out + ".error.txt") -Encoding UTF8
                continue
            }
            ($Json -join [Environment]::NewLine) | Set-Content -LiteralPath $Out -Encoding UTF8
            $Done++
        }
    }
}
else {
    Write-Host "[rea] WSL/Ghidra indisponivel; usando REA 6.7.0 + Rizin nativo no Windows." -ForegroundColor Yellow
    $Rizin = Ensure-WindowsRizin
    $Rea = Resolve-WindowsRea
    $env:REA_RIZIN_COMMAND = $Rizin
    Write-Host "[rea] Rizin: $Rizin" -ForegroundColor Green

    foreach ($Prop in $Manifest.modules.PSObject.Properties) {
        $Module = $Prop.Name
        $Info = $Prop.Value
        $Image = [string]$Info.image
        if (-not [IO.Path]::IsPathRooted($Image)) { $Image = Join-Path $RepoRoot $Image }
        $BaseNum = [Convert]::ToUInt64(([string]$Info.base).Substring(2),16)

        foreach ($Fn in @($Info.hot_functions)) {
            $Addr = [string]$Fn.address
            $AddrNum = [Convert]::ToUInt64($Addr.Substring(2),16)
            if ($AddrNum -lt $BaseNum) { continue }
            $Rel = $AddrNum - $BaseNum
            $RelHex = "0x{0:x}" -f $Rel
            $Out = Join-Path $OutDir (($Addr -replace "^0x","") + ".json")

            Write-Host "[rea] Rizin $Module $Addr (file+$RelHex)" -ForegroundColor Cyan

            # Rizin sees the exported raw image from offset zero. Analyze only the
            # profiled function and its immediate graph; avoid a whole-image AAA
            # pass on Scarlet's very large NSO.
            $RzCommand = @(
                "e asm.arch=arm",
                "e asm.bits=64",
                "e anal.arch=arm",
                "e anal.bits=64",
                "af @$RelHex",
                "s $RelHex",
                "?e === FUNCTION $Addr / relative $RelHex ===",
                "afij",
                "?e === DISASSEMBLY ===",
                "pdf",
                "?e === XREFS TO ===",
                "axt",
                "?e === XREFS FROM ===",
                "axf",
                "?e === CFG JSON ===",
                "agfj"
            ) -join "; "

            $Args = @($Rea.Prefix) + @("execute-rizin-command", $Image, $RzCommand)
            $Result = @(& $Rea.Program @Args 2>&1)
            if ($LASTEXITCODE -ne 0) {
                ($Result -join [Environment]::NewLine) | Set-Content -LiteralPath ($Out + ".error.txt") -Encoding UTF8
                continue
            }
            ($Result -join [Environment]::NewLine) | Set-Content -LiteralPath $Out -Encoding UTF8
            $Done++
        }
    }
}

Write-Host "[rea] $Done dossiers -> $OutDir" -ForegroundColor Green
if ($Done -eq 0) {
    throw "REA nao conseguiu produzir nenhum dossier; veja analysis\rea\functions\*.error.txt"
}
