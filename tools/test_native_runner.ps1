# Test runner control flow without game files or starting the game executable.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = Join-Path $PSScriptRoot 'scarlet_native_run.ps1'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('Scarlet Runner Test ' + [guid]::NewGuid())
$script:capture = $null
$names = @('SCARLET_RUNTIME','SCARLET_NATIVE_PROFILE','LARECOMP_NO_LAUNCHER',
    'LARECOMP_HLE_PROF','LARECOMP_EXEFS_DIR','LARECOMP_ROMFS_DIR','LARECOMP_ROMFS_IMAGE',
    'LARECOMP_PACKAGE_STREAM','LARECOMP_PROF_SVC','LARECOMP_VK_PROF',
    'LARECOMP_COMMAND_COUNTS','LARECOMP_FRAME_CSV')
$before = @{}
foreach ($name in $names) { $before[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }

function Assert-Runner([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# The script under test resolves this function instead of the process cmdlet.
function Start-Process {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$WorkingDirectory,
        [switch]$PassThru, [switch]$Wait)
    $script:capture = @{
        File = $FilePath
        Arguments = @($ArgumentList)
        Directory = $WorkingDirectory
        Runtime = $env:SCARLET_RUNTIME
        Profile = $env:SCARLET_NATIVE_PROFILE
        Hle = $env:LARECOMP_HLE_PROF
        FrameCsv = $env:LARECOMP_FRAME_CSV
    }
    return [pscustomobject]@{ ExitCode = 0 }
}

try {
    New-Item -ItemType Directory -Path (Join-Path $temp 'tools') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $temp 'build\native') -Force | Out-Null
    $game = Join-Path $temp 'Pokemon Scarlet'
    New-Item -ItemType Directory -Path $game -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $temp 'build\native\ScarletPC.exe') | Out-Null
    $runner = Join-Path $temp 'tools\scarlet_native_run.ps1'
    Copy-Item -LiteralPath $source -Destination $runner

    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, 'test-before', 'Process') }
    & $runner -Runtime native -GamePath $game -Profile -NoLauncher
    Assert-Runner ($null -ne $script:capture) 'Runner did not reach process creation.'
    Assert-Runner ($script:capture.Runtime -eq 'native') 'Wrong runtime selected.'
    Assert-Runner ($script:capture.Profile -eq '1' -and $script:capture.Hle -eq '1') 'Profiling was not enabled in child environment.'
    Assert-Runner ($script:capture.Arguments.Count -eq 2) 'Unexpected process arguments.'
    Assert-Runner ($script:capture.Arguments[0] -eq '--play') 'NoLauncher was ignored.'
    Assert-Runner ($script:capture.Arguments[1] -eq ('"' + $game + '"')) 'Path with spaces was not quoted.'
    Assert-Runner ($script:capture.FrameCsv -eq (Join-Path $temp 'build\native\profile\frames.csv')) 'Incorrect profile output.'
    foreach ($name in $names) {
        Assert-Runner ([Environment]::GetEnvironmentVariable($name, 'Process') -eq 'test-before') "Environment leaked: $name"
    }

    # Explicit DLC support must be a declared array parameter, even with none supplied.
    $parameters = (Get-Command $runner).Parameters
    Assert-Runner ($parameters.ContainsKey('DlcPath')) 'DlcPath is referenced but not declared.'
    Assert-Runner ($parameters['DlcPath'].ParameterType -eq [string[]]) 'DlcPath must accept multiple files.'

    # With no previous value, finally must remove the temporary profiler variables.
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    & $runner -Runtime native -GameRoot $game -Profile -NoLauncher
    foreach ($name in $names) {
        Assert-Runner ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'Process'))) "Environment not removed: $name"
    }
    Write-Host 'Native runner: default DLC, spaced path, profiler propagation and restoration passed.'
}
finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
