# Synthetic filesystem/command-contract tests. No Nintendo content or keys.
$ErrorActionPreference='Stop'
$temp=Join-Path ([IO.Path]::GetTempPath()) ('Scarlet Update Test '+[guid]::NewGuid())
New-Item -ItemType Directory -Path $temp | Out-Null
try {
 $fake=Join-Path $temp 'fake-hactool.ps1'
 @'
$global:LASTEXITCODE=0
Add-Content (Join-Path $PSScriptRoot 'calls.txt') 'called'
function Option([string]$Prefix) {
 foreach ($a in $script:args) { if ($a.StartsWith($Prefix)) { return $a.Substring($Prefix.Length) } }
 return $null
}
$out=Option '--outdir='
if (-not $out) { $out=Option '--securedir=' }
if ($out) {
 [IO.File]::WriteAllBytes((Join-Path $out 'program.nca'),[byte[]]@(1))
 return
}
if ($args -contains '-i') {
 'Content Type:                       Program'
 'Title ID:                           0100a3d008c5c000'
 return
}
$exe=Option '--exefsdir='
if ($exe) {
 $npdm=New-Object byte[] 152
 [Text.Encoding]::ASCII.GetBytes('META').CopyTo($npdm,0)
 [BitConverter]::GetBytes([uint32]128).CopyTo($npdm,112)
 [Text.Encoding]::ASCII.GetBytes('ACI0').CopyTo($npdm,128)
 [BitConverter]::GetBytes([uint64]0x0100A3D008C5C000).CopyTo($npdm,144)
 [IO.File]::WriteAllBytes((Join-Path $exe 'main.npdm'),$npdm)
 foreach ($name in @('main','sdk','subsdk0')) {
  $nso=New-Object byte[] 96
  [Text.Encoding]::ASCII.GetBytes('NSO0').CopyTo($nso,0)
  $nso[64]=42
  [IO.File]::WriteAllBytes((Join-Path $exe $name),$nso)
 }
 return
}
$section=Option '--section1='
if ($section) {
 if ($args -contains '--raw') { [IO.File]::WriteAllBytes($section,[byte[]]@(1,2,3)); return }
 $base=Option '--baseromfs='
 if (-not $base -or -not (Test-Path -LiteralPath $base)) { throw 'Missing raw base for patch.' }
 $rom=New-Object byte[] 80
 [BitConverter]::GetBytes([uint64]80).CopyTo($rom,0)
 [IO.File]::WriteAllBytes($section,$rom)
 return
}
throw 'Unexpected tool operation.'
'@ | Set-Content -LiteralPath $fake -Encoding UTF8
 $base=Join-Path $temp 'base game.nsp'; $update=Join-Path $temp 'update game.nsp'; $keys=Join-Path $temp 'prod.keys'
 foreach ($f in @($base,$update,$keys)) { [IO.File]::WriteAllText($f,'fixture') }
 $invoke=@{ PackagePath=$base; UpdatePath=$update; KeysPath=$keys; HactoolPath=$fake; CacheRoot=(Join-Path $temp 'cache') }
 $prepare=Join-Path $PSScriptRoot 'scarlet_prepare_update.ps1'
 $root=@(& $prepare @invoke)[-1]
 if (-not (Test-Path (Join-Path $root 'ready.json'))) { throw 'Cache not published.' }
 $calls=(Get-Content (Join-Path $temp 'calls.txt')).Count
 $again=@(& $prepare @invoke)[-1]
 if ($root -ne $again -or (Get-Content (Join-Path $temp 'calls.txt')).Count -ne $calls) { throw 'Cache not reused.' }
 # Corrupted completed cache must be rejected, never silently used.
 [IO.File]::WriteAllBytes((Join-Path $root 'romfs.bin'),[byte[]]@(0))
 $failed=$false
 try { & $prepare @invoke | Out-Null } catch { $failed=$true }
 if (-not $failed) { throw 'Corrupt cache was accepted.' }
 # A failed preparation never publishes ready.json.
 $bad=Join-Path $temp 'bad-tool.ps1'
 'throw "synthetic tool failure"' | Set-Content $bad
 $invoke.HactoolPath=$bad; $invoke.CacheRoot=Join-Path $temp 'bad-cache'
 $failed=$false
 try { & $prepare @invoke | Out-Null } catch { $failed=$true }
 if (-not $failed -or @(Get-ChildItem $invoke.CacheRoot -Recurse -Filter ready.json).Count) { throw 'Failed preparation published a cache.' }
 Write-Host 'Update preparation: raw base, patch input, validation, cache reuse and failure isolation passed.'
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
