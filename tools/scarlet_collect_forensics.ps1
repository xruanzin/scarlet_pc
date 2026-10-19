param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"
$build = Join-Path $Root "build"
if (-not (Test-Path $build)) {
    throw "build directory not found: $build"
}

$forensics = Get-ChildItem $build -Filter "scarlet_forensics_*.txt" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $forensics) {
    throw "No scarlet_forensics_*.txt found. Run the game, press F9 once on the broken scene, wait a few seconds, then close it."
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = Join-Path $Root "scarlet-forensics-$stamp.zip"
$tmp = Join-Path $env:TEMP "scarlet-forensics-$stamp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

Copy-Item $forensics.FullName (Join-Path $tmp $forensics.Name)

# Keep only passes from the same capture. Older F9 runs leave pass_*.png
# behind in build/, which previously mixed unrelated frames in the ZIP.
$cutoff = $forensics.LastWriteTime.AddSeconds(-8)
$passes = Get-ChildItem $build -Filter "pass_*.png" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $cutoff -and $_.LastWriteTime -le $forensics.LastWriteTime.AddSeconds(3) } |
    Sort-Object Name
if ($passes) {
    $passDir = Join-Path $tmp "passes"
    New-Item -ItemType Directory -Force -Path $passDir | Out-Null
    $passes | ForEach-Object { Copy-Item $_.FullName (Join-Path $passDir $_.Name) }
}

$gpuProfile = Join-Path $build "scarlet_gpuprof_latest.txt"
if (Test-Path $gpuProfile) {
    Copy-Item $gpuProfile (Join-Path $tmp "scarlet_gpuprof_latest.txt")
}

$shaderDir = Join-Path $build "forensics-shaders"
if (Test-Path $shaderDir) {
    Copy-Item $shaderDir (Join-Path $tmp "forensics-shaders") -Recurse
}

$log = Get-ChildItem $Root -Filter "scarlet_boot*.log" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($log) { Copy-Item $log.FullName (Join-Path $tmp $log.Name) }

Compress-Archive -Path (Join-Path $tmp "*") -DestinationPath $out -CompressionLevel Optimal
Remove-Item $tmp -Recurse -Force
Write-Host "FORENSICS_ZIP=$out"
