# A completed Scarlet update reuses its prepared ExeFS/RomFS without
# prod.keys, hactool or NSTool. No game files or secret keys needed.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Join-Path ([IO.Path]::GetTempPath()) ('ScarletPreparedBoot-'+[guid]::NewGuid().ToString('N'))
try {
 New-Item -ItemType Directory -Path $root -Force | Out-Null
 $base=Join-Path $root 'base.nsp'
 $update=Join-Path $root 'update.nsp'
 [IO.File]::WriteAllBytes($base,[byte[]]@(1,2,3,4))
 [IO.File]::WriteAllBytes($update,[byte[]]@(5,6,7,8))
 $cache=Join-Path $root 'prepared'
 $exefs=Join-Path $cache 'exefs'
 New-Item -ItemType Directory -Path $exefs -Force | Out-Null
 $npdm=New-Object byte[] 0xA0
 [Text.Encoding]::ASCII.GetBytes('META').CopyTo($npdm,0)
 [BitConverter]::GetBytes([uint32]0x80).CopyTo($npdm,0x70)
 [Text.Encoding]::ASCII.GetBytes('ACI0').CopyTo($npdm,0x80)
 [BitConverter]::GetBytes([uint64]0x0100A3D008C5C000).CopyTo($npdm,0x90)
 [IO.File]::WriteAllBytes((Join-Path $exefs 'main.npdm'),$npdm)
 foreach($module in @('main','sdk','subsdk0')) {
  $nso=New-Object byte[] 0x80
  [Text.Encoding]::ASCII.GetBytes('NSO0').CopyTo($nso,0)
  $id='421c5411b487eb4d'+('0'*48)
  for($i=0;$i -lt 32;$i++) { $nso[0x40+$i]=[Convert]::ToByte($id.Substring($i*2,2),16) }
  [IO.File]::WriteAllBytes((Join-Path $exefs $module),$nso)
 }
 $romfs=New-Object byte[] 0x60
 [BitConverter]::GetBytes([uint64]0x50).CopyTo($romfs,0)
 [IO.File]::WriteAllBytes((Join-Path $cache 'romfs.bin'),$romfs)
 function Identity([string]$p) {
  $f=Get-Item -LiteralPath $p
  return "$($f.FullName)|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)"
 }
 $identity=(Identity $base)+'|'+(Identity $update)
 @{source_identity=$identity;main_build_id='421c5411b487eb4d';created_utc=[DateTime]::UtcNow.ToString('o')} |
   ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cache '.source.json') -Encoding UTF8
 $preparer=Join-Path $PSScriptRoot 'scarlet_prepare_update.ps1'
 $cmd=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$preparer,
   '-BasePath',$base,'-UpdatePath',$update,'-OutputRoot',$cache,
   '-KeysPath',(Join-Path $root 'missing.keys'),
   '-HactoolPath',(Join-Path $root 'missing-hactool.exe'))
 $output=@(& powershell.exe @cmd 2>&1)
 if ($LASTEXITCODE -ne 0) { throw "Cached boot failed: $($output -join ' | ')" }
 $json=@($output | Where-Object { [string]$_ -match '^\{' } | Select-Object -Last 1)
 if ($json.Count -ne 1) { throw 'Missing prepared result JSON.' }
 $result=$json[0] | ConvertFrom-Json
 if (-not $result.Cached -or $result.ExeFs -ne $exefs -or $result.RomFsImage -ne (Join-Path $cache 'romfs.bin')) {
  throw 'Prepared path was not returned correctly.'
 }
 Write-Host 'Native direct cache test passed: no NSP tools or keys for prepared game.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
