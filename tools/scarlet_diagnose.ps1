param(
    [string]$GameRoot = 'D:\Pokemon Scarlet\Extracted\Base',
    [ValidatePattern('^\d+:\d+$')][string]$FunctionFrames = '600:660',
    [switch]$NoFunctionCounts
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $Repo 'build\ScarletRecomp.exe'
if (Get-Process -Name ScarletRecomp -ErrorAction SilentlyContinue) { throw 'Feche o Scarlet antes de iniciar o diagnostico.' }
$Window = $FunctionFrames.Split(':')
if ([long]$Window[1] -le [long]$Window[0]) { throw 'FunctionFrames exige inicio:fim, com fim maior que inicio.' }
$Out = Join-Path $Repo ('build\scarlet-performance-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $Out -Force
$StopFile = Join-Path $Out 'monitor.stop'
$StartUtc = [DateTime]::UtcNow
$Saved = @{}
$Vars = @('LARECOMP_FRAME_CSV','LARECOMP_FNPROF','LARECOMP_FNPROF_OUT','LARECOMP_FNPROF_BT','LARECOMP_FNPROF_BT_MAX','LARECOMP_GPUPROF_AT')
foreach ($Name in $Vars) { $Saved[$Name] = [Environment]::GetEnvironmentVariable($Name,'Process') }
$Code = 1
$Monitor = $null
try {
    $env:LARECOMP_FRAME_CSV = Join-Path $Out 'frames.csv'
    $env:LARECOMP_FNPROF_OUT = Join-Path $Out 'fnprof.txt'
    if ($NoFunctionCounts) { Remove-Item Env:LARECOMP_FNPROF -ErrorAction SilentlyContinue }
    else { $env:LARECOMP_FNPROF = $FunctionFrames }
    Remove-Item -LiteralPath @('Env:LARECOMP_FNPROF_BT', 'Env:LARECOMP_FNPROF_BT_MAX') -ErrorAction SilentlyContinue
    # Sparse GPU pass captures, without F9's texture/readback capture overhead.
    $env:LARECOMP_GPUPROF_AT = '300,900,1800,3600'
    $Meta = [ordered]@{
        StartedUtc=$StartUtc.ToString('o'); GitHead=(& git -C $Repo rev-parse HEAD | Out-String).Trim()
        BuiltHead=$(if (Test-Path "$Repo\build\.runtime_build_head") { (Get-Content "$Repo\build\.runtime_build_head" -Raw).Trim() } else { $null })
        FunctionFrames=$(if ($NoFunctionCounts) { $null } else { $FunctionFrames })
        LogicalProcessors=[Environment]::ProcessorCount
        Notes='Instrumented run. HLE times include waits/callbacks; function counts are not CPU time. GPU engine samples may be unavailable.'
        Environment=@(Get-ChildItem Env:LARECOMP_* | Select-Object -Property @('Name','Value'))
    }
    foreach ($Item in @(@('CPU','Win32_Processor'),@('GPU','Win32_VideoController'),@('OS','Win32_OperatingSystem'))) {
        try {
            $Meta[$Item[0]] = @(Get-CimInstance $Item[1] | Select-Object -Property @('Name','Version','DriverVersion','NumberOfCores','NumberOfLogicalProcessors','TotalVisibleMemorySize','FreePhysicalMemory'))
        } catch { $Meta[$Item[0]] = "Unavailable: $_" }
    }
    $Meta | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out 'run.json') -Encoding UTF8
    # Independent job samples while the existing runner owns launch/build/backend
    # handling. Its own overhead is outside the game's process CPU measurement.
    $Monitor = Start-Job -ArgumentList $Out,$StopFile,$Exe -ScriptBlock {
        param($Dir,$Stop,$ExpectedExe)
        $ErrorActionPreference='Stop'
        $Prev=@{}; $ThreadPrev=@{}; $GpuAvailable=$true
        while (-not (Test-Path -LiteralPath $Stop)) {
            $Utc=[DateTime]::UtcNow
            foreach ($Proc in @(Get-Process -Name ScarletRecomp -ErrorAction SilentlyContinue)) {
                try {
                    if ($Proc.Path -ne $ExpectedExe) { continue }
                    $ProcId=$Proc.Id; $Key="$ProcId/$($Proc.StartTime.Ticks)"
                    $CpuMs=$Proc.TotalProcessorTime.TotalMilliseconds
                    $Elapsed=$null; $Cores=$null
                    if ($Prev.ContainsKey($Key)) {
                        $Elapsed=($Utc-$Prev[$Key].Time).TotalSeconds
                        if ($Elapsed -gt 0) { $Cores=($CpuMs-$Prev[$Key].Cpu)/($Elapsed*1000) }
                    }
                    $Prev[$Key]=@{Time=$Utc;Cpu=$CpuMs}
                    [pscustomobject]@{utc=$Utc.ToString('o');pid=$ProcId;cpu_cores=$Cores;cpu_machine_pct=$(if($null -ne $Cores){100*$Cores/[Environment]::ProcessorCount}else{$null});working_set_mb=$Proc.WorkingSet64 / 1048576.0;private_mb=$Proc.PrivateMemorySize64 / 1048576.0;threads=$Proc.Threads.Count;handles=$Proc.HandleCount} |
                        Export-Csv (Join-Path $Dir 'process.csv') -Append -NoTypeInformation -Encoding UTF8
                    foreach ($Thread in $Proc.Threads) {
                        try {
                            $Tk="$Key/$($Thread.Id)"; $Tc=$Thread.TotalProcessorTime.TotalMilliseconds
                            if ($ThreadPrev.ContainsKey($Tk) -and $Elapsed -gt 0) {
                                $Delta=$Tc-$ThreadPrev[$Tk]
                                if ($Delta -ge 0) {
                                    [pscustomobject]@{utc=$Utc.ToString('o');pid=$ProcId;tid=$Thread.Id;cpu_ms=$Delta;interval_ms=$Elapsed*1000;cpu_core_pct=100*$Delta/($Elapsed*1000);state=[string]$Thread.ThreadState} |
                                        Export-Csv (Join-Path $Dir 'threads.csv') -Append -NoTypeInformation -Encoding UTF8
                                }
                            }
                            $ThreadPrev[$Tk]=$Tc
                        } catch { } # A thread can exit while sampled.
                    }
                    if ($GpuAvailable) {
                        try {
                            Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -Filter "Name LIKE 'pid_${ProcId}_%'" -ErrorAction Stop |
                                ForEach-Object { [pscustomobject]@{utc=$Utc.ToString('o');pid=$ProcId;engine=$_.Name;utilization_pct=$_.UtilizationPercentage} } |
                                Export-Csv (Join-Path $Dir 'gpu-engines.csv') -Append -NoTypeInformation -Encoding UTF8
                        } catch {
                            "GPU engine counters unavailable: $_" | Add-Content (Join-Path $Dir 'monitor-errors.txt')
                            $GpuAvailable=$false
                        }
                    }
                } catch { "Sample unavailable: $_" | Add-Content (Join-Path $Dir 'monitor-errors.txt') }
            }
            Start-Sleep -Milliseconds 1000
        }
    }
    Write-Host "[diagnose] Jogue normalmente por 2-3 minutos e feche o jogo. Coleta: $Out" -ForegroundColor Cyan
    Write-Host '[diagnose] A instrumentacao adiciona custo. Nao use esta rodada como benchmark sem profiler.'
    # Child shell isolates the runner's exit statement; no live Tee in the hot path.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'scarlet_run.ps1') -GameRoot $GameRoot -Profile
    $Code=$LASTEXITCODE
} catch {
    $_ | Out-String | Set-Content (Join-Path $Out 'launcher-error.txt') -Encoding UTF8
    Write-Warning $_
} finally {
    Set-Content -LiteralPath $StopFile -Value 'stop'
    if ($Monitor) {
        $null=Wait-Job $Monitor -Timeout 10
        if ($Monitor.State -eq 'Running') { Stop-Job $Monitor }
        Receive-Job $Monitor -ErrorAction Continue | Out-File (Join-Path $Out 'monitor-job.txt') -Encoding UTF8
        Remove-Job $Monitor
    }
    foreach ($Name in $Vars) { [Environment]::SetEnvironmentVariable($Name,$Saved[$Name],'Process') }
    foreach ($File in @('scarlet_boot.log','scarlet_boot.vulkan.log','scarlet_boot.directx.log','scarlet_build.log')) {
        $Src=Join-Path $Repo $File
        if ((Test-Path $Src) -and (Get-Item $Src).LastWriteTimeUtc -ge $StartUtc) { Copy-Item $Src $Out }
    }
    $Final=[ordered]@{EndedUtc=[DateTime]::UtcNow.ToString('o');ExitCode=$Code}
    foreach ($File in @('ScarletRecomp.exe','lasgame.dll')) {
        $Src=Join-Path $Repo "build\$File"
        if(Test-Path $Src){ $Final[$File]=(Get-FileHash $Src -Algorithm SHA256).Hash }
    }
    if(Test-Path "$Repo\build\.runtime_build_head"){ $Final['BuiltHead']=(Get-Content "$Repo\build\.runtime_build_head" -Raw).Trim() }
    $Final | ConvertTo-Json | Set-Content (Join-Path $Out 'result.json') -Encoding UTF8
    $Python=Join-Path $Repo '.venv\Scripts\python.exe'
    if(-not(Test-Path $Python)){ $Python='C:\msys64\ucrt64\bin\python.exe' }
    if(Test-Path $Python){
        & $Python (Join-Path $PSScriptRoot 'scarlet_performance_report.py') $Out
        if($LASTEXITCODE -ne 0){ Write-Warning 'Resumo falhou; os dados brutos continuam na pasta.' }
    } else {
        'Python nao encontrado. Dados brutos preservados. Gere o resumo com tools/scarlet_performance_report.py <pasta>.' | Set-Content (Join-Path $Out 'report.txt')
    }
    $Zip="$Out.zip"
    Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $Zip -CompressionLevel Fastest
    Write-Host "[diagnose] Envie este arquivo: $Zip" -ForegroundColor Green
}
exit $Code
