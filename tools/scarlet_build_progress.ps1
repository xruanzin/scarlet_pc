param([string]$LogPath = "", [int]$RefreshMs = 500)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($LogPath)) { $LogPath = Join-Path $RepoRoot "scarlet_build.log" }
if ($RefreshMs -lt 100) { $RefreshMs = 100 }

function Format-Duration([double]$Seconds) {
    if ($Seconds -lt 0) { return "--:--" }
    $ts = [TimeSpan]::FromSeconds([Math]::Round($Seconds))
    if ($ts.TotalHours -ge 1) { return "{0:00}:{1:00}:{2:00}" -f [int]$ts.TotalHours,$ts.Minutes,$ts.Seconds }
    return "{0:00}:{1:00}" -f $ts.Minutes,$ts.Seconds
}

function Count-Files([string]$Dir, [string[]]$Patterns) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return 0 }
    $n = 0
    foreach ($p in $Patterns) {
        $n += @(Get-ChildItem -LiteralPath $Dir -Filter $p -File -ErrorAction SilentlyContinue).Count
    }
    return $n
}

function Get-CompileSource([string]$Line) {
    $a = $Line.IndexOf(" -c ")
    if ($a -lt 0) { return $Line }
    $a += 4
    $b = $Line.IndexOf(" -o ", $a)
    if ($b -lt 0) { return $Line.Substring($a).Trim() }
    return $Line.Substring($a, $b - $a).Trim()
}

function Make-Bar([int]$Percent, [int]$Width = 34) {
    $p = [Math]::Max(0, [Math]::Min(100, $Percent))
    $filled = [Math]::Floor($Width * $p / 100.0)
    return ("#" * $filled).PadRight($Width, "-")
}

function Make-ActivityBar([int]$Tick, [int]$Width = 34) {
    $pulse = 7
    $span = [Math]::Max(1, $Width - $pulse)
    $pos = $Tick % (($span * 2) + 1)
    if ($pos -gt $span) { $pos = ($span * 2) - $pos }
    return (("-" * $pos) + ("#" * $pulse)).PadRight($Width, "-")
}

# Total de unidades guest realmente compiladas pelo wildcard do Makefile.
$guestTotal = 0
foreach ($mod in @("main","sdk","subsdk0")) {
    $guestTotal += Count-Files (Join-Path $RepoRoot ("generated\" + $mod)) @("*.c")
}

# Espelha RT_SRC/BACKEND_SRC/UI_SRC/IMGUI_OBJ do Makefile para Windows.
$runtimeTotal = 0
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime") @("*.c")
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime\hle") @("*.c")
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime\services") @("*.c")
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime\nvn") @("*.c")
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime\nvn\maxwell") @("*.c")
if (Test-Path (Join-Path $RepoRoot "runtime\platform\windows.c")) { $runtimeTotal++ }
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime\nvn\vulkan") @("*.c","*.cpp")
$runtimeTotal += Count-Files (Join-Path $RepoRoot "runtime\ui") @("*.mm")
$runtimeTotal += 6 # imgui core(4) + SDL3 + Vulkan
$runtimeTotal += 1 # windows.rc -> windows_rc.o

$buildStart = $null
$lastLength = -1L
$cachedLines = @()
$tick = 0

Write-Host "[scarlet-progress] Monitorando $LogPath" -ForegroundColor Cyan
Write-Host "[scarlet-progress] Runtime esperado: $runtimeTotal unidades | Guest: $guestTotal unidades" -ForegroundColor DarkGray
Write-Host "[scarlet-progress] Ctrl+C fecha somente este monitor." -ForegroundColor DarkGray

while ($true) {
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        Clear-Host
        Write-Host "Scarlet PC - Build" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "[----------------------------------] aguardando log..."
        Write-Host ""
        Write-Host "Estado: aguardando scarlet_build.log"
        Start-Sleep -Milliseconds $RefreshMs
        continue
    }

    $item = Get-Item -LiteralPath $LogPath
    if ($null -eq $buildStart) { $buildStart = $item.CreationTime }
    if ($item.Length -ne $lastLength) {
        $lastLength = $item.Length
        $cachedLines = @(Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue)
    }
    $lines = $cachedLines
    $elapsed = ((Get-Date) - $buildStart).TotalSeconds

    $failed = $false
    $done = $false
    foreach ($line in $lines) {
        if ($line.Contains("error:") -or $line.Contains("make: ***") -or $line.Contains("Build falhou")) { $failed = $true }
        if ($line.Contains("BUILD CONCLUIDO") -or ($line.Contains("echo ") -and $line.Contains("lasgame.dll.id"))) { $done = $true }
    }

    if ($failed) {
        Write-Progress -Activity "Scarlet PC - Build" -Completed
        Clear-Host
        Write-Host "Scarlet PC - Build" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "[!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!] ERRO" -ForegroundColor Red
        Write-Host ""
        Write-Host ("Tempo:  {0}" -f (Format-Duration $elapsed))
        Write-Host "Estado: compilacao interrompida; veja scarlet_build.log" -ForegroundColor Red
        break
    }

    if ($done) {
        Write-Progress -Activity "Scarlet PC - Build" -Completed
        Clear-Host
        Write-Host "Scarlet PC - Build" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "[##################################] 100%" -ForegroundColor Green
        Write-Host ""
        Write-Host ("Tempo:  {0}" -f (Format-Duration $elapsed))
        Write-Host "Estado: BUILD CONCLUIDO - ScarletRecomp.exe + lasgame.dll" -ForegroundColor Green
        break
    }

    $runtimeSeen = New-Object 'System.Collections.Generic.HashSet[string]'
    $runtimeCurrent = ""
    foreach ($line in $lines) {
        $isRuntime = $line.Contains(" -c runtime/") -and $line.Contains(" -o build/rt/")
        $isImGui = $line.Contains(" -c third_party/imgui/") -and $line.Contains(" -o build/imgui/")
        $isRc = $line.Contains("windres ") -and $line.Contains("build/rt/platform/windows_rc.o")
        if ($isRuntime -or $isImGui -or $isRc) {
            $outPos = $line.LastIndexOf(" -o ")
            $key = if ($outPos -ge 0) { $line.Substring($outPos + 4).Trim() } else { $line.Trim() }
            [void]$runtimeSeen.Add($key)
            if ($isRc) { $runtimeCurrent = "runtime/platform/windows.rc" }
            else { $runtimeCurrent = Get-CompileSource $line }
        }
    }
    $runtimeCount = $runtimeSeen.Count

    $gamePrefix = "[game-cc] "
    $game = @($lines | Where-Object { $_.StartsWith($gamePrefix) })
    $guestCount = $game.Count
    $guestCurrent = if ($game.Count -gt 0) { $game[-1].Substring($gamePrefix.Length).Trim() } else { "" }

    $exeLinkStarted = @($lines | Where-Object { $_.Contains("-o build/link/ScarletRecomp.exe") }).Count -gt 0
    $exeLinked = @($lines | Where-Object { $_.Contains("mv -f build/link/ScarletRecomp.exe build/ScarletRecomp.exe") }).Count -gt 0
    $thinStarted = @($lines | Where-Object { $_.Contains("-o build/lasgame.dll.tmp") }).Count -gt 0

    $phase = ""
    $detail = ""
    $percent = 0
    $bar = ""
    $indeterminate = $false

    if ($thinStarted) {
        $phase = "FASE 4/4 - ThinLTO + link de lasgame.dll"
        $detail = "Otimizando e linkando milhares de objetos guest com lld"
        $indeterminate = $true
        $bar = Make-ActivityBar $tick
        $tmp = Join-Path $RepoRoot "build\lasgame.dll.tmp"
        if (Test-Path -LiteralPath $tmp -PathType Leaf) {
            $mb = [Math]::Round((Get-Item -LiteralPath $tmp).Length / 1MB, 1)
            $detail += " | lasgame.dll.tmp: $mb MB"
        } else {
            $detail += " | aguardando/produzindo arquivo temporario"
        }
    } elseif ($exeLinkStarted -and -not $exeLinked) {
        $phase = "FASE 2/4 - Link do ScarletRecomp.exe"
        $detail = "lld esta montando o executavel do runtime"
        $indeterminate = $true
        $bar = Make-ActivityBar $tick
    } elseif ($guestCount -gt 0 -and $guestCount -lt $guestTotal) {
        $phase = "FASE 3/4 - Codigo recompilado do jogo"
        $percent = if ($guestTotal -gt 0) { [Math]::Floor($guestCount * 100.0 / $guestTotal) } else { 0 }
        $bar = Make-Bar $percent
        $detail = $guestCurrent
    } elseif (-not $exeLinked) {
        $phase = "FASE 1/4 - Runtime / NVN / Vulkan"
        $percent = if ($runtimeTotal -gt 0) { [Math]::Floor($runtimeCount * 100.0 / $runtimeTotal) } else { 0 }
        $percent = [Math]::Min(100, $percent)
        $bar = Make-Bar $percent
        $detail = if ($runtimeCurrent) { $runtimeCurrent } else { "Preparando objetos do runtime..." }
    } elseif (-not $thinStarted) {
        $phase = "FASE 3/4 - Preparando biblioteca do jogo"
        if ($guestTotal -gt 0 -and $guestCount -gt 0) {
            $percent = [Math]::Min(100, [Math]::Floor($guestCount * 100.0 / $guestTotal))
            $bar = Make-Bar $percent
            $detail = "$guestCount / $guestTotal objetos guest"
        } else {
            $indeterminate = $true
            $bar = Make-ActivityBar $tick
            $detail = "Objetos guest ja estavam incrementais; preparando link final"
        }
    }

    Clear-Host
    Write-Host "Scarlet PC - Build" -ForegroundColor Cyan
    Write-Host ""
    if ($indeterminate) {
        Write-Host ("[{0}] trabalhando" -f $bar) -ForegroundColor Yellow
    } else {
        Write-Host ("[{0}] {1,3}%" -f $bar,$percent)
    }
    Write-Host ""
    Write-Host ("Estado:   {0}" -f $phase)
    Write-Host ("Atual:    {0}" -f $detail)
    if ($phase.Contains("Runtime")) {
        Write-Host ("Objetos:  {0} / {1}" -f $runtimeCount,$runtimeTotal)
    } elseif ($phase.Contains("Codigo recompilado")) {
        Write-Host ("Objetos:  {0} / {1}" -f $guestCount,$guestTotal)
    }
    Write-Host ("Tempo:    {0}" -f (Format-Duration $elapsed))
    Write-Host ""
    if ($thinStarted) {
        Write-Host "Nota: o lld/ThinLTO nao fornece percentual interno; a barra animada indica atividade." -ForegroundColor DarkGray
    }
    Write-Host "Ctrl+C fecha apenas o monitor; o build continua." -ForegroundColor DarkGray

    if ($indeterminate) {
        Write-Progress -Activity "Scarlet PC - Build" -Status $phase -CurrentOperation $detail
    } else {
        Write-Progress -Activity "Scarlet PC - Build" -Status $phase -CurrentOperation $detail -PercentComplete $percent
    }

    $tick++
    Start-Sleep -Milliseconds $RefreshMs
}
