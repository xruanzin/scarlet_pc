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
        $TextureOut = Join-Path $tmp 'textures'
        Copy-Item -LiteralPath $TextureDir -Destination $TextureOut -Recurse

        # Build a compact asset index so one capture can be inspected without
        # manually correlating texture_N/texture.txt directories with draw lines.
        $ForensicLines = Get-Content -LiteralPath $forensics.FullName
        $rows = @()
        Get-ChildItem -LiteralPath $TextureOut -Filter 'texture.txt' -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object FullName |
            ForEach-Object {
                $meta = @{}
                Get-Content -LiteralPath $_.FullName | ForEach-Object {
                    if ($_ -match '^([^=]+)=(.*)

# Keep only passes from the same capture. Older F9 runs leave pass_*.png
# behind in build/, which previously mixed unrelated frames in the ZIP.
$cutoff = $forensics.LastWriteTimeUtc.AddSeconds(-8)
$StartLine = Select-String -LiteralPath $forensics.FullName -Pattern '^# capture_started_unix_s=([0-9]{1,11})$' | Select-Object -First 1
if ($StartLine) {
    $started = [DateTimeOffset]::FromUnixTimeSeconds([long]$StartLine.Matches[0].Groups[1].Value).UtcDateTime
    if ($started -le $forensics.LastWriteTimeUtc -and $started -ge $forensics.LastWriteTimeUtc.AddHours(-1)) {
        $cutoff = $started.AddSeconds(-1)
    }
}
$passes = Get-ChildItem $build -Filter "pass_*.png" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTimeUtc -ge $cutoff -and $_.LastWriteTimeUtc -le $forensics.LastWriteTimeUtc.AddSeconds(3) } |
    Sort-Object Name
if ($passes) {
    $passDir = Join-Path $tmp "passes"
    New-Item -ItemType Directory -Force -Path $passDir | Out-Null
    $passes | ForEach-Object { Copy-Item $_.FullName (Join-Path $passDir $_.Name) }
}

# LARECOMP_FORENSICS_DEPTH=1 writes depth/snapshot PGM files next to the pass
# captures. Package only files from this F9 capture, using the same timestamp
# window as pass_*.png, so DoF/depth regressions can be diagnosed from one ZIP.
$depths = Get-ChildItem $build -Filter "fbdepth_*.pgm" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTimeUtc -ge $cutoff -and $_.LastWriteTimeUtc -le $forensics.LastWriteTimeUtc.AddSeconds(3) } |
    Sort-Object Name
if ($depths) {
    $depthDir = Join-Path $tmp "depth"
    New-Item -ItemType Directory -Force -Path $depthDir | Out-Null
    $depths | ForEach-Object { Copy-Item $_.FullName (Join-Path $depthDir $_.Name) }
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
) { $meta[$matches[1]] = $matches[2] }
                }
                $addr = $meta['addr']
                $uses = @()
                if ($addr) {
                    for ($li = 0; $li -lt $ForensicLines.Count; $li++) {
                        $line = $ForensicLines[$li]
                        if ($line -match ('(?:^| )((?:v)?t[0-9]+)=' + [regex]::Escape($addr) + '/')) {
                            $slot = $matches[1]
                            $fs = if ($line -match ' fs=([0-9a-fA-F]+)') { $matches[1] } else { '?' }
                            $uses += ('L{0}:{1}:fs={2}' -f ($li + 1), $slot, $fs)
                            if ($uses.Count -ge 24) { break }
                        }
                    }
                }
                $raw = Get-ChildItem -LiteralPath $_.Directory.FullName -Filter '*.bin' -File -ErrorAction SilentlyContinue |
                    Select-Object -First 1
                $rows += [pscustomobject]@{
                    id = $meta['id']
                    addr = $addr
                    format = $meta['format']
                    width = $meta['width']
                    height = $meta['height']
                    depth = $meta['depth']
                    layers = $meta['layers']
                    levels = $meta['levels']
                    target = $meta['target']
                    swizzle = $meta['swizzle']
                    status = $meta['status']
                    captured_bytes = $meta['captured_bytes']
                    raw_file = if ($raw) { $raw.Name } else { '' }
                    uses = ($uses -join ';')
                }
            }
        if ($rows.Count) {
            $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $tmp 'texture-index.csv')

            $summary = Join-Path $tmp 'ASSET_CAPTURE_README.txt'
            @(
                'Scarlet material asset capture',
                '',
                'texture-index.csv maps captured raw texture sources to the draw slots/shaders that sampled them.',
                'textures/texture_<id>/texture.txt contains original NVN metadata.',
                'textures/texture_<id>/*.bin is the CPU-side raw guest texture source when available.',
                'forensics-shaders/ contains translated GLSL for shaders used by the captured frame.',
                'passes/ and pass_mrt_* images show intermediate render/G-buffer outputs from the same F9 capture.',
                '',
                'Format values are NVN guest format IDs; decode only after checking texture.txt layout metadata.'
            ) | Set-Content -Encoding UTF8 -LiteralPath $summary
        }
    }
}


# Keep only passes from the same capture. Older F9 runs leave pass_*.png
# behind in build/, which previously mixed unrelated frames in the ZIP.
$cutoff = $forensics.LastWriteTimeUtc.AddSeconds(-8)
$StartLine = Select-String -LiteralPath $forensics.FullName -Pattern '^# capture_started_unix_s=([0-9]{1,11})$' | Select-Object -First 1
if ($StartLine) {
    $started = [DateTimeOffset]::FromUnixTimeSeconds([long]$StartLine.Matches[0].Groups[1].Value).UtcDateTime
    if ($started -le $forensics.LastWriteTimeUtc -and $started -ge $forensics.LastWriteTimeUtc.AddHours(-1)) {
        $cutoff = $started.AddSeconds(-1)
    }
}
$passes = Get-ChildItem $build -Filter "pass_*.png" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTimeUtc -ge $cutoff -and $_.LastWriteTimeUtc -le $forensics.LastWriteTimeUtc.AddSeconds(3) } |
    Sort-Object Name
if ($passes) {
    $passDir = Join-Path $tmp "passes"
    New-Item -ItemType Directory -Force -Path $passDir | Out-Null
    $passes | ForEach-Object { Copy-Item $_.FullName (Join-Path $passDir $_.Name) }
}

# LARECOMP_FORENSICS_DEPTH=1 writes depth/snapshot PGM files next to the pass
# captures. Package only files from this F9 capture, using the same timestamp
# window as pass_*.png, so DoF/depth regressions can be diagnosed from one ZIP.
$depths = Get-ChildItem $build -Filter "fbdepth_*.pgm" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTimeUtc -ge $cutoff -and $_.LastWriteTimeUtc -le $forensics.LastWriteTimeUtc.AddSeconds(3) } |
    Sort-Object Name
if ($depths) {
    $depthDir = Join-Path $tmp "depth"
    New-Item -ItemType Directory -Force -Path $depthDir | Out-Null
    $depths | ForEach-Object { Copy-Item $_.FullName (Join-Path $depthDir $_.Name) }
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
