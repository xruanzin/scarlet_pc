param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = New-Object System.Text.UTF8Encoding($false)

$cmdCandidates = @(
    (Join-Path $SourceDir 'src\suyu_cmd\suyu.cpp'),
    (Join-Path $SourceDir 'src\yuzu_cmd\yuzu.cpp'),
    (Join-Path $SourceDir 'src\runtime_cmd\runtime.cpp')
)
$cmdPath = $cmdCandidates | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
if (-not $cmdPath) {
    throw 'Scarlet ports parity: runtime command source not found.'
}

$cmd = [IO.File]::ReadAllText($cmdPath)
if (-not $cmd.Contains('[Scarlet.Perf] LAS-style AOT dispatch cache')) {
    $old = @'
        Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
            for (const auto& m : s_recomp_modules) {
                if (auto fn = m.lookup(pc)) return m.run_slice ? m.run_slice : fn;
            }
            return nullptr;
        });
'@.Trim()

    $new = @'
        Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
            // [Scarlet.Perf] LAS-style AOT dispatch cache
            struct DispatchCacheEntry {
                u64 pc{};
                Core::RecompBlockFn fn{};
            };
            constexpr size_t CacheBits = 16;
            constexpr size_t CacheSize = size_t{1} << CacheBits;
            static thread_local DispatchCacheEntry cache[CacheSize]{};

            const u64 mixed = (pc >> 2) * 11400714819323198485ull;
            auto& entry = cache[static_cast<size_t>(mixed >> (64 - CacheBits))];
            if (entry.pc == pc && entry.fn != nullptr) {
                return entry.fn;
            }

            for (const auto& m : s_recomp_modules) {
                if (auto fn = m.lookup(pc)) {
                    const auto result = m.run_slice ? m.run_slice : fn;
                    entry.pc = pc;
                    entry.fn = result;
                    return result;
                }
            }
            return nullptr;
        });
'@.Trim()

    if (-not $cmd.Contains($old)) {
        throw 'Scarlet ports parity: combined AOT lookup anchor missing.'
    }
    $cmd = $cmd.Replace($old, $new)
    [IO.File]::WriteAllText($cmdPath, $cmd, $utf8)
    Write-Host '[Scarlet PC] LAS-style AOT dispatch cache installed (64K/thread).' -ForegroundColor Green
} else {
    Write-Host '[Scarlet PC] LAS-style AOT dispatch cache already installed.' -ForegroundColor DarkGray
}

$armPath = Join-Path $SourceDir 'src\core\arm\recomp\arm_recomp.cpp'
if (-not (Test-Path $armPath -PathType Leaf)) {
    throw 'Scarlet ports parity: ArmRecomp source not found.'
}
$arm = [IO.File]::ReadAllText($armPath)

$before = $arm
$arm = [regex]::Replace(
    $arm,
    'std::atomic<int>\s+g_recomp_chain_budget\{\d+\};',
    'std::atomic<int> g_recomp_chain_budget{16384};',
    1
)
$arm = $arm.Replace(
    'g_recomp_chain_budget.store(enabled ? 1024 : 32, std::memory_order_relaxed);',
    'g_recomp_chain_budget.store(enabled ? 16384 : 32, std::memory_order_relaxed);'
)
$arm = $arm.Replace(
    'g_recomp_chain_budget.store(enabled ? 4096 : 32, std::memory_order_relaxed);',
    'g_recomp_chain_budget.store(enabled ? 16384 : 32, std::memory_order_relaxed);'
)

if (-not $arm.Contains('[Scarlet.Perf] production SVC total gate')) {
    $svc = 'g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);'
    if ($arm.Contains($svc)) {
        $svcGated = @'
if (kScarletAotDiagnostics) {
                // [Scarlet.Perf] production SVC total gate
                g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
            }
'@.Trim()
        $arm = $arm.Replace($svc, $svcGated)
        Write-Host '[Scarlet PC] Aggregate SVC telemetry disabled during normal gameplay.' -ForegroundColor Green
    }
}

if (-not $arm.Contains('[Scarlet.Perf] production block tally gate')) {
    $seenOld = @'
        const u64 seen = t_blocks.slot->n.load(std::memory_order_relaxed) + 1;
        t_blocks.slot->n.store(seen, std::memory_order_relaxed);
'@.Trim()
    $seenNew = @'
        // [Scarlet.Perf] production block tally gate
        static const bool scarlet_coverage_enabled = [] {
            const char* value = std::getenv("SCARLET_RECOMP_COVERAGE");
            return value && *value && *value != '0';
        }();
        const bool scarlet_count_blocks =
            kScarletAotDiagnostics || scarlet_coverage_enabled;
        u64 seen = 0;
        if (scarlet_count_blocks) {
            seen = t_blocks.slot->n.load(std::memory_order_relaxed) + 1;
            t_blocks.slot->n.store(seen, std::memory_order_relaxed);
        }
'@.Trim()

    if ($arm.Contains($seenOld)) {
        $arm = $arm.Replace($seenOld, $seenNew)
    }

    $spentOld = @'
        {
            const int spent = chain_budget - impl->ctx.chain_budget;
'@.Trim()
    $spentNew = @'
        if (scarlet_count_blocks) {
            const int spent = chain_budget - impl->ctx.chain_budget;
'@.Trim()
    if ($arm.Contains($spentOld)) {
        $arm = $arm.Replace($spentOld, $spentNew)
    }
    Write-Host '[Scarlet PC] AOT block tally disabled unless coverage/diagnostics are requested.' -ForegroundColor Green
}

if ($arm -ne $before) {
    [IO.File]::WriteAllText($armPath, $arm, $utf8)
}

Write-Host '[Scarlet PC] Native-port runtime parity active: 16K slices + cached dispatch + lean telemetry.' -ForegroundColor Green
