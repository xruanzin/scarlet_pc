# Static regression for the user's Windows D:-only native NSP preparation.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$preparer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scarlet_prepare_update.ps1') -Raw
$cleanup = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scarlet_cleanup_legacy_cache.ps1') -Raw
$runner = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scarlet_native_run.ps1') -Raw
function Assert-DOnly([bool]$ok,[string]$msg) { if (-not $ok) { throw $msg } }
Assert-DOnly ($preparer.Contains("D:\ScarletNativeCache")) 'D cache default missing.'
Assert-DOnly ($preparer.Contains('Repair-ScarletNspNcaCache')) 'Direct NSP recovery missing.'
Assert-DOnly (-not $preparer.Contains('robocopy.exe')) 'Legacy C-to-D robocopy reintroduced.'
Assert-DOnly (-not $preparer.Contains('legacyOutputRoot')) 'Legacy C source cache reference reintroduced.'
Assert-DOnly (-not $preparer.Contains('usingDefaultD')) 'Old migration code reintroduced.'
Assert-DOnly ($preparer.Contains('D:\ScarletNativeCache\tools\hactool')) 'hactool installer writes to source checkout.'
Assert-DOnly ($preparer.Contains('D:\ScarletNativeCache\tools\nstool')) 'NSTool installer not on D.'
Assert-DOnly ($cleanup.Contains('Get-ScarletSha256Prefix')) 'Cleanup no longer verifies NCA SHA256.'
Assert-DOnly ($cleanup.Contains("'.work'") -and $cleanup.Contains("'.hactool.log'")) 'Cleanup paths broadened unexpectedly.'
Assert-DOnly ($cleanup.Contains('if (-not $Apply)')) 'Destructive cleanup missing explicit -Apply.'
Assert-DOnly (-not $cleanup.Contains('git clean')) 'Cleanup must not use Git destructive commands.'
Assert-DOnly ($runner.Contains('UpdateCacheRoot')) 'D cache override missing in runner.'
Write-Host 'D-only native cache test passed: no C migration, tool downloads on D, guarded legacy cleanup.'
