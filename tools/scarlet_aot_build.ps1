param(
    [Parameter(Mandatory = $true)]
    [string]$ExportRoot,

    [string]$OutputDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\aot\modules'),

    [ValidateRange(1, 4)]
    [int]$Jobs = 1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$export = (Resolve-Path $ExportRoot).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$output = (Resolve-Path $OutputDir).Path

$buildScript = Get-ChildItem $export -Recurse -File -Filter 'build_native_windows.cmd' |
    Sort-Object FullName |
    Select-Object -First 1

if (-not $buildScript) {
    throw "build_native_windows.cmd was not found under: $export"
}

Write-Host "[Scarlet PC] AOT source export: $export" -ForegroundColor Cyan
Write-Host "[Scarlet PC] Build script: $($buildScript.FullName)" -ForegroundColor Cyan
Write-Host "[Scarlet PC] Parallel compiler jobs: $Jobs" -ForegroundColor Cyan

$oldParallel = $env:CMAKE_BUILD_PARALLEL_LEVEL
Push-Location $buildScript.DirectoryName
try {
    $env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"
    & $buildScript.FullName

    if ($LASTEXITCODE -ne 0) {
        throw "AOT module build failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
    $env:CMAKE_BUILD_PARALLEL_LEVEL = $oldParallel
}

$expected = [ordered]@{
    'recompiled_image.dll'   = 'main'
    'recompiled_rtld.dll'    = 'rtld'
    'recompiled_subsdk0.dll' = 'subsdk0'
    'recompiled_sdk.dll'     = 'sdk'
}

$manifestModules = @()

foreach ($name in $expected.Keys) {
    $dll = Get-ChildItem $export -Recurse -File -Filter $name |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $dll) {
        throw "Expected AOT module was not produced: $name"
    }

    $destination = Join-Path $output $name
    Copy-Item $dll.FullName $destination -Force

    $copied = Get-Item $destination
    $manifestModules += [pscustomobject]@{
        name   = $name
        module = $expected[$name]
        bytes  = $copied.Length
        source = $dll.FullName
    }

    Write-Host ("[OK] {0} ({1:N1} MB)" -f $name, ($copied.Length / 1MB)) -ForegroundColor Green
}

[ordered]@{
    format           = 1
    project          = 'Scarlet PC'
    title_id         = '0100A3D008C5C000'
    architecture     = 'x86-64'
    backend          = 'Hybrid AOT shared modules'
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    modules          = $manifestModules
} |
    ConvertTo-Json -Depth 6 |
    Set-Content (Join-Path $output 'scarlet-aot-manifest.json') -Encoding UTF8

Write-Host ""
Write-Host "[Scarlet PC] AOT modules ready in: $output" -ForegroundColor Green
