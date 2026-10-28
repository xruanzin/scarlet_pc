# Validate failing NCA data against the NCA Content ID (= first 16 bytes
# of SHA-256). No secrets, crypto keys, or game data are printed.
function Get-ScarletSha256Prefix([string]$Path,[long]$Offset=0,[long]$Length=-1) {
 $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 $sha = [Security.Cryptography.SHA256]::Create()
 try {
  if ($Offset -lt 0 -or $Offset -gt $stream.Length) { throw 'Offset SHA256 fora do arquivo.' }
  if ($Length -lt 0) { $Length = $stream.Length - $Offset }
  if ($Length -gt $stream.Length - $Offset) { throw 'Tamanho SHA256 excede arquivo.' }
  $stream.Position = $Offset
  $buffer = New-Object byte[] (4MB)
  $remaining = $Length
  while ($remaining -gt 0) {
   $take = [int][math]::Min([long]$buffer.Length,$remaining)
   $read = $stream.Read($buffer,0,$take)
   if ($read -eq 0) { throw 'Fim inesperado do arquivo durante SHA256.' }
   $null = $sha.TransformBlock($buffer,0,$read,$buffer,0)
   $remaining -= $read
  }
  $null = $sha.TransformFinalBlock([byte[]]@(),0,0)
  return ([BitConverter]::ToString($sha.Hash,0,16)).Replace('-','').ToLowerInvariant()
 }
 finally { $sha.Dispose(); $stream.Dispose() }
}

function Test-ScarletFailedNca {
 param(
  [Parameter(Mandatory=$true)][string]$NcaPath,
  [Parameter(Mandatory=$true)][string]$NspPath,
  [switch]$RepairCache
 )
 $name = [IO.Path]::GetFileName($NcaPath).ToLowerInvariant()
 if ($name -notmatch '^[0-9a-f]{32}\.nca$') { throw "Nome NCA invalido: $name" }
 $id = $name.Substring(0,32)
 $entries = @(Get-ScarletNspNcaEntries -PackagePath $NspPath)
 $entry = @($entries | Where-Object { $_.Name -eq $name } | Select-Object -First 1)
 if ($entry.Count -ne 1) {
  Write-Warning ("[native-update] NCA {0} nao pertence ao indice NSP original; ignorando arquivo avulso." -f $name)
  return 'not-in-package'
 }
 $cache = Get-ScarletSha256Prefix -Path $NcaPath
 if ($cache -eq $id) {
  Write-Host ("[native-update] SHA256 NCA {0}: conteudo do cache integro (ID confere)." -f $name) -ForegroundColor DarkGray
  return 'valid'
 }
 Write-Warning ("[native-update] SHA256 NCA {0}: bytes do cache nao conferem com o Content ID. Conferindo NSP original..." -f $name)
 $origin = Get-ScarletSha256Prefix -Path $NspPath -Offset $entry[0].SourceOffset -Length $entry[0].Length
 if ($origin -ne $id) {
  Write-Warning ("[native-update] SHA256 do NCA {0} tambem diverge na origem. O NSP nao corresponde ao Content ID: gere novo dump integro da atualizacao." -f $name)
  return 'source-mismatch'
 }
 Write-Warning ("[native-update] NSP original validado. O cache NCA {0} foi alterado mesmo mantendo o tamanho esperado." -f $name)
 if (-not $RepairCache) { return 'cache-mismatch' }
 $target = $NcaPath
 $stage = $target + '.scarlet-hash-repair'
 $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($target)))
 if (-not $drive.IsReady -or $drive.AvailableFreeSpace -lt ($entry[0].Length + 1GB)) {
  Write-Warning '[native-update] Espaco insuficiente para regravar NCA integro. Nenhum NCA sera substituido.'
  return 'low-space'
 }
 $source = [IO.File]::Open($NspPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 try {
  if (Test-Path -LiteralPath $stage -PathType Leaf) { Remove-Item -LiteralPath $stage -Force }
  $out = [IO.File]::Open($stage,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try {
   $source.Position = $entry[0].SourceOffset
   $remaining = [long]$entry[0].Length
   $buffer = New-Object byte[] (4MB)
   while ($remaining -gt 0) {
    $take = [int][math]::Min([long]$buffer.Length,$remaining)
    $count = $source.Read($buffer,0,$take)
    if ($count -eq 0) { throw 'NSP de origem terminou antes do fim do NCA.' }
    $out.Write($buffer,0,$count)
    $remaining -= $count
   }
  }
  finally { $out.Dispose() }
  if ((Get-ScarletSha256Prefix -Path $stage) -ne $id) {
   throw 'SHA256 do arquivo de recuperacao nao confere; arquivo de cache antigo preservado.'
  }
  [IO.File]::Replace($stage,$target,$null)
  Write-Host ("[native-update] NCA {0} restaurado do NSP original apos validacao SHA256." -f $name) -ForegroundColor Green
  return 'repaired'
 }
 finally { $source.Dispose() }
}
