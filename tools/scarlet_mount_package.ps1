param(
    [Parameter(Mandatory=$true)][string]$PackagePath,
    [string]$KeysPath = "",
    [string]$FuseNxPath = "",
    [string]$DriveLetter = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
$Package = Get-Item -LiteralPath $PackagePath
$Ext = $Package.Extension.ToLowerInvariant()
if ($Ext -notin @(".xci", ".nsp")) { throw "Formato nao suportado para montagem direta: $Ext" }

function Find-ProdKeys {
    param([string]$Requested)
    $Candidates = @()
    if ($Requested) { $Candidates += $Requested }
    if ($env:SCARLET_PROD_KEYS) { $Candidates += $env:SCARLET_PROD_KEYS }
    if ($env:USERPROFILE) { $Candidates += (Join-Path $env:USERPROFILE ".switch\prod.keys") }
    if ($env:APPDATA) {
        $Candidates += @(
            (Join-Path $env:APPDATA "Ryujinx\system\prod.keys"),
            (Join-Path $env:APPDATA "yuzu\keys\prod.keys"),
            (Join-Path $env:APPDATA "Eden\keys\prod.keys"),
            (Join-Path $env:APPDATA "eden\keys\prod.keys")
        )
    }
    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $Candidate).Path }
    }
    throw "prod.keys nao encontrado. Informe -KeysPath ou use as chaves obtidas do seu proprio Switch."
}

function Ensure-FuseNx {
    param([string]$Requested)
    $Candidates = @()
    if ($Requested) { $Candidates += $Requested }
    if ($env:SCARLET_FUSENX) { $Candidates += $env:SCARLET_FUSENX }
    $Candidates += @((Join-Path $RepoRoot "tools\fuse-nx\fuse-nx.exe"), (Join-Path $RepoRoot "tools\fuse-nx.exe"))
    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $Candidate).Path }
    }
    $Cmd = Get-Command fuse-nx.exe -ErrorAction SilentlyContinue
    if ($Cmd) { return $Cmd.Source }

    $Dir = Join-Path $RepoRoot "tools\fuse-nx"
    $Exe = Join-Path $Dir "fuse-nx.exe"
    $Url = "https://github.com/averne/Fuse-Nx/releases/download/v1.1/fuse-nx.exe"
    Write-Host "[scarlet-package] Fuse-Nx nao encontrado; baixando o executavel oficial v1.1 para Windows..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Exe
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "Download do Fuse-Nx falhou." }
    return $Exe
}

function Ensure-WinFsp {
    param([string]$FuseExe)
    $Pf86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    $Candidates = @()
    if ($Pf86) { $Candidates += (Join-Path $Pf86 "WinFsp\bin\winfsp-x64.dll") }
    if ($env:ProgramFiles) { $Candidates += (Join-Path $env:ProgramFiles "WinFsp\bin\winfsp-x64.dll") }
    $Dll = $null
    foreach ($Candidate in $Candidates) { if (Test-Path -LiteralPath $Candidate -PathType Leaf) { $Dll = $Candidate; break } }
    if (-not $Dll) {
        $Dir = Join-Path $RepoRoot "tools\winfsp"
        $Msi = Join-Path $Dir "winfsp-2.1.25156.msi"
        if (-not (Test-Path -LiteralPath $Msi -PathType Leaf)) {
            New-Item -ItemType Directory -Force -Path $Dir | Out-Null
            $Url = "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi"
            Write-Host "[scarlet-package] WinFsp ainda nao esta instalado; baixando o instalador oficial..." -ForegroundColor Yellow
            Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Msi
        }
        throw "WinFsp precisa ser instalado uma vez para montar XCI/NSP sem extrair. Instalador pronto em: $Msi"
    }
    $LocalDll = Join-Path (Split-Path -Parent $FuseExe) "winfsp-x64.dll"
    Copy-Item -LiteralPath $Dll -Destination $LocalDll -Force
}

function Get-ProgramId {
    param([string]$Npdm)
    if (-not (Test-Path -LiteralPath $Npdm -PathType Leaf)) { return [UInt64]0 }
    $Fs = [System.IO.File]::Open($Npdm, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $Meta = New-Object byte[] 0x80
        if ($Fs.Read($Meta,0,$Meta.Length) -ne $Meta.Length) { return [UInt64]0 }
        if ([Text.Encoding]::ASCII.GetString($Meta,0,4) -ne "META") { return [UInt64]0 }
        $AciOff = [BitConverter]::ToUInt32($Meta,0x70)
        $null = $Fs.Seek([Int64]$AciOff,[IO.SeekOrigin]::Begin)
        $Aci = New-Object byte[] 0x18
        if ($Fs.Read($Aci,0,$Aci.Length) -ne $Aci.Length) { return [UInt64]0 }
        if ([Text.Encoding]::ASCII.GetString($Aci,0,4) -ne "ACI0") { return [UInt64]0 }
        return [BitConverter]::ToUInt64($Aci,0x10)
    } finally { $Fs.Dispose() }
}

function To-HostPath {
    param([string]$MountPoint,[string]$VirtualPath)
    $Rel = $VirtualPath.TrimStart("/") -replace "/", "\"
    return Join-Path ($MountPoint + "\") $Rel
}

$Keys = Find-ProdKeys $KeysPath
$FuseNx = Ensure-FuseNx $FuseNxPath
Ensure-WinFsp $FuseNx

$StateDir = Join-Path $RepoRoot "build\package-direct"
New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
$FindErr = Join-Path $StateDir "find.err.log"
Remove-Item -LiteralPath $FindErr -Force -ErrorAction SilentlyContinue

Write-Host "[scarlet-package] Localizando ExeFS dentro do ${Ext} sem extrair..." -ForegroundColor Cyan
$SearchStart = if ($Ext -eq ".xci") { "/secure" } else { "/" }
# Stop at the first program NPDM. For XCI, search only the secure game
# partition so the system-update partition is never walked.
$Found = @(& $FuseNx -k $Keys find -m 1 -d 6 "main.npdm" $PackagePath $SearchStart 2> $FindErr)
$FindCode = $LASTEXITCODE
if ($FindCode -ne 0) {
    $Tail = if (Test-Path $FindErr) { (Get-Content $FindErr -Tail 20) -join [Environment]::NewLine } else { "" }
    throw "Fuse-Nx nao conseguiu ler o pacote (exit $FindCode). $Tail"
}
$VirtualNpdms = @($Found | ForEach-Object { "$_".Trim() } | Where-Object { $_ -match "(^|/)main[.]npdm$" })
if ($VirtualNpdms.Count -eq 0) { throw "Nenhum main.npdm foi localizado dentro do pacote." }

if (-not $DriveLetter) {
    $Used = @([IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0,1).ToUpperInvariant() })
    foreach ($Code in 90..68) {
        $Try = ([char]$Code).ToString()
        if ($Used -notcontains $Try) { $DriveLetter = $Try; break }
    }
}
if (-not $DriveLetter) { throw "Nao ha letra de unidade livre para a montagem virtual." }
$DriveLetter = $DriveLetter.TrimEnd(":").ToUpperInvariant()
$MountPoint = "${DriveLetter}:"
$MountOut = Join-Path $StateDir ("fuse-" + $DriveLetter + ".out.log")
$MountErr = Join-Path $StateDir ("fuse-" + $DriveLetter + ".err.log")
Remove-Item $MountOut,$MountErr -Force -ErrorAction SilentlyContinue

function Quote-Arg([string]$Value) { return ([char]34) + $Value + ([char]34) }
$Arguments = "-k " + (Quote-Arg $Keys) + " mount -r " + (Quote-Arg $PackagePath) + " " + (Quote-Arg $MountPoint)
Write-Host "[scarlet-package] Montando diretamente em $MountPoint (zero extracao)..." -ForegroundColor Cyan
$Proc = Start-Process -FilePath $FuseNx -ArgumentList $Arguments -RedirectStandardOutput $MountOut -RedirectStandardError $MountErr -WindowStyle Hidden -PassThru

$Mounted = $false
for ($i=0; $i -lt 200; $i++) {
    Start-Sleep -Milliseconds 100
    $Proc.Refresh()
    if ($Proc.HasExited) { break }
    if (Test-Path -LiteralPath ($MountPoint + "\")) { $Mounted = $true; break }
}
if (-not $Mounted) {
    if (-not $Proc.HasExited) { Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue }
    $Tail = if (Test-Path $MountErr) { (Get-Content $MountErr -Tail 30) -join [Environment]::NewLine } else { "" }
    throw "A montagem virtual nao ficou pronta. $Tail"
}

$ChosenNpdm = $null
$ChosenVirtual = $null
foreach ($Virtual in $VirtualNpdms) {
    $Host = To-HostPath $MountPoint $Virtual
    if ((Get-ProgramId $Host) -eq [UInt64]0x0100A3D008C5C000) { $ChosenNpdm=$Host; $ChosenVirtual=$Virtual; break }
}
if (-not $ChosenNpdm) {
    Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    throw "O pacote abriu, mas nao encontrei o Program NCA do Pokemon Scarlet (0100A3D008C5C000)."
}

$ExeFs = Split-Path -Parent $ChosenNpdm
$NcaRoot = Split-Path -Parent $ExeFs
$RomCandidates = @(Get-ChildItem -LiteralPath $NcaRoot -File -Filter "*.romfs" -ErrorAction SilentlyContinue)
$RomFsImage = $null
foreach ($Candidate in $RomCandidates) {
    try {
        $Stream = [IO.File]::Open($Candidate.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        try {
            $Header = New-Object byte[] 8
            if ($Stream.Read($Header,0,8) -eq 8 -and [BitConverter]::ToUInt64($Header,0) -eq 0x50) { $RomFsImage=$Candidate.FullName; break }
        } finally { $Stream.Dispose() }
    } catch {}
}
if (-not $RomFsImage) {
    Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    throw "Program NCA encontrado, mas nenhuma secao RomFS virtual valida foi exposta."
}
$RomFsDir = Join-Path $NcaRoot ([IO.Path]::GetFileNameWithoutExtension($RomFsImage))
if (-not (Test-Path -LiteralPath $RomFsDir -PathType Container)) {
    Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    throw "A arvore RomFS virtual nao foi exposta em $RomFsDir"
}

Write-Host "[scarlet-package] ExeFS virtual: $ExeFs" -ForegroundColor Green
Write-Host "[scarlet-package] RomFS virtual: $RomFsImage" -ForegroundColor Green
Write-Host "[scarlet-package] Nenhum arquivo do jogo foi extraido." -ForegroundColor Green

[PSCustomObject]@{
    ProcessId = $Proc.Id
    MountPoint = $MountPoint
    ExeFs = $ExeFs
    RomFsDir = $RomFsDir
    RomFsImage = $RomFsImage
    PackagePath = $PackagePath
} | ConvertTo-Json -Compress
