param(
    [Alias("Package","PackagePath")]
    [string]$GameRoot = "D:\Pokemon Scarlet\Extracted\Base",
    [string]$KeysPath = "",
    [string]$FuseNxPath = "",
    [switch]$PackageExtract,
    [string]$HactoolPath = "",
    [switch]$PackageForceExtract,
    [switch]$RegenerateHot,
    [switch]$RebuildGameDll,
    [switch]$Profile,
    [switch]$Performance,
    [switch]$LiveLog,
    [switch]$GraphicsDiagnostics,
    [ValidateSet("Vulkan","DirectX")][string]$Backend = "Vulkan",
    [string]$DirectXDirectory = "",
    [switch]$NoBackendFallback,
    [switch]$RenderDoc,
    [string]$RenderDocCommand = "C:\Program Files\RenderDoc\renderdoccmd.exe",
    [switch]$MaterialViews
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot

$PackageMountPid = 0
$PackageMountPoint = ""
$PackageEnvBefore = @{}
foreach ($Name in @("LARECOMP_EXEFS_DIR","LARECOMP_ROMFS_DIR","LARECOMP_ROMFS_IMAGE","LARECOMP_PACKAGE_STREAM")) {
    $PackageEnvBefore[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
}
function Stop-ScarletPackageMount {
    if ($script:PackageMountPid -gt 0) {
        $p = Get-Process -Id $script:PackageMountPid -ErrorAction SilentlyContinue
        if ($p) {
            Write-Host "[scarlet-package] Desmontando $script:PackageMountPoint ..." -ForegroundColor DarkGray
            Stop-Process -Id $script:PackageMountPid -Force -ErrorAction SilentlyContinue
            for ($i=0; $i -lt 30 -and $script:PackageMountPoint -and (Test-Path -LiteralPath ($script:PackageMountPoint + "\")); $i++) {
                Start-Sleep -Milliseconds 100
            }
        }
        $script:PackageMountPid = 0
    }
}
trap {
    $Failure = $_
    Stop-ScarletPackageMount
    Write-Host "" 
    Write-Host ("[scarlet-run] ERRO: " + $Failure.Exception.Message) -ForegroundColor Red
    exit 1
}

# GameRoot can be an extracted root (exefs/ + romfs/) or the user's own XCI/NSP.
# Packages default to a read-only virtual mount: no game data is extracted.
$OriginalGameInput = $GameRoot
$DirectPackage = $false
$DirectPackageInfo = $null
if (Test-Path -LiteralPath $GameRoot -PathType Leaf) {
    $Ext = [System.IO.Path]::GetExtension($GameRoot).ToLowerInvariant()
    if ($Ext -notin @(".xci", ".nsp")) {
        throw "Arquivo de jogo nao suportado: $GameRoot. Use uma pasta extraida, .xci ou .nsp."
    }

    if ($PackageExtract) {
        $PrepareScript = Join-Path $PSScriptRoot "scarlet_prepare_package.ps1"
        if (-not (Test-Path -LiteralPath $PrepareScript -PathType Leaf)) { throw "Preparador XCI/NSP ausente: $PrepareScript" }
        $PrepareArgs = @{ PackagePath = (Resolve-Path -LiteralPath $GameRoot).Path }
        if ($KeysPath) { $PrepareArgs.KeysPath = $KeysPath }
        if ($HactoolPath) { $PrepareArgs.HactoolPath = $HactoolPath }
        if ($PackageForceExtract) { $PrepareArgs.Force = $true }
        Write-Host "[scarlet-run] Modo legado: extraindo pacote ${Ext} para cache..." -ForegroundColor Yellow
        $Prepared = @(& $PrepareScript @PrepareArgs)
        if (-not $Prepared -or -not $Prepared[-1]) { throw "A preparacao do pacote nao retornou a pasta extraida." }
        $GameRoot = [string]$Prepared[-1]
    }
    else {
        $MountScript = Join-Path $PSScriptRoot "scarlet_mount_package.ps1"
        if (-not (Test-Path -LiteralPath $MountScript -PathType Leaf)) { throw "Montador direto XCI/NSP ausente: $MountScript" }
        $MountArgs = @{ PackagePath = (Resolve-Path -LiteralPath $GameRoot).Path }
        if ($KeysPath) { $MountArgs.KeysPath = $KeysPath }
        if ($FuseNxPath) { $MountArgs.FuseNxPath = $FuseNxPath }

        Write-Host "[scarlet-run] Abrindo ${Ext} diretamente, sem extrair..." -ForegroundColor Cyan
        $MountOutput = @(& $MountScript @MountArgs)
        $MountJson = @($MountOutput | Where-Object { $_ -is [string] -and $_.Trim().StartsWith("{") } | Select-Object -Last 1)
        if (-not $MountJson) { throw "O montador XCI/NSP nao retornou os caminhos virtuais." }
        $DirectPackageInfo = $MountJson[0] | ConvertFrom-Json
        $PackageMountPid = [int]$DirectPackageInfo.ProcessId
        $PackageMountPoint = [string]$DirectPackageInfo.MountPoint
        $DirectPackage = $true

        $env:LARECOMP_EXEFS_DIR = [string]$DirectPackageInfo.ExeFs
        $env:LARECOMP_ROMFS_DIR = [string]$DirectPackageInfo.RomFsDir
        $env:LARECOMP_ROMFS_IMAGE = [string]$DirectPackageInfo.RomFsImage
        $env:LARECOMP_PACKAGE_STREAM = "1"
        Write-Host "[scarlet-run] XCI/NSP montado virtualmente em $PackageMountPoint; zero extracao." -ForegroundColor Green
    }
}
elseif (-not (Test-Path -LiteralPath $GameRoot -PathType Container)) {
    throw "GameRoot nao encontrado: $GameRoot"
}

. (Join-Path $PSScriptRoot "scarlet_graphics_backend.ps1")
if (-not $DirectXDirectory) { $DirectXDirectory = Join-Path $RepoRoot "tools\directx\mesa" }

# Profiling now uses host RIP sampling in the runtime. The normal game DLL is
# reused unchanged; no FNINSTRUMENT/profile guest build is needed.
$BuildVariantDir = Join-Path $RepoRoot "build"
$Exe = Join-Path $BuildVariantDir "ScarletRecomp.exe"
$GameDll = Join-Path $BuildVariantDir "lasgame.dll"
$BuildScript = Join-Path $PSScriptRoot "scarlet_build_windows.ps1"
$BuildFlavorArgs = @()

if ($Profile) {
    $HaveExe = Test-Path -LiteralPath $Exe -PathType Leaf
    $HaveDll = Test-Path -LiteralPath $GameDll -PathType Leaf
    if (-not $HaveDll -and -not $RebuildGameDll) {
        throw "build\lasgame.dll nao existe. A DLL do jogo esta congelada por padrao; use -RebuildGameDll somente para cria-la/recompila-la explicitamente."
    }
    if (-not $HaveExe -and $HaveDll) {
        Write-Host "[scarlet-run] PROFILE por amostragem: reconstruindo somente ScarletRecomp.exe; lasgame.dll congelada." -ForegroundColor Magenta
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime -RuntimeOnly -ThinLTO
        if ($LASTEXITCODE -ne 0) { throw "Build do runtime falhou (exit $LASTEXITCODE). Envie scarlet_build.log." }
    }
    elseif ((-not $HaveExe -or -not $HaveDll) -and $RebuildGameDll) {
        Write-Host "[scarlet-run] -RebuildGameDll autorizado explicitamente." -ForegroundColor Yellow
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime -ThinLTO
        if ($LASTEXITCODE -ne 0) { throw "Build completo falhou (exit $LASTEXITCODE). Envie scarlet_build.log." }
    }
}
if ($DirectPackage) {
    $ExeFs = [string]$DirectPackageInfo.ExeFs
    $RomFs = [string]$DirectPackageInfo.RomFsDir
} else {
    $ExeFs = Join-Path $GameRoot "exefs"
    $RomFs = Join-Path $GameRoot "romfs"
}

# Regenerate only the modules that contain the current CPU-hot direct HLE/PLT
# rewrites. This runs while a direct NSP/XCI mount is alive, so no extraction is
# needed. write_if_changed in gen.py keeps unchanged chunks untouched.
if ($RegenerateHot) {
    $GenerateScript = Join-Path $PSScriptRoot "scarlet_generate.ps1"
    if (-not (Test-Path -LiteralPath $GenerateScript -PathType Leaf)) {
        throw "Gerador Scarlet ausente: $GenerateScript"
    }
    foreach ($HotModule in @("sdk", "main")) {
        Write-Host "[scarlet-run] Regenerando modulo quente $HotModule a partir de $ExeFs ..." -ForegroundColor Magenta
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $GenerateScript -ExeFs $ExeFs -Module $HotModule
        if ($LASTEXITCODE -ne 0) {
            throw "Geracao de $HotModule falhou (exit $LASTEXITCODE)."
        }
    }

    # Guard against stale generated chunks from an interrupted/older generator.
    # The SignalLightEvent fast path must use an integer weak=false argument so
    # generated C does not depend on <stdbool.h>.
    $StaleAtomic = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "generated\sdk") -Filter '*.c' -File -ErrorAction SilentlyContinue |
        Select-String -SimpleMatch '__atomic_compare_exchange_n(_sle_state, &_sle_expected, 2, false' |
        Select-Object -First 1
    if ($StaleAtomic) {
        throw "generated\sdk ainda contem fast path antigo com 'false': $($StaleAtomic.Path):$($StaleAtomic.LineNumber)"
    }

    # Never rebuild/relink the huge guest DLL implicitly. Generated changes stay
    # staged until -RebuildGameDll is explicitly requested.
    if ($RebuildGameDll) {
        Write-Host "[scarlet-run] -RebuildGameDll autorizado: compilando chunks gerados alterados..." -ForegroundColor Magenta
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime -ThinLTO @BuildFlavorArgs
        if ($LASTEXITCODE -ne 0) {
            $WhichLog = if ($Profile) { "scarlet_build_profile.log" } else { "scarlet_build.log" }
            throw "Build apos regeneracao falhou (exit $LASTEXITCODE). Envie $WhichLog."
        }
    } else {
        Write-Host "[scarlet-run] lasgame.dll CONGELADA: fontes geradas atualizadas, mas a DLL NAO sera recompilada." -ForegroundColor Green
    }
}

# git pull updates source files, not the already-built EXE. Compare the exact
# Git revision that produced the runtime; timestamps are unreliable after checkout.
$SkipAutoBuild = $false
if ($env:LARECOMP_SKIP_AUTO_BUILD) {
    $SkipAutoBuild = @('1','true','yes','on') -contains $env:LARECOMP_SKIP_AUTO_BUILD.ToLowerInvariant()
}
if (Test-Path -LiteralPath $Exe -PathType Leaf) {
    $Head = (& git -C $RepoRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $Head) { $Head = "" } else { $Head = $Head.Trim() }
    $Marker = Join-Path $BuildVariantDir ".runtime_build_head"
    $BuiltHead = if (Test-Path -LiteralPath $Marker) { (Get-Content -LiteralPath $Marker -Raw).Trim() } else { "" }
    Write-Host "[scarlet-run] Runtime HEAD atual: $Head"
    Write-Host "[scarlet-run] Runtime HEAD do EXE: $BuiltHead"
    if ($SkipAutoBuild) {
        Write-Host "[scarlet-run] Auto-build desativado por LARECOMP_SKIP_AUTO_BUILD=$env:LARECOMP_SKIP_AUTO_BUILD" -ForegroundColor Yellow
    }
    elseif (-not $BuiltHead -or $BuiltHead -ne $Head) {
        $NeedNativeBuild = $true
        $RuntimeOnlyBuild = -not $RebuildGameDll
        if ($BuiltHead -and $Head) {
            # The local checkout may be shallow while .runtime_build_head still
            # points at an older commit. Avoid revision syntax such as ^{commit}
            # here (PowerShell/native argument handling is fragile around the
            # braces) and validate the raw object id instead.
            $ObjectType = @(& git -C $RepoRoot cat-file -t $BuiltHead 2>$null)
            $ObjectOk = ($LASTEXITCODE -eq 0 -and $ObjectType -contains "commit")

            if (-not $ObjectOk) {
                Write-Host "[scarlet-run] Commit do EXE nao esta no clone local; buscando $BuiltHead..." -ForegroundColor DarkGray
                & git -C $RepoRoot fetch --no-tags --depth=1 origin $BuiltHead 2>$null
                $FetchCode = $LASTEXITCODE
                $ObjectType = @(& git -C $RepoRoot cat-file -t $BuiltHead 2>$null)
                $ObjectOk = ($LASTEXITCODE -eq 0 -and $ObjectType -contains "commit")

                if (-not $ObjectOk) {
                    Write-Host "[scarlet-run] Nao foi possivel recuperar o commit antigo (fetch exit $FetchCode); lasgame.dll permanece congelada e somente o runtime sera recompilado." -ForegroundColor Yellow
                    $RuntimeOnlyBuild = -not $RebuildGameDll
                }
            }

            if ($ObjectOk) {
                $Changed = @(& git -C $RepoRoot diff --name-only $BuiltHead $Head 2>$null)
                if ($LASTEXITCODE -eq 0) {
                    $NativeChanged = @($Changed | Where-Object {
                        $_ -match "^(runtime/|generated/)" -or $_ -match "^Makefile$" -or
                        $_ -match "^tools/(gen[.]py|lifter[.]py|analyze[.]py|nso[.]py|passes[.]py|patches[.]py|simd_c[.]py|gamelib_entry[.]c)$"
                    })
                    $GameCodeChanged = @($Changed | Where-Object {
                        $_ -match "^generated/" -or $_ -match "^Makefile$" -or
                        $_ -match "^tools/(gen[.]py|lifter[.]py|analyze[.]py|nso[.]py|passes[.]py|patches[.]py|simd_c[.]py|gamelib_entry[.]c)$" -or
                        $_ -match "^runtime/(cpu[.]h|simd.*[.]h|gamelib[.]h|hle/hle_symbols[.]txt)$"
                    })
                    $NeedNativeBuild = $NativeChanged.Count -gt 0
                    # lasgame.dll is frozen by default. Even if generator/game-code
                    # sources changed, automatic rebuilds update only the runtime.
                    # Rebuilding the guest DLL requires explicit -RebuildGameDll.
                    $RuntimeOnlyBuild = $NeedNativeBuild -and -not $RebuildGameDll
                    if ($NeedNativeBuild -and $GameCodeChanged.Count -gt 0 -and -not $RebuildGameDll) {
                        Write-Host "[scarlet-run] Mudancas de guest detectadas, mas lasgame.dll esta CONGELADA; recompilando somente o runtime." -ForegroundColor Green
                    }
                }
            }
        }
        if (-not $NeedNativeBuild) {
            Write-Host "[scarlet-run] HEAD mudou apenas em scripts/docs; mantendo EXE/DLL ja compilados." -ForegroundColor DarkGray
        } else {
            $ThinLtoMarker = Join-Path $BuildVariantDir ".thinlto_enabled"
            if ($RuntimeOnlyBuild) {
                Write-Host "[scarlet-run] Mudou apenas o runtime; recompilando ScarletRecomp.exe sem relinkar lasgame.dll..." -ForegroundColor Yellow
                if (Test-Path -LiteralPath $ThinLtoMarker -PathType Leaf) {
                    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime -RuntimeOnly -ThinLTO @BuildFlavorArgs
                } else {
                    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime -RuntimeOnly @BuildFlavorArgs
                }
            } else {
                Write-Host "[scarlet-run] -RebuildGameDll autorizado; executando build completo..." -ForegroundColor Yellow
                if (Test-Path -LiteralPath $ThinLtoMarker -PathType Leaf) {
                    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime -ThinLTO @BuildFlavorArgs
                } else {
                    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript -ForceRuntime @BuildFlavorArgs
                }
            }
            if ($LASTEXITCODE -ne 0) { throw "Build do runtime falhou (exit $LASTEXITCODE). Envie scarlet_build.log." }
        }
    }
}

foreach ($P in @($Exe, $GameDll, (Join-Path $ExeFs "main"), (Join-Path $ExeFs "main.npdm"))) {
    if (-not (Test-Path -LiteralPath $P)) {
        throw "Arquivo necessario ausente: $P"
    }
}

if (-not (Test-Path -LiteralPath $RomFs -PathType Container)) {
    throw "RomFS nao encontrado: $RomFs"
}
if ($DirectPackage -and -not (Test-Path -LiteralPath $env:LARECOMP_ROMFS_IMAGE -PathType Leaf)) {
    throw "RomFS virtual do pacote nao encontrado: $env:LARECOMP_ROMFS_IMAGE"
}

$MsysBin = "C:\msys64\ucrt64\bin"
if (-not (Test-Path -LiteralPath $MsysBin -PathType Container)) {
    throw "Runtime DLLs do MSYS2 nao encontradas. Rode tools\scarlet_build_windows.ps1 -Setup."
}

$env:Path = "$MsysBin;C:\msys64\usr\bin;$env:Path"
$env:LARECOMP_NO_LAUNCHER = "1"
Remove-Item Env:LARECOMP_LOG_HEAPS -ErrorAction SilentlyContinue
$env:LARECOMP_LOG_BINDINGS = ""
if ($Performance) {
    # Vega-class integrated GPUs benefit more from reducing the scalable color
    # targets than from lowering texture quality.  0.50 = 960x540 guest color
    # buffers at 16:9; depth/stencil stays full-size for correctness.  Skip the
    # extra three host-side SMAA passes; the configured upscaler (FSR1 when
    # selected) can still upscale to the window.
    $env:LARECOMP_SCALE = "0.50"
    $env:LARECOMP_NO_SMAA = "1"
    $env:LARECOMP_PERFORMANCE = "1"
    Write-Host "[scarlet-run] Performance mode: 960x540 scalable targets, host SMAA bypassed, CPU scheduling optimized." -ForegroundColor Green
}
if ($MaterialViews) {
    $env:LARECOMP_LOG_MATERIAL_VIEWS = "1"
    Write-Host "[scarlet-run] Material-view tracing enabled." -ForegroundColor Cyan
}

# Keep graphics A/B tests mutually isolated. A neutral-tint run is not
# interpretable if older BC7/channel/lighting experiments are inherited from
# the parent PowerShell session.
if ($env:LARECOMP_NEUTRAL_HAIR_TINT) {
    foreach ($name in @(
        "LARECOMP_BC7_SWAP_RB",
        "LARECOMP_BC7_LINEAR",
        "LARECOMP_HAIR_ISOTROPIC"
    )) {
        if (Test-Path "Env:$name") {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
            Write-Host "[scarlet-run] Cleared incompatible diagnostic: $name" -ForegroundColor Yellow
        }
    }
}

$Log = Join-Path $RepoRoot "scarlet_boot.log"
if (Test-Path -LiteralPath $Log) {
    Remove-Item -LiteralPath $Log -Force
}

Write-Host "[scarlet-run] Primeiro boot do Scarlet..." -ForegroundColor Cyan
Write-Host "[scarlet-run] Game input: $OriginalGameInput"
Write-Host "[scarlet-run] Game root: $GameRoot"
if ($DirectPackage) {
    Write-Host "[scarlet-run] Package mode: DIRECT STREAM (sem extracao)" -ForegroundColor Green
    Write-Host "[scarlet-run] ExeFS virtual: $ExeFs"
    Write-Host "[scarlet-run] RomFS virtual: $env:LARECOMP_ROMFS_IMAGE"
}
Write-Host "[scarlet-run] Log: $Log"
$CurrentHead = (& git -C $RepoRoot rev-parse HEAD 2>$null)
$MarkerNow = Join-Path $BuildVariantDir ".runtime_build_head"
$BuiltHeadNow = if (Test-Path -LiteralPath $MarkerNow) { (Get-Content -LiteralPath $MarkerNow -Raw).Trim() } else { "<sem marcador>" }
Write-Host "[scarlet-run] SCRIPT_REV=$($CurrentHead.Trim())"
Write-Host "[scarlet-run] EXE_REV=$BuiltHeadNow"
if ($Profile) {
    Write-Host "[scarlet-run] PROFILE guest: host RIP sampling (lasgame.dll normal congelada)" -ForegroundColor Magenta
}
Write-Host ""

# Windows PowerShell 5.1 turns native stderr into NativeCommandError when
# ErrorActionPreference is Stop. cmd.exe merges stderr into stdout before
# PowerShell sees it, so the runtime can log normally without being aborted.
$Q = [char]34
$BaseCmdLine = $Q + $Exe + $Q + " " + $Q + $GameRoot + $Q + " --play"
if ($RenderDoc) {
    if ($Backend -ne 'Vulkan') { throw 'Para esta captura de cores, use -Backend Vulkan.' }
    if (-not (Test-Path -LiteralPath $RenderDocCommand -PathType Leaf)) {
        throw 'RenderDoc nao encontrado. Instale o RenderDoc x64 de https://renderdoc.org ou informe -RenderDocCommand.'
    }
    $RenderDocCommand = (Resolve-Path -LiteralPath $RenderDocCommand).Path
    $CaptureDirectory = Join-Path $RepoRoot ('build\rdc\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    $null = New-Item -ItemType Directory -Force -Path $CaptureDirectory
    $CaptureTemplate = Join-Path $CaptureDirectory 'scarlet'
    $BaseCmdLine = $Q + $RenderDocCommand + $Q + ' capture --wait-for-exit --working-dir ' + $Q + $RepoRoot + $Q +
        ' --capture-file ' + $Q + $CaptureTemplate + $Q + ' ' + $BaseCmdLine
    Write-Host "[scarlet-run] RenderDoc: aperte F12 uma vez com o cabelo afetado visivel. Capturas: $CaptureDirectory" -ForegroundColor Cyan
}
$PreviousVkProf = $env:LARECOMP_VK_PROF
$PreviousVkOpt = $env:LARECOMP_VK_OPT
$LocationPushed = $false
$PreviousDiagnosticEnv = @{}
foreach ($Name in @(
    'LARECOMP_HLE_PROF',
    'LARECOMP_TF_DIAG_DIR',
    'LARECOMP_RENDERDOC_AT',
    'LARECOMP_RENDERDOC_FRAMES',
    'LARECOMP_FRAME_CSV',
    'LARECOMP_FNPROF',
    'LARECOMP_FNPROF_OUT',
    'LARECOMP_FNPROF_AUTO_MS',
    'LARECOMP_FNPROF_AUTO_CONSEC',
    'LARECOMP_FNPROF_AUTO_FRAMES',
    'LARECOMP_FNPROF_AUTO_STEP_MS',
    'LARECOMP_FNPROF_AUTO_CAPTURES',
    'LARECOMP_HOSTPROF_OUT',
    'LARECOMP_HOSTPROF_MS',
    'LARECOMP_GPUPROF_AT',
    'LARECOMP_SLOW_FRAME_MS',
    'LARECOMP_INJECT_FRAME_DELAY_MS'
)) {
    $PreviousDiagnosticEnv[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
}
$GraphicsDir = $null
$PerformanceDir = $null
$PerformanceMonitor = $null
$PerformanceStamp = $null
$PerformanceZip = $null
try {
    # Anchor launcher-relative paths. The runtime chooses its own data root and reports it in the log.
    Push-Location -LiteralPath $RepoRoot
    $LocationPushed = $true
    # Normal runs use the stable compiler mode, even after experiments in this PowerShell session.
    # Shader translation/compilation stays enabled asynchronously; optimizer remains off because -O caused
    # large dispatcher shaders to regress on startup. Timeout is non-fatal in the runtime: affected draws are
    # skipped temporarily until their shader/pipeline finishes instead of crashing the game.
    if ($RenderDoc) {
        Remove-Item Env:LARECOMP_RENDERDOC_AT, Env:LARECOMP_RENDERDOC_FRAMES -ErrorAction SilentlyContinue
    }
    $env:LARECOMP_VK_OPT = "0"
    $env:LARECOMP_SHADER_WAIT_MS = "12000"

    Remove-Item Env:LARECOMP_SHADER_TIMEOUT_EXIT -ErrorAction SilentlyContinue
    Remove-Item Env:LARECOMP_SYNC_SHADERS -ErrorAction SilentlyContinue
    Remove-Item Env:LARECOMP_TF_CHECK -ErrorAction SilentlyContinue
    if ($GraphicsDiagnostics) {
        $Stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $GraphicsDir = Join-Path $RepoRoot "build\scarlet-diagnostics-$Stamp"
        $null = New-Item -ItemType Directory -Path $GraphicsDir -Force
        $env:LARECOMP_TF_DIAG_DIR = $GraphicsDir
        Write-Host "[scarlet-run] Coleta de controles NVN: $GraphicsDir"
    }
    else {
        Remove-Item Env:LARECOMP_TF_DIAG_DIR -ErrorAction SilentlyContinue
    }
    if ($Profile) {
        # A former diagnostic variable named LARECOMP_SLOW_FRAME_MS actually
        # injected a real SDL_Delay after frame 2600. Never let an inherited
        # value contaminate a performance capture.
        Remove-Item Env:LARECOMP_SLOW_FRAME_MS -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_INJECT_FRAME_DELAY_MS -ErrorAction SilentlyContinue

        $PerformanceStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $PerformanceDir = Join-Path $RepoRoot ("build\scarlet-performance-" + $PerformanceStamp)
        $null = New-Item -ItemType Directory -Path $PerformanceDir -Force

        $env:LARECOMP_HLE_PROF = '1'
        $env:LARECOMP_VK_PROF = '1'
        $env:LARECOMP_FRAME_CSV = Join-Path $PerformanceDir 'frames.csv'

        # Guest hotspots are sampled from host RIPs in the normal lasgame.dll.
        # Do not arm FN_ENTER/FNPROF: that would require the expensive instrumented DLL.
        Remove-Item Env:LARECOMP_FNPROF -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FNPROF_OUT -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FNPROF_AUTO_MS -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FNPROF_AUTO_CONSEC -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FNPROF_AUTO_FRAMES -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FNPROF_AUTO_STEP_MS -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FNPROF_AUTO_CAPTURES -ErrorAction SilentlyContinue
        $env:LARECOMP_HOSTPROF_OUT = Join-Path $PerformanceDir 'hostprof.txt'
        if (-not $env:LARECOMP_HOSTPROF_MS) { $env:LARECOMP_HOSTPROF_MS = '5' }

        if (-not $env:LARECOMP_GPUPROF_AT) { $env:LARECOMP_GPUPROF_AT = '1200,2200,2900,4000' }

        $RunInfo = [ordered]@{
            StartedUtc = [DateTime]::UtcNow.ToString('o')
            GitHead = $CurrentHead.Trim()
            BuiltHead = $BuiltHeadNow
            Backend = $Backend
            Performance = [bool]$Performance
            FunctionFrames = "host-rip-sampling"
            LogicalProcessors = [Environment]::ProcessorCount
            Environment = [ordered]@{
                LARECOMP_SCALE = $env:LARECOMP_SCALE
                LARECOMP_NO_SMAA = $env:LARECOMP_NO_SMAA
                LARECOMP_PERFORMANCE = $env:LARECOMP_PERFORMANCE
                LARECOMP_VK_PROF = $env:LARECOMP_VK_PROF
                LARECOMP_HLE_PROF = $env:LARECOMP_HLE_PROF
                LARECOMP_FRAME_CSV = $env:LARECOMP_FRAME_CSV
                LARECOMP_FNPROF = $env:LARECOMP_FNPROF
                LARECOMP_FNPROF_OUT = $env:LARECOMP_FNPROF_OUT
                LARECOMP_FNPROF_AUTO_MS = $env:LARECOMP_FNPROF_AUTO_MS
                LARECOMP_FNPROF_AUTO_CONSEC = $env:LARECOMP_FNPROF_AUTO_CONSEC
                LARECOMP_FNPROF_AUTO_FRAMES = $env:LARECOMP_FNPROF_AUTO_FRAMES
                LARECOMP_FNPROF_AUTO_STEP_MS = $env:LARECOMP_FNPROF_AUTO_STEP_MS
                LARECOMP_FNPROF_AUTO_CAPTURES = $env:LARECOMP_FNPROF_AUTO_CAPTURES
                LARECOMP_HOSTPROF_OUT = $env:LARECOMP_HOSTPROF_OUT
                LARECOMP_HOSTPROF_MS = $env:LARECOMP_HOSTPROF_MS
                LARECOMP_GPUPROF_AT = $env:LARECOMP_GPUPROF_AT
            }
        }
        $RunInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PerformanceDir 'run.json') -Encoding UTF8

        $MonitorScript = Join-Path $PSScriptRoot 'scarlet_performance_monitor.ps1'
        if (Test-Path -LiteralPath $MonitorScript -PathType Leaf) {
            $MonitorArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$MonitorScript`" -OutputDir `"$PerformanceDir`""
            try {
                $PerformanceMonitor = Start-Process -FilePath 'powershell.exe' -ArgumentList $MonitorArgs -WindowStyle Hidden -PassThru
            }
            catch {
                Write-Host "[scarlet-run] Monitor do Windows nao iniciou: $_" -ForegroundColor Yellow
            }
        }

        Write-Host "[scarlet-run] Perfil completo ativo: frames, CPU/threads, GPU, HLE e guest hotspots por amostragem." -ForegroundColor Cyan
        Write-Host "[scarlet-run] Captura: $PerformanceDir" -ForegroundColor Cyan
    }
    else {
        # Do not inherit profiling or old delay injection from an earlier
        # PowerShell session. These are diagnostics, never normal-play options.
        Remove-Item Env:LARECOMP_VK_PROF -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_HLE_PROF -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_FRAME_CSV -ErrorAction SilentlyContinue
        # Preserve an explicitly requested function-profile session even on a
        # normal run. This lets developers arm FNPROF from PowerShell without
        # enabling the heavier full -Profile capture.
        $ManualFnProf = $env:LARECOMP_FNPROF -or $env:LARECOMP_FNPROF_AUTO_MS
        if (-not $ManualFnProf) {
            Remove-Item Env:LARECOMP_FNPROF -ErrorAction SilentlyContinue
            Remove-Item Env:LARECOMP_FNPROF_OUT -ErrorAction SilentlyContinue
            Remove-Item Env:LARECOMP_FNPROF_AUTO_MS -ErrorAction SilentlyContinue
            Remove-Item Env:LARECOMP_FNPROF_AUTO_CONSEC -ErrorAction SilentlyContinue
            Remove-Item Env:LARECOMP_FNPROF_AUTO_FRAMES -ErrorAction SilentlyContinue
            Remove-Item Env:LARECOMP_FNPROF_AUTO_STEP_MS -ErrorAction SilentlyContinue
            Remove-Item Env:LARECOMP_FNPROF_AUTO_CAPTURES -ErrorAction SilentlyContinue
        } elseif (-not $env:LARECOMP_FNPROF_OUT) {
            $env:LARECOMP_FNPROF_OUT = Join-Path $RepoRoot 'build\fnprof.txt'
            Write-Host "[scarlet-run] FNPROF manual ativo: $env:LARECOMP_FNPROF_OUT" -ForegroundColor Cyan
        }
        Remove-Item Env:LARECOMP_GPUPROF_AT -ErrorAction SilentlyContinue
        Remove-Item Env:LARECOMP_SLOW_FRAME_MS -ErrorAction SilentlyContinue
    }
    $Code = Invoke-ScarletGraphicsBackend -Backend $Backend -DirectXDirectory $DirectXDirectory `
        -Log $Log -NoFallback:$NoBackendFallback -Run {
        param($AttemptLog)
        if ($LiveLog) {
            # Diagnostic convenience only: piping thousands of native log lines
            # through PowerShell/Tee-Object can stall the game itself on Windows.
            $CmdLine = $BaseCmdLine + " 2>&1"
            & $env:ComSpec /d /s /c $CmdLine | Tee-Object -FilePath $AttemptLog | Out-Host
            return $LASTEXITCODE
        }
        else {
            # Fast path: cmd.exe writes directly to disk. PowerShell is not in the
            # runtime's stdout/stderr hot path, so loading/shader bursts cannot block
            # waiting for Tee-Object to consume lines.
            $CmdLine = $BaseCmdLine + " > " + $Q + $AttemptLog + $Q + " 2>&1"
            & $env:ComSpec /d /s /c $CmdLine
            return $LASTEXITCODE
        }
    }
}
finally {
    if ($PerformanceDir) {
        try {
            Set-Content -LiteralPath (Join-Path $PerformanceDir 'monitor.stop') -Value 'stop' -Encoding ASCII
            if ($PerformanceMonitor -and -not $PerformanceMonitor.HasExited) {
                $null = $PerformanceMonitor.WaitForExit(8000)
                if (-not $PerformanceMonitor.HasExited) { Stop-Process -Id $PerformanceMonitor.Id -Force -ErrorAction SilentlyContinue }
            }
        }
        catch {
            Write-Host "[scarlet-run] Nao foi possivel encerrar o monitor de desempenho: $_" -ForegroundColor Yellow
        }
    }
    if ($LocationPushed) { Pop-Location }
    foreach ($Name in $PreviousDiagnosticEnv.Keys) {
        if ($null -eq $PreviousDiagnosticEnv[$Name]) {
            Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue
        }
        else {
            Set-Item -LiteralPath "Env:$Name" -Value $PreviousDiagnosticEnv[$Name]
        }
    }
    if ($null -eq $PreviousVkOpt) {
        Remove-Item Env:LARECOMP_VK_OPT -ErrorAction SilentlyContinue
    }
    else {
        $env:LARECOMP_VK_OPT = $PreviousVkOpt
    }
    if ($null -eq $PreviousVkProf) {
        Remove-Item Env:LARECOMP_VK_PROF -ErrorAction SilentlyContinue
    }
    else {
        $env:LARECOMP_VK_PROF = $PreviousVkProf
    }
}

if ($PerformanceDir) {
    try {
        if (Test-Path -LiteralPath $Log -PathType Leaf) {
            Copy-Item -LiteralPath $Log -Destination (Join-Path $PerformanceDir 'scarlet_boot.log') -Force
        }

        foreach ($Candidate in @(
            (Join-Path $RepoRoot 'build\scarlet_gpuprof_latest.txt'),
            (Join-Path $RepoRoot 'build\build\scarlet_gpuprof_latest.txt')
        )) {
            if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
                Copy-Item -LiteralPath $Candidate -Destination (Join-Path $PerformanceDir 'scarlet_gpuprof_latest.txt') -Force
                break
            }
        }

        $ResultInfo = [ordered]@{
            EndedUtc = [DateTime]::UtcNow.ToString('o')
            ExitCode = $Code
            GitHead = $CurrentHead.Trim()
            BuiltHead = $BuiltHeadNow
            ExecutableSha256 = if (Test-Path -LiteralPath $Exe) { (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash } else { $null }
            GameDllSha256 = if (Test-Path -LiteralPath $GameDll) { (Get-FileHash -LiteralPath $GameDll -Algorithm SHA256).Hash } else { $null }
        }
        $ResultInfo | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PerformanceDir 'result.json') -Encoding UTF8

        $ReportTool = Join-Path $PSScriptRoot 'scarlet_performance_report.py'
        $Python = Join-Path $RepoRoot '.venv\Scripts\python.exe'
        if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
            $PyCmd = Get-Command python.exe -ErrorAction SilentlyContinue
            if (-not $PyCmd) { $PyCmd = Get-Command python -ErrorAction SilentlyContinue }
            if ($PyCmd) { $Python = $PyCmd.Source } else { $Python = $null }
        }

        # Merge every FNPROF capture into a REA-ready ranking. Keep it inside
        # the performance bundle so the next step never depends on a separate
        # manual command.
        $HotAnalyzer = Join-Path $PSScriptRoot 'rea\analyze_hot.py'
        if ($Python -and (Test-Path -LiteralPath $HotAnalyzer -PathType Leaf)) {
            try {
                $ProfilesForRea = @(Get-ChildItem -LiteralPath $PerformanceDir -Filter 'fnprof-auto-*.txt' -File -ErrorAction SilentlyContinue | Sort-Object Name)
                if (-not $ProfilesForRea.Count) {
                    $ProfilesForRea = @(Get-ChildItem -LiteralPath $PerformanceDir -Filter 'fnprof.txt' -File -ErrorAction SilentlyContinue)
                }
                if ($ProfilesForRea.Count) {
                    $HotJson = Join-Path $PerformanceDir 'hot_functions.json'
                    $ProfileArgs = @($ProfilesForRea | ForEach-Object { $_.FullName })
                    & $Python $HotAnalyzer @ProfileArgs -o $HotJson
                    if ($LASTEXITCODE -eq 0) {
                        Write-Host "[scarlet-run] Hotspots REA: $HotJson" -ForegroundColor Cyan
                    }
                }
            }
            catch {
                Write-Host "[scarlet-run] Ranking REA dos hotspots falhou: $_" -ForegroundColor Yellow
            }
        }

        # Resolve/disassemble a small set of the hottest guest entries automatically.
        # This is diagnostic-only and runs after the game has exited.
        $Inspector = Join-Path $PSScriptRoot 'scarlet_inspect_addr.py'
        if ($Python -and (Test-Path -LiteralPath $Inspector -PathType Leaf)) {
            try {
                $InspectOut = Join-Path $PerformanceDir 'fnprof-inspect.txt'
                Remove-Item -LiteralPath $InspectOut -Force -ErrorAction SilentlyContinue
                $SeenAddr = @{}
                $Profiles = @(Get-ChildItem -LiteralPath $PerformanceDir -Filter 'fnprof*.txt' -File -ErrorAction SilentlyContinue | Sort-Object Name)
                foreach ($Pf in $Profiles) {
                    Add-Content -LiteralPath $InspectOut -Value ('===== ' + $Pf.Name + ' =====') -Encoding UTF8
                    $Rows = @(Get-Content -LiteralPath $Pf.FullName | Select-Object -First 8)
                    foreach ($Row in $Rows) {
                        $Parts = @($Row -split '\s+')
                        if ($Parts.Count -lt 1) { continue }
                        $Addr = [string]$Parts[0]
                        if ($Addr -notmatch '^[0-9A-Fa-f]{8,}$') { continue }
                        $Key = $Addr.ToLowerInvariant()
                        if ($SeenAddr.ContainsKey($Key)) { continue }
                        $SeenAddr[$Key] = $true
                        Add-Content -LiteralPath $InspectOut -Value ('--- 0x' + $Addr + ' ---') -Encoding UTF8
                        & $Python $Inspector $ExeFs ('0x' + $Addr) 2>&1 | Out-File -LiteralPath $InspectOut -Append -Encoding utf8
                    }
                }
            }
            catch {
                Write-Host "[scarlet-run] Inspecao automatica dos hotspots falhou: $_" -ForegroundColor Yellow
            }
        }

        if ($Python -and (Test-Path -LiteralPath $ReportTool -PathType Leaf)) {
            & $Python $ReportTool $PerformanceDir
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[scarlet-run] Gerador do relatorio terminou com exit $LASTEXITCODE; empacotando dados brutos mesmo assim." -ForegroundColor Yellow
            }
        }
        else {
            Write-Host "[scarlet-run] Python/gerador de relatorio indisponivel; empacotando dados brutos." -ForegroundColor Yellow
        }

        Remove-Item -LiteralPath (Join-Path $PerformanceDir 'monitor.stop') -Force -ErrorAction SilentlyContinue
        $PerformanceZip = Join-Path $RepoRoot ("scarlet-performance-" + $PerformanceStamp + ".zip")
        if (Test-Path -LiteralPath $PerformanceZip) { Remove-Item -LiteralPath $PerformanceZip -Force }
        Compress-Archive -Path (Join-Path $PerformanceDir '*') -DestinationPath $PerformanceZip -CompressionLevel Fastest
        Write-Host ""
        Write-Host "[scarlet-run] PERFIL COMPLETO GERADO" -ForegroundColor Green
        Write-Host "PERFORMANCE_ZIP=$PerformanceZip" -ForegroundColor Cyan
    }
    catch {
        Write-Host "[scarlet-run] Falha ao empacotar perfil de desempenho: $_" -ForegroundColor Yellow
        Write-Host "[scarlet-run] Dados parciais: $PerformanceDir" -ForegroundColor Yellow
    }
}

if ($RenderDoc) {
    $Captures = @(Get-ChildItem -LiteralPath $CaptureDirectory -Filter '*.rdc' -File -ErrorAction SilentlyContinue)
    if ($Captures.Count) {
        Write-Host '[scarlet-run] Capturas RenderDoc para enviar:' -ForegroundColor Cyan
        $Captures | ForEach-Object { Write-Host $_.FullName }
    } else {
        Write-Host "[scarlet-run] Nenhuma captura .rdc encontrada em $CaptureDirectory. Use F12 com a cena visivel e aguarde terminar." -ForegroundColor Yellow
    }
}

# Inspect narrow fast-boot guards too: LR is the return address, so LR-4 is
# the guest instruction that made the bad indirect call.
if (Test-Path -LiteralPath $Log) {
    $Python = Join-Path $RepoRoot ".venv\Scripts\python.exe"
    $Inspector = Join-Path $RepoRoot "tools\scarlet_inspect_addr.py"
    if ((Test-Path -LiteralPath $Python) -and (Test-Path -LiteralPath $Inspector)) {
        # A host backtrace names the containing compiled function, which can
        # hide the exact missing interior branch target. Inspect the failed PC
        # and its guest caller before the more general backtrace inspection.
        $Missing = Select-String -Path $Log -Pattern 'no function at ([0-9a-fA-F]+) \(lr=([0-9a-fA-F]+)' | Select-Object -First 1
        if ($Missing -and $Missing.Matches.Count -gt 0) {
            $Match = $Missing.Matches[0]
            $Targets = @("0x" + $Match.Groups[1].Value)
            $Lr = [Convert]::ToUInt64($Match.Groups[2].Value, 16)
            if ($Lr -ge 4) { $Targets += ('0x{0:x}' -f ($Lr - 4)) }
            foreach ($Addr in $Targets) {
                Write-Host "[scarlet-run] Inspecionando salto sem funcao $Addr ..." -ForegroundColor Yellow
                Add-Content -LiteralPath $Log -Value "[missing-dispatch-inspect] $Addr" -Encoding UTF8
                & $Python $Inspector $ExeFs $Addr 2>&1 | Out-File -LiteralPath $Log -Append -Encoding utf8
            }
        }

        $GuardLine = Select-String -Path $Log -Pattern '\[boot-guard\].*lr=0x([0-9a-fA-F]+)' | Select-Object -First 1
        if ($GuardLine -and $GuardLine.Matches.Count -gt 0) {
            $Lr = [Convert]::ToUInt64($GuardLine.Matches[0].Groups[1].Value, 16)
            if ($Lr -ge 4) {
                $Caller = ('0x{0:x}' -f ($Lr - 4))
                Write-Host ""
                Write-Host "[scarlet-run] Inspecionando caller do boot-guard $Caller ..." -ForegroundColor Yellow
                Add-Content -LiteralPath $Log -Value "[guard-caller-inspect] $Caller" -Encoding UTF8
                & $Python $Inspector $ExeFs $Caller 2>&1 | Out-File -LiteralPath $Log -Append -Encoding utf8
            }
        }

        # If the runtime crashed inside recompiled guest code, inspect the first
        # guest function from the host backtrace automatically.
        $CrashLine = Select-String -Path $Log -Pattern 'f_([0-9a-fA-F]{8,})\+' | Select-Object -First 1
        if ($CrashLine -and $CrashLine.Matches.Count -gt 0) {
            $Addr = "0x" + $CrashLine.Matches[0].Groups[1].Value
            Write-Host ""
            Write-Host "[scarlet-run] Inspecionando crash guest $Addr ..." -ForegroundColor Yellow
            Add-Content -LiteralPath $Log -Value "[crash-inspect] $Addr" -Encoding UTF8
            & $Python $Inspector $ExeFs $Addr 2>&1 | Out-File -LiteralPath $Log -Append -Encoding utf8
        }
    }
}

Write-Host ""
Write-Host "[scarlet-run] Processo terminou com exit code $Code"
if ($Code -eq 86) {
    Write-Host "[scarlet-run] Compilacao grafica excedeu o limite de espera; processo encerrado no modo de diagnostico." -ForegroundColor Yellow
}
$HadSlowBuild = (Test-Path -LiteralPath $Log) -and [bool](Select-String -LiteralPath $Log -Pattern '\[(shader-slow|build-timeout)\]' -Quiet)
if ($Code -eq 86 -or $HadSlowBuild) {
    # Bundle only shaders referenced by this run, plus its log. No game dump, save or complete cache.
    try {
        $Files = @($Log)
        # rt_paths_init changes the child process directory (usually to build/ when the game is external).
        # Its relative diagnostic paths must be resolved from [paths] data, not the launcher's directory.
        $RuntimeRoot = Split-Path -Parent $Exe
        $DataLine = Select-String -LiteralPath $Log -Pattern '^\[paths\] data: (.+)$' | Select-Object -First 1
        if ($DataLine) { $RuntimeRoot = $DataLine.Matches[0].Groups[1].Value.Trim() }
        $Diagnostics = Select-String -LiteralPath $Log -Pattern '\[shader-diagnostic\] input (build/shader-diagnostics/[0-9a-f]+\.(?:vert|frag|comp))\.glsl'
        foreach ($Entry in $Diagnostics) {
            $Base = Join-Path $RuntimeRoot $Entry.Matches[0].Groups[1].Value
            foreach ($Suffix in @('.glsl', '.spv')) {
                $Path = $Base + $Suffix
                if (Test-Path -LiteralPath $Path -PathType Leaf) { $Files += $Path }
            }
        }
        $Files = @($Files | Select-Object -Unique)
        $Archive = Join-Path $RepoRoot 'build\scarlet_shader_failure.zip'
        Compress-Archive -LiteralPath $Files -DestinationPath $Archive -CompressionLevel Fastest -Force
        Write-Host "[scarlet-run] Diagnostico para analise: $Archive" -ForegroundColor Yellow
    }
    catch {
        Write-Host "[scarlet-run] Nao foi possivel compactar o diagnostico: $_" -ForegroundColor Yellow
    }
}
if ($GraphicsDir) {
    try {
        Copy-Item -LiteralPath $Log -Destination (Join-Path $GraphicsDir 'scarlet_boot.log')
        $RuntimeRoot = Split-Path -Parent $Exe
        $DataLine = Select-String -LiteralPath $Log -Pattern '^\[paths\] data: (.+)$' | Select-Object -First 1
        if ($DataLine) { $RuntimeRoot = $DataLine.Matches[0].Groups[1].Value.Trim() }
        $NoPosition = Select-String -LiteralPath $Log -Pattern 'VS has no position export: code=([0-9a-fA-F]+)'
        foreach ($Entry in $NoPosition) {
            $ShaderCode = $Entry.Matches[0].Groups[1].Value
            foreach ($Suffix in @('.glsl', '.sass.bin')) {
                $Path = Join-Path $RuntimeRoot ("build/shader-diagnostics/nopos_{0}_vs{1}" -f $ShaderCode, $Suffix)
                if (Test-Path -LiteralPath $Path -PathType Leaf) {
                    Copy-Item -LiteralPath $Path -Destination $GraphicsDir -Force
                }
            }
        }
        $Manifest = [ordered]@{
            CreatedUtc = [DateTime]::UtcNow.ToString('o')
            ExecutableSha256 = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
            Profile = [bool]$Profile
            ExitCode = $Code
            Note = 'Diagnostic collection only; transform feedback is not implemented by this update.'
        }
        $Manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $GraphicsDir 'run.json') -Encoding UTF8
        $Archive = "$GraphicsDir.zip"
        Compress-Archive -Path (Join-Path $GraphicsDir '*') -DestinationPath $Archive -CompressionLevel Fastest
        Write-Host "[scarlet-run] Envie este arquivo: $Archive" -ForegroundColor Cyan
    }
    catch {
        Write-Host "[scarlet-run] Falha ao compactar; arquivos disponiveis em $GraphicsDir : $_" -ForegroundColor Yellow
    }
}
# Keep the virtual package mounted through post-run diagnostics, then release it.
Stop-ScarletPackageMount
foreach ($Name in $PackageEnvBefore.Keys) {
    if ($null -eq $PackageEnvBefore[$Name]) { Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue }
    else { Set-Item -LiteralPath "Env:$Name" -Value $PackageEnvBefore[$Name] }
}

exit $Code
