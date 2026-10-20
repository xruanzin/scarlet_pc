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

    $Roots = @()
    $Pf86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    if ($Pf86) { $Roots += (Join-Path $Pf86 "WinFsp") }
    if ($env:ProgramFiles) { $Roots += (Join-Path $env:ProgramFiles "WinFsp") }

    $Dll = $null
    foreach ($Root in $Roots) {
        if (-not (Test-Path -LiteralPath $Root -PathType Container)) { continue }
        $Direct = Join-Path $Root "bin\winfsp-x64.dll"
        if (Test-Path -LiteralPath $Direct -PathType Leaf) { $Dll = $Direct; break }

        # Newer WinFsp installers may keep the active binaries in an SxS
        # version directory. Prefer the newest x64 DLL if the compatibility
        # path is not present.
        $Sxs = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "winfsp-x64.dll" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending)
        if ($Sxs.Count) { $Dll = $Sxs[0].FullName; break }
    }

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
    Write-Host "[scarlet-package] WinFsp runtime: $Dll" -ForegroundColor DarkGray
}

function Get-PeImports {
    param([string]$Path)

    $ObjdumpCandidates = @(
        "C:\msys64\ucrt64\bin\objdump.exe",
        "C:\msys64\mingw64\bin\objdump.exe",
        "C:\msys64\clang64\bin\objdump.exe",
        "C:\msys64\usr\bin\objdump.exe"
    )
    $Objdump = $ObjdumpCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $Objdump) { return @() }

    $Out = @(& $Objdump -p $Path 2>$null)
    if ($LASTEXITCODE -ne 0) { return @() }

    return @($Out | ForEach-Object {
        if ($_ -match "^\s*DLL Name:\s*(.+?)\s*$") { $Matches[1] }
    } | Where-Object { $_ } | Select-Object -Unique)
}

function Install-FuseNxCryptoRuntime {
    $Bash = "C:\msys64\usr\bin\bash.exe"
    if (-not (Test-Path -LiteralPath $Bash -PathType Leaf)) {
        throw "MSYS2 nao encontrado em C:\msys64; nao consigo instalar automaticamente libgcrypt."
    }

    Write-Host "[scarlet-package] Instalando runtime criptografico do Fuse-Nx pelo MSYS2..." -ForegroundColor Cyan
    $Cmd = "export PATH='/c/msys64/mingw64/bin:/c/msys64/usr/bin':\$PATH; pacman -S --needed --noconfirm mingw-w64-x86_64-libgcrypt"
    & $Bash -lc $Cmd
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 pacman falhou ao instalar mingw-w64-x86_64-libgcrypt (exit $LASTEXITCODE)."
    }

    $Required = @(
        "C:\msys64\mingw64\bin\libgcrypt-20.dll",
        "C:\msys64\mingw64\bin\libgpg-error-0.dll"
    )
    foreach ($Path in $Required) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "Pacote instalado, mas a DLL esperada nao apareceu: $Path"
        }
    }
}

function Resolve-FuseNxDependencies {
    param([string]$FuseExe)

    $ToolDir = Split-Path -Parent $FuseExe
    $SearchRoots = @(
        $ToolDir,
        "C:\msys64\ucrt64\bin",
        "C:\msys64\mingw64\bin",
        "C:\msys64\clang64\bin",
        "C:\msys64\usr\bin"
    )

    $Pf86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    if ($Pf86) { $SearchRoots += (Join-Path $Pf86 "WinFsp\bin") }
    if ($env:WINDIR) {
        $System32 = Join-Path $env:WINDIR "System32"
        $SysWow64 = Join-Path $env:WINDIR "SysWOW64"
    } else {
        $System32 = $null
        $SysWow64 = $null
    }

    $Queue = New-Object System.Collections.Generic.Queue[string]
    $Queue.Enqueue($FuseExe)
    $Seen = @{}
    $Missing = New-Object System.Collections.Generic.List[string]

    while ($Queue.Count -gt 0) {
        $Binary = $Queue.Dequeue()
        $Key = $Binary.ToLowerInvariant()
        if ($Seen.ContainsKey($Key)) { continue }
        $Seen[$Key] = $true

        foreach ($Name in @(Get-PeImports $Binary)) {
            $Local = Join-Path $ToolDir $Name
            if (Test-Path -LiteralPath $Local -PathType Leaf) {
                if ($Name -match "\.dll$") { $Queue.Enqueue($Local) }
                continue
            }

            if (($System32 -and (Test-Path -LiteralPath (Join-Path $System32 $Name) -PathType Leaf)) -or
                ($SysWow64 -and (Test-Path -LiteralPath (Join-Path $SysWow64 $Name) -PathType Leaf))) {
                continue
            }

            # API-set names are virtual contracts resolved by the Windows
            # loader; they do not exist as ordinary files on every version.
            if ($Name -match "^(api-ms-win-|ext-ms-win-)") { continue }

            $Found = $null
            foreach ($Root in $SearchRoots) {
                if (-not $Root -or -not (Test-Path -LiteralPath $Root -PathType Container)) { continue }
                $Candidate = Join-Path $Root $Name
                if (Test-Path -LiteralPath $Candidate -PathType Leaf) { $Found = $Candidate; break }
            }

            if (-not $Found -and $Name -ieq "winfsp-x64.dll" -and $Pf86) {
                $WRoot = Join-Path $Pf86 "WinFsp"
                if (Test-Path -LiteralPath $WRoot -PathType Container) {
                    $Any = @(Get-ChildItem -LiteralPath $WRoot -Recurse -File -Filter $Name -ErrorAction SilentlyContinue |
                        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
                    if ($Any.Count) { $Found = $Any[0].FullName }
                }
            }

            if ($Found) {
                Copy-Item -LiteralPath $Found -Destination $Local -Force
                Write-Host "[scarlet-package] Runtime DLL: $Name <- $Found" -ForegroundColor DarkGray
                $Queue.Enqueue($Local)
            }
            elseif (-not $Missing.Contains($Name)) {
                $Missing.Add($Name)
            }
        }
    }

    if ($Missing.Count) {
        $CryptoMissing = @($Missing | Where-Object { $_ -in @("libgcrypt-20.dll","libgpg-error-0.dll") })
        if ($CryptoMissing.Count -gt 0 -and -not $script:FuseCryptoInstallAttempted) {
            $script:FuseCryptoInstallAttempted = $true
            Install-FuseNxCryptoRuntime

            # Retry from scratch now that pacman populated mingw64\bin.
            Resolve-FuseNxDependencies $FuseExe
            return
        }
        throw ("Dependencias do Fuse-Nx ausentes: " + ($Missing -join ", "))
    }

    # The 2021 release was built with MinGW64. If PE import inspection is not
    # available, stage the conventional MinGW runtime DLLs opportunistically.
    if ((Get-PeImports $FuseExe).Count -eq 0) {
        foreach ($Name in @("libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll")) {
            $Local = Join-Path $ToolDir $Name
            if (Test-Path -LiteralPath $Local -PathType Leaf) { continue }
            foreach ($Root in $SearchRoots) {
                if (-not $Root) { continue }
                $Candidate = Join-Path $Root $Name
                if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
                    Copy-Item -LiteralPath $Candidate -Destination $Local -Force
                    break
                }
            }
        }
    }
}

function Test-FuseNxRuntime {
    param([string]$FuseExe)

    $ProbeOut = Join-Path $RepoRoot "build\package-direct\fuse-runtime-probe.out.log"
    $ProbeErr = Join-Path $RepoRoot "build\package-direct\fuse-runtime-probe.err.log"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ProbeOut) | Out-Null
    Remove-Item $ProbeOut,$ProbeErr -Force -ErrorAction SilentlyContinue

    $p = Start-Process -FilePath $FuseExe -ArgumentList "--help" -RedirectStandardOutput $ProbeOut -RedirectStandardError $ProbeErr -WindowStyle Hidden -PassThru -Wait
    if ($p.ExitCode -eq -1073741515) {
        $Imports = @(Get-PeImports $FuseExe)
        $ImportText = if ($Imports.Count) { " Imports: " + ($Imports -join ", ") } else { "" }
        throw "Fuse-Nx nao inicia: Windows STATUS_DLL_NOT_FOUND (0xC0000135).$ImportText"
    }
    if ($p.ExitCode -eq -1073741701) {
        throw "Fuse-Nx nao inicia: Windows STATUS_INVALID_IMAGE_FORMAT (0xC000007B), normalmente DLL de arquitetura errada."
    }
    if ($p.ExitCode -ne 0) {
        $Tail = if (Test-Path $ProbeErr) { (Get-Content $ProbeErr -Tail 20) -join [Environment]::NewLine } else { "" }
        throw "Fuse-Nx falhou no autoteste (exit $($p.ExitCode)). $Tail"
    }
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
Resolve-FuseNxDependencies $FuseNx
Test-FuseNxRuntime $FuseNx

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
