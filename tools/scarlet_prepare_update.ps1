# Base NSP/XCI + update NSP -> validated Scarlet 4.0.0 ExeFS and BKTR-patched RomFS.
# This is a cached preparation step for the native runtime; it never invokes suyu.
param(
 [Parameter(Mandatory=$true)][string]$BasePath,
 [Parameter(Mandatory=$true)][string]$UpdatePath,
 [string]$KeysPath = '',
 [string]$TitleKeysPath = '',
 [string]$HactoolPath = '',
 [string]$NstoolPath = '',
 [string]$AotManifestPath = '',
 [string]$OutputRoot = '',
 [switch]$Force
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'scarlet_native_titlekeys.ps1')
. (Join-Path $PSScriptRoot 'scarlet_nsp_cache.ps1')
. (Join-Path $PSScriptRoot 'scarlet_nca_hash.ps1')
$titleId = [UInt64]0x0100A3D008C5C000
$expectedMain = '421c5411b487eb4d'

function Resolve-Required([string]$path, [string]$description) {
 if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "$description nao encontrado: $path" }
 (Resolve-Path -LiteralPath $path).Path
}
$base = Resolve-Required $BasePath 'NSP/XCI base'
$update = Resolve-Required $UpdatePath 'NSP update'
if ($base -ieq $update) { throw 'Base e update nao podem ser o mesmo arquivo.' }
if ([IO.Path]::GetExtension($base).ToLowerInvariant() -notin @('.nsp','.xci')) { throw 'A base precisa ser NSP/XCI.' }
if ([IO.Path]::GetExtension($update).ToLowerInvariant() -ne '.nsp') { throw 'A atualizacao precisa ser NSP.' }

function Find-Keys([string]$requested) {
 $paths = @($requested, $env:SCARLET_PROD_KEYS)
 if ($env:USERPROFILE) { $paths += (Join-Path $env:USERPROFILE '.switch\prod.keys') }
 if ($env:APPDATA) {
  $paths += (Join-Path $env:APPDATA 'Ryujinx\system\prod.keys')
  $paths += (Join-Path $env:APPDATA 'Eden\keys\prod.keys')
  $paths += (Join-Path $env:APPDATA 'yuzu\keys\prod.keys')
 }
 $paths += (Join-Path $repo 'dist\ScarletPC-Independent\user\keys\prod.keys')
 # The user's original checkout and the isolated native worktree may differ.
 # Reuse only locally available key files; never download, log or copy keys.
 $parent = Split-Path $repo -Parent
 foreach ($sibling in @('scarlet_pc', 'scarlet_pc-native-test')) {
  $other = Join-Path $parent $sibling
  $paths += (Join-Path $other 'dist\ScarletPC-Independent\user\keys\prod.keys')
  $paths += (Join-Path $other 'build\native\user\keys\prod.keys')
 }
 foreach ($p in $paths) {
  if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return (Resolve-Path -LiteralPath $p).Path }
 }
 throw 'prod.keys nao encontrado. Passe -KeysPath com as chaves do seu console.'
}
function Find-Hactool([string]$requested) {
 $paths = @($requested,$env:SCARLET_HACTOOL,
  (Join-Path $repo 'tools\hactool.exe'),
  (Join-Path $repo 'tools\hactool\hactool.exe'),
  (Join-Path $repo 'build\tools\hactool.exe'),
  (Join-Path $repo 'build\tools\hactool\hactool.exe'))
 foreach ($p in $paths) {
  if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return (Resolve-Path -LiteralPath $p).Path }
 }
 $cmd = Get-Command hactool.exe -ErrorAction SilentlyContinue
 if (-not $cmd) { $cmd = Get-Command hactool -ErrorAction SilentlyContinue }
 if ($cmd) { return $cmd.Source }

 # Other Scarlet checkouts may already have the tool. In particular, the
 # separate worktree typically sits next to the user's original scarlet_pc.
 $parent = Split-Path $repo -Parent
 foreach ($sibling in @('scarlet_pc', 'scarlet_pc-native-test')) {
  $other = Join-Path $parent $sibling
  foreach ($rel in @('tools\hactool.exe', 'tools\hactool\hactool.exe', 'build\tools\hactool.exe', 'build\tools\hactool\hactool.exe')) {
   $candidate = Join-Path $other $rel
   if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
    Write-Host "[native-update] Reutilizando hactool existente: $candidate" -ForegroundColor DarkGray
    return (Resolve-Path -LiteralPath $candidate).Path
   }
  }
 }

 # Keep downloaded executable in ignored build/ rather than source-controlled
 # tools/. Use only the upstream SciresM 1.4.0 Windows release, never download
 # games, title keys or AOT images. The published hactool supports --basenca.
 if ($env:OS -ne 'Windows_NT') {
  throw 'hactool.exe nao encontrado. Informe -HactoolPath com uma versao que suporte --basenca.'
 }
 $dir = Join-Path $repo 'build\tools\hactool'
 $toolExe = Join-Path $dir 'hactool.exe'
 $zip = Join-Path $dir 'hactool-1.4.0-win.zip'
 $unpacked = Join-Path $dir '_download_extract'
 $url = 'https://github.com/SciresM/hactool/releases/download/1.4.0/hactool-1.4.0-win.zip'
 try {
  Write-Host '[native-update] hactool ausente; obtendo a release oficial SciresM 1.4.0 para Windows...' -ForegroundColor Cyan
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip -ErrorAction Stop
  Expand-Archive -LiteralPath $zip -DestinationPath $unpacked -Force -ErrorAction Stop
  $found = Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter 'hactool.exe' | Select-Object -First 1
  if (-not $found) { throw 'O pacote oficial nao contem hactool.exe.' }
  Copy-Item -LiteralPath $found.FullName -Destination $toolExe -Force -ErrorAction Stop
  Write-Host "[native-update] hactool instalado: $toolExe" -ForegroundColor Green
  return $toolExe
 }
 catch {
  throw "Falha ao baixar hactool 1.4.0 da release oficial: $($_.Exception.Message). Instale a ferramenta manualmente e informe -HactoolPath."
 }
 finally {
  Remove-Item -LiteralPath $unpacked -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
 }
}
$keys = Find-Keys $KeysPath
$hactool = Find-Hactool $HactoolPath

# NSTool is a newer NCA reader with BKTR support. It is only obtained if
# hactool fails on verified update NCAs. The release binary stays on D:.
function Find-Nstool([string]$requested) {
 $list = @($requested,$env:SCARLET_NSTOOL,
  'D:\ScarletNativeCache\tools\nstool\nstool.exe',
  (Join-Path $repo 'build\tools\nstool\nstool.exe'))
 foreach ($p in $list) {
  if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) {
   return (Resolve-Path -LiteralPath $p).Path
  }
 }
 $cmd = Get-Command 'nstool.exe' -ErrorAction SilentlyContinue
 if ($cmd) { return $cmd.Source }
 if ($env:OS -ne 'Windows_NT' -or -not (Test-Path -LiteralPath 'D:\' -PathType Container)) {
  throw 'NSTool nao encontrado. Informe -NstoolPath para habilitar o fallback.'
 }
 $dir = 'D:\ScarletNativeCache\tools\nstool'
 $zip = Join-Path $dir 'nstool-v1.9.2-win_x64.zip'
 $unpacked = Join-Path $dir '.unpack'
 $official = 'https://github.com/jakcron/nstool/releases/download/v1.9.2/nstool-v1.9.2-win_x64.zip'
 try {
  Write-Host '[native-update] Instalando NSTool v1.9.2 oficial no cache D: (fallback do hactool)...' -ForegroundColor Cyan
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -UseBasicParsing -Uri $official -OutFile $zip -ErrorAction Stop
  Expand-Archive -LiteralPath $zip -DestinationPath $unpacked -Force -ErrorAction Stop
  $binary = Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter 'nstool.exe' | Select-Object -First 1
  if (-not $binary) { throw 'A release oficial nao contem nstool.exe.' }
  Copy-Item -LiteralPath $binary.FullName -Destination (Join-Path $dir 'nstool.exe') -Force
  return (Join-Path $dir 'nstool.exe')
 }
 finally {
  Remove-Item -LiteralPath $unpacked -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
 }
}

function File-Identity([string]$p) {
 $f = Get-Item -LiteralPath $p
 "$($f.FullName)|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)"
}
$identity = (File-Identity $base) + '|' + (File-Identity $update)
$hash = [Security.Cryptography.SHA256]::Create()
try {
 $tag = ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity)))).Replace('-','').Substring(0,16).ToLowerInvariant()
} finally { $hash.Dispose() }
# Prefer D: for all native base+update builds on Windows. Keep a portable
# fallback on hosts without D:, and continue honoring explicit -OutputRoot.
$legacyOutputRoot = Join-Path $repo ('build\native-update-cache\scarlet-' + $tag)
$usingDefaultD = $false
if (-not $OutputRoot) {
 if ($env:OS -eq 'Windows_NT' -and (Test-Path -LiteralPath 'D:\' -PathType Container)) {
  $OutputRoot = Join-Path 'D:\ScarletNativeCache' ('scarlet-' + $tag)
  $usingDefaultD = $true
 } else {
  $OutputRoot = $legacyOutputRoot
 }
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$work = $OutputRoot + '.work'
$log = $OutputRoot + '.hactool.log'
Write-Host "[native-update] Cache de preparacao: $OutputRoot" -ForegroundColor Cyan

$expected = @{main=$expectedMain}
if (-not $AotManifestPath -and $env:SCARLET_AOT_MANIFEST) { $AotManifestPath = $env:SCARLET_AOT_MANIFEST }
if (-not $AotManifestPath) {
 # Prebuilt imports retain their source export_root; prefer its original
 # manifest (which carries the NSO build IDs) over the DLL-only copy manifest.
 foreach ($p in @(
  (Join-Path $repo 'build\aot\modules\scarlet-aot-manifest.json'),
  (Join-Path $repo 'dist\ScarletPC-Independent\aot\scarlet-aot-manifest.json')
 )) {
  if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
  $copiedManifest = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
  if ($copiedManifest.PSObject.Properties.Name -notcontains 'export_root') { continue }
  $origin = Join-Path ([string]$copiedManifest.export_root) 'aot_cache\aot_manifest.json'
  if (Test-Path -LiteralPath $origin -PathType Leaf) { $AotManifestPath = $origin; break }
 }
}
if ($AotManifestPath) {
 $manifest = Get-Content -LiteralPath (Resolve-Required $AotManifestPath 'Manifesto AOT') -Raw | ConvertFrom-Json
 if ($manifest.PSObject.Properties.Name -contains 'modules') {
 foreach ($entry in @($manifest.modules)) {
  if (-not $entry) { continue }
  $name = ''
  if ($entry.PSObject.Properties.Name -contains 'name') { $name = [string]$entry.name }
  if ($entry.PSObject.Properties.Name -contains 'module') { $name = [string]$entry.module }
  $name = $name.ToLowerInvariant()
  if ($name -eq 'image') { $name='main' }
  if ($name -notin @('main','sdk','subsdk0','rtld')) { continue }
  if ($entry.PSObject.Properties.Name -contains 'build_id') {
   $id = ([string]$entry.build_id).ToLowerInvariant()
   if ($id -match '^[0-9a-f]{16,64}$') { $expected[$name] = $id }
  }
 }
 }
 Write-Host ("[native-update] Build IDs expected from source manifest: {0}" -f ($expected.Keys -join ', ')) -ForegroundColor DarkGray
}
if (-not $expected.main.StartsWith($expectedMain)) { throw 'Manifesto AOT nao e da versao 4.0.0.' }

function Get-ProgramId([string]$exeFs) {
 $p = Join-Path $exeFs 'main.npdm'
 if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return [UInt64]0 }
 $s = [IO.File]::OpenRead($p)
 try {
  $meta = New-Object byte[] 0x80
  if ($s.Read($meta,0,0x80) -ne 0x80) { return [UInt64]0 }
  if ([Text.Encoding]::ASCII.GetString($meta,0,4) -ne 'META') { return [UInt64]0 }
  $off = [BitConverter]::ToUInt32($meta,0x70)
  if ([long]$off -gt $s.Length - 0x18) { return [UInt64]0 }
  $null = $s.Seek([long]$off,[IO.SeekOrigin]::Begin)
  $aci = New-Object byte[] 0x18
  if ($s.Read($aci,0,0x18) -ne 0x18) { return [UInt64]0 }
  if ([Text.Encoding]::ASCII.GetString($aci,0,4) -ne 'ACI0') { return [UInt64]0 }
  [BitConverter]::ToUInt64($aci,0x10)
 } finally { $s.Dispose() }
}
function Get-BuildId([string]$p) {
 if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return '' }
 $s = [IO.File]::OpenRead($p)
 try {
  if ($s.Length -lt 0x60) { return '' }
  $magic = New-Object byte[] 4
  if ($s.Read($magic,0,4) -ne 4 -or [Text.Encoding]::ASCII.GetString($magic) -ne 'NSO0') { return '' }
  $null = $s.Seek(0x40,[IO.SeekOrigin]::Begin)
  $bytes = New-Object byte[] 32
  if ($s.Read($bytes,0,32) -ne 32) { return '' }
  ([BitConverter]::ToString($bytes)).Replace('-','').ToLowerInvariant()
 } finally { $s.Dispose() }
}
function Test-ExeFs([string]$p) {
 if ((Get-ProgramId $p) -ne $titleId) { return $false }
 foreach ($name in @('main','sdk','subsdk0')) {
  $got = Get-BuildId (Join-Path $p $name)
  if (-not $got) { return $false }
  if ($expected.ContainsKey($name) -and -not $got.StartsWith($expected[$name])) { return $false }
 }
 return $true
}
function Test-Romfs([string]$p) {
 if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $false }
 $s = [IO.File]::OpenRead($p)
 try {
  if ($s.Length -lt 0x50) { return $false }
  $header = New-Object byte[] 8
  if ($s.Read($header,0,8) -ne 8) { return $false }
  [BitConverter]::ToUInt64($header,0) -eq [UInt64]0x50
 } finally { $s.Dispose() }
}
function Emit-Result([string]$root, [bool]$cached) {
 [PSCustomObject]@{
  ExeFs = (Join-Path $root 'exefs')
  RomFsImage = (Join-Path $root 'romfs.bin')
  RomFsDir = ''
  PackagePath = $base
  UpdatePath = $update
  CacheRoot = $root
  Cached = $cached
  MainBuildId = (Get-BuildId (Join-Path $root 'exefs\main'))
 } | ConvertTo-Json -Compress
}
$metaPath = Join-Path $OutputRoot '.source.json'
if (-not $Force -and (Test-Path -LiteralPath $metaPath -PathType Leaf)) {
 $meta = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
 if ($meta.source_identity -eq $identity -and
  (Test-ExeFs (Join-Path $OutputRoot 'exefs')) -and
  (Test-Romfs (Join-Path $OutputRoot 'romfs.bin'))) {
  Write-Host '[native-update] Cache validado. Reusando.' -ForegroundColor Green
  Emit-Result $OutputRoot $true
  exit 0
 }
}
if (Test-Path -LiteralPath $OutputRoot) {
 if (-not $Force) { throw "Cache invalido em $OutputRoot. Confira antes de usar -Force." }
 Remove-Item -LiteralPath $OutputRoot -Recurse -Force
}
# If this is the first run after switching to D:, copy the previously
# extracted NCAs from C: instead of extracting two NSPs again. Do not delete
# the source: it remains a recovery copy until the user confirms success.
# robocopy can resume a partially copied directory after interruption.
if ($usingDefaultD -and (Test-Path -LiteralPath ($legacyOutputRoot + '.work') -PathType Container)) {
 $oldWork = $legacyOutputRoot + '.work'
 # After a successful migration, do not recopy all NCAs on every launch.
 # Compare cached NCA/ticket file lengths without reading key material.
 $copyNeeded = $true
 if (Test-Path -LiteralPath $work -PathType Container) {
  $copyNeeded = $false
  $sourceParts = @(Get-ChildItem -LiteralPath $oldWork -Recurse -File | Where-Object { $_.Extension -in @('.nca','.tik') })
  foreach ($part in $sourceParts) {
   $relative = $part.FullName.Substring($oldWork.Length).TrimStart('\')
   $target = Join-Path $work $relative
   if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
       (Get-Item -LiteralPath $target).Length -ne $part.Length) {
    $copyNeeded = $true
    break
   }
  }
  if ($sourceParts.Count -eq 0) { $copyNeeded = $true }
 }
 if ($copyNeeded -and -not (Test-Path -LiteralPath $work -PathType Container)) {
  $size = (Get-ChildItem -LiteralPath $oldWork -File -Recurse | Measure-Object -Property Length -Sum).Sum
  if ($null -eq $size) { $size = 0 }
  $drive = [IO.DriveInfo]::new('D:\')
  if (-not $drive.IsReady) { throw 'A unidade D: nao esta pronta para receber o cache.' }
  $needed = [long]$size + [long](8GB)
  if ($drive.AvailableFreeSpace -lt $needed) {
   throw ("Espaco insuficiente em D: para transferir o cache e extrair o RomFS: livre={0:N1} GiB, minimo estimado={1:N1} GiB. O cache original em C: foi preservado." -f ($drive.AvailableFreeSpace / 1GB),($needed / 1GB))
  }
 }
 if ($copyNeeded) {
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  Write-Host '[native-update] Transferindo NCAs existentes de C: para D: (preservando a copia original)...' -ForegroundColor Cyan
  & robocopy.exe $oldWork $work /E /R:1 /W:1 /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
  $copyStatus = $LASTEXITCODE
  if ($copyStatus -ge 8) {
   throw "Transferencia do cache para D: falhou (robocopy exit $copyStatus). Copia em C: preservada. Verifique espaco livre/permissoes e execute novamente."
  }
  Write-Host '[native-update] NCAs reutilizados em D:.' -ForegroundColor Green
 } else {
  Write-Host '[native-update] NCAs de D: ja estao completos. Sem nova copia.' -ForegroundColor DarkGray
 }
}
# Failed probes leave a .work directory. Keep already-extracted NCAs and retry
# only the scanning/patching steps. Never delete an existing work directory.
$resume = Test-Path -LiteralPath $work -PathType Container
if ($resume) {
 Write-Host "[native-update] Reutilizando NCAs da tentativa anterior: $work" -ForegroundColor Cyan
} else {
 New-Item -ItemType Directory -Force -Path (Split-Path $OutputRoot -Parent),$work | Out-Null
}
$baseDir = Join-Path $work 'base'
$updateDir = Join-Path $work 'update'
$probe = Join-Path $work 'probe'
$assembled = Join-Path $work 'assembled'
New-Item -ItemType Directory -Force -Path $baseDir,$updateDir,$assembled | Out-Null
if (-not (Test-Path -LiteralPath $log -PathType Leaf)) {
 Set-Content -LiteralPath $log -Value '[native-update] hactool diagnostics' -Encoding UTF8
} else {
 Add-Content -LiteralPath $log -Value "[native-update] Retry/resume $(Get-Date -Format o)"
}

$script:ticketHome = ''
function Run-Hactool([string[]]$arguments, [bool]$allowFailure=$false, [string]$capturePath='') {
 $safe = ($arguments | ForEach-Object {
  if ($_ -eq $keys) { '<keys-file>' }
  elseif ($_ -match '^--(?:titlekey|contentkey)=') { '--key=<redacted>' }
  else { $_ }
 }) -join ' '
 Add-Content -LiteralPath $log -Value ([Environment]::NewLine + '> hactool ' + $safe)
 $old = $ErrorActionPreference
 $oldHome = $env:HOME
 try {
  $ErrorActionPreference = 'Continue'
  if ($script:ticketHome) { $env:HOME = $script:ticketHome }
  $output = @(& $hactool @arguments 2>&1)
  $code = $LASTEXITCODE
 } finally {
  $ErrorActionPreference = $old
  if ($null -eq $oldHome) { Remove-Item Env:HOME -ErrorAction SilentlyContinue }
  else { $env:HOME = $oldHome }
 }
 if ($output.Count -gt 0) {
  $output | Out-File -LiteralPath $log -Append -Encoding UTF8
 }
 if ($capturePath) {
  # The command always has --suppresskeys: no decrypted key material in logs.
  $output | Out-File -LiteralPath $capturePath -Encoding UTF8
 }
 if ($code -ne 0 -and -not $allowFailure) { throw "hactool falhou (exit $code). Log: $log" }
 return $code
}

Write-Host '[native-update] Conferindo NCAs de base e update...' -ForegroundColor Cyan
$baseFiles = @(Get-ChildItem -LiteralPath $baseDir -Recurse -File -Filter '*.nca')
if ($baseFiles.Count -eq 0) {
 Write-Host '[native-update] Extraindo NCAs base (primeira execucao)...' -ForegroundColor Cyan
 if ([IO.Path]::GetExtension($base).ToLowerInvariant() -eq '.xci') {
  $null = Run-Hactool -arguments @('-k',$keys,'--disablekeywarns','--suppresskeys','-t','xci',"--securedir=$baseDir",$base)
 } else {
  $null = Run-Hactool -arguments @('-k',$keys,'--disablekeywarns','--suppresskeys','-t','pfs0',"--outdir=$baseDir",$base)
 }
 $baseFiles = @(Get-ChildItem -LiteralPath $baseDir -Recurse -File -Filter '*.nca')
}
if ($baseFiles.Count -eq 0) { throw "NSP base sem NCAs apos extracao: $base. Confira o pacote. Log: $log" }
$updateFiles = @(Get-ChildItem -LiteralPath $updateDir -Recurse -File -Filter '*.nca')
if ($updateFiles.Count -eq 0) {
 Write-Host '[native-update] Extraindo NCAs da atualizacao...' -ForegroundColor Cyan
 $null = Run-Hactool -arguments @('-k',$keys,'--disablekeywarns','--suppresskeys','-t','pfs0',"--outdir=$updateDir",$update)
 $updateFiles = @(Get-ChildItem -LiteralPath $updateDir -Recurse -File -Filter '*.nca')
}
if ($updateFiles.Count -eq 0) { throw "Update sem NCAs apos extracao: $update. Confira o pacote. Log: $log" }
Write-Host ("[native-update] Encontrados: {0} NCA(s) base; {1} NCA(s) update." -f $baseFiles.Count,$updateFiles.Count) -ForegroundColor DarkGray

# A previous extraction on a full C: drive can leave correctly named but
# truncated NCA files. Compare cached sizes to the original NSP PFS0 index
# before hactool reads any of them. Recover only damaged entries into D:.
# A complete file with a wrong size is replaced only after a full staged copy.
if ([IO.Path]::GetExtension($base).ToLowerInvariant() -eq '.nsp') {
 Repair-ScarletNspNcaCache -PackagePath $base -CacheDirectory $baseDir -Label 'base'
}
Repair-ScarletNspNcaCache -PackagePath $update -CacheDirectory $updateDir -Label 'update'
# Only inspect NCAs listed inside the actual update NSP. Old .work caches
# can contain an extra NCA from earlier failed attempts.
$updateIndexNames = @{}
foreach ($entry in @(Get-ScarletNspNcaEntries -PackagePath $update)) {
 $updateIndexNames[$entry.Name] = $true
}
$baseFiles = @(Get-ChildItem -LiteralPath $baseDir -Recurse -File -Filter '*.nca')
$updateFiles = @(Get-ChildItem -LiteralPath $updateDir -Recurse -File -Filter '*.nca')

# NSP tickets contain encrypted titlekeys, not plaintext/decrypted keys.
# Hactool 1.4.0 automatically loads HOME/.switch/title.keys; supply both the
# base and update ticket keys through a temporary HOME so --basenca can decrypt
# BOTH NCAs. Nothing is downloaded or placed in the repository.
$existingTitleKeyFile = Find-ScarletTitleKeysFile -Requested $TitleKeysPath -ProdKeys $keys -Repository $repo
$allTitleKeys = Get-ScarletExistingTitleKeys -Path $existingTitleKeyFile
$tickets = Get-ScarletTicketTitleKeys -Directories @($baseDir,$updateDir)
$ticketKeysAdded = 0
foreach ($rights in @($tickets.Keys.Keys)) {
 if (-not $allTitleKeys.ContainsKey($rights)) { $ticketKeysAdded++ }
 $allTitleKeys[$rights] = $tickets.Keys[$rights]
}
if ($allTitleKeys.Count -gt 0) {
 $script:ticketHome = New-ScarletTemporaryKeyHome -Keys $allTitleKeys -BaseDirectory $work
 Write-Host ("[native-update] Chaves de ticket locais prontas: {0} Rights ID(s) (inclui {1} do NSP)." -f $allTitleKeys.Count,$ticketKeysAdded) -ForegroundColor DarkGray
} else {
 Write-Warning '[native-update] Nenhum ticket comum ou title.keys local encontrado. Program NCAs protegidos por Rights ID nao poderao ser extraidos.'
}
if ($tickets.PersonalizedCount -gt 0) {
 Write-Warning "[native-update] $($tickets.PersonalizedCount) ticket(s) personalizado(s) detectado(s); esses tickets nao podem ser importados automaticamente."
}
if ($tickets.UnsupportedCount -gt 0) {
 Write-Warning ("[native-update] {0} arquivo(s) .tik com assinatura nao reconhecida; ignorados com seguranca." -f $tickets.UnsupportedCount)
}
Write-Host ("[native-update] Tickets locais analisados: {0}; validos comuns: {1}; personalizados: {2}; assinatura desconhecida: {3}." -f $tickets.TicketCount,$tickets.Keys.Count,$tickets.PersonalizedCount,$tickets.UnsupportedCount) -ForegroundColor DarkGray

# Redacted, bounded hactool diagnostics. No key/rights-id byte sequences are
# emitted to the console. We only need error wording from the Program NCA;
# generic printed NCA metadata is deliberately not shown.
function Get-HactoolSafeErrors([string]$message) {
 if (-not $message) { return @('hactool nao retornou texto de erro.') }
 $lines = @($message -split '\r?\n' | Where-Object {
  $_ -match '(?i)^\s*(?:(?:hactool(?:\.exe)?\s*:\s*)?(?:Error:|Fatal:|Failed to |Unable to |Invalid NCA header|Missing |Unknown Base File Type|Out of memory|Could not ))'
 } | Select-Object -Last 5)
 if ($lines.Count -eq 0) {
  if ($message -match '(?i)section\s+[0-9]+\s+is\s+corrupted') {
   return @('Secao NCA corrompida ou descriptografia falhou; confira titlekey/prod.keys e integridade do pacote.')
  }
  return @('Saida do hactool nao contem mensagem de erro reconhecida. Consulte o diagnostico local.')
 }
 $safeLines = @()
 foreach ($line in $lines) {
  $safe = [string]$line
  $safe = [regex]::Replace($safe,'(?i)(?:0x)?[0-9a-f]{32,}', '<redacted>')
  $safe = [regex]::Replace($safe,'(?i)\(value\s+"[^"]*"\)', '(value redacted)')
  $safe = $safe.Trim()
  if ($safe.Length -gt 220) { $safe = $safe.Substring(0,220) }
  if ($safe) { $safeLines += $safe }
 }
 return $safeLines
}

# hactool 1.4.0 emits this error when fwrite returns a short write.
# Check storage space before assuming the decrypted NCA content is wrong.
function Show-NativeWriteFailure([string]$hactoolOutput, [string]$targetDirectory) {
 if (-not $hactoolOutput -or $hactoolOutput -notmatch 'Failed to write file!') { return }
 $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($targetDirectory))
 Write-Warning ("[native-update] Falha de gravacao na unidade {0}; confira espaco livre e erros de E/S." -f $root)
 try {
  $drive = [IO.DriveInfo]::new($root)
  if ($drive.IsReady) {
   $freeGiB = [math]::Round($drive.AvailableFreeSpace / 1GB, 2)
   Write-Warning ("[native-update] Espaco livre na unidade {0}: {1} GiB." -f $root,$freeGiB)
  }
 } catch {
  Write-Warning '[native-update] Nao foi possivel consultar o espaco livre.'
 }
 $saveLines = @($hactoolOutput -split '\r?\n' | Where-Object { $_ -match 'Saving ' -and $_ -match ' to ' })
 if ($saveLines.Count -gt 0) {
  $lastSave = [string]$saveLines[-1]
  if ($lastSave.Length -gt 240) { $lastSave = $lastSave.Substring(0,240) }
  Write-Host ("[native-update] Ultimo arquivo informado: {0}" -f $lastSave) -ForegroundColor DarkGray
 }
}

function Find-ProgramNca([string]$dir,[string]$baseNca='',[string]$mainId='') {
 $allNcas = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Filter '*.nca' | Sort-Object Length -Descending)
 $type = if ($baseNca) { 'update' } else { 'base' }
 $ncas = $allNcas
 if ($baseNca) {
  $ncas = @($allNcas | Where-Object { $updateIndexNames.ContainsKey($_.Name.ToLowerInvariant()) })
  if ($allNcas.Count -gt $ncas.Count) {
   Write-Warning ("[native-update] {0} NCA(s) avulsos no cache de update, ausentes do NSP original: ignorados na varredura." -f ($allNcas.Count-$ncas.Count))
  }
 }
 $ncaFailed = 0
 $missingNpdm = 0
 $missingTitlekeys = 0
 $programCandidates = 0
 $invalidHeaders = 0
 $wrongId = @{}
 $missingMain = 0
 Write-Host ("[native-update] Analisando {0} NCA(s) de {1}..." -f $ncas.Count,$type) -ForegroundColor Cyan
 foreach ($nca in $ncas) {
  if (Test-Path -LiteralPath $probe) { Remove-Item -LiteralPath $probe -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $probe | Out-Null
  # Do NOT use --basenca while detecting the update ExeFS.
  # It is only needed for BKTR RomFS reconstruction, not for the update's
  # independently encrypted Program NCA ExeFS. Supplying a base NCA for
  # every Meta/Control/Manual NCA can make hactool fail before classification.
  $hactoolArgs = @('-k',$keys,'--disablekeywarns','--suppresskeys','-t','nca','--xcontenttype=program')
  $hactoolArgs += @("--exefsdir=$probe",$nca.FullName)
  $probeOutput = Join-Path $work 'last-hactool-probe.log'
  $code = Run-Hactool -arguments $hactoolArgs -allowFailure $true -capturePath $probeOutput
  $info = Get-Content -LiteralPath $probeOutput -Raw -ErrorAction SilentlyContinue
  # Some update NCAs contain BKTR sections that cannot be inspected without
  # a virtual base. --basefake is safe ONLY for this ExeFS identification:
  # final RomFS assembly MUST still pass the real --basenca.
  if ($baseNca -and $code -ne 0) {
   $firstInfo = $info
   $firstCode = $code
   $fakeArgs = @('-k',$keys,'--disablekeywarns','--suppresskeys','-t','nca','--xcontenttype=program','--basefake',"--exefsdir=$probe",$nca.FullName)
   $fakeOutput = Join-Path $work 'last-update-basefake-probe.log'
   $fakeCode = Run-Hactool -arguments $fakeArgs -allowFailure $true -capturePath $fakeOutput
   $fakeInfo = Get-Content -LiteralPath $fakeOutput -Raw -ErrorAction SilentlyContinue
   if ($fakeCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $probe 'main.npdm') -PathType Leaf)) {
    $code = $fakeCode
    $info = $fakeInfo
    Write-Host ("[native-update] ExeFS do candidato {0} acessivel com base simulada (apenas probe)." -f $nca.Name) -ForegroundColor DarkGray
   } else {
    Write-Warning ("[native-update] NCA {0}: leitura normal exit={1}; com --basefake exit={2}." -f $nca.Name,$firstCode,$fakeCode)
    foreach ($reason in @(Get-HactoolSafeErrors $fakeInfo)) {
     Write-Warning ("[native-update] Probe alternativo: {0}" -f $reason)
    }
    $info = $firstInfo
    $code = $firstCode
   }
  }
  if ($baseNca -and $code -ne 0) {
   # If the NCA is still unreadable after the full-size restoration, validate
   # its actual ciphertext hash against the content ID encoded by its name.
   # This distinguishes same-size corruption from a hactool decoding issue.
   Write-Host ("[native-update] Verificando SHA256 de {0} apos falha do hactool..." -f $nca.Name) -ForegroundColor DarkGray
   $status = Test-ScarletFailedNca -NcaPath $nca.FullName -NspPath $update -RepairCache
   if ($status -eq 'source-mismatch') {
    throw "NCA $($nca.Name) do update nao corresponde ao SHA256 Content ID nem no NSP original. Gere um novo dump integro da atualizacao. Cache e AOT preservados."
   }
   if ($status -eq 'repaired') {
    if (Test-Path -LiteralPath $probe -PathType Container) {
     Remove-Item -LiteralPath $probe -Recurse -Force
    }
    New-Item -ItemType Directory -Path $probe -Force | Out-Null
    $code = Run-Hactool -arguments $hactoolArgs -allowFailure $true -capturePath $probeOutput
    $info = Get-Content -LiteralPath $probeOutput -Raw -ErrorAction SilentlyContinue
   }
  }
  $kind = if ($info -match '(?m)^Content Type:\s+([a-zA-Z0-9]+)') { $Matches[1] } else { 'unknown' }
  if ($kind -eq 'Program') { $programCandidates++ }
  $titleKeyMissing = [bool]($info -match '(?m)^Titlekey:\s+Unknown\b')
  if ($titleKeyMissing) { $missingTitlekeys++ }
  if ($info -match 'Invalid NCA header!') { $invalidHeaders++ }
  if ($code -ne 0) {
   $ncaFailed++
   # Surface bounded metadata for *all* failed update NCAs, including an
   # unknown kind: early failures may prevent hactool from printing its type.
   if ($kind -eq 'Program' -or $type -eq 'update') {
    Write-Warning ("[native-update] NCA {0}: tipo={1}, hactool exit={2}; ExeFS nao extraido." -f $nca.Name,$kind,$code)
    foreach ($reason in @(Get-HactoolSafeErrors $info)) {
     Write-Warning ("[native-update] Causa informada pelo hactool: {0}" -f $reason)
    }
    Show-NativeWriteFailure -hactoolOutput $info -targetDirectory $probe
   }
   continue
  }
  if (-not (Test-Path -LiteralPath (Join-Path $probe 'main.npdm') -PathType Leaf)) {
   $missingNpdm++
   if ($kind -eq 'Program' -or $titleKeyMissing) {
    $hint = if ($titleKeyMissing) { 'Titlekey ausente (NCA protegido por Rights ID)' } else { 'ExeFS nao extraido' }
    Write-Warning ("[native-update] {0}: Content Type={1}; {2}; hactool exit={3}" -f $nca.Name,$kind,$hint,$code)
   }
   continue
  }
  $programId = Get-ProgramId $probe
  if ($programId -ne $titleId) {
   $id = $programId.ToString('x16')
   if (-not $wrongId.ContainsKey($id)) { $wrongId[$id] = 0 }
   $wrongId[$id]++
   continue
  }
  $got = Get-BuildId (Join-Path $probe 'main')
  if (-not $got) { $missingMain++; continue }
  if ($mainId -and -not $got.StartsWith($mainId)) {
   Write-Host ("[native-update] Program NCA {0}: main {1}, esperado {2}" -f $nca.Name,$got.Substring(0,16),$mainId) -ForegroundColor Yellow
   continue
  }
  Write-Host ("[native-update] Program NCA: {0} / main {1}" -f $nca.Name,$got.Substring(0,16)) -ForegroundColor Green
  return $nca.FullName
 }
 $details = "NCA(s)=$($ncas.Count); tipo Program=$programCandidates; titlekeys ausentes=$missingTitlekeys; headers invalidos=$invalidHeaders; hactool exit nao-zero=$ncaFailed; sem main.npdm=$missingNpdm; sem main NSO=$missingMain"
 if ($wrongId.Count) { $details += '; title IDs divergentes=' + (($wrongId.Keys | Sort-Object) -join ',') }
 Write-Warning "[native-update] Nenhum Program NCA valido ($type). $details"
 if ($missingTitlekeys -gt 0) {
  Write-Warning '[native-update] Pelo menos um NCA usa titlekey nao localizada. Verifique suas chaves locais de console e title.keys; nao envie chaves no chat.'
 } elseif ($programCandidates -eq 0 -and $invalidHeaders -gt 0) {
  Write-Warning '[native-update] Hactool nao validou o cabecalho dos NCAs. Verifique prod.keys e a integridade do pacote.'
 } elseif ($programCandidates -gt 0 -and $missingNpdm -gt 0) {
  Write-Warning '[native-update] Program NCA identificado, mas ExeFS nao foi extraido. Consulte as mensagens do ultimo probe no log.'
 }
 Add-Content -LiteralPath $log -Value ("[native-update] NCA scan summary for {0}: {1}" -f $type,$details)
 return ''
}
try {
$baseNca = Find-ProgramNca $baseDir
if (-not $baseNca) {
 throw "Program NCA do Scarlet na base nao extraido. Consulte a mensagem de erro do hactool acima. NSP e tickets ja foram encontrados; preserve a pasta .work. Log: $log"
}
Write-Host '[native-update] Identificando update ExeFS sem --basenca; BKTR sera aplicado depois...' -ForegroundColor Cyan
$patchNca = Find-ProgramNca $updateDir $baseNca $expectedMain
if (-not $patchNca) { throw "Update 4.0.0 (main $expectedMain) nao identificado sem --basenca. Consulte diagnosticos dos NCAs da atualizacao; cache .work preservado. Log: $log" }

Write-Host '[native-update] Aplicando patch BKTR com --basenca...' -ForegroundColor Cyan
$exeFs = Join-Path $assembled 'exefs'
$romfs = Join-Path $assembled 'romfs.bin'
New-Item -ItemType Directory -Force -Path $exeFs | Out-Null
$null = Run-Hactool -arguments @('-k',$keys,'--disablekeywarns','--suppresskeys','-t','nca','--xcontenttype=program',
 "--basenca=$baseNca","--exefsdir=$exeFs","--romfs=$romfs",$patchNca)
if (-not (Test-ExeFs $exeFs)) {
 $found = Get-BuildId (Join-Path $exeFs 'main')
 throw "ExeFS nao corresponde ao AOT 4.0.0 (main=$found). Log: $log"
}
if (-not (Test-Romfs $romfs)) { throw "RomFS atualizado invalido; BKTR nao aplicado. Log: $log" }
[ordered]@{
 source_identity=$identity
 base_path=$base
 update_path=$update
 main_build_id=(Get-BuildId (Join-Path $exeFs 'main'))
 created_utc=[DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $assembled '.source.json') -Encoding UTF8
Move-Item -LiteralPath $assembled -Destination $OutputRoot
Remove-Item -LiteralPath $work -Recurse -Force
Write-Host '[native-update] Conteudo 4.0.0 pronto; DLLs AOT preservadas.' -ForegroundColor Green
Emit-Result $OutputRoot $false
}
finally {
 # The derived encrypted ticket keys exist only in the user's temp directory
 # while hactool is running. Always clean them up, including on failure.
 if ($script:ticketHome) {
  Remove-Item -LiteralPath $script:ticketHome -Recurse -Force -ErrorAction SilentlyContinue
  $script:ticketHome = ''
 }
}
