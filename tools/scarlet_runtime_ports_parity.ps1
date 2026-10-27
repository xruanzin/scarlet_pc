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



# ---------------------------------------------------------------------------
# Let's Go Recomp mutex tactic: short spin before a contended ArbitrateLock
# sleeps in the kernel. Their adopted Windows path uses 40 us by default.
# ---------------------------------------------------------------------------
$scarletCondPath = Join-Path $SourceDir 'src\core\hle\kernel\k_condition_variable.cpp'
$scarletCondText = [IO.File]::ReadAllText($scarletCondPath)

if (-not $scarletCondText.Contains('[Scarlet.Perf] LetsGo mutex spin')) {
    if (-not $scarletCondText.Contains('#include <chrono>')) {
        $includeAnchor = '#include "core/arm/exclusive_monitor.h"'
        if (-not $scarletCondText.Contains($includeAnchor)) {
            throw 'Scarlet ports parity: condition-variable include anchor missing'
        }
        $includeBlock = @'
#include <chrono>
#include <cstdlib>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include "core/arm/exclusive_monitor.h"
'@.Trim()
        $scarletCondText = $scarletCondText.Replace($includeAnchor, $includeBlock)
    }

    $helperAnchor = 'class ThreadQueueImplForKConditionVariableWaitForAddress final'
    if (-not $scarletCondText.Contains($helperAnchor)) {
        throw 'Scarlet ports parity: WaitForAddress queue anchor missing'
    }

    $helper = @'
// [Scarlet.Perf] LetsGo mutex spin
u64 ScarletMutexSpinNs() {
    static const u64 value = [] {
        const char* env = std::getenv("SCARLET_LOCK_SPIN_US");
        long us = env ? std::strtol(env, nullptr, 10) : 40;
        if (us < 0) {
            us = 0;
        }
        if (us > 200) {
            us = 200;
        }
        return static_cast<u64>(us) * 1000ULL;
    }();
    return value;
}

inline void ScarletMutexCpuRelax() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

bool ScarletSpinUntilMutexChanges(KernelCore& kernel, KProcessAddress address,
                                  u32 expected_tag) {
    if (LockTrace::Enabled()) {
        return false;
    }

    const u64 spin_ns = ScarletMutexSpinNs();
    if (spin_ns == 0) {
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);

    do {
        u32 current{};
        if (!ReadFromUser(kernel, std::addressof(current), address)) {
            return false;
        }
        if (current != expected_tag) {
            return true;
        }
        ScarletMutexCpuRelax();
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
}

'@
    $scarletCondText =
        $scarletCondText.Replace($helperAnchor, $helper + $helperAnchor)

    $waitAnchor = @'
KThread* cur_thread = GetCurrentThreadPointer(kernel);
    ThreadQueueImplForKConditionVariableWaitForAddress wait_queue(kernel);

    // Wait for the address.
    KThread* owner_thread{};
'@.Trim()

    $waitReplacement = @'
KThread* cur_thread = GetCurrentThreadPointer(kernel);
    ThreadQueueImplForKConditionVariableWaitForAddress wait_queue(kernel);

    // [Scarlet.Perf] LetsGo mutex spin
    const u32 scarlet_expected_tag = handle | Svc::HandleWaitMask;
    if (ScarletSpinUntilMutexChanges(kernel, addr, scarlet_expected_tag) &&
        !cur_thread->IsTerminationRequested()) {
        R_SUCCEED();
    }

    // Wait for the address.
    KThread* owner_thread{};
'@.Trim()

    if (-not $scarletCondText.Contains($waitAnchor)) {
        throw 'Scarlet ports parity: WaitForAddress body anchor missing'
    }
    $scarletCondText = $scarletCondText.Replace($waitAnchor, $waitReplacement)

    [IO.File]::WriteAllText($scarletCondPath, $scarletCondText, $utf8)
    Write-Host '[Scarlet PC] LetsGo-style mutex spin enabled: 40 us default, kernel fallback preserved.' -ForegroundColor Green
} else {
    Write-Host '[Scarlet PC] LetsGo-style mutex spin already installed.' -ForegroundColor DarkGray
}


Write-Host '[Scarlet PC] Native-port runtime parity active: 16K slices + cached dispatch + lean telemetry.' -ForegroundColor Green
