param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$armPath = Join-Path $SourceDir 'src\core\arm\recomp\arm_recomp.cpp'
if (-not (Test-Path -LiteralPath $armPath -PathType Leaf)) {
    throw "ArmRecomp source not found: $armPath"
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$text = [IO.File]::ReadAllText($armPath)

# This overlay runs after scarlet_runtime_overlay.ps1. Keep it idempotent so
# FastIncremental can reuse the already-patched runtime tree.
if ($text.Contains('[Scarlet.Perf] inline sync SVC fast paths')) {
    Write-Host '[Scarlet PC] Inline sync SVC fast paths already applied.' -ForegroundColor DarkGray
    return
}

# ResultInvalidState is needed only for a proven SignalToAddress predicate miss.
if (-not $text.Contains('#include "core/hle/kernel/svc_results.h"')) {
    $includeAnchor = '#include "core/hle/kernel/k_process.h"'
    if (-not $text.Contains($includeAnchor)) {
        throw 'Scarlet sync overlay: k_process include anchor missing'
    }
    $text = $text.Replace(
        $includeAnchor,
        $includeAnchor + [Environment]::NewLine + '#include "core/hle/kernel/svc_results.h"'
    )
}

# Track how many expensive AOT -> HLE round trips the fast paths actually avoid.
$counterAnchor = 'RecompCounters g_counters;'
$counterBlock = @'
RecompCounters g_counters;
std::array<std::atomic<u64>, 3> g_inline_sync_svc_hits{};
'@
if (-not $text.Contains($counterAnchor)) {
    throw 'Scarlet sync overlay: RecompCounters anchor missing'
}
$text = $text.Replace($counterAnchor, $counterBlock.TrimEnd())

# Surface the effect in recomp_coverage.txt so one test run tells us whether
# these paths matter on the user's workload.
$reportAnchor = '    o += fmt::format("  SVCs to HLE            : {}\n", g_counters.svc_calls.load());'
$reportBlock = @'
    o += fmt::format("  SVCs to HLE            : {}\n", g_counters.svc_calls.load());
    {
        const u64 inline_unlock =
            g_inline_sync_svc_hits[0].load(std::memory_order_relaxed);
        const u64 inline_cv_signal =
            g_inline_sync_svc_hits[1].load(std::memory_order_relaxed);
        const u64 inline_signal_reject =
            g_inline_sync_svc_hits[2].load(std::memory_order_relaxed);
        o += fmt::format(
            "  inline sync fast paths : {} (unlock {}, cv-signal {}, signal-reject {})\n",
            inline_unlock + inline_cv_signal + inline_signal_reject,
            inline_unlock, inline_cv_signal, inline_signal_reject);
    }
'@
if (-not $text.Contains($reportAnchor)) {
    throw 'Scarlet sync overlay: coverage report anchor missing'
}
$text = $text.Replace($reportAnchor, $reportBlock.TrimEnd())

# The existing inline SVC 0x10 path sits immediately before this LOG_TRACE.
# Insert only proof-based synchronization cases here:
#   0x1B ArbitrateUnlock: inline only if the lock word has no waiter bit and
#        an exclusive store succeeds. A racing waiter invalidates the
#        reservation and falls through to the exact kernel path.
#   0x1D SignalProcessWideKey: inline only when the userspace CV waiter flag
#        is zero, matching the kernel fast path already used by Scarlet.
#   0x35 SignalToAddress: inline only the already-proven invalid-state result
#        for conditional signal variants whose userspace value mismatches.
# Blocking waits (0x1A/0x1C/0x34), IPC and every contended case remain HLE.
$svcAnchor = '            LOG_TRACE(Core_ARM, "recomp SVC {} at pc={:#x} x0={:#x} x1={:#x} x2={:#x} x3={:#x}",'
if (-not $text.Contains($svcAnchor)) {
    throw 'Scarlet sync overlay: ArmRecomp SVC exit anchor missing'
}

$fastBlock = @'
            // [Scarlet.Perf] inline sync SVC fast paths
            // Set SCARLET_INLINE_SYNC_SVC=0 to restore the previous behavior.
            static const bool scarlet_inline_sync_svc = [] {
                const char* value = std::getenv("SCARLET_INLINE_SYNC_SVC");
                return value == nullptr || *value == '\0' || *value != '0';
            }();

            if (scarlet_inline_sync_svc && !Kernel::LockTrace::Enabled()) {
                const u64 user_limit = impl->bridge.address_space_max;

                // SVC 0x1B: ArbitrateUnlock.
                //
                // This is the same proof used by KConditionVariable's Scarlet
                // fast path, moved one layer earlier so the common uncontended
                // unlock never leaves ArmRecomp. The exclusive reservation is
                // important: if another core adds a waiter between our read and
                // store, the store fails and the real HLE path handles ownership.
                if (svc_number == 0x1B && impl->exclusive_monitor != nullptr) {
                    const u64 address = impl->ctx.x[0];
                    if ((address & 3u) == 0 && user_limit != 0 && address < user_limit) {
                        constexpr u32 ScarletHandleWaitMask = 1u << 30;
                        const u32 current =
                            impl->exclusive_monitor->ExclusiveRead32(impl->core_index, address);
                        if ((current & ScarletHandleWaitMask) == 0) {
                            std::atomic_thread_fence(std::memory_order_seq_cst);
                            if (impl->exclusive_monitor->ExclusiveWrite32(
                                    impl->core_index, address, 0)) {
                                g_inline_sync_svc_hits[0].fetch_add(
                                    1, std::memory_order_relaxed);
                                g_counters.RecordSvc(svc_number);
                                impl->ctx.x[0] = 0; // ResultSuccess
                                impl->ctx.pending_svc = kNoPendingSvc;
                                impl->ctx.halted = 0;
                                continue;
                            }
                        } else {
                            impl->exclusive_monitor->ClearExclusive(impl->core_index);
                        }
                    }
                }

                // SVC 0x1D: SignalProcessWideKey.
                //
                // WaitProcessWideKeyAtomic publishes a 1 in this userspace word
                // before a waiter is parked. Zero therefore means this signal
                // can linearize before any new waiter, exactly like the existing
                // KConditionVariable::Signal no-waiter fast path.
                if (svc_number == 0x1D && user_limit != 0) {
                    const u64 cv_key = impl->ctx.x[0] & ~u64{3};
                    if (cv_key < user_limit) {
                        const u32 has_waiter = static_cast<u32>(
                            Impl::HostLoad(impl.get(), cv_key, sizeof(u32)));
                        if (has_waiter == 0) {
                            g_inline_sync_svc_hits[1].fetch_add(
                                1, std::memory_order_relaxed);
                            g_counters.RecordSvc(svc_number);
                            impl->ctx.pending_svc = kNoPendingSvc;
                            impl->ctx.halted = 0;
                            continue;
                        }
                    }
                }

                // SVC 0x35: SignalToAddress.
                //
                // Signal types 1 and 2 both return ResultInvalidState when the
                // userspace word does not equal x2. Scarlet's kernel overlay
                // already performs this proof before taking the scheduler lock;
                // doing the same here also removes the AOT -> HLE round trip.
                if (svc_number == 0x35 && user_limit != 0) {
                    const u64 address = impl->ctx.x[0];
                    const u32 signal_type = static_cast<u32>(impl->ctx.x[1]);
                    if ((address & 3u) == 0 && address < user_limit &&
                        (signal_type == 1 || signal_type == 2)) {
                        const s32 current = static_cast<s32>(static_cast<u32>(
                            Impl::HostLoad(impl.get(), address, sizeof(u32))));
                        const s32 expected =
                            static_cast<s32>(static_cast<u32>(impl->ctx.x[2]));
                        if (current != expected) {
                            g_inline_sync_svc_hits[2].fetch_add(
                                1, std::memory_order_relaxed);
                            g_counters.RecordSvc(svc_number);
                            impl->ctx.x[0] =
                                static_cast<u64>(Kernel::ResultInvalidState.GetInnerValue());
                            impl->ctx.pending_svc = kNoPendingSvc;
                            impl->ctx.halted = 0;
                            continue;
                        }
                    }
                }
            }

'@

$text = $text.Replace($svcAnchor, $fastBlock + $svcAnchor)

[IO.File]::WriteAllText($armPath, $text, $utf8)
Write-Host '[Scarlet PC] Inline sync SVC fast paths enabled: 0x1B / 0x1D / 0x35 reject.' -ForegroundColor Green
