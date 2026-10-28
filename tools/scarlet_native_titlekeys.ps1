# Local ticket/title key discovery for the native NSP+update preparer.
# Key values must NEVER be printed, added to diagnostics, or committed.
# Source of common-ticket offsets: Nintendo Switch ticket format:
# RSA-2048 signature header 0x140 bytes; key block 0x40; rights id 0x160;
# key type byte 0x141, all ticket-data-relative.
function Get-ScarletTicketTitleKeys {
 param([string[]]$Directories)
 $found = @{}
 $personalized = 0
 $unsupported = 0
 $tickets = 0
 foreach ($dir in $Directories) {
  if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
  foreach ($ticket in @(Get-ChildItem -LiteralPath $dir -File -Recurse -Filter '*.tik')) {
   $tickets++
   $stream = [IO.File]::OpenRead($ticket.FullName)
   try {
    $data = New-Object byte[] 0x400
    $count = $stream.Read($data, 0, $data.Length)
    if ($count -lt 4) { continue }
    $signature = ([BitConverter]::ToString($data,0,4)).Replace('-','')
    # 'continue' inside a PowerShell switch does not skip the enclosing
    # foreach. Explicitly validate the signature before using the offset.
    $start = -1
    # Ticket SignatureType is a little-endian uint32 in actual NSP .tik
    # files (see nut/Fs/Ticket.py -> readInt32). The previous parser accepted
    # only the reversed byte order and silently ignored every real ticket.
    # Accept the documented little-endian representation and the older
    # big-endian test fixtures for backward compatibility.
    switch ($signature) {
     { $_ -in @('00000100','00010000') } { $start = 0x240; break } # 0x010000 RSA-4096/SHA-1
     { $_ -in @('03000100','00010003') } { $start = 0x240; break } # 0x010003 RSA-4096/SHA-256
     { $_ -in @('01000100','00010001') } { $start = 0x140; break } # 0x010001 RSA-2048/SHA-1
     { $_ -in @('04000100','00010004') } { $start = 0x140; break } # 0x010004 RSA-2048/SHA-256
     { $_ -in @('02000100','00010002') } { $start = 0x80; break }  # 0x010002 ECDSA/SHA-1
     { $_ -in @('05000100','00010005') } { $start = 0x80; break }  # 0x010005 ECDSA/SHA-256
     { $_ -in @('06000100','00010006') } { $start = 0x40; break }  # 0x010006 HMAC/SHA-1
    }
    if ($start -lt 0) {
     $unsupported++
     continue
    }
    if ($count -lt ($start + 0x170)) { continue }
    # Personalized (RSA-encrypted) tickets cannot use --titlekey directly.
    if ($data[$start + 0x141] -ne 0) { $personalized++; continue }
    $rights = ([BitConverter]::ToString($data,$start + 0x160,16)).Replace('-','').ToLowerInvariant()
    $key = ([BitConverter]::ToString($data,$start + 0x40,16)).Replace('-','').ToLowerInvariant()
    if ($rights -eq ('0' * 32) -or $key -eq ('0' * 32)) { continue }
    if ($found.ContainsKey($rights) -and $found[$rights] -ne $key) {
     throw "Tickets conflitantes para o mesmo Rights ID ($($ticket.Name)). Confira seus pacotes NSP."
    }
    $found[$rights] = $key
   } finally { $stream.Dispose() }
  }
 }
 return [PSCustomObject]@{ Keys=$found; TicketCount=$tickets; PersonalizedCount=$personalized; UnsupportedCount=$unsupported }
}

function Find-ScarletTitleKeysFile {
 param([string]$Requested,[string]$ProdKeys,[string]$Repository)
 if ($Requested) {
  if (-not (Test-Path -LiteralPath $Requested -PathType Leaf)) {
   throw "title.keys nao encontrado em -TitleKeysPath: $Requested"
  }
  return (Resolve-Path -LiteralPath $Requested).Path
 }
 $paths = @($env:SCARLET_TITLE_KEYS)
 if ($ProdKeys) { $paths += (Join-Path (Split-Path $ProdKeys -Parent) 'title.keys') }
 if ($env:USERPROFILE) {
  $paths += (Join-Path $env:USERPROFILE '.switch\title.keys')
  $paths += (Join-Path $env:USERPROFILE '.config\switch\title.keys')
 }
 if ($env:APPDATA) {
  foreach ($app in @('Ryujinx\system','Eden\keys','eden\keys','yuzu\keys')) {
   $paths += (Join-Path (Join-Path $env:APPDATA $app) 'title.keys')
  }
 }
 $paths += (Join-Path $Repository 'dist\ScarletPC-Independent\user\keys\title.keys')
 $parent = Split-Path $Repository -Parent
 foreach ($other in @('scarlet_pc','scarlet_pc-native-test')) {
  $paths += (Join-Path (Join-Path $parent $other) 'dist\ScarletPC-Independent\user\keys\title.keys')
 }
 foreach ($candidate in $paths) {
  if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
   return (Resolve-Path -LiteralPath $candidate).Path
  }
 }
 return ''
}

function Get-ScarletExistingTitleKeys {
 param([string]$Path)
 $found = @{}
 if (-not $Path) { return ,$found }
 foreach ($line in [IO.File]::ReadAllLines($Path)) {
  if ($line -match '(?i)^\s*([0-9a-f]{32})\s*=\s*([0-9a-f]{32})(?:\s*(?:[#;].*)?)?$') {
   $found[$Matches[1].ToLowerInvariant()] = $Matches[2].ToLowerInvariant()
  }
 }
 return ,$found
}

function New-ScarletTemporaryKeyHome {
 param([hashtable]$Keys,[string]$BaseDirectory='')
 if ($Keys.Count -eq 0) { return '' }
 if (-not $BaseDirectory) { $BaseDirectory = [IO.Path]::GetTempPath() }
 $homePath = Join-Path $BaseDirectory ('ScarletTitleKeys-' + [Guid]::NewGuid().ToString('N'))
 $switchDir = Join-Path $homePath '.switch'
 try {
  New-Item -ItemType Directory -Path $switchDir -Force | Out-Null
  $records = [string[]]@($Keys.Keys | Sort-Object | ForEach-Object { "$_ = $($Keys[$_])" })
  [IO.File]::WriteAllLines((Join-Path $switchDir 'title.keys'), $records, [Text.Encoding]::ASCII)
  return $homePath
 } catch {
  Remove-Item -LiteralPath $homePath -Recurse -Force -ErrorAction SilentlyContinue
  throw
 }
}
