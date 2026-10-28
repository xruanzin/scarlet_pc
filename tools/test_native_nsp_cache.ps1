# Synthetic NSP/PFS0 test: no keys or game contents required.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scarlet_nsp_cache.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('ScarletNspCacheTest-' + [guid]::NewGuid().ToString('N'))
function Assert-Nsp([bool]$ok,[string]$message) {
 if (-not $ok) { throw $message }
}
try {
 $cache = Join-Path $root 'cache'
 New-Item -ItemType Directory -Path $cache -Force | Out-Null
 $name1 = ('a' * 32) + '.nca'
 $name2 = ('b' * 32) + '.nca'
 $nca1 = [byte[]](0..127)
 $nca2 = New-Object byte[] 80
 for ($i=0;$i -lt $nca2.Length;$i++) { $nca2[$i] = [byte]($i+70) }
 $nameBytes = [Text.Encoding]::ASCII.GetBytes($name1 + [char]0 + $name2 + [char]0)
 $nsp = Join-Path $root 'synthetic.nsp'
 $output = [IO.File]::Open($nsp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
 $writer = [IO.BinaryWriter]::new($output,[Text.Encoding]::ASCII,$true)
 try {
  $writer.Write([Text.Encoding]::ASCII.GetBytes('PFS0'))
  $writer.Write([uint32]2)
  $writer.Write([uint32]$nameBytes.Length)
  $writer.Write([uint32]0)
  $writer.Write([uint64]0)
  $writer.Write([uint64]$nca1.Length)
  $writer.Write([uint32]0)
  $writer.Write([uint32]0)
  $writer.Write([uint64]$nca1.Length)
  $writer.Write([uint64]$nca2.Length)
  $writer.Write([uint32]($name1.Length + 1))
  $writer.Write([uint32]0)
  $writer.Write($nameBytes)
  $writer.Write($nca1)
  $writer.Write($nca2)
  $writer.Flush()
 } finally { $writer.Dispose(); $output.Dispose() }
 $entries = @(Get-ScarletNspNcaEntries -PackagePath $nsp)
 Assert-Nsp ($entries.Count -eq 2) 'Could not read both PFS0 entries.'
 Assert-Nsp ($entries[0].Length -eq 128 -and $entries[1].Length -eq 80) 'PFS0 sizes were parsed incorrectly.'
 [IO.File]::WriteAllBytes((Join-Path $cache $name1),$nca1[0..31])
 [IO.File]::WriteAllBytes((Join-Path $cache $name2),$nca2)
 Repair-ScarletNspNcaCache -PackagePath $nsp -CacheDirectory $cache -Label 'synthetic'
 Assert-Nsp ((Get-Item -LiteralPath (Join-Path $cache $name1)).Length -eq 128) 'Truncated NCA not recovered.'
 Assert-Nsp (([BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $cache $name1)))) -eq ([BitConverter]::ToString($nca1))) 'Recovered NCA bytes differ from source.'
 Assert-Nsp (([BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $cache $name2)))) -eq ([BitConverter]::ToString($nca2))) 'Valid cached NCA was changed.'
 Assert-Nsp (-not (Test-Path -LiteralPath (Join-Path $cache ($name1 + '.scarlet-repair')))) 'Recovery staging file was left behind.'
 # Re-running must leave the valid cache intact; no source extraction.
 Repair-ScarletNspNcaCache -PackagePath $nsp -CacheDirectory $cache -Label 'synthetic'
 $broken = Join-Path $root 'broken.nsp'
 $bytes = [IO.File]::ReadAllBytes($nsp)
 [IO.File]::WriteAllBytes($broken,$bytes[0..($bytes.Length-4)])
 $failed = $false
 try { $null = @(Get-ScarletNspNcaEntries -PackagePath $broken) } catch { $failed=$true }
 Assert-Nsp $failed 'Truncated source NSP was not rejected.'
 Write-Host 'NSP cache test passed: PFS0 index, targeted NCA recovery, intact cache preservation, truncated source rejection.'
} finally {
 Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
