param(
    [string]$GameRoot = "D:\Pokemon Scarlet\Extracted\Base"
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
$CmdLine = $Q + $Exe + $Q + " " + $Q + $GameRoot + $Q + " --play 2>&1"
& $env:ComSpec /d /s /c $CmdLine | Tee-Object -FilePath $Log

$Code = $LASTEXITCODE

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
                "[missing-dispatch-inspect] $Addr" | Tee-Object -FilePath $Log -Append
                & $Python $Inspector $ExeFs $Addr 2>&1 | Tee-Object -FilePath $Log -Append
            }
        }

        $GuardLine = Select-String -Path $Log -Pattern '\[boot-guard\].*lr=0x([0-9a-fA-F]+)' | Select-Object -First 1
        if ($GuardLine -and $GuardLine.Matches.Count -gt 0) {
            $Lr = [Convert]::ToUInt64($GuardLine.Matches[0].Groups[1].Value, 16)
            if ($Lr -ge 4) {
                $Caller = ('0x{0:x}' -f ($Lr - 4))
                Write-Host ""
                Write-Host "[scarlet-run] Inspecionando caller do boot-guard $Caller ..." -ForegroundColor Yellow
                "[guard-caller-inspect] $Caller" | Tee-Object -FilePath $Log -Append
                & $Python $Inspector $ExeFs $Caller 2>&1 | Tee-Object -FilePath $Log -Append
            }
        }

        # If the runtime crashed inside recompiled guest code, inspect the first
        # guest function from the host backtrace automatically.
        $CrashLine = Select-String -Path $Log -Pattern 'f_([0-9a-fA-F]{8,})\+' | Select-Object -First 1
        if ($CrashLine -and $CrashLine.Matches.Count -gt 0) {
            $Addr = "0x" + $CrashLine.Matches[0].Groups[1].Value
            Write-Host ""
            Write-Host "[scarlet-run] Inspecionando crash guest $Addr ..." -ForegroundColor Yellow
            "[crash-inspect] $Addr" | Tee-Object -FilePath $Log -Append
            & $Python $Inspector $ExeFs $Addr 2>&1 | Tee-Object -FilePath $Log -Append
        }
    }
}

Write-Host ""
Write-Host "[scarlet-run] Processo terminou com exit code $Code"
exit $Code

