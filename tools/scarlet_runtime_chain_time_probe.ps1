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
    throw "Scarlet chain-time probe: missing $rel"
}

$text = [IO.File]::ReadAllText($path)
if ($text.Contains('[Scarlet.ChainTime] sampled wall-time')) {
    Write-Host '[Scarlet PC] Chain-time probe already applied.' -ForegroundColor DarkGray
    exit 0
}

# Gate and cheap per-thread sequence counter beside the existing PC sampler.
$gatePattern = '(const bool kSamplePc\s*=\s*\[\]\s*\{.*?\}\(\);)'
$gateRegex = [regex]::new(
    $gatePattern,
    [Text.RegularExpressions.RegexOptions]::Singleline
)
if (-not $gateRegex.IsMatch($text)) {
    throw 'Scarlet chain-time probe: sampling gate anchor missing'
}
$gateInsert = @'
$1

// [Scarlet.ChainTime] sampled wall-time
const bool kScarletChainTimeProbe = [] {
    const char* e = std::getenv("SCARLET_CHAIN_TIME_PROBE");
    return e && *e && *e != '0';
}();
thread_local u64 g_scarlet_chain_probe_seq = 0;

u64 ScarletChainProbeNow() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
'@
$text = $gateRegex.Replace($text, $gateInsert.TrimEnd(), 1)

# Add aggregate maps to the existing counter structure.
$mapPattern = '(\s*std::map<u64,\s*u64>\s+sampled_pc;\s*///<\s*sparse samples for zero-transition stalls)'
$mapRegex = [regex]::new($mapPattern)
if (-not $mapRegex.IsMatch($text)) {
    throw 'Scarlet chain-time probe: sampled_pc anchor missing'
}
$mapInsert = @'
$1
    std::map<std::pair<u64, u64>, u64> chain_pair_ns;
    std::map<std::pair<u64, u64>, u64> chain_pair_count;
    std::map<std::pair<u64, u64>, u64> chain_pair_blocks;
'@
$text = $mapRegex.Replace($text, $mapInsert.TrimEnd(), 1)

# Add a recorder after RecordSample, regardless of whether the caller-aware
# hotspot sampler has already changed its signature.
$recordAnchor = '};\r?\n\s*\};\r?\n\};\r?\n\r?\nRecompCounters g_counters;'
$recordRegex = [regex]::new($recordAnchor)
if (-not $recordRegex.IsMatch($text)) {
    # Fall back to the explicit structure tail used by the pinned runtime.
    $recordAnchor2 = '(\s*void RecordSample\(.*?\n\s*\}\r?\n)\};\r?\n\r?\nRecompCounters g_counters;'
    $recordRegex2 = [regex]::new(
        $recordAnchor2,
        [Text.RegularExpressions.RegexOptions]::Singleline
    )
    if (-not $recordRegex2.IsMatch($text)) {
        throw 'Scarlet chain-time probe: RecompCounters tail anchor missing'
    }
    $recordInsert2 = @'
$1
    void RecordChainTime(u64 start_pc, u64 end_pc, u64 ns, u64 blocks) {
        std::scoped_lock lk{hist_lock};
        const auto key = std::pair<u64, u64>{start_pc, end_pc};
        chain_pair_ns[key] += ns;
        chain_pair_count[key] += 1;
        chain_pair_blocks[key] += blocks;
    }
};

RecompCounters g_counters;
'@
    $text = $recordRegex2.Replace($text, $recordInsert2.TrimEnd(), 1)
} else {
    throw 'Scarlet chain-time probe: unexpected counter layout'
}

# Time only one of every 64 host->AOT chain invocations.
$runStart = $text.IndexOf('HaltReason ArmRecomp::RunThread')
$stepStart = $text.IndexOf('HaltReason ArmRecomp::StepThread', $runStart)
if ($runStart -lt 0 -or $stepStart -le $runStart) {
    throw 'Scarlet chain-time probe: RunThread boundaries missing'
}
$prefix = $text.Substring(0, $runStart)
$runText = $text.Substring($runStart, $stepStart - $runStart)
$suffix = $text.Substring($stepStart)

$blockPattern = '(?m)^([ \t]*)block\s*\(\s*&impl->ctx\s*\)\s*;'
$blockRegex = [regex]::new(
    $blockPattern,
    [Text.RegularExpressions.RegexOptions]::Multiline
)
if (-not $blockRegex.IsMatch($runText)) {
    throw 'Scarlet chain-time probe: static block call missing'
}

$timedCall = @'
$1const bool scarlet_chain_sample =
$1    kScarletChainTimeProbe && ((++g_scarlet_chain_probe_seq & 63ULL) == 0);
$1const u64 scarlet_chain_start_pc = scarlet_chain_sample ? impl->ctx.pc : 0;
$1const int scarlet_chain_start_budget = scarlet_chain_sample ? impl->ctx.chain_budget : 0;
$1const u64 scarlet_chain_start_ns = scarlet_chain_sample ? ScarletChainProbeNow() : 0;
$1block(&impl->ctx);
$1if (scarlet_chain_sample) {
$1    const u64 scarlet_chain_ns = ScarletChainProbeNow() - scarlet_chain_start_ns;
$1    const int raw_spent = scarlet_chain_start_budget - impl->ctx.chain_budget;
$1    const u64 scarlet_chain_blocks =
$1        raw_spent > 0 ? static_cast<u64>(raw_spent) : 1ULL;
$1    g_counters.RecordChainTime(
$1        scarlet_chain_start_pc, impl->ctx.pc, scarlet_chain_ns, scarlet_chain_blocks);
$1}
'@
$runText = $blockRegex.Replace($runText, $timedCall.TrimEnd(), 1)
$text = $prefix + $runText + $suffix

# Add report before the uncovered-PC section.
$reportAnchor = @'
    if (!g_counters.miss_pc.empty()) {
'@
if (-not $text.Contains($reportAnchor.Trim())) {
    throw 'Scarlet chain-time probe: report anchor missing'
}
$reportInsert = @'
    if (!g_counters.chain_pair_ns.empty()) {
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

        o += "  --- sampled AOT chain wall-time by total time ---\n";
        for (const auto& [pair, ns] : TopN(g_counters.chain_pair_ns, 64)) {
            const u64 count = g_counters.chain_pair_count[pair];
            const u64 blocks = g_counters.chain_pair_blocks[pair];
            const double total_ms = static_cast<double>(ns) * 1.0e-6;
            const double avg_us = count ? static_cast<double>(ns) / count * 1.0e-3 : 0.0;
            const double ns_per_block = blocks ? static_cast<double>(ns) / blocks : 0.0;
            o += fmt::format(
                "    {:>10.3f} ms  n={:<7} avg={:>8.2f} us  {:>8.1f} ns/block  {} -> {}\n",
                total_ms, count, avg_us, ns_per_block,
                resolve(pair.first), resolve(pair.second));
        }
        o += fmt::format("    {} distinct sampled chain pairs\n",
                         g_counters.chain_pair_ns.size());
    }

    if (!g_counters.miss_pc.empty()) {
'@
$text = $text.Replace($reportAnchor.Trim(), $reportInsert.Trim())

[IO.File]::WriteAllText($path, $text, $utf8)
Write-Host '[Scarlet PC] Chain-time probe: sampled wall-time enabled via SCARLET_CHAIN_TIME_PROBE=1.' -ForegroundColor Green
