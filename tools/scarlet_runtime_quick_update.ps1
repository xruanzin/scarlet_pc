param(
    [ValidateRange(1, 4)]
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $PSScriptRoot 'scarlet_runtime_build.ps1'
$stageDir = Join-Path $repoRoot 'build\scarlet-runtime\scarlet-stage'
$packageDir = Join-Path $repoRoot 'dist\ScarletPC-Independent'

if (-not (Test-Path $buildScript -PathType Leaf)) {
    throw "Missing runtime build script: $buildScript"
}
if (-not (Test-Path $packageDir -PathType Container)) {
    throw 'Independent package is missing. Run scarlet_independent_setup.ps1 once first.'
}

Write-Host '=== Scarlet PC quick runtime update ===' -ForegroundColor Magenta
Write-Host '[Scarlet PC] Reusing runtime source, CMake cache, AOT modules, launcher and optional graphics assets.' -ForegroundColor Green

& $buildScript -Jobs $Jobs -FastIncremental
if ($LASTEXITCODE -ne 0) {
    throw 'Fast incremental runtime build failed.'
}

$runtimeExe = Join-Path $stageDir 'ScarletPC.exe'
if (-not (Test-Path $runtimeExe -PathType Leaf)) {
    throw "Updated ScarletPC.exe was not staged: $runtimeExe"
}

$packageExe = Join-Path $packageDir 'ScarletPC.exe'

# A running Windows image cannot be overwritten. Find Scarlet specifically by the
# exact package path, because the process name can differ and an elevated instance
# may not be returned cleanly by Get-Process -Name alone.
$packageExeFull = [IO.Path]::GetFullPath($packageExe)
$lockingProcesses = @(
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and
        [string]::Equals(
            [IO.Path]::GetFullPath([string]$_.ExecutablePath),
            $packageExeFull,
            [StringComparison]::OrdinalIgnoreCase)
    }
)

foreach ($process in $lockingProcesses) {
    Write-Host ("[Scarlet PC] Closing running package process PID {0} ({1}) before update..." -f
                $process.ProcessId, $process.Name) -ForegroundColor Yellow
    try {
        Stop-Process -Id ([int]$process.ProcessId) -Force -ErrorAction Stop
        Wait-Process -Id ([int]$process.ProcessId) -Timeout 5 -ErrorAction SilentlyContinue
    } catch {
        # taskkill /T also closes child processes that can keep the package alive.
        & taskkill.exe /PID ([string]$process.ProcessId) /T /F 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw ("ScarletPC.exe is still in use by PID {0}. Close that process or rerun " +
                   "PowerShell as Administrator, then run this quick updater again.") -f
                  $process.ProcessId
        }
    }
}

# Antivirus/driver hooks can release the image handle a moment after process exit.
$copySucceeded = $false
for ($attempt = 1; $attempt -le 10; ++$attempt) {
    try {
        Copy-Item $runtimeExe $packageExe -Force -ErrorAction Stop
        $copySucceeded = $true
        break
    } catch [System.IO.IOException] {
        if ($attempt -eq 10) {
            throw
        }
        Start-Sleep -Milliseconds 300
    }
}
if (-not $copySucceeded) {
    throw "Could not update package executable: $packageExe"
}

Get-ChildItem $stageDir -File -Filter '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
    $target = Join-Path $packageDir $_.Name
    if (-not (Test-Path $target -PathType Leaf) -or
        (Get-Item $_.FullName).LastWriteTimeUtc -gt (Get-Item $target).LastWriteTimeUtc) {
        Copy-Item $_.FullName $target -Force
    }
}

$manifest = Join-Path $stageDir 'scarlet_runtime_manifest.json'
if (Test-Path $manifest -PathType Leaf) {
    Copy-Item $manifest (Join-Path $packageDir 'scarlet_runtime_manifest.json') -Force
}

Write-Host ''
Write-Host '[Scarlet PC] Quick runtime update complete.' -ForegroundColor Green
Write-Host "[Scarlet PC] Updated executable: $packageDir\ScarletPC.exe" -ForegroundColor Green
