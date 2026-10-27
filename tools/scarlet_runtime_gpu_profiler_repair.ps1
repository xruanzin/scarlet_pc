param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$path = Join-Path $SourceDir 'src\video_core\gpu_thread.cpp'
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Scarlet GPU profiler repair: missing $path"
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$text = [IO.File]::ReadAllText($path)
$before = $text

# A locally-preserved temporal profiler revision tried to call size() directly
# on Tegra::CommandList. CommandList is a pair of small_vectors, not a container
# itself. Preserve the metric but count both underlying vectors instead.
$text = [regex]::Replace(
    $text,
    '([A-Za-z0-9_>\.]+entries)\s*\.\s*size\s*\(\s*\)',
    '($1.command_lists.size() + $1.prefetch_command_list.size())'
)

# Also repair common spelling when the profiler first aliases the CommandList.
$text = [regex]::Replace(
    $text,
    '([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*size\s*\(\s*\)(?=\s*[;,)])',
    {
        param($m)
        $name = $m.Groups[1].Value
        if ($name -match '^(entries|command_list|list|commands)$') {
            return "($name.command_lists.size() + $name.prefetch_command_list.size())"
        }
        return $m.Value
    }
)

if ($text -ne $before) {
    [IO.File]::WriteAllText($path, $text, $utf8)
    Write-Host '[Scarlet PC] Repaired temporal GPU profiler CommandList sizing.' -ForegroundColor Green
} else {
    Write-Host '[Scarlet PC] GPU profiler CommandList sizing already valid.' -ForegroundColor DarkGray
}
