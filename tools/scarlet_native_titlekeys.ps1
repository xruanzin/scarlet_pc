# Local ticket/title key discovery for the native NSP+update preparer.
# Key values must NEVER be printed, added to diagnostics, or committed.
# Source of common-ticket offsets: Nintendo Switch ticket format:
# RSA-2048 signature header 0x140 bytes; key block 0x40; rights id 0x160;
# key type byte 0x141, all ticket-data-relative.
function Get-ScarletTicketTitleKeys {
 param([string[]]$Directories)
 $found = @{}
 $personalized = 0
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
    switch ($signature) {
     '00010000' { $start = 0x240 }
     '00010003' { $start = 0x240 }
     '00010001' { $start = 0x140 }
     '00010004' { $start = 0x140 }
     default { continue }
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
 return [PSCustomObject]@{ Keys=$found; TicketCount=$tickets; PersonalizedCount=$personalized }
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
 param([hashtable]$Keys)
 if ($Keys.Count -eq 0) { return '' }
 $homePath = Join-Path ([IO.Path]::GetTempPath()) ('ScarletTitleKeys-' + [Guid]::NewGuid().ToString('N'))
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
