# Base NSP/XCI + update NSP -> validated Scarlet 4.0.0 ExeFS and BKTR-patched RomFS.
# This is a cached preparation step for the native runtime; it never invokes suyu.
param(
 [Parameter(Mandatory=$true)][string]$BasePath,
 [Parameter(Mandatory=$true)][string]$UpdatePath,
 [string]$KeysPath = '',
 [string]$HactoolPath = '',
 [string]$AotManifestPath = '',
 [string]$OutputRoot = '',
 [switch]$Force
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
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
 foreach ($p in $paths) {
  if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return (Resolve-Path -LiteralPath $p).Path }
 }
 throw 'prod.keys nao encontrado. Passe -KeysPath com as chaves do seu console.'
}
function Find-Hactool([string]$requested) {
 $paths = @($requested,$env:SCARLET_HACTOOL,
  (Join-Path $repo 'tools\hactool.exe'),
  (Join-Path $repo 'tools\hactool\hactool.exe'),
  (Join-Path $repo 'build\tools\hactool.exe'))
 foreach ($p in $paths) {
  if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return (Resolve-Path -LiteralPath $p).Path }
 }
 $cmd = Get-Command hactool.exe -ErrorAction SilentlyContinue
 if (-not $cmd) { $cmd = Get-Command hactool -ErrorAction SilentlyContinue }
 if ($cmd) { return $cmd.Source }
 throw 'hactool.exe ausente; informe -HactoolPath (requer --basenca/BKTR).'
}
$keys = Find-Keys $KeysPath
$hactool = Find-Hactool $HactoolPath

function File-Identity([string]$p) {
 $f = Get-Item -LiteralPath $p
 "$($f.FullName)|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)"
}
$identity = (File-Identity $base) + '|' + (File-Identity $update)
$hash = [Security.Cryptography.SHA256]::Create()
try {
 $tag = ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity)))).Replace('-','').Substring(0,16).ToLowerInvariant()
} finally { $hash.Dispose() }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repo ('build\native-update-cache\scarlet-' + $tag) }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$work = $OutputRoot + '.work'
$log = $OutputRoot + '.hactool.log'

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
if (Test-Path -LiteralPath $work) { throw "Preparacao anterior presente em $work. Verifique antes de remover." }
New-Item -ItemType Directory -Force -Path (Split-Path $OutputRoot -Parent),$work | Out-Null
$baseDir = Join-Path $work 'base'
$updateDir = Join-Path $work 'update'
$probe = Join-Path $work 'probe'
$assembled = Join-Path $work 'assembled'
New-Item -ItemType Directory -Force -Path $baseDir,$updateDir,$assembled | Out-Null
Set-Content -LiteralPath $log -Value '[native-update] hactool diagnostics' -Encoding UTF8

function Run-Hactool([string[]]$arguments, [bool]$allowFailure=$false) {
 $safe = ($arguments | ForEach-Object { if ($_ -eq $keys) { '<keys-file>' } else { $_ } }) -join ' '
 Add-Content -LiteralPath $log -Value ([Environment]::NewLine + '> hactool ' + $safe)
 $old = $ErrorActionPreference
 try {
  $ErrorActionPreference = 'Continue'
  & $hactool @arguments 2>&1 | Out-File -LiteralPath $log -Append -Encoding UTF8
  $code = $LASTEXITCODE
 } finally { $ErrorActionPreference = $old }
 if ($code -ne 0 -and -not $allowFailure) { throw "hactool falhou (exit $code). Log: $log" }
 return $code
}

Write-Host '[native-update] Extraindo NCAs para cache temporario (primeiro boot)...' -ForegroundColor Cyan
if ([IO.Path]::GetExtension($base).ToLowerInvariant() -eq '.xci') {
 $null = Run-Hactool -arguments @('-k',$keys,'--suppresskeys','-t','xci',"--securedir=$baseDir",$base)
} else {
 $null = Run-Hactool -arguments @('-k',$keys,'--suppresskeys','-t','pfs0',"--outdir=$baseDir",$base)
}
$null = Run-Hactool -arguments @('-k',$keys,'--suppresskeys','-t','pfs0',"--outdir=$updateDir",$update)

function Find-ProgramNca([string]$dir,[string]$baseNca='',[string]$mainId='') {
 $ncas = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Filter '*.nca' | Sort-Object Length -Descending)
 foreach ($nca in $ncas) {
  if (Test-Path -LiteralPath $probe) { Remove-Item -LiteralPath $probe -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $probe | Out-Null
  $hactoolArgs = @('-k',$keys,'--suppresskeys','-t','nca','--xcontenttype=program')
  if ($baseNca) { $hactoolArgs += "--basenca=$baseNca" }
  $hactoolArgs += @("--exefsdir=$probe",$nca.FullName)
  $code = Run-Hactool -arguments $hactoolArgs -allowFailure $true
  if ($code -ne 0 -or (Get-ProgramId $probe) -ne $titleId) { continue }
  $got = Get-BuildId (Join-Path $probe 'main')
  if (-not $got -or ($mainId -and -not $got.StartsWith($mainId))) { continue }
  Write-Host ("[native-update] Program NCA: {0} / main {1}" -f $nca.Name,$got.Substring(0,16)) -ForegroundColor Green
  return $nca.FullName
 }
 return ''
}
$baseNca = Find-ProgramNca $baseDir
if (-not $baseNca) { throw "Program NCA da base nao encontrado. Log: $log" }
$patchNca = Find-ProgramNca $updateDir $baseNca $expectedMain
if (-not $patchNca) { throw "Update 4.0.0 (main $expectedMain) nao encontrado. Log: $log" }

Write-Host '[native-update] Aplicando patch BKTR com --basenca...' -ForegroundColor Cyan
$exeFs = Join-Path $assembled 'exefs'
$romfs = Join-Path $assembled 'romfs.bin'
New-Item -ItemType Directory -Force -Path $exeFs | Out-Null
$null = Run-Hactool -arguments @('-k',$keys,'--suppresskeys','-t','nca','--xcontenttype=program',
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
