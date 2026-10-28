# Synthetic fixture regression: no Switch console keys or game assets required.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scarlet_native_titlekeys.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ScarletTicketTest-' + [guid]::NewGuid().ToString('N'))
$tempHome = ''
function Assert-TitleKeys([bool]$Ok, [string]$Message) {
 if (-not $Ok) { throw $Message }
}
function Put-Hex([byte[]]$Target,[int]$Offset,[string]$Hex) {
 $hexBytes = [byte[]]@()
 for ($i = 0; $i -lt $Hex.Length; $i += 2) {
  $hexBytes += [Convert]::ToByte($Hex.Substring($i,2),16)
 }
 [Array]::Copy($hexBytes,0,$Target,$Offset,$hexBytes.Length)
}
try {
 New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
 $rights = '0123456789abcdef0123456789abcdef'
 $encrypted = '00112233445566778899aabbccddeeff'
 $ticket = New-Object byte[] 0x400
 # Actual NSP ticket header: RSA-2048 SHA-256 0x010004,
 # stored as 04 00 01 00 (little-endian), not 00 01 00 04.
 Put-Hex $ticket 0 '04000100'
 Put-Hex $ticket 0x180 $encrypted
 Put-Hex $ticket 0x2a0 $rights
 $tikPath = Join-Path $testRoot 'example.tik'
 [IO.File]::WriteAllBytes($tikPath,$ticket)
 $result = Get-ScarletTicketTitleKeys -Directories @($testRoot)
 Assert-TitleKeys ($result.TicketCount -eq 1) 'Ticket not counted.'
 Assert-TitleKeys ($result.Keys.Count -eq 1) 'Common titlekey was not parsed.'
 Assert-TitleKeys ($result.Keys[$rights] -eq $encrypted) 'Titlekey or Rights ID offset is wrong.'

 # Exercise every documented little-endian SignatureType with the correct
 # ticket-data offset. This catches future mistakes in 2048/4096/ECDSA
 # headers even when only one signature type occurs in the user's NSP.
 foreach ($format in @(
  @{ Hex='00000100'; Start=0x240 }, # RSA-4096 SHA1
  @{ Hex='01000100'; Start=0x140 }, # RSA-2048 SHA1
  @{ Hex='02000100'; Start=0x80 },  # ECDSA SHA1
  @{ Hex='03000100'; Start=0x240 }, # RSA-4096 SHA256
  @{ Hex='04000100'; Start=0x140 }, # RSA-2048 SHA256
  @{ Hex='05000100'; Start=0x80 },  # ECDSA SHA256
  @{ Hex='06000100'; Start=0x40 }   # HMAC SHA1
 )) {
  $other = New-Object byte[] 0x600
  Put-Hex $other 0 $format.Hex
  Put-Hex $other ([int]$format.Start + 0x40) $encrypted
  Put-Hex $other ([int]$format.Start + 0x160) $rights
  [IO.File]::WriteAllBytes($tikPath,$other)
  $parsed = Get-ScarletTicketTitleKeys -Directories @($testRoot)
  Assert-TitleKeys ($parsed.Keys.Count -eq 1 -and $parsed.Keys[$rights] -eq $encrypted) "Signature parser failed for $($format.Hex)."
  Assert-TitleKeys ($parsed.UnsupportedCount -eq 0) "Incorrectly classified supported signature $($format.Hex)."
 }
 # Restore the common RSA-2048 fixture used by the following checks.
 [IO.File]::WriteAllBytes($tikPath,$ticket)

 # The real NSP previously contained a ticket signature not recognized by the
 # parser. With StrictMode enabled, an unknown type must be safely skipped,
 # never fall through and read the uninitialized $start offset.
 $badTicket = New-Object byte[] 0x400
 [IO.File]::WriteAllBytes((Join-Path $testRoot 'unknown.tik'),$badTicket)
 $mixed = Get-ScarletTicketTitleKeys -Directories @($testRoot)
 Assert-TitleKeys ($mixed.TicketCount -eq 2) 'Unknown ticket not counted.'
 Assert-TitleKeys ($mixed.UnsupportedCount -eq 1) 'Unknown ticket not classified.'
 Assert-TitleKeys ($mixed.Keys.Count -eq 1 -and $mixed.Keys[$rights] -eq $encrypted) 'Unknown ticket should not invalidate valid common ticket.'
 # An NSP with only unknown signatures should return zero keys, not crash.
 Remove-Item -LiteralPath $tikPath -Force
 $unknownOnly = Get-ScarletTicketTitleKeys -Directories @($testRoot)
 Assert-TitleKeys ($unknownOnly.UnsupportedCount -eq 1 -and $unknownOnly.Keys.Count -eq 0) 'Unknown-only ticket set caused an error.'
 [IO.File]::WriteAllBytes($tikPath,$ticket)
 Remove-Item -LiteralPath (Join-Path $testRoot 'unknown.tik') -Force

 $titlekeys = Join-Path $testRoot 'title.keys'
 [IO.File]::WriteAllText($titlekeys, "$rights = $encrypted")
 $local = Get-ScarletExistingTitleKeys -Path $titlekeys
 Assert-TitleKeys ($local.Count -eq 1 -and $local[$rights] -eq $encrypted) 'Existing title.keys parser failed.'
 $tempHome = New-ScarletTemporaryKeyHome -Keys $local
 $generated = Join-Path $tempHome '.switch\title.keys'
 Assert-TitleKeys (Test-Path -LiteralPath $generated -PathType Leaf) 'Expected hactool HOME/.switch/title.keys.'
 Assert-TitleKeys ((Get-Content -LiteralPath $generated -Raw) -match $rights) 'Temporary keys file malformed.'

 # Personalized ticket type=1: do not try to use its RSA title key block.
 $ticket[0x281] = 1
 [IO.File]::WriteAllBytes($tikPath,$ticket)
 $personalizedResult = Get-ScarletTicketTitleKeys -Directories @($testRoot)
 Assert-TitleKeys ($personalizedResult.PersonalizedCount -eq 1) 'Personalized ticket type not recognized.'
 Assert-TitleKeys ($personalizedResult.Keys.Count -eq 0) 'Personalized ticket was incorrectly imported.'
 Write-Host 'Native NSP ticket tests: common ticket, existing title.keys, temporary HOME and personalized ticket passed.'
}
finally {
 if ($tempHome) { Remove-Item -LiteralPath $tempHome -Recurse -Force -ErrorAction SilentlyContinue }
 Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
