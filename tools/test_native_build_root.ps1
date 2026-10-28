# D:-only by default on Windows. Uses a synthetic explicit build root in CI,
# never downloads dependencies, builds the game or touches AOT DLLs.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scarlet_native_paths.ps1')
function Assert-Path([bool]$ok,[string]$msg) { if (-not $ok) { throw $msg } }
$root = Join-Path ([IO.Path]::GetTempPath()) ('ScarletNativeBuildPath-'+[guid]::NewGuid().ToString('N'))
$old = $env:SCARLET_NATIVE_BUILD_ROOT
try {
 $explicit = Join-Path $root 'native build'
 $resolved = Resolve-ScarletNativeBuildRoot -BuildRoot $explicit
 Assert-Path ($resolved -eq [IO.Path]::GetFullPath($explicit)) 'Explicit native build directory changed.'
 $env:SCARLET_NATIVE_BUILD_ROOT = $explicit
 Assert-Path ((Resolve-ScarletNativeBuildRoot -BuildRoot '') -eq $resolved) 'Build environment override ignored.'
 Remove-Item Env:SCARLET_NATIVE_BUILD_ROOT -ErrorAction SilentlyContinue
 if ($env:OS -eq 'Windows_NT') {
  if (Test-Path -LiteralPath 'D:\' -PathType Container) {
   $expected = [IO.Path]::GetFullPath('D:\ScarletNativeCache\native-build')
   Assert-Path ((Resolve-ScarletNativeBuildRoot -BuildRoot '') -eq $expected) 'Default build not on D:.'
  } else {
   $rejected = $false
   try { $null = Resolve-ScarletNativeBuildRoot -BuildRoot '' } catch { $rejected = $true }
   Assert-Path $rejected 'Windows silently fell back to C: when D: was absent.'
  }
 }
 $make = Get-Content -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'Makefile') -Raw
 $builder = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scarlet_native_build.ps1') -Raw
 $runner = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scarlet_native_run.ps1') -Raw
 Assert-Path ($make.Contains('HOST_DEBUG ?= 1') -and $make.Contains('HOST_DBGFLAGS')) 'Debug size switch missing.'
 Assert-Path ($builder.Contains('HOST_DEBUG={0}') -and $builder.Contains('Resolve-ScarletNativeBuildRoot')) 'Build root or no-debug mode missing.'
 Assert-Path ($runner.Contains('Resolve-ScarletNativeBuildRoot') -and $runner.Contains('-WorkingDirectory $buildDir')) 'Runner still uses source checkout as working directory.'
 Assert-Path ($runner.Contains('$env:LARECOMP_DATA_ROOT = $buildDir')) 'Caches may write to C:.'
 Write-Host 'Native build paths: explicit path, D-only default, no C fallback, no-debug mode and D runtime data passed.'
} finally {
 if ($null -eq $old) { Remove-Item Env:SCARLET_NATIVE_BUILD_ROOT -ErrorAction SilentlyContinue }
 else { $env:SCARLET_NATIVE_BUILD_ROOT=$old }
}
