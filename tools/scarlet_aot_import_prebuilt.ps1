param(
    [Parameter(Mandatory = $true)]
    [string]$ExportRoot,

    [string]$AotDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\aot\modules'),

    [switch]$Repackage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$export = (Resolve-Path $ExportRoot).Path
New-Item -ItemType Directory -Force -Path $AotDir | Out-Null
$aot = (Resolve-Path $AotDir).Path

$expected = [ordered]@{
    'recompiled_image.dll'   = 'main'
    'recompiled_rtld.dll'    = 'rtld'
    'recompiled_subsdk0.dll' = 'subsdk0'
    'recompiled_sdk.dll'     = 'sdk'
}

$expectedMainBuildId = '421c5411b487eb4d'
$probeFiles = @(
    (Join-Path $export 'aot_cache\exefs\recomp_registration.c'),
    (Join-Path $export 'aot_cache\exefs\main\recomp_export.c'),
    (Join-Path $export 'aot_cache\exefs\main\recomp_runtime.h')
)
$buildIdConfirmed = $false
foreach ($probe in $probeFiles) {
    if (Test-Path $probe -PathType Leaf) {
        $raw = Get-Content $probe -Raw -ErrorAction SilentlyContinue
        if ($raw -and $raw.ToLowerInvariant().Contains($expectedMainBuildId)) {
            $buildIdConfirmed = $true
            break
        }
    }
}
if ($buildIdConfirmed) {
    Write-Host "[Scarlet PC] Export identifies Scarlet 4.0.0 main build $expectedMainBuildId." -ForegroundColor Green
}
else {
    Write-Warning "[Scarlet PC] Could not confirm the 4.0.0 build ID from export text; the next runtime log must bind main build 421c5411..."
}

$found = @{}
foreach ($name in $expected.Keys) {
    $dll = Get-ChildItem $export -Recurse -File -Filter $name -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $dll) {
        throw "Prebuilt AOT module not found under export: $name"
    }
    $found[$name] = $dll
}

Write-Host '[Scarlet PC] Importing already-built AOT modules (no recompilation).' -ForegroundColor Cyan
foreach ($name in $expected.Keys) {
    $src = $found[$name]
    $dst = Join-Path $aot $name
    Copy-Item $src.FullName $dst -Force
    $copied = Get-Item $dst
    Write-Host ("[OK] {0,-26} {1,8:N1} MB <- {2}" -f $name, ($copied.Length / 1MB), $src.FullName) -ForegroundColor Green
}

[ordered]@{
    format           = 2
    project          = 'Scarlet PC'
    title_id         = '0100A3D008C5C000'
    architecture     = 'x86-64'
    backend          = 'Hybrid AOT shared modules'
    imported_prebuilt = $true
    export_root      = $export
    imported_at_utc  = [DateTime]::UtcNow.ToString('o')
    modules          = @(
        foreach ($name in $expected.Keys) {
            $dst = Get-Item (Join-Path $aot $name)
            [ordered]@{
                name = $name
                module = $expected[$name]
                bytes = $dst.Length
                source = $found[$name].FullName
            }
        }
    )
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $aot 'scarlet-aot-manifest.json') -Encoding UTF8

if ($Repackage) {
    & (Join-Path $PSScriptRoot 'scarlet_independent_package.ps1')
}

Write-Host ''
Write-Host "[Scarlet PC] Prebuilt AOT modules imported into: $aot" -ForegroundColor Green
if (-not $Repackage) {
    Write-Host "[Scarlet PC] Run tools\scarlet_independent_package.ps1 to refresh the package." -ForegroundColor DarkGray
}
