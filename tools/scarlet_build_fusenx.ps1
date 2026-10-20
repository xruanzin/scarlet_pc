param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $RepoRoot "tools\fuse-nx"
$OutExe = Join-Path $OutDir "fuse-nx-scarlet.exe"
$SrcDir = Join-Path $RepoRoot "build\fuse-nx-src"
$BuildDir = Join-Path $SrcDir "build-scarlet"
$Log = Join-Path $RepoRoot "build\fuse-nx-build.log"
$Bash = "C:\msys64\usr\bin\bash.exe"

if ((Test-Path -LiteralPath $OutExe -PathType Leaf) -and -not $Force) {
    Write-Output $OutExe
    exit 0
}
if (-not (Test-Path -LiteralPath $Bash -PathType Leaf)) {
    throw "MSYS2 nao encontrado em C:\msys64."
}

Write-Host "[fuse-nx] Preparando build corrigida para acesso direto a NCA..." -ForegroundColor Cyan

function Ensure-WinFspDeveloper {
    $Pf86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    if (-not $Pf86) { throw "Program Files (x86) nao encontrado." }

    $WinFspRoot = Join-Path $Pf86 "WinFsp"
    $FuseHeader = Join-Path $WinFspRoot "inc\fuse\fuse.h"
    $ImportLib = Join-Path $WinFspRoot "lib\winfsp-x64.lib"

    if ((Test-Path -LiteralPath $FuseHeader -PathType Leaf) -and
        (Test-Path -LiteralPath $ImportLib -PathType Leaf)) {
        Write-Host "[fuse-nx] WinFsp Developer ja instalado." -ForegroundColor DarkGray
        return
    }

    Write-Host "[fuse-nx] WinFsp esta instalado sem o componente Developer; adicionando headers + import library..." -ForegroundColor Yellow

    $InstallerDir = Join-Path $RepoRoot "tools\winfsp"
    $Msi = Join-Path $InstallerDir "winfsp-2.1.25156.msi"
    if (-not (Test-Path -LiteralPath $Msi -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path $InstallerDir | Out-Null
        $Url = "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi"
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Msi
    }

    # The official MSI keeps development headers/import libraries in feature
    # F.Developer (not selected by the default Core-only install).
    $Args = '/i "' + $Msi + '" ADDLOCAL=F.Main,F.User,F.Developer /qn /norestart'
    Write-Host "[fuse-nx] O Windows pode pedir permissao de administrador para completar o WinFsp Developer." -ForegroundColor Cyan
    $Proc = Start-Process -FilePath "msiexec.exe" -Verb RunAs -ArgumentList $Args -Wait -PassThru
    if ($Proc.ExitCode -notin @(0, 3010)) {
        throw "WinFsp Developer nao foi instalado (msiexec exit $($Proc.ExitCode))."
    }

    if (-not (Test-Path -LiteralPath $FuseHeader -PathType Leaf)) {
        throw "WinFsp Developer terminou, mas fuse.h ainda nao existe em: $FuseHeader"
    }
    if (-not (Test-Path -LiteralPath $ImportLib -PathType Leaf)) {
        throw "WinFsp Developer terminou, mas winfsp-x64.lib ainda nao existe em: $ImportLib"
    }

    Write-Host "[fuse-nx] WinFsp Developer pronto." -ForegroundColor Green
}

Ensure-WinFspDeveloper

$Pacman = "pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-libgcrypt mingw-w64-x86_64-re2"
& $Bash -lc "export PATH='/c/msys64/mingw64/bin:/c/msys64/usr/bin':`$PATH; $Pacman"
if ($LASTEXITCODE -ne 0) { throw "Falha instalando dependencias do Fuse-Nx (exit $LASTEXITCODE)." }

if (-not (Test-Path -LiteralPath (Join-Path $SrcDir ".git") -PathType Container)) {
    if (Test-Path -LiteralPath $SrcDir) { Remove-Item -LiteralPath $SrcDir -Recurse -Force }
    & git clone --depth 1 https://github.com/averne/Fuse-Nx.git $SrcDir
    if ($LASTEXITCODE -ne 0) { throw "git clone do Fuse-Nx falhou." }
}
else {
    & git -C $SrcDir fetch --depth 1 origin master
    if ($LASTEXITCODE -eq 0) {
        & git -C $SrcDir reset --hard origin/master
        if ($LASTEXITCODE -ne 0) { throw "Nao foi possivel atualizar a fonte do Fuse-Nx." }
    }
}

$Keyset = Join-Path $SrcDir "lib\keyset.cpp"
$FuseCpp = Join-Path $SrcDir "src\fuse.cpp"
if (-not (Test-Path $Keyset) -or -not (Test-Path $FuseCpp)) { throw "Fonte Fuse-Nx incompleta." }

$Text = Get-Content -LiteralPath $Keyset -Raw
$OldMaster = 'else if ("master_key_" == id && is_hex(id.substr(mkey_len))) {'
$NewMaster = 'else if (id.size() == mkey_len + 2 && id.substr(0, mkey_len) == "master_key_" && is_hex(id.substr(mkey_len))) {'
$OldTitle = 'else if ("titlekek_" == id && is_hex(id.substr(tkek_len))) {'
$NewTitle = 'else if (id.size() == tkek_len + 2 && id.substr(0, tkek_len) == "titlekek_" && is_hex(id.substr(tkek_len))) {'
if (-not $Text.Contains($OldMaster) -or -not $Text.Contains($OldTitle)) {
    throw "A fonte do Fuse-Nx mudou; patch de keyset nao encontrou os trechos esperados."
}
$Text = $Text.Replace($OldMaster, $NewMaster).Replace($OldTitle, $NewTitle)
Set-Content -LiteralPath $Keyset -Value $Text -Encoding UTF8

# Fix the Windows/FUSE directory type bug too: readdir used S_IFREG for child dirs.
$Text = Get-Content -LiteralPath $FuseCpp -Raw
$OldDir = 'st.st_mode = S_IFREG | 0555;'
$First = $Text.IndexOf($OldDir)
if ($First -lt 0) { throw "Patch de diretorio Fuse-Nx nao encontrou o trecho esperado." }
$Text = $Text.Remove($First, $OldDir.Length).Insert($First, 'st.st_mode = S_IFDIR | 0555;')
Set-Content -LiteralPath $FuseCpp -Value $Text -Encoding UTF8

if (Test-Path -LiteralPath $BuildDir) { Remove-Item -LiteralPath $BuildDir -Recurse -Force }

$RepoPosix = $RepoRoot -replace "\\","/"
if ($RepoPosix -match "^([A-Za-z]):/(.*)$") { $RepoPosix = "/" + $matches[1].ToLowerInvariant() + "/" + $matches[2] }
$SrcPosix = $SrcDir -replace "\\","/"
if ($SrcPosix -match "^([A-Za-z]):/(.*)$") { $SrcPosix = "/" + $matches[1].ToLowerInvariant() + "/" + $matches[2] }

$Cmd = "set -o pipefail; export PATH='/c/msys64/mingw64/bin:/c/msys64/usr/bin':`$PATH; cd '$SrcPosix'; meson setup build-scarlet -Dcryptobackend=gcrypt 2>&1 | tee '$RepoPosix/build/fuse-nx-build.log' && meson compile -C build-scarlet 2>&1 | tee -a '$RepoPosix/build/fuse-nx-build.log'"
& $Bash -lc $Cmd
if ($LASTEXITCODE -ne 0) { throw "Build corrigida do Fuse-Nx falhou. Envie build\fuse-nx-build.log." }

$BuiltCandidates = @(
    (Join-Path $BuildDir "fuse-nx.exe"),
    (Join-Path $BuildDir "src\fuse-nx.exe")
)
$Built = $BuiltCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $Built) { throw "Build terminou sem fuse-nx.exe." }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item -LiteralPath $Built -Destination $OutExe -Force
Write-Host "[fuse-nx] Build corrigida pronta: $OutExe" -ForegroundColor Green
Write-Output $OutExe
