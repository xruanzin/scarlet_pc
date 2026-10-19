param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"
$Candidates = @((Join-Path $Root "build"))
$BootLog = Join-Path $Root 'scarlet_boot.log'
if (Test-Path -LiteralPath $BootLog) {
    $DataLine = Select-String -LiteralPath $BootLog -Pattern '^\[paths\] data: (.+)$' | Select-Object -First 1
    if ($DataLine) {
        $Candidates += Join-Path $DataLine.Matches[0].Groups[1].Value.Trim() 'build'
    }
}
$forensics = @($Candidates | Select-Object -Unique | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -Filter "scarlet_forensics_*.txt" -File -ErrorAction SilentlyContinue
}) | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $forensics) {
    throw "No scarlet_forensics_*.txt found. Run the game, press F9 once on the broken scene, wait a few seconds, then close it."
}

$build = $forensics.Directory.FullName

$stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$out = Join-Path $Root "scarlet-forensics-$stamp.zip"
$tmp = Join-Path $env:TEMP "scarlet-forensics-$stamp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

Copy-Item $forensics.FullName (Join-Path $tmp $forensics.Name)
# Exact per-frame source directory recorded by the runtime, not a glob of
# textures left by earlier runs. Restrict the relative path to our own format.
$TextureLine = Select-String -LiteralPath $forensics.FullName -Pattern '^# texture_sources=(build/forensics-textures/frame_[0-9]+_[0-9]+)$' | Select-Object -First 1
if ($TextureLine) {
    $RuntimeRoot = Split-Path -Parent $build
    $TextureDir = Join-Path $RuntimeRoot $TextureLine.Matches[0].Groups[1].Value
    if (Test-Path -LiteralPath $TextureDir -PathType Container) {
        Copy-Item -LiteralPath $TextureDir -Destination (Join-Path $tmp 'textures') -Recurse
    }
}


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
