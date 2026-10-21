param(
    [int]$Jobs = 4,
    [switch]$Setup,
    [switch]$ForceRuntime,
    [switch]$IncrementalRuntime,
    [switch]$RuntimeOnly,
    [switch]$GameDllOnly,
    [switch]$FunctionInstrument,
    [switch]$ThinLTO,
    [switch]$NoThinLTO,  # compatibility switch; upstream-style game builds already default to no ThinLTO
    [int]$LtoJobs = 4
)

$ErrorActionPreference = "Stop"

if ($RuntimeOnly -and $GameDllOnly) {
    throw "-RuntimeOnly e -GameDllOnly sao mutuamente exclusivos."
}
if ($ThinLTO -and $NoThinLTO) {
    throw "-ThinLTO e -NoThinLTO sao mutuamente exclusivos."
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bash = "C:\msys64\usr\bin\bash.exe"
$BuildRel = if ($FunctionInstrument) { "build/profile" } else { "build" }
$BuildDir = Join-Path $RepoRoot $BuildRel
$BuildLogName = if ($FunctionInstrument) { "scarlet_build_profile.log" } else { "scarlet_build.log" }

if ($Setup -or -not (Test-Path -LiteralPath $Bash)) {
    Write-Host "[scarlet-build] Preparando toolchain Windows/MSYS2..." -ForegroundColor Cyan

    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "setup_windows.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "setup_windows.ps1 falhou (exit $LASTEXITCODE)"
    }
}

if (-not (Test-Path -LiteralPath $Bash -PathType Leaf)) {
    throw "MSYS2 nao encontrado em C:\msys64. Rode novamente com -Setup."
}

$RequiredGenerated = @(
    "generated\main\main_table.c",
    "generated\sdk\sdk_table.c",
    "generated\subsdk0\subsdk0_table.c"
)

foreach ($Rel in $RequiredGenerated) {
    $Path = Join-Path $RepoRoot $Rel
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Codigo gerado ausente: $Path. Rode tools\scarlet_generate.ps1 primeiro."
    }
}

# Main now needs the same interior-PC resume support as SDK. Check both before
# compilation so an old main table cannot silently reproduce the NVN boot stop.
foreach ($Name in @("main", "sdk")) {
    $Table = Join-Path $RepoRoot "generated\$Name\${Name}_table.c"
    $Pattern = "const uint64_t ${Name}_resume_count = [1-9][0-9]*;"
    if (-not (Select-String -LiteralPath $Table -Pattern $Pattern -Quiet)) {
        throw "Modulo $Name gerado sem retomadas de saltos indiretos. Rode tools\scarlet_generate.ps1 -ExeFs 'D:\Pokemon Scarlet\Extracted\Base\exefs' -Module $Name e compile novamente."
    }
}

$MainTable = Join-Path $RepoRoot "generated\main\main_table.c"
if (-not (Select-String -LiteralPath $MainTable -Pattern 'const uint64_t main_setjmp_site_count = [1-9][0-9]*;' -Quiet)) {
    throw "Main gerado sem escopos setjmp/longjmp. Rode tools\scarlet_generate.ps1 -ExeFs 'D:\Pokemon Scarlet\Extracted\Base\exefs' -Module main e compile novamente."
}

if ($Jobs -lt 1) {
    $Jobs = 1
}
if ($LtoJobs -lt 1) {
    $LtoJobs = 1
}

# Convert C:\foo\bar to /c/foo/bar for MSYS2 without invoking cygpath inside
# the shell command. Keeping the command substitution out avoids quoting/parser
# problems when PowerShell passes bash -lc a command string.
$RepoPosix = $RepoRoot -replace "\\", "/"
if ($RepoPosix -match "^([A-Za-z]):/(.*)$") {
    $Drive = $matches[1].ToLowerInvariant()
    $Tail = $matches[2]
    $RepoPosix = "/$Drive/$Tail"
}

$Log = Join-Path $RepoRoot $BuildLogName
if (Test-Path -LiteralPath $Log) {
    Remove-Item -LiteralPath $Log -Force
}

if ($FunctionInstrument) {
    Write-Host "[scarlet-build] Modo PROFILE: FNINSTRUMENT=1 em build\profile (build normal preservado)." -ForegroundColor Magenta
}
if ($RuntimeOnly) {
    Write-Host "[scarlet-build] Compilando somente ScarletRecomp.exe com -j$Jobs; lasgame.dll sera preservado." -ForegroundColor Cyan
} elseif ($GameDllOnly) {
    Write-Host "[scarlet-build] Compilando somente lasgame.dll com -j$Jobs; o jogo NAO sera iniciado." -ForegroundColor Cyan
} else {
    Write-Host "[scarlet-build] Compilando runtime + codigo gerado com -j$Jobs..." -ForegroundColor Cyan
}
Write-Host "[scarlet-build] Repo MSYS2: $RepoPosix"
Write-Host "[scarlet-build] Log: $Log"

# Runtime rebuilds are cheap compared with lasgame.dll and must never reuse
# stale Vulkan/NVN objects after a Git update.  Previous versions could write
# .runtime_build_head even when make rebuilt nothing, falsely marking an old EXE
# as current.  Default to a clean runtime rebuild; opt into incremental runtime
# builds only with -IncrementalRuntime.
if ((-not $GameDllOnly) -and (-not $IncrementalRuntime -or $ForceRuntime)) {
    $RtDir = Join-Path $BuildDir "rt"
    if (Test-Path -LiteralPath $RtDir) {
        Write-Host "[scarlet-build] Recompilando runtime do zero (lasgame permanece incremental)..." -ForegroundColor Yellow
        Remove-Item -LiteralPath $RtDir -Recurse -Force
    }
    $OldExe = Join-Path $BuildDir "ScarletRecomp.exe"
    if (Test-Path -LiteralPath $OldExe) {
        Remove-Item -LiteralPath $OldExe -Force
    }
}

# The backtick keeps $PATH for Bash instead of expanding it in PowerShell.
# Match upstream LASRecomp by default: no LTO for generated game code.
# ThinLTO is now opt-in only via -ThinLTO.
$PerfMake = " THINLTO=0"
if ($ThinLTO) {
    $PerfMake = " THINLTO=1 LTO_JOBS=$LtoJobs"
    Write-Host "[scarlet-build] ThinLTO ativado explicitamente para lasgame.dll." -ForegroundColor Green
} else {
    Write-Host "[scarlet-build] lasgame.dll: perfil upstream LASRecomp (sem ThinLTO)." -ForegroundColor Green
}
$InstrumentMake = if ($FunctionInstrument) { " FNINSTRUMENT=1 B=$BuildRel" } else { "" }

# Runtime-only means exactly that: preserve the existing huge lasgame.dll and
# compile the EXE against the DLL's existing GAMELIB_ID, even if generator/HLE
# source files changed since that DLL was built.
$GameIdMake = ""
if ($RuntimeOnly) {
    $FrozenDll = Join-Path $BuildDir "lasgame.dll"
    $FrozenIdPath = Join-Path $BuildDir "lasgame.dll.id"
    if (-not (Test-Path -LiteralPath $FrozenDll -PathType Leaf) -or
        -not (Test-Path -LiteralPath $FrozenIdPath -PathType Leaf)) {
        throw "RuntimeOnly pediu para preservar lasgame.dll, mas a DLL/ID existente nao foi encontrada em $BuildDir."
    }
    $FrozenId = (Get-Content -LiteralPath $FrozenIdPath -Raw).Trim()
    if (-not $FrozenId) { throw "lasgame.dll.id vazio em $FrozenIdPath" }
    $GameIdMake = " GAMELIB_ID=$FrozenId"
    Write-Host "[scarlet-build] lasgame.dll CONGELADA: id=$FrozenId" -ForegroundColor Green
}

$Targets = if ($RuntimeOnly) {
    "$BuildRel/ScarletRecomp.exe"
} elseif ($GameDllOnly) {
    "$BuildRel/lasgame.dll"
} else {
    "$BuildRel/ScarletRecomp.exe $BuildRel/lasgame.dll"
}
$Cmd = "set -o pipefail; export PATH='/c/msys64/ucrt64/bin:/c/msys64/usr/bin':`$PATH; cd '$RepoPosix'; make -j$Jobs EXE_NAME=ScarletRecomp$PerfMake$InstrumentMake$GameIdMake $Targets 2>&1 | tee $BuildLogName"

& $Bash -lc $Cmd
$Code = $LASTEXITCODE

if ($Code -ne 0) {
    throw "Build falhou (exit $Code). Envie scarlet_build.log."
}

$Exe = Join-Path $BuildDir "ScarletRecomp.exe"
$GameDll = Join-Path $BuildDir "lasgame.dll"
$Id = Join-Path $BuildDir "lasgame.dll.id"

$ExpectedArtifacts = if ($GameDllOnly) { @($GameDll, $Id) } else { @($Exe, $GameDll, $Id) }
foreach ($Path in $ExpectedArtifacts) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Build terminou sem criar o artefato esperado: $Path"
    }
}

# Record exactly which repository revision produced the native runtime.
# File timestamps are not reliable after git checkout/pull, because Git may
# restore source mtimes older than an already-built EXE.
if (-not $GameDllOnly) {
    $Head = (& git -C $RepoRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $Head) {
        Set-Content -LiteralPath (Join-Path $BuildDir ".runtime_build_head") -Value $Head.Trim() -Encoding ASCII
    }
}

$ThinLtoMarker = Join-Path $BuildDir ".thinlto_enabled"
if (-not $RuntimeOnly) {
    if ($ThinLTO) {
        Set-Content -LiteralPath $ThinLtoMarker -Value "1" -Encoding ASCII
    }
    elseif (Test-Path -LiteralPath $ThinLtoMarker) {
        Remove-Item -LiteralPath $ThinLtoMarker -Force
    }
}

Write-Host ""
Write-Host "[scarlet-build] BUILD CONCLUIDO" -ForegroundColor Green

$DisplayArtifacts = if ($GameDllOnly) { @($GameDll, $Id) } else { @($Exe, $GameDll, $Id) }
Get-Item $DisplayArtifacts |
    Select-Object Name, @{N="SizeMB";E={[Math]::Round($_.Length / 1MB, 2)}}, LastWriteTime |
    Format-Table -AutoSize

