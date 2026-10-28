# Remove only redundant Scarlet NSP preparation caches generated on C:.
# This never deletes a source NSP, AOT DLL, exe, prod.keys or unrelated cache.
# Requires the D: cache to contain *all* expected NCAs with valid content-ID
# hashes before deleting anything from the legacy C: generated-cache path.
param(
 [Parameter(Mandatory=$true)][string]$BasePath,
 [Parameter(Mandatory=$true)][string]$UpdatePath,
 [switch]$Apply
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'scarlet_nsp_cache.ps1')
. (Join-Path $PSScriptRoot 'scarlet_nca_hash.ps1')

function Resolve-Input([string]$p) {
 if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "NSP ausente: $p" }
 (Resolve-Path -LiteralPath $p).Path
}
$base = Resolve-Input $BasePath
$update = Resolve-Input $UpdatePath
if ([IO.Path]::GetExtension($base) -ine '.nsp' -or [IO.Path]::GetExtension($update) -ine '.nsp') {
 throw 'A limpeza automatica exige NSPs (base e update), para validar todo o cache pelo indice PFS0.'
}
function Identity([string]$p) {
 $f = Get-Item -LiteralPath $p
 return "$($f.FullName)|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)"
}
$identity = (Identity $base) + '|' + (Identity $update)
$sha = [Security.Cryptography.SHA256]::Create()
try {
 $tag = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity)))).Replace('-','').Substring(0,16).ToLowerInvariant()
} finally { $sha.Dispose() }

$legacyParent = Join-Path $repo 'build\native-update-cache'
$legacy = Join-Path $legacyParent ('scarlet-' + $tag)
$modern = Join-Path 'D:\ScarletNativeCache' ('scarlet-' + $tag + '.work')
$oldWork = $legacy + '.work'
$oldLog = $legacy + '.hactool.log'
$oldOutput = $legacy
Write-Host "[cleanup] Cache mantido em D: $modern" -ForegroundColor Cyan
Write-Host "[cleanup] Cache antigo gerado na C: $oldWork" -ForegroundColor DarkGray

# Only the exact identity-specific generated path may be deleted. The old
# result root itself is deliberately untouched (could be the last valid boot).
$candidates = @($oldWork,$oldLog) | Where-Object { Test-Path -LiteralPath $_ }
if ($candidates.Count -eq 0) {
 Write-Host '[cleanup] Nenhuma copia antiga deste NSP encontrada no cache da C:.' -ForegroundColor Green
 exit 0
}
if (-not (Test-Path -LiteralPath $modern -PathType Container)) {
 throw '[cleanup] Cache na D: nao existe. Nada foi apagado. Execute o preparador usando D: antes de limpar.'
}
# Check both sets of NCAs on D: against their NSP Content IDs; a simple
# size check is not enough. The original NSPs and AOT remain untouched.
foreach ($pair in @(
 @{ Label='base'; Package=$base },
 @{ Label='update'; Package=$update }
)) {
 $folder = Join-Path $modern $pair.Label
 if (-not (Test-Path -LiteralPath $folder -PathType Container)) {
  throw "[cleanup] Pasta D: ausente: $folder. Nada foi apagado."
 }
 $entries = @(Get-ScarletNspNcaEntries -PackagePath $pair.Package)
 if ($entries.Count -eq 0) { throw "[cleanup] Nenhum NCA no indice $($pair.Label). Nada foi apagado." }
 foreach ($entry in $entries) {
  $p = Join-Path $folder $entry.Name
  if (-not (Test-Path -LiteralPath $p -PathType Leaf)) {
   throw "[cleanup] NCA ausente em D: $($entry.Name). Nada foi apagado."
  }
  if ((Get-Item -LiteralPath $p).Length -ne $entry.Length) {
   throw "[cleanup] Tamanho incorreto em D: $($entry.Name). Nada foi apagado."
  }
  $hash = Get-ScarletSha256Prefix -Path $p
  if ($hash -ne $entry.Name.Substring(0,32)) {
   throw "[cleanup] SHA256 incorreto em D: $($entry.Name). Nada foi apagado."
  }
 }
 Write-Host ("[cleanup] D: {0} validado: {1} NCA(s) com SHA256 correto." -f $pair.Label,$entries.Count) -ForegroundColor Green
}

foreach ($candidate in $candidates) {
 $object = Get-Item -LiteralPath $candidate -Force
 if (($object.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
  throw "[cleanup] Link simbolico/juncao detectado: $candidate. Nada foi apagado."
 }
 if ($object.PSIsContainer) {
  $links = @(Get-ChildItem -LiteralPath $candidate -Recurse -Force -ErrorAction Stop |
    Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })
  if ($links.Count -gt 0) { throw '[cleanup] Juncao ou link encontrado dentro do cache antigo. Nada foi apagado.' }
 }
}
$bytes = [long]0
foreach ($candidate in $candidates) {
 if (Test-Path -LiteralPath $candidate -PathType Leaf) {
  $bytes += (Get-Item -LiteralPath $candidate).Length
 } else {
  foreach ($f in @(Get-ChildItem -LiteralPath $candidate -File -Recurse -Force)) { $bytes += $f.Length }
 }
}
Write-Host ("[cleanup] Copias antigas na C: {0}; total aproximado {1:N2} GiB." -f $candidates.Count,($bytes / 1GB)) -ForegroundColor Cyan
if (-not $Apply) {
 Write-Host '[cleanup] Apenas verificacao. Para APAGAR as copias antigas, execute novamente com -Apply.' -ForegroundColor Yellow
 exit 0
}
foreach ($candidate in $candidates) {
 Remove-Item -LiteralPath $candidate -Recurse -Force -ErrorAction Stop
 Write-Host ("[cleanup] Excluido da C: {0}" -f $candidate) -ForegroundColor Green
}
# Leave unrelated files, the source checkout, AOT, and any completed old
# output untouched. Only remove the parent folder if it became empty.
if ((Test-Path -LiteralPath $legacyParent -PathType Container) -and
    @(Get-ChildItem -LiteralPath $legacyParent -Force).Count -eq 0) {
 Remove-Item -LiteralPath $legacyParent -Force
}
Write-Host '[cleanup] Concluido: caches NSP antigos apagados da C:. Cache, NSPs e AOT na D:/origens preservados.' -ForegroundColor Green
