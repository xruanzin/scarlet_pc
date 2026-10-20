param(
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [int]$SampleMs = 1500
)

$ErrorActionPreference = "SilentlyContinue"
$null = New-Item -ItemType Directory -Force -Path $OutputDir
$ErrorFile = Join-Path $OutputDir "monitor-errors.txt"
$StopFile = Join-Path $OutputDir "monitor.stop"
$ProcessCsv = Join-Path $OutputDir "process.csv"
$ThreadsCsv = Join-Path $OutputDir "threads.csv"
$GpuCsv = Join-Path $OutputDir "gpu-engines.csv"

$ProcessRows = New-Object System.Collections.Generic.List[object]
$ThreadRows = New-Object System.Collections.Generic.List[object]
$GpuRows = New-Object System.Collections.Generic.List[object]
$ThreadPrev = @{}
$PrevProcCpuMs = $null
$PrevUtc = $null
$Seen = $false
$Logical = [Environment]::ProcessorCount
if ($Logical -lt 1) { $Logical = 1 }
$WaitDeadline = (Get-Date).AddMinutes(3)

function Add-MonitorError([string]$Message) {
    try { Add-Content -LiteralPath $ErrorFile -Value ("{0:o} {1}" -f [DateTime]::UtcNow, $Message) -Encoding UTF8 } catch {}
}

while ($true) {
    if ((Test-Path -LiteralPath $StopFile) -and -not $Seen) { break }

    $Proc = $null
    try {
        $Candidates = @(Get-Process -Name "ScarletRecomp" -ErrorAction SilentlyContinue)
        if ($Candidates.Count -gt 0) {
            $Proc = $Candidates | Sort-Object StartTime -Descending | Select-Object -First 1
        }
    } catch {
        Add-MonitorError ("Get-Process: " + $_.Exception.Message)
    }

    if (-not $Proc) {
        if ($Seen) { break }
        if ((Get-Date) -gt $WaitDeadline) {
            Add-MonitorError "ScarletRecomp.exe did not appear within 3 minutes."
            break
        }
        Start-Sleep -Milliseconds 200
        continue
    }

    $Seen = $true
    $Now = [DateTime]::UtcNow
    $NowText = $Now.ToString("o")
    $IntervalMs = $null
    if ($PrevUtc) { $IntervalMs = ($Now - $PrevUtc).TotalMilliseconds }

    try {
        $Proc.Refresh()
        $CpuMs = $Proc.TotalProcessorTime.TotalMilliseconds
        $CpuCores = $null
        $CpuMachine = $null
        if ($null -ne $PrevProcCpuMs -and $IntervalMs -and $IntervalMs -gt 0) {
            $CpuCores = ($CpuMs - $PrevProcCpuMs) / $IntervalMs
            $CpuMachine = ($CpuCores * 100.0) / $Logical
        }
        $ProcessRows.Add([pscustomobject]@{
            utc = $NowText
            pid = $Proc.Id
            cpu_cores = $CpuCores
            cpu_machine_pct = $CpuMachine
            working_set_mb = $Proc.WorkingSet64 / 1MB
            private_mb = $Proc.PrivateMemorySize64 / 1MB
            threads = $Proc.Threads.Count
            handles = $Proc.HandleCount
        })

        foreach ($Th in @($Proc.Threads)) {
            try {
                $Tid = [int]$Th.Id
                $TotalMs = $Th.TotalProcessorTime.TotalMilliseconds
                $Key = "$($Proc.Id):$Tid"
                $DeltaMs = 0.0
                if ($ThreadPrev.ContainsKey($Key)) {
                    $DeltaMs = $TotalMs - [double]$ThreadPrev[$Key]
                    if ($DeltaMs -lt 0) { $DeltaMs = 0 }
                }
                $ThreadPrev[$Key] = $TotalMs
                $CorePct = $null
                if ($IntervalMs -and $IntervalMs -gt 0) { $CorePct = 100.0 * $DeltaMs / $IntervalMs }
                $State = ""
                try { $State = [string]$Th.ThreadState } catch {}
                $ThreadRows.Add([pscustomobject]@{
                    utc = $NowText
                    pid = $Proc.Id
                    tid = $Tid
                    cpu_ms = $DeltaMs
                    interval_ms = $IntervalMs
                    cpu_core_pct = $CorePct
                    state = $State
                })
            } catch {}
        }

        $PrevProcCpuMs = $CpuMs
        $PrevUtc = $Now
    } catch {
        Add-MonitorError ("process/thread sample: " + $_.Exception.Message)
    }

    try {
        $Counter = Get-Counter -Counter "\GPU Engine(*)\Utilization Percentage" -ErrorAction Stop
        foreach ($Sample in @($Counter.CounterSamples)) {
            $Name = [string]$Sample.InstanceName
            if ($Name -match ("^pid_" + [regex]::Escape([string]$Proc.Id) + "_")) {
                $GpuRows.Add([pscustomobject]@{
                    utc = $NowText
                    pid = $Proc.Id
                    engine = $Name
                    utilization_pct = [double]$Sample.CookedValue
                })
            }
        }
    } catch {
        if (-not (Test-Path -LiteralPath $ErrorFile) -or (Get-Item -LiteralPath $ErrorFile).Length -lt 32768) {
            Add-MonitorError ("GPU counters unavailable: " + $_.Exception.Message)
        }
    }

    if (Test-Path -LiteralPath $StopFile) {
        # Capture one final sample if the process is still alive, then stop.
        break
    }
    Start-Sleep -Milliseconds $SampleMs
}

try {
    if ($ProcessRows.Count) { $ProcessRows | Export-Csv -LiteralPath $ProcessCsv -NoTypeInformation -Encoding UTF8 }
    else { '"utc","pid","cpu_cores","cpu_machine_pct","working_set_mb","private_mb","threads","handles"' | Set-Content -LiteralPath $ProcessCsv -Encoding UTF8 }
    if ($ThreadRows.Count) { $ThreadRows | Export-Csv -LiteralPath $ThreadsCsv -NoTypeInformation -Encoding UTF8 }
    else { '"utc","pid","tid","cpu_ms","interval_ms","cpu_core_pct","state"' | Set-Content -LiteralPath $ThreadsCsv -Encoding UTF8 }
    if ($GpuRows.Count) { $GpuRows | Export-Csv -LiteralPath $GpuCsv -NoTypeInformation -Encoding UTF8 }
    else { '"utc","pid","engine","utilization_pct"' | Set-Content -LiteralPath $GpuCsv -Encoding UTF8 }
} catch {
    Add-MonitorError ("CSV export: " + $_.Exception.Message)
}
