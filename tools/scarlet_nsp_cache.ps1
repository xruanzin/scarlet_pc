# PFS0 NSP cache validation / targeted recovery. Never changes source NSPs.
# This module reads only the cleartext NSP PFS0 index. No Switch keys needed.
Set-StrictMode -Version Latest

function Get-ScarletNspNcaEntries {
 param([Parameter(Mandatory=$true)][string]$PackagePath)
 $stream = [IO.File]::Open($PackagePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 $reader = [IO.BinaryReader]::new($stream,[Text.Encoding]::UTF8,$true)
 try {
  if ($stream.Length -lt 16 -or [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'PFS0') {
   throw "Pacote sem cabecalho NSP/PFS0 valido: $PackagePath"
  }
  $count = [int]$reader.ReadUInt32()
  $stringsLen = [int]$reader.ReadUInt32()
  $null = $reader.ReadUInt32()
  if ($count -lt 1 -or $count -gt 4096 -or $stringsLen -lt 1 -or $stringsLen -gt 16777216) {
   throw 'Indice PFS0 invalido: numero de arquivos ou tabela de nomes fora do limite.'
  }
  $dataStart = [long]16 + ([long]$count * 24) + [long]$stringsLen
  if ($dataStart -gt $stream.Length) { throw 'Indice PFS0 truncado no NSP de origem.' }
  $records = @()
  for ($i=0; $i -lt $count; $i++) {
   $relativeOffset = $reader.ReadUInt64()
   $size = $reader.ReadUInt64()
   $nameOffset = [int]$reader.ReadUInt32()
   $null = $reader.ReadUInt32()
   $records += [PSCustomObject]@{ Offset=$relativeOffset; Size=$size; NameOffset=$nameOffset }
  }
  $strings = $reader.ReadBytes($stringsLen)
  if ($strings.Length -ne $stringsLen) { throw 'Tabela de nomes PFS0 truncada.' }
  $seen = @{}
  foreach ($item in $records) {
   if ($item.NameOffset -lt 0 -or $item.NameOffset -ge $stringsLen) { throw 'Offset de nome fora da tabela PFS0.' }
   $endName = [Array]::IndexOf($strings,[byte]0,$item.NameOffset)
   if ($endName -lt 0) { throw 'Nome PFS0 sem terminador.' }
   $name = [Text.Encoding]::UTF8.GetString($strings,$item.NameOffset,$endName-$item.NameOffset)
   if ($name -notmatch '^[0-9a-fA-F]{32}\.nca$') { continue }
   if ($seen.ContainsKey($name.ToLowerInvariant())) { throw 'NCA duplicado no indice PFS0.' }
   $seen[$name.ToLowerInvariant()] = $true
   $absolute = [uint64]$dataStart + [uint64]$item.Offset
   if ($absolute -gt [uint64]$stream.Length -or [uint64]$item.Size -gt ([uint64]$stream.Length - $absolute)) {
    throw "Arquivo NCA $name incompleto dentro do proprio NSP de origem. Recupere um dump integro antes de continuar."
   }
   [PSCustomObject]@{ Name=$name.ToLowerInvariant(); Length=[long]$item.Size; SourceOffset=[long]$absolute }
  }
 }
 finally { $reader.Dispose(); $stream.Dispose() }
}

function Repair-ScarletNspNcaCache {
 param(
  [Parameter(Mandatory=$true)][string]$PackagePath,
  [Parameter(Mandatory=$true)][string]$CacheDirectory,
  [string]$Label='NSP'
 )
 if (-not (Test-Path -LiteralPath $CacheDirectory -PathType Container)) {
  throw "Cache NCA ausente: $CacheDirectory"
 }
 $entries = @(Get-ScarletNspNcaEntries -PackagePath $PackagePath)
 if ($entries.Count -lt 1) { throw "$Label nao contem NCAs no indice PFS0." }
 $bad = @()
 foreach ($entry in $entries) {
  $target = Join-Path $CacheDirectory $entry.Name
  $present = Test-Path -LiteralPath $target -PathType Leaf
  $actual = if ($present) { (Get-Item -LiteralPath $target).Length } else { 0 }
  if (-not $present -or $actual -ne $entry.Length) {
   $bad += [PSCustomObject]@{ Entry=$entry; Target=$target; Actual=[long]$actual }
  }
 }
 Write-Host ("[native-update] Integridade $($Label): $($entries.Count) NCA(s) no indice; $($bad.Count) com tamanho incorreto/ausente.") -ForegroundColor DarkGray
 if ($bad.Count -eq 0) { return }
 foreach ($entry in $bad) {
  Write-Warning ("[native-update] {0}: {1} incompleto (cache {2:N2} GiB; NSP {3:N2} GiB)." -f $Label,$entry.Entry.Name,($entry.Actual / 1GB),($entry.Entry.Length / 1GB))
 }
 $required = [long]0
 foreach ($entry in $bad) { $required += $entry.Entry.Length }
 $drivePath = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($CacheDirectory))
 $drive = [IO.DriveInfo]::new($drivePath)
 if (-not $drive.IsReady -or $drive.AvailableFreeSpace -lt ($required + 1GB)) {
  throw ("Espaco insuficiente em {0} para recuperar os NCA(s) incompletos de {1}: requer {2:N2} GiB + 1 GiB de margem. Nenhum arquivo foi alterado." -f $drivePath,$Label,($required/1GB))
 }
 $source = [IO.File]::Open($PackagePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 try {
  $buffer = New-Object byte[] (4MB)
  foreach ($entry in $bad) {
   $target = $entry.Target
   $stage = $target + '.scarlet-repair'
   if (Test-Path -LiteralPath $stage -PathType Leaf) {
    Remove-Item -LiteralPath $stage -Force
   }
   $out = [IO.File]::Open($stage,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
   try {
    $source.Position = $entry.Entry.SourceOffset
    $remaining = [long]$entry.Entry.Length
    while ($remaining -gt 0) {
     $take = [int][Math]::Min([long]$buffer.Length,$remaining)
     $read = $source.Read($buffer,0,$take)
     if ($read -ne $take) { throw "NSP de origem nao forneceu dados suficientes para $($entry.Entry.Name)." }
     $out.Write($buffer,0,$read)
     $remaining -= $read
    }
    $out.Flush()
   }
   finally { $out.Dispose() }
   if ((Get-Item -LiteralPath $stage).Length -ne $entry.Entry.Length) {
    throw "Recuperacao de $($entry.Entry.Name) incompleta; arquivo original preservado."
   }
   if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target -Force }
   Move-Item -LiteralPath $stage -Destination $target
   Write-Host ("[native-update] Recuperado da origem: {0}" -f $entry.Entry.Name) -ForegroundColor Green
  }
 }
 finally { $source.Dispose() }
}
