param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"

$Candidates = @((Join-Path $Root "build"))
$BootLog = Join-Path $Root "scarlet_boot.log"

if (Test-Path -LiteralPath $BootLog) {
    $DataLine = Select-String -LiteralPath $BootLog -Pattern "^\[paths\] data: (.+)$" | Select-Object -First 1
    if ($DataLine) {
        $Candidates += Join-Path $DataLine.Matches[0].Groups[1].Value.Trim() "build"
    }
}

$forensics = @(
    $Candidates |
        Select-Object -Unique |
        ForEach-Object {
            Get-ChildItem -LiteralPath $_ -Filter "scarlet_forensics_*.txt" -File -ErrorAction SilentlyContinue
        }
) | Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $forensics) {
    throw "No scarlet_forensics_*.txt found. Run the game, press F9 once on the broken scene, wait a few seconds, then close it."
}

$build = $forensics.Directory.FullName
$stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$out = Join-Path $Root "scarlet-forensics-$stamp.zip"
$tmp = Join-Path $env:TEMP "scarlet-forensics-$stamp"

New-Item -ItemType Directory -Force -Path $tmp | Out-Null
Copy-Item -LiteralPath $forensics.FullName -Destination (Join-Path $tmp $forensics.Name)

# Exact per-frame texture-source directory recorded by the runtime.
$TextureLine = Select-String -LiteralPath $forensics.FullName -Pattern "^# texture_sources=(build/forensics-textures/frame_[0-9]+_[0-9]+)$" | Select-Object -First 1

if ($TextureLine) {
    $RuntimeRoot = Split-Path -Parent $build
    $TextureRel = $TextureLine.Matches[0].Groups[1].Value
    $TextureDir = Join-Path $RuntimeRoot $TextureRel

    if (Test-Path -LiteralPath $TextureDir -PathType Container) {
        $TextureOut = Join-Path $tmp "textures"
        Copy-Item -LiteralPath $TextureDir -Destination $TextureOut -Recurse

        # Correlate each captured texture with draw slots and fragment shaders.
        $ForensicLines = @(Get-Content -LiteralPath $forensics.FullName)
        $rows = @()
        $metaFiles = @(
            Get-ChildItem -LiteralPath $TextureOut -Filter "texture.txt" -File -Recurse -ErrorAction SilentlyContinue |
                Sort-Object FullName
        )

        foreach ($metaFile in $metaFiles) {
            $meta = @{}

            foreach ($metaLine in (Get-Content -LiteralPath $metaFile.FullName)) {
                if ($metaLine -match "^([^=]+)=(.*)$") {
                    $meta[$matches[1]] = $matches[2]
                }
            }

            $addr = $meta["addr"]
            $uses = @()

            if ($addr) {
                $addrEsc = [regex]::Escape($addr)

                for ($li = 0; $li -lt $ForensicLines.Count; $li++) {
                    $line = $ForensicLines[$li]

                    if ($line -match ("(?:^| )((?:v)?t[0-9]+)=" + $addrEsc + "/")) {
                        $slot = $matches[1]
                        $fs = "?"

                        if ($line -match " fs=([0-9a-fA-F]+)") {
                            $fs = $matches[1]
                        }

                        $uses += ("L{0}:{1}:fs={2}" -f ($li + 1), $slot, $fs)

                        if ($uses.Count -ge 24) {
                            break
                        }
                    }
                }
            }

            $raw = Get-ChildItem -LiteralPath $metaFile.Directory.FullName -Filter "*.bin" -File -ErrorAction SilentlyContinue | Select-Object -First 1

            $rows += [pscustomobject]@{
                id             = $meta["id"]
                addr           = $addr
                format         = $meta["format"]
                width          = $meta["width"]
                height         = $meta["height"]
                depth          = $meta["depth"]
                layers         = $meta["layers"]
                levels         = $meta["levels"]
                target         = $meta["target"]
                swizzle        = $meta["swizzle"]
                status         = $meta["status"]
                captured_bytes = $meta["captured_bytes"]
                raw_file       = if ($raw) { $raw.Name } else { "" }
                uses           = ($uses -join ";")
            }
        }

        if ($rows.Count -gt 0) {
            $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $tmp "texture-index.csv")

            @(
                "Scarlet material asset capture",
                "",
                "texture-index.csv maps captured raw texture sources to draw slots and fragment shaders.",
                "textures/texture_<id>/texture.txt contains original NVN texture metadata.",
                "textures/texture_<id>/*.bin is the CPU-side raw guest texture source when available.",
                "forensics-shaders/ contains translated GLSL for shaders used by the captured frame.",
                "passes/ contains intermediate render targets from the same F9 capture.",
                "",
                "Format values are NVN guest format IDs; decode only after checking texture.txt layout metadata."
            ) | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $tmp "ASSET_CAPTURE_README.txt")
        }
    }
}

# Keep only pass images from this capture.
$cutoff = $forensics.LastWriteTimeUtc.AddSeconds(-8)
$StartLine = Select-String -LiteralPath $forensics.FullName -Pattern "^# capture_started_unix_s=([0-9]{1,11})$" | Select-Object -First 1

if ($StartLine) {
    $started = [DateTimeOffset]::FromUnixTimeSeconds([long]$StartLine.Matches[0].Groups[1].Value).UtcDateTime

    if ($started -le $forensics.LastWriteTimeUtc -and $started -ge $forensics.LastWriteTimeUtc.AddHours(-1)) {
        $cutoff = $started.AddSeconds(-1)
    }
}

$passes = @(
    Get-ChildItem -LiteralPath $build -Filter "pass_*.png" -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.LastWriteTimeUtc -ge $cutoff -and
            $_.LastWriteTimeUtc -le $forensics.LastWriteTimeUtc.AddSeconds(3)
        } |
        Sort-Object Name
)

if ($passes.Count -gt 0) {
    $passDir = Join-Path $tmp "passes"
    New-Item -ItemType Directory -Force -Path $passDir | Out-Null

    foreach ($pass in $passes) {
        Copy-Item -LiteralPath $pass.FullName -Destination (Join-Path $passDir $pass.Name)
    }
}

$depths = @(
    Get-ChildItem -LiteralPath $build -Filter "fbdepth_*.pgm" -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.LastWriteTimeUtc -ge $cutoff -and
            $_.LastWriteTimeUtc -le $forensics.LastWriteTimeUtc.AddSeconds(3)
        } |
        Sort-Object Name
)

if ($depths.Count -gt 0) {
    $depthDir = Join-Path $tmp "depth"
    New-Item -ItemType Directory -Force -Path $depthDir | Out-Null

    foreach ($depth in $depths) {
        Copy-Item -LiteralPath $depth.FullName -Destination (Join-Path $depthDir $depth.Name)
    }
}

$gpuProfile = Join-Path $build "scarlet_gpuprof_latest.txt"
if (Test-Path -LiteralPath $gpuProfile) {
    Copy-Item -LiteralPath $gpuProfile -Destination (Join-Path $tmp "scarlet_gpuprof_latest.txt")
}

$shaderDir = Join-Path $build "forensics-shaders"
if (Test-Path -LiteralPath $shaderDir -PathType Container) {
    Copy-Item -LiteralPath $shaderDir -Destination (Join-Path $tmp "forensics-shaders") -Recurse
}

$log = Get-ChildItem -LiteralPath $Root -Filter "scarlet_boot*.log" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($log) {
    Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $tmp $log.Name)
}

Compress-Archive -Path (Join-Path $tmp "*") -DestinationPath $out -CompressionLevel Optimal
Remove-Item -LiteralPath $tmp -Recurse -Force
Write-Host "FORENSICS_ZIP=$out"
