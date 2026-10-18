param(
    [string]$GameRoot = "D:\Pokemon Scarlet\Extracted\Base",
    [switch]$Profile,
    [switch]$LiveLog
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $RepoRoot "build\ScarletRecomp.exe"
$GameDll = Join-Path $RepoRoot "build\lasgame.dll"
$ExeFs = Join-Path $GameRoot "exefs"
$RomFs = Join-Path $GameRoot "romfs"

foreach ($P in @($Exe, $GameDll, (Join-Path $ExeFs "main"), (Join-Path $ExeFs "main.npdm"))) {
    if (-not (Test-Path -LiteralPath $P)) {
        throw "Arquivo necessario ausente: $P"
    }
}

if (-not (Test-Path -LiteralPath $RomFs -PathType Container)) {
    throw "RomFS extraido nao encontrado: $RomFs"
}

$MsysBin = "C:\msys64\ucrt64\bin"
if (-not (Test-Path -LiteralPath $MsysBin -PathType Container)) {
    throw "Runtime DLLs do MSYS2 nao encontradas. Rode tools\scarlet_build_windows.ps1 -Setup."
}

$env:Path = "$MsysBin;C:\msys64\usr\bin;$env:Path"
$env:LARECOMP_NO_LAUNCHER = "1"
$env:LARECOMP_LOG_HEAPS = "1"
$env:LARECOMP_LOG_BINDINGS = ""

$Log = Join-Path $RepoRoot "scarlet_boot.log"
if (Test-Path -LiteralPath $Log) {
    Remove-Item -LiteralPath $Log -Force
}

Write-Host "[scarlet-run] Primeiro boot do Scarlet..." -ForegroundColor Cyan
Write-Host "[scarlet-run] Game root: $GameRoot"
Write-Host "[scarlet-run] Log: $Log"
Write-Host ""

# Windows PowerShell 5.1 turns native stderr into NativeCommandError when
# ErrorActionPreference is Stop. cmd.exe merges stderr into stdout before
# PowerShell sees it, so the runtime can log normally without being aborted.
$Q = [char]34
$BaseCmdLine = $Q + $Exe + $Q + " " + $Q + $GameRoot + $Q + " --play"
$PreviousVkProf = $env:LARECOMP_VK_PROF
$LocationPushed = $false
try {
    # Keep save and shader caches in the repository even when invoked by absolute path from another directory.
    Push-Location -LiteralPath $RepoRoot
    $LocationPushed = $true
    if ($Profile) {
        $env:LARECOMP_VK_PROF = "1"
        Write-Host "[scarlet-run] Perfil Vulkan ativo: tempos de CPU, GPU e apresentacao no log."
    }
    else {
        # Do not inherit profiling from an older PowerShell session. GPU timestamp
        # queries and per-frame profiling are useful for diagnostics, not normal play.
        Remove-Item Env:LARECOMP_VK_PROF -ErrorAction SilentlyContinue
    }
    if ($LiveLog) {
        # Diagnostic convenience only: piping thousands of native log lines
        # through PowerShell/Tee-Object can stall the game itself on Windows.
        $CmdLine = $BaseCmdLine + " 2>&1"
        & $env:ComSpec /d /s /c $CmdLine | Tee-Object -FilePath $Log
        $Code = $LASTEXITCODE
    }
    else {
        # Fast path: cmd.exe writes directly to disk. PowerShell is not in the
        # runtime's stdout/stderr hot path, so loading/shader bursts cannot block
        # waiting for Tee-Object to consume lines.
        $CmdLine = $BaseCmdLine + " > " + $Q + $Log + $Q + " 2>&1"
        & $env:ComSpec /d /s /c $CmdLine
        $Code = $LASTEXITCODE
    }
}
finally {
    if ($LocationPushed) { Pop-Location }
    if ($null -eq $PreviousVkProf) {
        Remove-Item Env:LARECOMP_VK_PROF -ErrorAction SilentlyContinue
    }
    else {
        $env:LARECOMP_VK_PROF = $PreviousVkProf
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
    Write-Host "[scarlet-run] Compilacao grafica excedeu 8 segundos. O processo foi encerrado; veja [build-timeout] no log." -ForegroundColor Yellow
}
exit $Code

