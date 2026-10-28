# Test runner control flow without game files or starting the game executable.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = Join-Path $PSScriptRoot 'scarlet_native_run.ps1'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('Scarlet Runner Test ' + [guid]::NewGuid())
$capture = @{ Process = $null }
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
    $capture.Process = @{
        File = $FilePath
        Arguments = @($ArgumentList)
        Directory = $WorkingDirectory
        Runtime = $env:SCARLET_RUNTIME
        Profile = $env:SCARLET_NATIVE_PROFILE
        Hle = $env:LARECOMP_HLE_PROF
        FrameCsv = $env:LARECOMP_FRAME_CSV
        ExeFs = $env:LARECOMP_EXEFS_DIR
        RomFsImage = $env:LARECOMP_ROMFS_IMAGE
        PackageStream = $env:LARECOMP_PACKAGE_STREAM
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
    Assert-Runner ($null -ne $capture.Process) 'Runner did not reach process creation.'
    Assert-Runner ($capture.Process.Runtime -eq 'native') 'Wrong runtime selected.'
    Assert-Runner ($capture.Process.Profile -eq '1' -and $capture.Process.Hle -eq '1') 'Profiling was not enabled in child environment.'
    Assert-Runner ($capture.Process.Arguments.Count -eq 2) 'Unexpected process arguments.'
    Assert-Runner ($capture.Process.Arguments[0] -eq '--play') 'NoLauncher was ignored.'
    Assert-Runner ($capture.Process.Arguments[1] -eq ('"' + $game + '"')) 'Path with spaces was not quoted.'
    Assert-Runner ($capture.Process.FrameCsv -eq (Join-Path $temp 'build\native\profile\frames.csv')) 'Incorrect profile output.'
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
    # A base NSP + update NSP must stay in native mode. Mock only the preparation
    # helper and process creation: no game assets, keys, hactool or suyu required.
    $baseNsp = Join-Path $temp 'Scarlet base.nsp'
    $updateNsp = Join-Path $temp 'Scarlet update.nsp'
    New-Item -ItemType File -Path $baseNsp, $updateNsp | Out-Null
    $patchedExeFs = Join-Path $temp 'patched\exefs'
    $patchedRomFs = Join-Path $temp 'patched\romfs.bin'
    New-Item -ItemType Directory -Force -Path $patchedExeFs | Out-Null
    New-Item -ItemType File -Force -Path $patchedRomFs | Out-Null
    $fakePrepare = Join-Path $temp 'tools\scarlet_prepare_update.ps1'
    @'
param([string]$BasePath,[string]$UpdatePath,[string]$KeysPath,[string]$HactoolPath,[string]$AotManifestPath)
[PSCustomObject]@{
    ExeFs = (Join-Path (Split-Path $PSScriptRoot -Parent) 'patched\exefs')
    RomFsImage = (Join-Path (Split-Path $PSScriptRoot -Parent) 'patched\romfs.bin')
    RomFsDir = ''
    CacheRoot = 'synthetic-update-cache'
} | ConvertTo-Json -Compress
'@ | Set-Content -LiteralPath $fakePrepare -Encoding UTF8

    & $runner -Runtime native -GamePath $baseNsp -UpdatePath $updateNsp -NoLauncher
    Assert-Runner ($capture.Process.Runtime -eq 'native') 'NSP update was redirected to legacy.'
    Assert-Runner ($capture.Process.ExeFs -eq $patchedExeFs) 'Patched ExeFS not forwarded.'
    Assert-Runner ($capture.Process.RomFsImage -eq $patchedRomFs) 'Patched RomFS not forwarded.'
    Assert-Runner ([string]::IsNullOrEmpty($capture.Process.PackageStream)) 'Cached update should permit memory mapping.'
    foreach ($name in $names) {
        Assert-Runner ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'Process'))) "Environment leaked after update: $name"
    }

    Write-Host 'Native runner: default DLC, spaced path, native NSP update, profiler propagation and restoration passed.'
}
finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
