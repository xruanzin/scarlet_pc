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


# ---------------------------------------------------------------------------
# Low-overhead SVC wall-time profiler.
# This lives in the already-invoked pre-build repair step so it also works with
# preserved FastIncremental runtime trees and Codex-only local instrumentation.
# Enabled only with SCARLET_SVC_TIME_PROFILE=1.
# ---------------------------------------------------------------------------
$svcPath = Join-Path $SourceDir 'src\core\hle\kernel\svc.cpp'
if (-not (Test-Path -LiteralPath $svcPath -PathType Leaf)) {
    throw "Scarlet SVC profiler: missing $svcPath"
}

$svc = [IO.File]::ReadAllText($svcPath)
if (-not $svc.Contains('[Scarlet.Perf] SVC wall-time profiler')) {
    if (-not $svc.Contains('#include <atomic>')) {
        $includeAnchor = '#include <type_traits>'
        if (-not $svc.Contains($includeAnchor)) {
            throw 'Scarlet SVC profiler: include anchor missing'
        }
        $includeBlock = @'
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <type_traits>
'@
        $svc = $svc.Replace($includeAnchor, $includeBlock.TrimEnd())
    }

    $callAnchor = @'
void Call(Core::System& system, u32 imm) {
    auto& kernel = system.Kernel();
    auto& process = GetCurrentProcess(kernel);
    std::array<uint64_t, 8> args;
    kernel.CurrentPhysicalCore().SaveSvcArguments(process, args);
    LOG_TRACE(Kernel_SVC, "{} [0]={:#x} [1]={:#x} [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x} [6]={:#x}",
        imm, GetArg32(args, 0), GetArg32(args, 1), GetArg32(args, 2),
        GetArg32(args, 3), GetArg32(args, 4), GetArg32(args, 5), GetArg32(args, 6));
    //kernel.EnterSVCProfile();
    if (process.Is64Bit())
        Call64(system, imm, args);
    else
        Call32(system, imm, args);
    //kernel.ExitSVCProfile();
    kernel.CurrentPhysicalCore().LoadSvcArguments(process, args);
}
'@

    $callReplacement = @'
namespace {

// [Scarlet.Perf] SVC wall-time profiler
struct ScarletSvcTimingProfile {
    std::array<std::atomic<u64>, 256> calls{};
    std::array<std::atomic<u64>, 256> save_ns{};
    std::array<std::atomic<u64>, 256> handler_ns{};
    std::array<std::atomic<u64>, 256> load_ns{};
    std::array<std::atomic<u64>, 256> max_handler_ns{};
    std::atomic<u64> next_report_ns{};
};

ScarletSvcTimingProfile& ScarletSvcProfile() {
    static ScarletSvcTimingProfile profile;
    return profile;
}

bool ScarletSvcProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SCARLET_SVC_TIME_PROFILE");
        return value && *value && *value != '0';
    }();
    return enabled;
}

u64 ScarletSvcNowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void ScarletAtomicMax(std::atomic<u64>& target, u64 value) {
    u64 current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

const char* ScarletSvcName(u32 imm) {
    switch (imm) {
    case 0x10: return "GetCurrentProcessorNumber";
    case 0x18: return "WaitSynchronization";
    case 0x1A: return "ArbitrateLock";
    case 0x1B: return "ArbitrateUnlock";
    case 0x1C: return "WaitProcessWideKeyAtomic";
    case 0x1D: return "SignalProcessWideKey";
    case 0x21: return "SendSyncRequest";
    case 0x34: return "WaitForAddress";
    case 0x35: return "SignalToAddress";
    default: return "other";
    }
}

void ScarletSvcMaybeReport(u64 now_ns) {
    auto& profile = ScarletSvcProfile();
    constexpr u64 ReportIntervalNs = 10'000'000'000ULL;

    u64 next = profile.next_report_ns.load(std::memory_order_relaxed);
    if (next == 0) {
        u64 expected = 0;
        profile.next_report_ns.compare_exchange_strong(
            expected, now_ns + ReportIntervalNs, std::memory_order_relaxed);
        return;
    }
    if (now_ns < next) {
        return;
    }
    if (!profile.next_report_ns.compare_exchange_strong(
            next, now_ns + ReportIntervalNs, std::memory_order_relaxed)) {
        return;
    }

    u64 total_calls = 0;
    u64 total_save = 0;
    u64 total_handler = 0;
    u64 total_load = 0;
    for (u32 id = 0; id < 256; ++id) {
        total_calls += profile.calls[id].load(std::memory_order_relaxed);
        total_save += profile.save_ns[id].load(std::memory_order_relaxed);
        total_handler += profile.handler_ns[id].load(std::memory_order_relaxed);
        total_load += profile.load_ns[id].load(std::memory_order_relaxed);
    }

    LOG_INFO(Kernel_SVC,
             "[Scarlet.SVC-Time] total calls={} save={:.3f}ms handler={:.3f}ms load={:.3f}ms",
             total_calls, static_cast<double>(total_save) / 1.0e6,
             static_cast<double>(total_handler) / 1.0e6,
             static_cast<double>(total_load) / 1.0e6);

    constexpr std::array<u32, 9> ids{0x10, 0x18, 0x1A, 0x1B, 0x1C,
                                     0x1D, 0x21, 0x34, 0x35};
    for (const u32 id : ids) {
        const u64 calls = profile.calls[id].load(std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        const u64 handler = profile.handler_ns[id].load(std::memory_order_relaxed);
        const u64 max_handler =
            profile.max_handler_ns[id].load(std::memory_order_relaxed);
        LOG_INFO(
            Kernel_SVC,
            "[Scarlet.SVC-Time] 0x{:02X} {} calls={} total={:.3f}ms avg={:.3f}us max={:.3f}us",
            id, ScarletSvcName(id), calls, static_cast<double>(handler) / 1.0e6,
            static_cast<double>(handler) / static_cast<double>(calls) / 1.0e3,
            static_cast<double>(max_handler) / 1.0e3);
    }
}

} // namespace

void Call(Core::System& system, u32 imm) {
    auto& kernel = system.Kernel();
    auto& process = GetCurrentProcess(kernel);
    std::array<uint64_t, 8> args;

    const bool scarlet_profile =
        ScarletSvcProfileEnabled() && imm < ScarletSvcProfile().calls.size();
    u64 scarlet_t0 = 0;
    u64 scarlet_t1 = 0;
    u64 scarlet_t2 = 0;
    if (scarlet_profile) {
        scarlet_t0 = ScarletSvcNowNs();
    }

    kernel.CurrentPhysicalCore().SaveSvcArguments(process, args);

    if (scarlet_profile) {
        scarlet_t1 = ScarletSvcNowNs();
    }

    LOG_TRACE(Kernel_SVC, "{} [0]={:#x} [1]={:#x} [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x} [6]={:#x}",
        imm, GetArg32(args, 0), GetArg32(args, 1), GetArg32(args, 2),
        GetArg32(args, 3), GetArg32(args, 4), GetArg32(args, 5), GetArg32(args, 6));

    if (process.Is64Bit())
        Call64(system, imm, args);
    else
        Call32(system, imm, args);

    if (scarlet_profile) {
        scarlet_t2 = ScarletSvcNowNs();
    }

    kernel.CurrentPhysicalCore().LoadSvcArguments(process, args);

    if (scarlet_profile) {
        const u64 scarlet_t3 = ScarletSvcNowNs();
        auto& profile = ScarletSvcProfile();
        profile.calls[imm].fetch_add(1, std::memory_order_relaxed);
        profile.save_ns[imm].fetch_add(scarlet_t1 - scarlet_t0,
                                       std::memory_order_relaxed);
        profile.handler_ns[imm].fetch_add(scarlet_t2 - scarlet_t1,
                                          std::memory_order_relaxed);
        profile.load_ns[imm].fetch_add(scarlet_t3 - scarlet_t2,
                                       std::memory_order_relaxed);
        ScarletAtomicMax(profile.max_handler_ns[imm], scarlet_t2 - scarlet_t1);
        ScarletSvcMaybeReport(scarlet_t3);
    }
}
'@

    if (-not $svc.Contains($callAnchor.Trim())) {
        throw 'Scarlet SVC profiler: Call() anchor differs from pinned runtime'
    }
    $svc = $svc.Replace($callAnchor.Trim(), $callReplacement.Trim())
    [IO.File]::WriteAllText($svcPath, $svc, $utf8)
    Write-Host '[Scarlet PC] SVC wall-time profiler installed (SCARLET_SVC_TIME_PROFILE=1).' -ForegroundColor Green
} else {
    Write-Host '[Scarlet PC] SVC wall-time profiler already installed.' -ForegroundColor DarkGray
}
