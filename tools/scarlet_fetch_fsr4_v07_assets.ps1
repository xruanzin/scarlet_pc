param(
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\fsr4-v07-assets'),
    [ValidateSet('1080','2160','both')]
    [string]$Tiers = 'both'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Same source-v07 asset source used by the GoW3 NVIDIA fork.
$commit = 'ae8d628fae208813172446d1e49ed94150b04658'
$base = "https://raw.githubusercontent.com/FireBurn/Q2RTX/$commit/baseq2/fsr4_shaders"
$selectedTiers = if ($Tiers -eq 'both') { @('1080','2160') } else { @($Tiers) }
$models = @('native','quality','balanced','performance','ultraperf','drs')

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$files = [System.Collections.Generic.List[string]]::new()
$files.Add('LICENSE-FSR4-v07.txt')
$files.Add('rcas.spv')
$files.Add('spd_auto_exposure.spv')

foreach ($model in $models) {
    $files.Add("fsr4_model_v07_i8_$($model)_initializers.bin")
    $files.Add("fsr4_model_v07_i8_$($model)_pre_weights.bin")
    $files.Add("fsr4_model_v07_i8_$($model)_shader_manifest.json")
    foreach ($tier in $selectedTiers) {
        $files.Add("fsr4_model_v07_i8_$($model)_$($tier)_pre.spv")
        $files.Add("fsr4_model_v07_i8_$($model)_$($tier)_post.spv")
        foreach ($pass in 1..12) {
            $files.Add("fsr4_model_v07_i8_$($model)_$($tier)_pass$pass.spv")
        }
    }
}

$downloaded = 0
foreach ($file in $files) {
    $target = Join-Path $Destination $file
    if (Test-Path $target -PathType Leaf -and (Get-Item $target).Length -gt 0) {
        continue
    }
    $part = "$target.part"
    Remove-Item $part -Force -ErrorAction SilentlyContinue
    Write-Host "[Scarlet PC] FSR4 v07 asset: $file" -ForegroundColor DarkGray
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$file" -OutFile $part
    if (-not (Test-Path $part -PathType Leaf) -or (Get-Item $part).Length -eq 0) {
        throw "FSR4 v07 asset download failed: $file"
    }
    Move-Item $part $target -Force
    $downloaded++
}

$license = Join-Path $Destination 'LICENSE-FSR4-v07.txt'
if (-not (Test-Path $license -PathType Leaf)) {
    throw 'FSR4 v07 asset license is missing; refusing to stage the model bundle.'
}

[ordered]@{
    provider = 'FireBurn/FSR-Vulkan source-v07'
    asset_source = 'FireBurn/Q2RTX'
    source_commit = $commit
    tiers = $selectedTiers
    files = $files.Count
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Destination 'scarlet-fsr4-v07-assets.json') -Encoding UTF8

Write-Host "[Scarlet PC] FSR4 v07 assets ready: $Destination" -ForegroundColor Green
Write-Host "[Scarlet PC] $($files.Count) files, $downloaded downloaded in this run." -ForegroundColor DarkGray
Write-Warning 'This is the experimental source-v07 FSR4 provider asset set, not AMD FSR 4.1.1.'
