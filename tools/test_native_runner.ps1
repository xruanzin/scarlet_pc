# Test runner control flow without game files or starting the game executable.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = Join-Path $PSScriptRoot 'scarlet_native_run.ps1'
# Regression: update ExeFS identification must not require a base NCA.
# Use --basenca for the final BKTR RomFS reconstruction only.
$preparer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scarlet_prepare_update.ps1') -Raw
$scanStart = $preparer.IndexOf('function Find-ProgramNca(')
$scanEnd = $preparer.IndexOf([Environment]::NewLine + 'try {', $scanStart)
if ($scanEnd -lt 0) { $scanEnd = $preparer.IndexOf([char]10 + 'try {', $scanStart) }
if ($scanStart -lt 0 -or $scanEnd -lt 0) { throw 'Native NCA scanner not found.' }
$scanCode = $preparer.Substring($scanStart, $scanEnd - $scanStart)
if ($scanCode.Contains('$hactoolArgs += "--basenca=$baseNca"')) {
    throw 'Do not use --basenca during Program NCA identification.'
}
if (-not $preparer.Contains('"--basenca=$baseNca","--exefsdir=$exeFs"')) {
    throw 'Expected --basenca during final BKTR RomFS assembly.'
}
$temp = Join-Path ([IO.Path]::GetTempPath()) ('Scarlet Runner Test ' + [guid]::NewGuid())
$capture = @{ Process = $null; ExitCode = 0 }
$names = @('SCARLET_RUNTIME','SCARLET_UPSCALER','SCARLET_GRAPHICS_REFERENCE','SCARLET_NATIVE_PROFILE','LARECOMP_NO_LAUNCHER',
    'LARECOMP_DATA_ROOT','SCARLET_AOT_DIR',
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
        [switch]$PassThru, [switch]$Wait,
        [string]$RedirectStandardError, [string]$RedirectStandardOutput)
    $capture.Process = @{
        File = $FilePath
        Arguments = @($ArgumentList)
        Directory = $WorkingDirectory
        Runtime = $env:SCARLET_RUNTIME
        DataRoot = $env:LARECOMP_DATA_ROOT
        AotDir = $env:SCARLET_AOT_DIR
        Upscaler = $env:SCARLET_UPSCALER
        GraphicsReference = $env:SCARLET_GRAPHICS_REFERENCE
        Profile = $env:SCARLET_NATIVE_PROFILE
        Hle = $env:LARECOMP_HLE_PROF
        FrameCsv = $env:LARECOMP_FRAME_CSV
        ExeFs = $env:LARECOMP_EXEFS_DIR
        RomFsImage = $env:LARECOMP_ROMFS_IMAGE
        RomFsDir = $env:LARECOMP_ROMFS_DIR
        PackageStream = $env:LARECOMP_PACKAGE_STREAM
        Stderr = $RedirectStandardError
        Stdout = $RedirectStandardOutput
    }
    if ($RedirectStandardError) { Set-Content -LiteralPath $RedirectStandardError -Value 'FATAL: synthetic runtime failure' }
    if ($RedirectStandardOutput) { Set-Content -LiteralPath $RedirectStandardOutput -Value 'synthetic runtime stdout' }
    return [pscustomobject]@{ ExitCode = $capture.ExitCode }
}

try {
    New-Item -ItemType Directory -Path (Join-Path $temp 'tools') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $temp 'build\native') -Force | Out-Null
    $game = Join-Path $temp 'Pokemon Scarlet'
    New-Item -ItemType Directory -Path $game -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $temp 'build\native\ScarletPC.exe') | Out-Null
    $runner = Join-Path $temp 'tools\scarlet_native_run.ps1'
    Copy-Item -LiteralPath $source -Destination $runner
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'scarlet_native_paths.ps1') -Destination (Join-Path $temp 'tools\scarlet_native_paths.ps1')
    $mockBuild = Join-Path $temp 'build\native'

    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, 'test-before', 'Process') }
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -Profile -NoLauncher
    Assert-Runner ($null -ne $capture.Process) 'Runner did not reach process creation.'
    Assert-Runner ($capture.Process.Runtime -eq 'native') 'Wrong runtime selected.'
    Assert-Runner ($capture.Process.Directory -eq $mockBuild) 'Native runtime must start from the build directory, not the C checkout.'
    Assert-Runner ($capture.Process.Profile -eq '1' -and $capture.Process.Hle -eq '1') 'Profiling was not enabled in child environment.'
    Assert-Runner ($capture.Process.Arguments.Count -eq 2) 'Unexpected process arguments.'
    Assert-Runner ($capture.Process.Arguments[0] -eq '--play') 'NoLauncher was ignored.'
    Assert-Runner ($capture.Process.Arguments[1] -eq ('"' + $game + '"')) 'Path with spaces was not quoted.'
    Assert-Runner ($capture.Process.FrameCsv -eq (Join-Path $temp 'build\native\profile\frames.csv')) 'Incorrect profile output.'
    foreach ($name in $names) {
        Assert-Runner ([Environment]::GetEnvironmentVariable($name, 'Process') -eq 'test-before') "Environment leaked: $name"
    }

    # A/B image-quality testing must change the native child configuration
    # without leaking the override into the user's next launch.
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -Upscaler fsr31 -NoLauncher
    Assert-Runner ($capture.Process.Upscaler -eq 'fsr31') 'FSR 3.1 mode not propagated.'
    Assert-Runner ([Environment]::GetEnvironmentVariable('SCARLET_UPSCALER', 'Process') -eq 'test-before') 'FSR upscaler leaked to parent.'
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -Upscaler spatial -NoLauncher
    Assert-Runner ($capture.Process.Upscaler -eq 'spatial') 'Native spatial mode not propagated.'
    Assert-Runner ([Environment]::GetEnvironmentVariable('SCARLET_UPSCALER', 'Process') -eq 'test-before') 'Spatial mode leaked to parent.'
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -Upscaler current -NoLauncher
    Assert-Runner ($capture.Process.Upscaler -eq 'test-before') 'Current mode must honor the user''s existing setting.'

    # A reference render must be isolated from the usual run and not change
    # user environment on exit (including when other graphics flags are set).
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -GraphicsReference -Upscaler fsr31 -NoLauncher
    Assert-Runner ($capture.Process.GraphicsReference -eq '1') 'Guest reference presentation option not propagated.'
    Assert-Runner ($capture.Process.Upscaler -eq 'fsr31') 'Reference mode must not mutate the user''s upscaler selection.'
    Assert-Runner ([Environment]::GetEnvironmentVariable('SCARLET_GRAPHICS_REFERENCE', 'Process') -eq 'test-before') 'Graphics reference mode leaked to parent.'
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -NoLauncher
    Assert-Runner ($capture.Process.GraphicsReference -eq 'test-before') 'Default run must honor previous reference-mode state.'

    # Explicit DLC support must be a declared array parameter, even with none supplied.
    $parameters = (Get-Command $runner).Parameters
    Assert-Runner ($parameters.ContainsKey('DlcPath')) 'DlcPath is referenced but not declared.'
    Assert-Runner ($parameters['DlcPath'].ParameterType -eq [string[]]) 'DlcPath must accept multiple files.'

    # With no previous value, finally must remove the temporary profiler variables.
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    & $runner -BuildRoot $mockBuild -Runtime native -GameRoot $game -Profile -NoLauncher
    Assert-Runner ($capture.Process.DataRoot -eq $mockBuild) 'Native data must default to build volume, not C.'
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
    $fixturePrepare = @'
param([string]$BasePath,[string]$UpdatePath,[string]$KeysPath,[string]$TitleKeysPath,[string]$HactoolPath,[string]$NstoolPath,[string]$AotManifestPath,[string]$OutputRoot)
$root = Split-Path $PSScriptRoot -Parent
if ($OutputRoot) { Set-Content -LiteralPath (Join-Path $root 'received-cache-root.txt') -Value $OutputRoot }
if ($NstoolPath) { Set-Content -LiteralPath (Join-Path $root 'received-nstool-path.txt') -Value $NstoolPath }
$directoryMode = Test-Path -LiteralPath (Join-Path $root 'directory-mode.txt')
$image = if ($directoryMode) { '' } else { Join-Path $root 'patched\romfs.bin' }
$directory = if ($directoryMode) { Join-Path $root 'patched\romfs' } else { '' }
[PSCustomObject]@{
    ExeFs = (Join-Path $root 'patched\exefs')
    RomFsImage = $image
    RomFsDir = $directory
    CacheRoot = 'synthetic-update-cache'
} | ConvertTo-Json -Compress
'@
    $fixturePrepare | Set-Content -LiteralPath $fakePrepare -Encoding UTF8

    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $baseNsp -UpdatePath $updateNsp -NoLauncher
    Assert-Runner ($capture.Process.Runtime -eq 'native') 'NSP update was redirected to legacy.'
    Assert-Runner ($capture.Process.ExeFs -eq $patchedExeFs) 'Patched ExeFS not forwarded.'
    Assert-Runner ($capture.Process.RomFsImage -eq $patchedRomFs) 'Patched RomFS not forwarded.'
    Assert-Runner ([string]::IsNullOrEmpty($capture.Process.PackageStream)) 'Cached update should permit memory mapping.'
    foreach ($name in $names) {
        Assert-Runner ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'Process'))) "Environment leaked after update: $name"
    }
    # Redirected cache must forward the selected path without changing
    # runtime selection or leaking environment variables.
    $alternateCache = Join-Path $temp 'other drive\scarlet-cache'
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $baseNsp -UpdatePath $updateNsp -UpdateCacheRoot $alternateCache -NoLauncher
    $received = Get-Content -LiteralPath (Join-Path $temp 'received-cache-root.txt') -Raw
    Assert-Runner ($received.Trim() -eq $alternateCache) 'Relocated NSP update cache root was not forwarded.'
    Assert-Runner ($capture.Process.Runtime -eq 'native') 'Relocated NSP update switched runtime.'
    foreach ($name in $names) {
        Assert-Runner ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'Process'))) "Environment leaked after relocated update: $name"
    }

    # NSTool may extract a patched RomFS directory instead of a raw image.
    # The native runtime must receive the directory, leaving image unset so
    # the existing native RomFS builder can generate its own cache on D:.
    $extractedDir = Join-Path $temp 'patched\romfs'
    New-Item -ItemType Directory -Path $extractedDir -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $temp 'directory-mode.txt') -Force | Out-Null
    $nstoolFixture = Join-Path $temp 'nstool.exe'
    & $runner -BuildRoot $mockBuild -Runtime native -GamePath $baseNsp -UpdatePath $updateNsp -NstoolPath $nstoolFixture -NoLauncher
    Assert-Runner ($capture.Process.Runtime -eq 'native') 'NSTool directory mode switched to legacy runtime.'
    Assert-Runner ($capture.Process.RomFsDir -eq $extractedDir) 'NSTool patched RomFS directory not forwarded.'
    Assert-Runner ([string]::IsNullOrEmpty($capture.Process.RomFsImage)) 'NSTool directory mode incorrectly forced a raw RomFS image.'
    $receivedNstool = Get-Content -LiteralPath (Join-Path $temp 'received-nstool-path.txt') -Raw
    Assert-Runner ($receivedNstool.Trim() -eq $nstoolFixture) 'NSTool executable override not passed to preparer.'
    foreach ($name in $names) {
        Assert-Runner ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'Process'))) "Environment leaked after NSTool directory mode: $name"
    }

    # The child must be redirected even with inherited console handles. Preserve
    # each run's diagnostics, report a failing exit, and still restore the env.
    $priorLog = $capture.Process.Stderr
    $capture.ExitCode = -1073740791
    $failure = ''
    try { & $runner -BuildRoot $mockBuild -Runtime native -GamePath $game -NoLauncher }
    catch { $failure = $_.Exception.Message }
    Assert-Runner ($failure.Contains('-1073740791')) 'Original crash exit code was hidden.'
    Assert-Runner ($failure.Contains($capture.Process.Stderr)) 'Crash report omitted runtime stderr path.'
    Assert-Runner ($capture.Process.Stderr -ne $capture.Process.Stdout) 'Separate streams must not target the same file.'
    Assert-Runner ((Split-Path $capture.Process.Stderr -Parent) -eq (Join-Path $mockBuild 'logs')) 'Crash log escaped the build volume.'
    Assert-Runner ($capture.Process.Stderr -ne $priorLog -and (Test-Path -LiteralPath $priorLog)) 'Another launch erased the previous log.'
    Assert-Runner ((Get-Content -LiteralPath $capture.Process.Stderr -Raw).Contains('FATAL: synthetic runtime failure')) 'Child stderr was not captured.'
    foreach ($name in $names) {
        Assert-Runner ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'Process'))) "Environment leaked after crash: $name"
    }

    Write-Host 'Native runner: DLC, spaced paths, native NSP update, profiling, crash logs and environment restoration passed.'
}
finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
