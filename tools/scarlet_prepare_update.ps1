# Build a cached patched filesystem without starting suyu or regenerating AOT.
param(
 [Parameter(Mandatory=$true)][string]$PackagePath,
 [Parameter(Mandatory=$true)][string]$UpdatePath,
 [string]$KeysPath='', [string]$HactoolPath='', [string]$CacheRoot='', [string]$AotManifest=''
)
$ErrorActionPreference='Stop'
$RepoRoot=Split-Path $PSScriptRoot -Parent
function Find-Hactool {
    param([string]$Requested)
    $Candidates = @()
    if ($Requested) { $Candidates += $Requested }
    if ($env:SCARLET_HACTOOL) { $Candidates += $env:SCARLET_HACTOOL }
    $Candidates += @((Join-Path $RepoRoot "tools\hactool.exe"), (Join-Path $RepoRoot "tools\hactool\hactool.exe"), (Join-Path $RepoRoot "build\tools\hactool.exe"))
    foreach ($Candidate in $Candidates) { if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $Candidate).Path } }
    $Cmd = Get-Command hactool.exe -ErrorAction SilentlyContinue
    if (-not $Cmd) { $Cmd = Get-Command hactool -ErrorAction SilentlyContinue }
    if ($Cmd) { return $Cmd.Source }

    # Windows convenience: fetch the official prebuilt release from SciresM when
    # the user has not installed hactool yet. This downloads only the open-source
    # tool; game files and keys are never downloaded or bundled by this project.
    if ($env:OS -eq "Windows_NT") {
        $ToolDir = Join-Path $RepoRoot "tools\hactool"
        $ToolExe = Join-Path $ToolDir "hactool.exe"
        $Zip = Join-Path $ToolDir "hactool-1.4.0-win.zip"
        $Tmp = Join-Path $ToolDir "_extract"
        $Url = "https://github.com/SciresM/hactool/releases/download/1.4.0/hactool-1.4.0-win.zip"
        try {
            Write-Host "[scarlet-package] hactool nao encontrado; baixando a build oficial 1.4.0 para Windows..." -ForegroundColor Cyan
            New-Item -ItemType Directory -Force -Path $ToolDir | Out-Null
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Zip
            Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
            Expand-Archive -LiteralPath $Zip -DestinationPath $Tmp -Force
            $Downloaded = Get-ChildItem -LiteralPath $Tmp -Recurse -File -Filter "hactool.exe" | Select-Object -First 1
            if (-not $Downloaded) { throw "o ZIP oficial nao continha hactool.exe" }
            Copy-Item -LiteralPath $Downloaded.FullName -Destination $ToolExe -Force
            Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
            Write-Host "[scarlet-package] hactool instalado em $ToolExe" -ForegroundColor Green
            return $ToolExe
        } catch {
            Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
            throw "Nao consegui baixar/extrair o hactool oficial automaticamente: $($_.Exception.Message). Informe -HactoolPath se ja tiver uma copia."
        }
    }

    throw "hactool nao encontrado. Informe -HactoolPath."
}

function Find-ProdKeys {
    param([string]$Requested)
    $Candidates = @()
    if ($Requested) { $Candidates += $Requested }
    if ($env:SCARLET_PROD_KEYS) { $Candidates += $env:SCARLET_PROD_KEYS }
    if ($env:USERPROFILE) { $Candidates += (Join-Path $env:USERPROFILE ".switch\prod.keys") }
    if ($env:APPDATA) { $Candidates += @((Join-Path $env:APPDATA "Ryujinx\system\prod.keys"), (Join-Path $env:APPDATA "yuzu\keys\prod.keys"), (Join-Path $env:APPDATA "Eden\keys\prod.keys"), (Join-Path $env:APPDATA "eden\keys\prod.keys")) }
    foreach ($Candidate in $Candidates) { if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $Candidate).Path } }
    throw "prod.keys nao encontrado. Informe -KeysPath ou use %USERPROFILE%\.switch\prod.keys. Use chaves obtidas do seu proprio Switch."
}

function Get-ProgramId {
    param([string]$ExeFs)
    $Npdm = Join-Path $ExeFs "main.npdm"
    if (-not (Test-Path -LiteralPath $Npdm -PathType Leaf)) { return [UInt64]0 }
    $Fs = [System.IO.File]::OpenRead($Npdm)
    try {
        $Meta = New-Object byte[] 0x80
        if ($Fs.Read($Meta, 0, $Meta.Length) -ne $Meta.Length) { return [UInt64]0 }
        if ([System.Text.Encoding]::ASCII.GetString($Meta, 0, 4) -ne "META") { return [UInt64]0 }
        $AciOff = [BitConverter]::ToUInt32($Meta, 0x70)
        $null = $Fs.Seek([Int64]$AciOff, [System.IO.SeekOrigin]::Begin)
        $Aci = New-Object byte[] 0x18
        if ($Fs.Read($Aci, 0, $Aci.Length) -ne $Aci.Length) { return [UInt64]0 }
        if ([System.Text.Encoding]::ASCII.GetString($Aci, 0, 4) -ne "ACI0") { return [UInt64]0 }
        return [BitConverter]::ToUInt64($Aci, 0x10)
    } finally { $Fs.Dispose() }
}


$PackagePath=(Resolve-Path -LiteralPath $PackagePath).Path
$UpdatePath=(Resolve-Path -LiteralPath $UpdatePath).Path
if ([IO.Path]::GetExtension($PackagePath).ToLowerInvariant() -notin @('.nsp','.xci')) { throw 'Base deve ser NSP ou XCI.' }
if ([IO.Path]::GetExtension($UpdatePath).ToLowerInvariant() -ne '.nsp') { throw 'Update deve ser NSP.' }
if ($KeysPath -and (Test-Path -LiteralPath $KeysPath -PathType Container)) { $KeysPath=Join-Path $KeysPath 'prod.keys' }
$Keys=Find-ProdKeys $KeysPath
if (-not $CacheRoot) { $CacheRoot=Join-Path $RepoRoot 'build\native\package-cache' }
New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
$CacheRoot=(Resolve-Path -LiteralPath $CacheRoot).Path
$identity='scarlet-update-v1'
foreach ($p in @($PackagePath,$UpdatePath)) {
 $f=Get-Item -LiteralPath $p
 $identity += "|$($f.FullName)|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)"
}
$sha=[Security.Cryptography.SHA256]::Create()
try { $tag=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
$dest=Join-Path $CacheRoot $tag
$work=$null; $lock=$null
$expectedIds=@()
if ($AotManifest) {
 $manifestText=Get-Content -LiteralPath $AotManifest -Raw
 $null=$manifestText | ConvertFrom-Json
 $expectedIds=@([regex]::Matches($manifestText, '"build_id"\s*:\s*"([0-9a-fA-F]{40,64})"') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant().PadRight(64,'0') })
 if ($expectedIds.Count -lt 3) { throw 'Manifesto AOT sem os Build IDs necessarios.' }
}
function Confirm-Prepared([string]$Root) {
 $exe=Join-Path $Root 'exefs'
 if ((Get-ProgramId $exe) -ne [uint64]0x0100A3D008C5C000) { throw 'NPDM nao identifica Scarlet.' }
 foreach ($name in @('main','sdk','subsdk0')) {
  $s=[IO.File]::OpenRead((Join-Path $exe $name))
  try {
   $h=New-Object byte[] 96
   if ($s.Read($h,0,96) -ne 96 -or [Text.Encoding]::ASCII.GetString($h,0,4) -ne 'NSO0') { throw "NSO invalido: $name" }
   $id=[BitConverter]::ToString($h,64,32).Replace('-','')
   if ($expectedIds.Count -and $expectedIds -notcontains $id.ToLowerInvariant()) { throw "Build ID de $name nao corresponde ao AOT. Boot bloqueado." }
   Write-Host "[package] $name build_id=$id"
  } finally { $s.Dispose() }
 }
 $s=[IO.File]::OpenRead((Join-Path $Root 'romfs.bin'))
 try {
  $h=New-Object byte[] 80
  if ($s.Read($h,0,80) -ne 80 -or [BitConverter]::ToUInt64($h,0) -ne 80) { throw 'RomFS atualizado invalido.' }
  for ($i=8;$i -le 56;$i+=16) {
   $off=[BitConverter]::ToUInt64($h,$i); $size=[BitConverter]::ToUInt64($h,$i+8)
   if ($off -gt $s.Length -or $size -gt $s.Length-$off) { throw 'Tabela RomFS fora dos limites.' }
  }
  if ([BitConverter]::ToUInt64($h,72) -gt $s.Length) { throw 'Dados RomFS fora dos limites.' }
 } finally { $s.Dispose() }
}
try {
 $lock=[IO.File]::Open((Join-Path $CacheRoot "$tag.lock"),'OpenOrCreate','ReadWrite','None')
 if (Test-Path -LiteralPath (Join-Path $dest 'ready.json')) {
  Confirm-Prepared $dest
  Write-Host '[package] Reutilizando cache base + update.'
  Write-Output $dest
  return
 }
 if (Test-Path -LiteralPath $dest) { throw "Cache incompleto preservado: $dest. Use outro -CacheRoot." }
 $work=Join-Path $CacheRoot ('.work-'+[guid]::NewGuid())
 New-Item -ItemType Directory -Path $work | Out-Null
 $log=Join-Path $CacheRoot "$tag.log"
 $Hactool=Find-Hactool $HactoolPath
 function Invoke-Tool([string[]]$Arguments) {
  $old=$ErrorActionPreference
  try {
   $ErrorActionPreference='Continue'
   $lines=@(& $Hactool @Arguments 2>&1)
   $code=$LASTEXITCODE
  } finally { $ErrorActionPreference=$old }
  # Do not log argv or key dumps emitted by hactool.
  $safe=@($lines | ForEach-Object { [string]$_ } | Where-Object { $_ -notmatch '(?i)key|[0-9a-f]{32}' })
  $safe | Out-File -LiteralPath $log -Append -Encoding utf8
  if ($code -ne 0) { throw "hactool falhou ($code). Log sem chaves: $log" }
  return $safe
 }
 function Open-Container([string]$Path,[string]$Label) {
  $out=Join-Path $work $Label
  New-Item -ItemType Directory -Path $out | Out-Null
  $type='pfs0'; $option="--outdir=$out"
  if ([IO.Path]::GetExtension($Path) -ieq '.xci') { $type='xci'; $option="--securedir=$out" }
  Write-Host "[package] Preparando $Label; primeira execucao pode demorar..."
  $null=Invoke-Tool @('-k',$Keys,'--disablekeywarns','-t',$type,$option,$Path)
  return $out
 }
 function Find-Program([string]$Container,[string]$Label,[string]$BaseRom='') {
  $titleKeys=@{}
  foreach ($tik in @(Get-ChildItem -LiteralPath $Container -Recurse -File -Filter '*.tik')) {
   $b=[IO.File]::ReadAllBytes($tik.FullName)
   if ($b.Length -lt 0x2b0 -or [BitConverter]::ToString($b,0,4) -ne '00-01-00-04' -or $b[0x281] -ne 0) { throw 'Ticket nao suportado: esperado common RSA2048/SHA256.' }
   $rid=[BitConverter]::ToString($b,0x2a0,16).Replace('-','').ToLowerInvariant()
   if ($rid -match '^0100a3d008c5c(000|800)') { $titleKeys[$rid]=[BitConverter]::ToString($b,0x180,16).Replace('-','') }
  }
  if ($titleKeys.Count -gt 1) { throw 'Mais de um ticket Scarlet: selecao ambigua.' }
  $toolArgs=@('-k',$Keys,'--disablekeywarns')
  if ($titleKeys.Count -eq 1) { $toolArgs += '--titlekey='+@($titleKeys.Values)[0] }
  if ($BaseRom) { $toolArgs += "--baseromfs=$BaseRom" }
  $found=@()
  foreach ($nca in @(Get-ChildItem -LiteralPath $Container -Recurse -File -Filter '*.nca')) {
   $info=(Invoke-Tool ($toolArgs+@('-i',$nca.FullName))) -join "`n"
   if ($info -notmatch '(?im)^Content Type:\s+Program\s*$' -or $info -notmatch '(?im)^Title ID:\s+0100a3d008c5c(?:000|800)\s*$') { continue }
   $exe=Join-Path $work ($Label+'-probe-'+$nca.BaseName)
   New-Item -ItemType Directory -Path $exe | Out-Null
   $null=Invoke-Tool ($toolArgs+@("--exefsdir=$exe",$nca.FullName))
   if ((Get-ProgramId $exe) -eq [uint64]0x0100A3D008C5C000) { $found += @{ Path=$nca.FullName; Arguments=$toolArgs; ExeFs=$exe } }
  }
  if ($found.Count -ne 1) { throw "Esperado um Program NCA Scarlet em $Label; encontrados $($found.Count). Veja $log" }
  return $found[0]
 }
 $base=Find-Program (Open-Container $PackagePath 'base') 'base'
 $raw=Join-Path $work 'base-romfs.raw'
 Write-Host '[package] Preparando secao base para BKTR...'
 $null=Invoke-Tool ($base.Arguments+@('--raw',"--section1=$raw",$base.Path))
 if (-not (Test-Path -LiteralPath $raw)) { throw 'Secao RomFS base ausente.' }
 $patch=Find-Program (Open-Container $UpdatePath 'update') 'update' $raw
 $out=Join-Path $work 'ready'
 New-Item -ItemType Directory -Path $out | Out-Null
 Write-Host '[package] Aplicando patch BKTR e gravando RomFS atualizado...'
 $rom=Join-Path $out 'romfs.bin'
 $null=Invoke-Tool ($patch.Arguments+@("--section1=$rom",$patch.Path))
 Copy-Item -LiteralPath $patch.ExeFs -Destination (Join-Path $out 'exefs') -Recurse
 Confirm-Prepared $out
 @{ Format=1; Base=$PackagePath; Update=$UpdatePath } | ConvertTo-Json | Set-Content (Join-Path $out 'ready.json') -Encoding UTF8
 Move-Item -LiteralPath $out -Destination $dest
 Write-Host '[package] Cache atualizado pronto. AOT preservado.'
 Write-Output $dest
} finally {
 if ($work -and (Test-Path -LiteralPath $work)) { Remove-Item -LiteralPath $work -Recurse -Force }
 if ($lock) { $lock.Dispose() }
}
