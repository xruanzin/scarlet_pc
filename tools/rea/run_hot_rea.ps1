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

# Prefer REA/Ghidra in WSL when it is genuinely available. Windows Ghidra P0
# does not admit raw ARM64, so on a Windows-only machine use REA's official
# Rizin integration instead of forcing the user to install WSL.
$WslPrefix = @()
if ($Distro) { $WslPrefix = @("-d", $Distro) }
$UseWslGhidra = $false
$ReaShell = ""

# bash -lc does not reliably load Ubuntu's interactive .bashrc, and NVM is
# normally registered there. Bootstrap NVM/JDK/Ghidra explicitly so REA works
# from PowerShell exactly as it does in the user's interactive Ubuntu shell.
$WslBootstrap = 'export NVM_DIR="$HOME/.nvm"; if [ -s "$NVM_DIR/nvm.sh" ]; then . "$NVM_DIR/nvm.sh"; fi; export GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$HOME/ghidra_12.1.4_PUBLIC}"; export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"; '

function Invoke-WslCaptured([string]$Command) {
    # Windows PowerShell 5.1 can turn native stderr into ErrorRecords when the
    # global ErrorActionPreference is Stop. Warnings must not become failures;
    # the native exit code is authoritative.
    $SavedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $Output = @(& wsl.exe @WslPrefix -- bash -lc ($WslBootstrap + $Command) 2>&1)
        $Code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $SavedPreference
    }
    [PSCustomObject]@{ Output = $Output; ExitCode = $Code }
}

if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
    $Probe = Invoke-WslCaptured "printf ready"
    if ($Probe.ExitCode -eq 0 -and (($Probe.Output -join "") -match "ready")) {
        $ReaProbe = Invoke-WslCaptured 'if command -v rea >/dev/null 2>&1; then command -v rea; elif command -v npx >/dev/null 2>&1; then echo __NPX__; fi'
        if ($ReaProbe.ExitCode -eq 0) {
            $Which = (($ReaProbe.Output | ForEach-Object { "$_" }) -join "").Trim()
            if ($Which -and $Which -ne "__NPX__") {
                # Use the absolute NVM-installed REA path to avoid PATH/profile drift.
                $ReaShell = "'" + ($Which -replace "'","'\\''") + "'"
            }
            elseif ($Which -eq "__NPX__") {
                $ReaShell = "npx -y rea-agents@6.7.0"
            }

            if ($ReaShell) {
                $Doctor = Invoke-WslCaptured "$ReaShell doctor --provider ghidra --json"
                if ($Doctor.ExitCode -eq 0) {
                    $DoctorText = ($Doctor.Output | ForEach-Object { "$_" }) -join [Environment]::NewLine
                    if ($DoctorText -match '"available"\s*:\s*true' -or $DoctorText -match '"healthy"\s*:\s*true') {
                        $UseWslGhidra = $true
                    }
                }
            }
        }
    }
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
        throw ($r -join [Environment]::NewLine)
    }
    return (($r | ForEach-Object { "$_" }) -join [Environment]::NewLine).Trim()
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
            $Cmd = "export REA_GHIDRA_SEED_FILE='$SeedsWsl'; $ReaShell function '$ImageWsl' '$Addr' --target-format raw-binary --architecture arm64 --entry-point '$Base' --provider ghidra --json"
            $Run = Invoke-WslCaptured $Cmd
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
