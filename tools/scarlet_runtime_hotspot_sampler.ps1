param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = New-Object System.Text.UTF8Encoding($false)

$rel = 'src\core\arm\recomp\arm_recomp.cpp'
$path = Join-Path $SourceDir $rel
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Scarlet hotspot sampler: missing $rel"
}

$text = [IO.File]::ReadAllText($path)
if ($text.Contains('[Scarlet.HotspotSampler] PC + LR histogram')) {
    Write-Host '[Scarlet PC] Hotspot sampler already applied.' -ForegroundColor DarkGray
    exit 0
}

# Add caller-aware histograms beside the existing sampled-PC map.
$mapPattern = '(\s*std::map<u64,\s*u64>\s+sampled_pc;\s*///<\s*sparse samples for zero-transition stalls)'
$mapRegex = [regex]::new($mapPattern)
if (-not $mapRegex.IsMatch($text)) {
    throw 'Scarlet hotspot sampler: sampled_pc map anchor missing'
}
$mapInsert = @'
$1
    // [Scarlet.HotspotSampler] PC + LR histogram
    // PC alone shows where a long static chain parks. LR tells us which caller
    // brought execution there, which is what we need for SDK-heavy hotspots.
    std::map<u64, u64> sampled_lr;
    std::map<std::pair<u64, u64>, u64> sampled_pc_lr;
'@
$text = $mapRegex.Replace($text, $mapInsert.TrimEnd(), 1)

# Replace the one-field recorder with a caller-aware recorder. Sampling is very
# sparse (controlled by SUYU_RECOMP_SAMPLE_SHIFT), so the extra two map bumps
# do not touch the per-block hot path.
$recordPattern =
    '(?s)void\s+RecordSample\s*\(\s*u64\s+pc\s*\)\s*\{\s*' +
    'std::scoped_lock\s+lk\s*\{\s*hist_lock\s*\}\s*;\s*' +
    '\+\+sampled_pc\s*\[\s*pc\s*\]\s*;\s*\}'
$recordRegex = [regex]::new(
    $recordPattern,
    [Text.RegularExpressions.RegexOptions]::Singleline
)
if (-not $recordRegex.IsMatch($text)) {
    throw 'Scarlet hotspot sampler: RecordSample anchor missing'
}
$recordNew = @'
void RecordSample(u64 pc, u64 lr) {
        std::scoped_lock lk{hist_lock};
        ++sampled_pc[pc];
        ++sampled_lr[lr];
        ++sampled_pc_lr[{pc, lr}];
    }
'@
$text = $recordRegex.Replace($text, $recordNew.TrimEnd(), 1)

# Pass x30/LR at the existing sample point.
$callPattern = 'g_counters\.RecordSample\s*\(\s*impl->ctx\.pc\s*\)\s*;'
$callRegex = [regex]::new($callPattern)
if (-not $callRegex.IsMatch($text)) {
    throw 'Scarlet hotspot sampler: RecordSample call anchor missing'
}
$text = $callRegex.Replace(
    $text,
    'g_counters.RecordSample(impl->ctx.pc, impl->ctx.x[30]);',
    1
)

# Add two report sections immediately after the existing sampled-PC section.
# Keep the original report intact so old tooling remains compatible.
$reportAnchor = @'
    if (!g_counters.miss_pc.empty()) {
'@
if (-not $text.Contains($reportAnchor.Trim())) {
    throw 'Scarlet hotspot sampler: report insertion anchor missing'
}

$reportInsert = @'
    if (!g_counters.sampled_pc_lr.empty()) {
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("{}+{:#x}", *name, pc - best)
                        : fmt::format("{:#x}", pc);
        };

        o += "  --- sampled hotspot -> LR/caller pairs ---\n";
        for (const auto& [pair, count] : TopN(g_counters.sampled_pc_lr, 64)) {
            const auto [pc, lr] = pair;
            o += fmt::format("    {:>10}  {}  <-  {}\n",
                             count, resolve(pc), resolve(lr));
        }
        o += fmt::format("    {} distinct PC/LR pairs\n",
                         g_counters.sampled_pc_lr.size());
    }

    if (!g_counters.sampled_lr.empty()) {
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("{}+{:#x}", *name, pc - best)
                        : fmt::format("{:#x}", pc);
        };

        o += "  --- sampled LR/callers by count ---\n";
        for (const auto& [lr, count] : TopN(g_counters.sampled_lr, 48)) {
            o += fmt::format("    {:>10}  {}\n", count, resolve(lr));
        }
        o += fmt::format("    {} distinct sampled LRs\n",
                         g_counters.sampled_lr.size());
    }

    if (!g_counters.miss_pc.empty()) {
'@
$text = $text.Replace($reportAnchor.Trim(), $reportInsert.Trim())

[IO.File]::WriteAllText($path, $text, $utf8)
Write-Host '[Scarlet PC] Hotspot sampler: PC + LR/caller histograms enabled.' -ForegroundColor Green
