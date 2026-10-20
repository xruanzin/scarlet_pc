param(
    [Parameter(Mandatory=$true)]
    [string]$PackagePath,
    [string]$KeysPath = "",
    [string]$HactoolPath = "",
    [string]$OutputRoot = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
$Package = Get-Item -LiteralPath $PackagePath
$Ext = $Package.Extension.ToLowerInvariant()
if ($Ext -notin @(".xci", ".nsp")) { throw "Formato nao suportado: $Ext. Use .xci ou .nsp." }

function Find-Hactool {
    param([string]$Requested)
    $Candidates = @()
    if ($Requested) { $Candidates += $Requested }
    if ($env:SCARLET_HACTOOL) { $Candidates += $env:SCARLET_HACTOOL }
    $Candidates += @((Join-Path $RepoRoot "tools\hactool.exe"), (Join-Path $RepoRoot "tools\hactool\hactool.exe"), (Join-Path $RepoRoot "build\tools\hactool.exe"))
    foreach ($Candidate in $Candidates) { if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $Candidate).Path } }
    $Cmd = Get-Command hactool.exe -ErrorAction SilentlyContinue
    if (-not $Cmd) { $Cmd = Get-Command hactool -ErrorAction SilentlyContinue }
    if ($Cmd) { return $Cmd.Source }

    # Windows convenience: fetch the official prebuilt release from SciresM when
    # the user has not installed hactool yet. This downloads only the open-source
    # tool; game files and keys are never downloaded or bundled by this project.
    if ($env:OS -eq "Windows_NT") {
        $ToolDir = Join-Path $RepoRoot "tools\hactool"
        $ToolExe = Join-Path $ToolDir "hactool.exe"
        $Zip = Join-Path $ToolDir "hactool-1.4.0-win.zip"
        $Tmp = Join-Path $ToolDir "_extract"
        $Url = "https://github.com/SciresM/hactool/releases/download/1.4.0/hactool-1.4.0-win.zip"
        try {
            Write-Host "[scarlet-package] hactool nao encontrado; baixando a build oficial 1.4.0 para Windows..." -ForegroundColor Cyan
            New-Item -ItemType Directory -Force -Path $ToolDir | Out-Null
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Zip
            Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
            Expand-Archive -LiteralPath $Zip -DestinationPath $Tmp -Force
            $Downloaded = Get-ChildItem -LiteralPath $Tmp -Recurse -File -Filter "hactool.exe" | Select-Object -First 1
            if (-not $Downloaded) { throw "o ZIP oficial nao continha hactool.exe" }
            Copy-Item -LiteralPath $Downloaded.FullName -Destination $ToolExe -Force
            Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
            Write-Host "[scarlet-package] hactool instalado em $ToolExe" -ForegroundColor Green
            return $ToolExe
        } catch {
            Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
            throw "Nao consegui baixar/extrair o hactool oficial automaticamente: $($_.Exception.Message). Informe -HactoolPath se ja tiver uma copia."
        }
    }

    throw "hactool nao encontrado. Informe -HactoolPath."
}

function Find-ProdKeys {
    param([string]$Requested)
    $Candidates = @()
    if ($Requested) { $Candidates += $Requested }
    if ($env:SCARLET_PROD_KEYS) { $Candidates += $env:SCARLET_PROD_KEYS }
    if ($env:USERPROFILE) { $Candidates += (Join-Path $env:USERPROFILE ".switch\prod.keys") }
    if ($env:APPDATA) { $Candidates += @((Join-Path $env:APPDATA "Ryujinx\system\prod.keys"), (Join-Path $env:APPDATA "yuzu\keys\prod.keys"), (Join-Path $env:APPDATA "Eden\keys\prod.keys"), (Join-Path $env:APPDATA "eden\keys\prod.keys")) }
    foreach ($Candidate in $Candidates) { if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $Candidate).Path } }
    throw "prod.keys nao encontrado. Informe -KeysPath ou use %USERPROFILE%\.switch\prod.keys. Use chaves obtidas do seu proprio Switch."
}

function Get-PackageTag {
    param([System.IO.FileInfo]$File)
    $Identity = $File.FullName + "|" + $File.Length + "|" + $File.LastWriteTimeUtc.Ticks
    $Sha = [System.Security.Cryptography.SHA256]::Create()
    try { $Bytes = [System.Text.Encoding]::UTF8.GetBytes($Identity); $Hash = $Sha.ComputeHash($Bytes); return ([BitConverter]::ToString($Hash)).Replace("-", "").Substring(0, 12).ToLowerInvariant() } finally { $Sha.Dispose() }
}

function Get-ProgramId {
    param([string]$ExeFs)
    $Npdm = Join-Path $ExeFs "main.npdm"
    if (-not (Test-Path -LiteralPath $Npdm -PathType Leaf)) { return [UInt64]0 }
    $Fs = [System.IO.File]::OpenRead($Npdm)
    try {
        $Meta = New-Object byte[] 0x80
        if ($Fs.Read($Meta, 0, $Meta.Length) -ne $Meta.Length) { return [UInt64]0 }
        if ([System.Text.Encoding]::ASCII.GetString($Meta, 0, 4) -ne "META") { return [UInt64]0 }
        $AciOff = [BitConverter]::ToUInt32($Meta, 0x70)
        $null = $Fs.Seek([Int64]$AciOff, [System.IO.SeekOrigin]::Begin)
        $Aci = New-Object byte[] 0x18
        if ($Fs.Read($Aci, 0, $Aci.Length) -ne $Aci.Length) { return [UInt64]0 }
        if ([System.Text.Encoding]::ASCII.GetString($Aci, 0, 4) -ne "ACI0") { return [UInt64]0 }
        return [BitConverter]::ToUInt64($Aci, 0x10)
    } finally { $Fs.Dispose() }
}

function Test-PreparedRoot {
    param([string]$Root)
    $ExeFs = Join-Path $Root "exefs"; $RomFs = Join-Path $Root "romfs"
    if (-not (Test-Path -LiteralPath (Join-Path $ExeFs "main") -PathType Leaf)) { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $ExeFs "main.npdm") -PathType Leaf)) { return $false }
    if (-not (Test-Path -LiteralPath $RomFs -PathType Container)) { return $false }
    return (Get-ProgramId $ExeFs) -eq [UInt64]0x0100A3D008C5C000
}

$Hactool = Find-Hactool $HactoolPath
$Keys = Find-ProdKeys $KeysPath
if (-not $OutputRoot) { $SafeName = [System.IO.Path]::GetFileNameWithoutExtension($Package.Name) -replace "[^A-Za-z0-9._-]", "_"; $Tag = Get-PackageTag $Package; $OutputRoot = Join-Path $RepoRoot ("build\package-cache\" + $SafeName + "-" + $Tag) }
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$Work = $OutputRoot + ".work"; $Log = $OutputRoot + ".extract.log"
if (-not $Force -and (Test-PreparedRoot $OutputRoot)) { Write-Host "[scarlet-package] Cache pronto: $OutputRoot" -ForegroundColor Green; Write-Output $OutputRoot; exit 0 }

Write-Host "[scarlet-package] Fonte: $PackagePath" -ForegroundColor Cyan
Write-Host "[scarlet-package] hactool: $Hactool"
Write-Host "[scarlet-package] prod.keys: $Keys"
Write-Host "[scarlet-package] Destino: $OutputRoot"
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $OutputRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $Log -Force -ErrorAction SilentlyContinue
$Container = Join-Path $Work "container"; $Probe = Join-Path $Work "probe"
New-Item -ItemType Directory -Force -Path $Container | Out-Null

function Invoke-Hactool {
    param([string[]]$Arguments, [switch]$AllowFailure)
    Add-Content -LiteralPath $Log -Value ([Environment]::NewLine + "> hactool " + ($Arguments -join " ")) -Encoding UTF8
    $Lines = @(& $Hactool @Arguments 2>&1); $Code = $LASTEXITCODE
    if ($Lines) { $Lines | Out-File -LiteralPath $Log -Append -Encoding utf8 }
    if ($Code -ne 0 -and -not $AllowFailure) { throw "hactool falhou (exit $Code). Veja $Log" }
    return $Code
}

Write-Host "[scarlet-package] Abrindo container $Ext ..." -ForegroundColor Cyan
if ($Ext -eq ".xci") { $null = Invoke-Hactool @("-k", $Keys, "--disablekeywarns", "-t", "xci", "--securedir=$Container", $PackagePath) } else { $null = Invoke-Hactool @("-k", $Keys, "--disablekeywarns", "-t", "pfs0", "--outdir=$Container", $PackagePath) }
$Ncas = @(Get-ChildItem -LiteralPath $Container -Recurse -File -Filter "*.nca" | Sort-Object Length -Descending)
if ($Ncas.Count -eq 0) { throw "Nenhum NCA foi encontrado dentro do pacote. Veja $Log" }
Write-Host "[scarlet-package] $($Ncas.Count) NCA(s); procurando o Program NCA do Pokemon Scarlet..." -ForegroundColor Cyan
$Found = $false
foreach ($Nca in $Ncas) {
    Remove-Item -LiteralPath $Probe -Recurse -Force -ErrorAction SilentlyContinue
    $ProbeExeFs = Join-Path $Probe "exefs"; $ProbeRomFs = Join-Path $Probe "romfs"
    New-Item -ItemType Directory -Force -Path $ProbeExeFs, $ProbeRomFs | Out-Null
    Write-Host "[scarlet-package] Testando $($Nca.Name) ($([math]::Round($Nca.Length/1MB,0)) MB) ..."
    $null = Invoke-Hactool @("-k", $Keys, "--disablekeywarns", "-t", "nca", "--xcontenttype=program", "--exefsdir=$ProbeExeFs", "--romfsdir=$ProbeRomFs", $Nca.FullName) -AllowFailure
    if ((Get-ProgramId $ProbeExeFs) -ne [UInt64]0x0100A3D008C5C000) { continue }
    $AnyRomFs = Get-ChildItem -LiteralPath $ProbeRomFs -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $AnyRomFs) { throw "Program NCA encontrado, mas o RomFS nao foi descriptografado. Confira prod.keys/title.keys. Log: $Log" }
    New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
    Move-Item -LiteralPath $ProbeExeFs -Destination (Join-Path $OutputRoot "exefs")
    Move-Item -LiteralPath $ProbeRomFs -Destination (Join-Path $OutputRoot "romfs")
    $Found = $true; break
}
if (-not $Found) { throw "Nao encontrei o Program NCA do Pokemon Scarlet (0100A3D008C5C000). Confira o pacote e prod.keys/title.keys. Log: $Log" }
$Info = [ordered]@{ Package=$PackagePath; Size=$Package.Length; LastWriteTimeUtc=$Package.LastWriteTimeUtc.ToString("o"); PreparedUtc=[DateTime]::UtcNow.ToString("o"); TitleId="0100A3D008C5C000" }
$Info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot ".package-source.json") -Encoding UTF8
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
if (-not (Test-PreparedRoot $OutputRoot)) { throw "A extracao terminou, mas a validacao final falhou: $OutputRoot" }
Write-Host "[scarlet-package] XCI/NSP pronto para boot." -ForegroundColor Green
Write-Output $OutputRoot
