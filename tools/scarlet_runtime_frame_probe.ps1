param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Read-Source([string]$RelativePath) {
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Scarlet frame probe: missing runtime source $RelativePath"
    }
    return [IO.File]::ReadAllText($path)
}

function Write-Source([string]$RelativePath, [string]$Text) {
    $path = Join-Path $SourceDir $RelativePath
    [IO.File]::WriteAllText($path, $Text, $utf8)
}

# ---------------------------------------------------------------------------
# Vulkan periodic frame probe.
# Existing StallProbe scopes already cover draws, pipeline work, acquire and
# present. Extend them with worker/submit/GPU-wait timing and publish a compact
# 120-frame/~2s summary. Disabled path remains a cached bool + branch.
# ---------------------------------------------------------------------------
$stallRel = 'src\video_core\renderer_vulkan\vk_stall_probe.h'
$stall = Read-Source $stallRel

if (-not $stall.Contains('[Scarlet.FrameProbe] periodic frame breakdown')) {
    $enabledOld = @'
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SUYU_STALL_PROBE");
        return value && *value && *value != '0';
    }();
    return enabled;
}
'@
    $enabledNew = @'
inline bool Enabled() {
    static const bool enabled = [] {
        // [Scarlet.FrameProbe] periodic frame breakdown
        // SCARLET_FRAME_PROBE=1 is the public switch. Keep the upstream
        // SUYU_STALL_PROBE alias so old diagnostic commands still work.
        const char* scarlet = std::getenv("SCARLET_FRAME_PROBE");
        if (scarlet && *scarlet && *scarlet != '0') {
            return true;
        }
        const char* legacy = std::getenv("SUYU_STALL_PROBE");
        return legacy && *legacy && *legacy != '0';
    }();
    return enabled;
}
'@
    if (-not $stall.Contains($enabledOld.Trim())) {
        throw 'Scarlet frame probe: StallProbe::Enabled anchor missing'
    }
    $stall = $stall.Replace($enabledOld.Trim(), $enabledNew.Trim())

    $counterOld = @'
inline std::atomic<u64> draw_flush_ns{0}, pipeline_lookup_ns{0}, pipeline_configure_ns{0},
    draw_record_ns{0};
'@
    $counterNew = @'
inline std::atomic<u64> draw_flush_ns{0}, pipeline_lookup_ns{0}, pipeline_configure_ns{0},
    draw_record_ns{0};
inline std::atomic<u64> dispatch_ns{0}, dispatch_count{0};
inline std::atomic<u64> worker_exec_ns{0}, worker_exec_count{0};
inline std::atomic<u64> submit_ns{0}, submit_count{0};
inline std::atomic<u64> gpu_wait_ns{0}, gpu_wait_count{0};
'@
    if (-not $stall.Contains($counterOld.Trim())) {
        throw 'Scarlet frame probe: StallProbe counter anchor missing'
    }
    $stall = $stall.Replace($counterOld.Trim(), $counterNew.Trim())

    $reportPattern = '(?s)inline void ReportFrame\(\) \{.*?\r?\n\}\r?\n\r?\n\} // namespace Vulkan::StallProbe'
    $reportRegex = [regex]::new(
        $reportPattern,
        [Text.RegularExpressions.RegexOptions]::Singleline
    )
    if (-not $reportRegex.IsMatch($stall)) {
        throw 'Scarlet frame probe: StallProbe::ReportFrame anchor missing'
    }

    $reportNew = @'
inline void ReportFrame() {
    if (!Enabled()) return;

    static thread_local u64 last_present = 0;
    static thread_local u64 window_start = 0;
    static thread_local u64 interval_sum = 0;
    static thread_local u64 interval_max = 0;
    static thread_local u64 frame_count = 0;

    const u64 now = Now();
    if (last_present != 0) {
        const u64 interval = now - last_present;
        interval_sum += interval;
        if (interval > interval_max) {
            interval_max = interval;
        }
        ++frame_count;
    }
    last_present = now;

    if (window_start == 0) {
        window_start = now;
    }

    // Report often enough to follow a gameplay scene, but not once per frame:
    // logging itself must not become the bottleneck being measured.
    if (frame_count < 120 && now - window_start < 2'000'000'000ULL) {
        return;
    }
    if (frame_count == 0) {
        window_start = now;
        return;
    }

    const u64 build = build_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 builds = build_wait_count.exchange(0, std::memory_order_relaxed);
    const u64 acquire = acquire_ns.exchange(0, std::memory_order_relaxed);
    const u64 present = present_ns.exchange(0, std::memory_order_relaxed);
    const u64 framew = frame_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 idle = gpu_idle_ns.exchange(0, std::memory_order_relaxed);
    const u64 draw = draw_ns.exchange(0, std::memory_order_relaxed);
    const u64 draws = draw_count.exchange(0, std::memory_order_relaxed);
    const u64 schedw = sched_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 translate = shader_translate_ns.exchange(0, std::memory_order_relaxed);
    const u64 emit = shader_emit_ns.exchange(0, std::memory_order_relaxed);
    const u64 shaders = shader_count.exchange(0, std::memory_order_relaxed);
    const u64 pipeline_build = pipeline_build_ns.exchange(0, std::memory_order_relaxed);
    const u64 pipeline_builds = pipeline_build_count.exchange(0, std::memory_order_relaxed);
    const u64 flush = draw_flush_ns.exchange(0, std::memory_order_relaxed);
    const u64 lookup = pipeline_lookup_ns.exchange(0, std::memory_order_relaxed);
    const u64 configure = pipeline_configure_ns.exchange(0, std::memory_order_relaxed);
    const u64 record = draw_record_ns.exchange(0, std::memory_order_relaxed);
    const u64 dispatch = dispatch_ns.exchange(0, std::memory_order_relaxed);
    const u64 dispatches = dispatch_count.exchange(0, std::memory_order_relaxed);
    const u64 worker = worker_exec_ns.exchange(0, std::memory_order_relaxed);
    const u64 workers = worker_exec_count.exchange(0, std::memory_order_relaxed);
    const u64 submit = submit_ns.exchange(0, std::memory_order_relaxed);
    const u64 submits = submit_count.exchange(0, std::memory_order_relaxed);
    const u64 gpu_wait = gpu_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 gpu_waits = gpu_wait_count.exchange(0, std::memory_order_relaxed);

    const auto milliseconds = [](u64 ns) { return static_cast<double>(ns) * 1.0e-6; };
    const double avg_frame_ms = milliseconds(interval_sum) / static_cast<double>(frame_count);
    const double fps = avg_frame_ms > 0.0 ? 1000.0 / avg_frame_ms : 0.0;

    LOG_INFO(
        Render_Vulkan,
        "[Scarlet.FrameProbe] frames={} avg={:.2f}ms fps={:.1f} max={:.2f}ms "
        "draw={:.1f}ms(n={}) record={:.1f}ms lookup={:.1f}ms configure={:.1f}ms "
        "dispatch={:.1f}ms(n={}) worker={:.1f}ms(n={}) submit={:.1f}ms(n={}) "
        "gpu_wait={:.1f}ms(n={}) sched_wait={:.1f}ms acquire={:.1f}ms present={:.1f}ms "
        "pipeline_build={:.1f}ms(n={}) shader_translate={:.1f}ms shader_emit={:.1f}ms(n={}) "
        "gpu_idle={:.1f}ms frame_wait={:.1f}ms build_wait={:.1f}ms(n={})",
        frame_count, avg_frame_ms, fps, milliseconds(interval_max),
        milliseconds(draw), draws, milliseconds(record), milliseconds(lookup),
        milliseconds(configure), milliseconds(dispatch), dispatches,
        milliseconds(worker), workers, milliseconds(submit), submits,
        milliseconds(gpu_wait), gpu_waits, milliseconds(schedw),
        milliseconds(acquire), milliseconds(present), milliseconds(pipeline_build),
        pipeline_builds, milliseconds(translate), milliseconds(emit), shaders,
        milliseconds(idle), milliseconds(framew), milliseconds(build), builds);

    window_start = now;
    interval_sum = 0;
    interval_max = 0;
    frame_count = 0;
}

} // namespace Vulkan::StallProbe
'@
    $stall = $reportRegex.Replace($stall, $reportNew.TrimEnd(), 1)
    Write-Source $stallRel $stall
    Write-Host '[Scarlet PC] Frame probe: periodic Vulkan breakdown enabled via SCARLET_FRAME_PROBE=1.' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Scheduler probe: queue handoff, worker command recording, queue submit and
# waits for GPU timeline completion.
# ---------------------------------------------------------------------------
$schedHeaderRel = 'src\video_core\renderer_vulkan\vk_scheduler.h'
$schedHeader = Read-Source $schedHeaderRel

if (-not $schedHeader.Contains('#include "video_core/renderer_vulkan/vk_stall_probe.h"')) {
    $anchor = '#include "video_core/renderer_vulkan/vk_master_semaphore.h"'
    if (-not $schedHeader.Contains($anchor)) {
        throw 'Scarlet frame probe: vk_scheduler.h include anchor missing'
    }
    $schedHeader = $schedHeader.Replace(
        $anchor,
        $anchor + [Environment]::NewLine + '#include "video_core/renderer_vulkan/vk_stall_probe.h"'
    )
}

if (-not $schedHeader.Contains('[Scarlet.FrameProbe] GPU timeline wait')) {
    $waitOld = '            master_semaphore->Wait(tick);'
    $waitNew = @'
            // [Scarlet.FrameProbe] GPU timeline wait
            const bool scarlet_probe = StallProbe::Enabled();
            const u64 scarlet_wait_start = scarlet_probe ? StallProbe::Now() : 0;
            master_semaphore->Wait(tick);
            if (scarlet_probe) {
                StallProbe::gpu_wait_ns.fetch_add(
                    StallProbe::Now() - scarlet_wait_start, std::memory_order_relaxed);
                StallProbe::gpu_wait_count.fetch_add(1, std::memory_order_relaxed);
            }
'@
    if (-not $schedHeader.Contains($waitOld)) {
        throw 'Scarlet frame probe: scheduler Wait anchor missing'
    }
    $schedHeader = $schedHeader.Replace($waitOld, $waitNew.TrimEnd())
}

# The existing Scarlet overlay made chunks 64 KiB. Increase to 256 KiB so the
# more aggressive 128-draw cadence is not defeated by chunk-full handoffs.
if (-not $schedHeader.Contains('[Scarlet.Perf] 256 KiB command chunks')) {
    $chunkPattern = 'alignas\(std::max_align_t\) std::array<u8, 0x(?:8000|10000)> data\{\};'
    $chunkRegex = [regex]::new($chunkPattern)
    if ($chunkRegex.IsMatch($schedHeader)) {
        $schedHeader = $chunkRegex.Replace(
            $schedHeader,
            '// [Scarlet.Perf] 256 KiB command chunks' + [Environment]::NewLine +
            '        alignas(std::max_align_t) std::array<u8, 0x40000> data{};',
            1
        )
    } elseif (-not $schedHeader.Contains('std::array<u8, 0x40000> data{};')) {
        throw 'Scarlet frame probe: command chunk storage anchor missing'
    }
}

Write-Source $schedHeaderRel $schedHeader

$schedRel = 'src\video_core\renderer_vulkan\vk_scheduler.cpp'
$sched = Read-Source $schedRel

if (-not $sched.Contains('[Scarlet.FrameProbe] scheduler dispatch')) {
    $old = @'
    if (chunk && !chunk->Empty()) {
        {
'@
    $new = @'
    if (chunk && !chunk->Empty()) {
        // [Scarlet.FrameProbe] scheduler dispatch
        StallProbe::Accum scarlet_dispatch_probe{
            StallProbe::dispatch_ns, &StallProbe::dispatch_count};
        {
'@
    if (-not $sched.Contains($old.Trim())) {
        throw 'Scarlet frame probe: DispatchWork anchor missing'
    }
    $sched = $sched.Replace($old.Trim(), $new.Trim())
}

if (-not $sched.Contains('[Scarlet.FrameProbe] Vulkan worker execution')) {
    $old = @'
            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
'@
    $new = @'
            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            // [Scarlet.FrameProbe] Vulkan worker execution
            StallProbe::Accum scarlet_worker_probe{
                StallProbe::worker_exec_ns, &StallProbe::worker_exec_count};
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
'@
    if (-not $sched.Contains($old.Trim())) {
        throw 'Scarlet frame probe: WorkerThread execution anchor missing'
    }
    $sched = $sched.Replace($old.Trim(), $new.Trim())
}

if (-not $sched.Contains('[Scarlet.FrameProbe] Vulkan queue submit path')) {
    $old = @'
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
'@
    $new = @'
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        // [Scarlet.FrameProbe] Vulkan queue submit path
        StallProbe::Accum scarlet_submit_probe{
            StallProbe::submit_ns, &StallProbe::submit_count};
        static constexpr VkMemoryBarrier WRITE_BARRIER{
'@
    if (-not $sched.Contains($old.Trim())) {
        throw 'Scarlet frame probe: SubmitExecution anchor missing'
    }
    $sched = $sched.Replace($old.Trim(), $new.Trim())
}

Write-Source $schedRel $sched

# ---------------------------------------------------------------------------
# Command batching v2. Default to one worker handoff per 128 draws instead of
# 32; SCARLET_VK_BATCH=0 restores the previous 32-draw cadence at runtime.
# A full GPU Flush remains at 4096 draws exactly as before.
# ---------------------------------------------------------------------------
$rasterRel = 'src\video_core\renderer_vulkan\vk_rasterizer.cpp'
$raster = Read-Source $rasterRel

if (-not $raster.Contains('#include <cstdlib>')) {
    $anchor = '#include <array>'
    if (-not $raster.Contains($anchor)) {
        throw 'Scarlet frame probe: vk_rasterizer include anchor missing'
    }
    $raster = $raster.Replace(
        $anchor,
        $anchor + [Environment]::NewLine + '#include <cstdlib>'
    )
}

if (-not $raster.Contains('[Scarlet.Perf] command batching v2')) {
    $flushPattern = '(?s)void RasterizerVulkan::FlushWork\(\) \{.*?\r?\n\}\r?\n\r?\nAccelerateDMA::AccelerateDMA'
    $flushRegex = [regex]::new(
        $flushPattern,
        [Text.RegularExpressions.RegexOptions]::Singleline
    )
    if (-not $flushRegex.IsMatch($raster)) {
        throw 'Scarlet frame probe: RasterizerVulkan::FlushWork anchor missing'
    }
    $flushNew = @'
void RasterizerVulkan::FlushWork() {
#ifdef __ANDROID__
    static constexpr u32 DRAWS_TO_DISPATCH = 512;
    static constexpr u32 CHECK_MASK = 3;
#else
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;
    // [Scarlet.Perf] command batching v2
    // Default: hand work to the Vulkan worker every 128 draws. The previous
    // Scarlet profile used 32. Chunk-full dispatch remains immediate.
    // SCARLET_VK_BATCH=0 restores the 32-draw cadence without rebuilding.
    static const u32 CHECK_MASK = [] {
        const char* value = std::getenv("SCARLET_VK_BATCH");
        const bool conservative = value && *value == '0';
        const u32 mask = conservative ? 31U : 127U;
        LOG_INFO(Render_Vulkan,
                 "[Scarlet.Perf] Vulkan command batching: {}-draw cadence, 256 KiB chunks{}",
                 mask + 1, conservative ? " (compatibility mode)" : "");
        return mask;
    }();
#endif // __ANDROID__

    if ((++draw_counter & CHECK_MASK) != CHECK_MASK) {
        return;
    }
    if (draw_counter < DRAWS_TO_DISPATCH) {
        scheduler.DispatchWork();
        return;
    }
    scheduler.Flush();
    draw_counter = 0;
}

AccelerateDMA::AccelerateDMA
'@
    $raster = $flushRegex.Replace($raster, $flushNew.TrimEnd(), 1)
}

Write-Source $rasterRel $raster

# ---------------------------------------------------------------------------
# AOT-side probe. Time only the generated-chain call (one clock pair per chain,
# not per guest block) and the host-side gap around SVC handling. This is
# diagnostic-only and gated by the same SCARLET_FRAME_PROBE switch.
# ---------------------------------------------------------------------------
$armRel = 'src\core\arm\recomp\arm_recomp.cpp'
$arm = Read-Source $armRel

if (-not $arm.Contains('[Scarlet.FrameProbe] AOT timing')) {
    $globalAnchor = 'std::atomic<u64> g_fpx_env_repairs{0};'
    $globalNew = @'
std::atomic<u64> g_fpx_env_repairs{0};

// [Scarlet.FrameProbe] AOT timing
const bool kScarletFrameProbe = [] {
    const char* value = std::getenv("SCARLET_FRAME_PROBE");
    return value && *value && *value != '0';
}();
std::atomic<u64> g_scarlet_aot_ns{0};
std::atomic<u64> g_scarlet_aot_calls{0};
std::atomic<u64> g_scarlet_svc_gap_ns{0};
std::atomic<u64> g_scarlet_svc_gap_count{0};

u64 ScarletProfileNow() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
'@
    if (-not $arm.Contains($globalAnchor)) {
        throw 'Scarlet frame probe: ArmRecomp global anchor missing'
    }
    $arm = $arm.Replace($globalAnchor, $globalNew.TrimEnd())

    $totalAnchor = @'
u64 TotalStaticBlocks() {
    std::scoped_lock lk{g_tally_lock};
    u64 sum = g_retired_blocks.load(std::memory_order_relaxed);
    for (const ThreadBlocks* s : g_tallies) {
        sum += s->n.load(std::memory_order_relaxed);
    }
    return sum;
}
'@
    $totalNew = @'
u64 TotalStaticBlocks() {
    std::scoped_lock lk{g_tally_lock};
    u64 sum = g_retired_blocks.load(std::memory_order_relaxed);
    for (const ThreadBlocks* s : g_tallies) {
        sum += s->n.load(std::memory_order_relaxed);
    }
    return sum;
}

void MaybeReportScarletCpuProfile() {
    if (!kScarletFrameProbe) {
        return;
    }

    static std::atomic<u64> next_report_ns{0};
    const u64 now = ScarletProfileNow();
    u64 next = next_report_ns.load(std::memory_order_relaxed);
    if (next != 0 && now < next) {
        return;
    }
    const u64 desired = now + 2'000'000'000ULL;
    if (!next_report_ns.compare_exchange_strong(
            next, desired, std::memory_order_relaxed, std::memory_order_relaxed)) {
        return;
    }

    static u64 last_blocks = 0;
    static u64 last_time = now;
    const u64 blocks = TotalStaticBlocks();
    const u64 elapsed = now - last_time;
    const u64 delta_blocks = blocks - last_blocks;
    const u64 aot_ns = g_scarlet_aot_ns.exchange(0, std::memory_order_relaxed);
    const u64 aot_calls = g_scarlet_aot_calls.exchange(0, std::memory_order_relaxed);
    const u64 svc_gap_ns = g_scarlet_svc_gap_ns.exchange(0, std::memory_order_relaxed);
    const u64 svc_gaps = g_scarlet_svc_gap_count.exchange(0, std::memory_order_relaxed);

    if (last_blocks != 0 && elapsed != 0) {
        const double seconds = static_cast<double>(elapsed) * 1.0e-9;
        const double mblocks_s = static_cast<double>(delta_blocks) / seconds / 1.0e6;
        LOG_INFO(
            Core_ARM,
            "[Scarlet.FrameProbe] CPU interval={:.1f}ms AOT={:.1f}ms chains={} "
            "blocks={} ({:.1f} Mblocks/s) svc_host_gap={:.1f}ms(n={})",
            static_cast<double>(elapsed) * 1.0e-6,
            static_cast<double>(aot_ns) * 1.0e-6, aot_calls,
            delta_blocks, mblocks_s,
            static_cast<double>(svc_gap_ns) * 1.0e-6, svc_gaps);
    }

    last_blocks = blocks;
    last_time = now;
}
'@
    if (-not $arm.Contains($totalAnchor.Trim())) {
        throw 'Scarlet frame probe: TotalStaticBlocks anchor missing'
    }
    $arm = $arm.Replace($totalAnchor.Trim(), $totalNew.Trim())

    $memberAnchor = @'
    Kernel::KThread* last_thread{};
    std::size_t core_index{};
'@
    $memberNew = @'
    Kernel::KThread* last_thread{};
    u64 scarlet_svc_depart_ns{};
    std::size_t core_index{};
'@
    if (-not $arm.Contains($memberAnchor.Trim())) {
        throw 'Scarlet frame probe: ArmRecomp Impl member anchor missing'
    }
    $arm = $arm.Replace($memberAnchor.Trim(), $memberNew.Trim())

    $runAnchor = 'HaltReason ArmRecomp::RunThread(Kernel::KThread* thread) {'
    $runNew = @'
HaltReason ArmRecomp::RunThread(Kernel::KThread* thread) {
    if (kScarletFrameProbe) {
        const u64 now = ScarletProfileNow();
        if (impl->scarlet_svc_depart_ns != 0) {
            g_scarlet_svc_gap_ns.fetch_add(
                now - impl->scarlet_svc_depart_ns, std::memory_order_relaxed);
            g_scarlet_svc_gap_count.fetch_add(1, std::memory_order_relaxed);
            impl->scarlet_svc_depart_ns = 0;
        }
        MaybeReportScarletCpuProfile();
    }
'@
    if (-not $arm.Contains($runAnchor)) {
        throw 'Scarlet frame probe: RunThread anchor missing'
    }
    $arm = $arm.Replace($runAnchor, $runNew.TrimEnd())

    $blockAnchor = '        block(&impl->ctx);'
    $blockNew = @'
        const u64 scarlet_aot_start = kScarletFrameProbe ? ScarletProfileNow() : 0;
        block(&impl->ctx);
        if (kScarletFrameProbe) {
            g_scarlet_aot_ns.fetch_add(
                ScarletProfileNow() - scarlet_aot_start, std::memory_order_relaxed);
            g_scarlet_aot_calls.fetch_add(1, std::memory_order_relaxed);
        }
'@
    if (-not $arm.Contains($blockAnchor)) {
        throw 'Scarlet frame probe: AOT block call anchor missing'
    }
    # Only the RunThread block call; StepThread has the same text later.
    $blockIndex = $arm.IndexOf($blockAnchor)
    $arm = $arm.Substring(0, $blockIndex) + $blockNew.TrimEnd() +
        $arm.Substring($blockIndex + $blockAnchor.Length)

    # There are several SupervisorCall returns. Patch the one in RunThread by
    # anchoring to the SVC counter immediately before it; this remains valid
    # after the separate inline-sync overlay has inserted its proof fast paths.
    $svcOld = @'
            g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordSvc(static_cast<u32>(impl->ctx.pending_svc));
            return HaltReason::SupervisorCall;
'@
    $svcNew = @'
            g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordSvc(static_cast<u32>(impl->ctx.pending_svc));
            if (kScarletFrameProbe) {
                impl->scarlet_svc_depart_ns = ScarletProfileNow();
            }
            return HaltReason::SupervisorCall;
'@
    if (-not $arm.Contains($svcOld.Trim())) {
        throw 'Scarlet frame probe: RunThread SVC return anchor missing'
    }
    $arm = $arm.Replace($svcOld.Trim(), $svcNew.Trim())

    Write-Source $armRel $arm
    Write-Host '[Scarlet PC] Frame probe: AOT chain + SVC host-gap timing added.' -ForegroundColor DarkGray
}

Write-Host '[Scarlet PC] Vulkan batching v2 + frame probe overlay applied.' -ForegroundColor Green
