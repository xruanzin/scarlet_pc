# Synthetic integrity test; no keys, game data or AOT files.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scarlet_nsp_cache.ps1')
. (Join-Path $PSScriptRoot 'scarlet_nca_hash.ps1')
function Assert-Integrity([bool]$ok,[string]$reason) { if (-not $ok) { throw $reason } }
$root=Join-Path ([IO.Path]::GetTempPath()) ('ScarletNcaHashTest-' + [guid]::NewGuid().ToString('N'))
try {
 New-Item -ItemType Directory -Path $root -Force | Out-Null
 $data=New-Object byte[] 32768
 for($i=0;$i -lt $data.Length;$i++){ $data[$i]=[byte](($i*13+47) % 256) }
 $sha=[Security.Cryptography.SHA256]::Create()
 try { $id=([BitConverter]::ToString($sha.ComputeHash($data),0,16)).Replace('-','').ToLowerInvariant() }
 finally { $sha.Dispose() }
 $name=$id+'.nca'
 $nsp=Join-Path $root 'synthetic.nsp'
 $nameBytes=[Text.Encoding]::ASCII.GetBytes($name+[char]0)
 $writer=[IO.BinaryWriter]::new([IO.File]::Open($nsp,[IO.FileMode]::CreateNew))
 try {
  $writer.Write([Text.Encoding]::ASCII.GetBytes('PFS0'))
  $writer.Write([uint32]1)
  $writer.Write([uint32]$nameBytes.Length)
  $writer.Write([uint32]0)
  $writer.Write([uint64]0)
  $writer.Write([uint64]$data.Length)
  $writer.Write([uint32]0)
  $writer.Write([uint32]0)
  $writer.Write($nameBytes)
  $writer.Write($data)
 } finally { $writer.Dispose() }
 $dest=Join-Path $root $name
 [IO.File]::WriteAllBytes($dest,$data)
 Assert-Integrity ((Get-ScarletSha256Prefix -Path $dest) -eq $id) 'Full-file SHA256 mismatch.'
 $entry=@(Get-ScarletNspNcaEntries -PackagePath $nsp)[0]
 Assert-Integrity ((Get-ScarletSha256Prefix -Path $nsp -Offset $entry.SourceOffset -Length $entry.Length) -eq $id) 'NSP-region hash mismatch.'
 Assert-Integrity ((Test-ScarletFailedNca -NcaPath $dest -NspPath $nsp) -eq 'valid') 'Valid NCA falsely rejected.'
 $bad=[byte[]]$data.Clone()
 $bad[8192] = $bad[8192] -bxor 0xff
 [IO.File]::WriteAllBytes($dest,$bad)
 Assert-Integrity ((Test-ScarletFailedNca -NcaPath $dest -NspPath $nsp) -eq 'cache-mismatch') 'Same-size corruption not detected.'
 Assert-Integrity ((Test-ScarletFailedNca -NcaPath $dest -NspPath $nsp -RepairCache) -eq 'repaired') 'Cache not recovered.'
 Assert-Integrity ((Get-ScarletSha256Prefix -Path $dest) -eq $id) 'Restoration failed.'
 [IO.File]::WriteAllBytes($dest,$bad)
 $raw=[IO.File]::ReadAllBytes($nsp)
 $raw[$raw.Length-7] = $raw[$raw.Length-7] -bxor 0xff
 [IO.File]::WriteAllBytes($nsp,$raw)
 Assert-Integrity ((Test-ScarletFailedNca -NcaPath $dest -NspPath $nsp -RepairCache) -eq 'source-mismatch') 'Corrupt original not detected.'
 Assert-Integrity ((Get-ScarletSha256Prefix -Path $dest) -ne $id) 'Corrupt original overwrote cache.'
 Write-Host 'NCA hash test passed: SHA256, NSP offset, corrupt cache, recovery, corrupt source.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
