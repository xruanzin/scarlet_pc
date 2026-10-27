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
    throw "Scarlet hotspot trace: missing $rel"
}

$text = [IO.File]::ReadAllText($path)
if ($text.Contains('[Scarlet.HotspotTrace] targeted ARM64 dump')) {
    # FastIncremental preserves a source tree even after a failed compile. The
    # first tracer revision used a non-existent GuestContextView::sp member;
    # repair that stale transformed source in-place before deciding it is done.
    if ($text.Contains('impl->ctx.sp')) {
        $text = $text.Replace('impl->ctx.sp', 'impl->ctx.x[31]')
        [IO.File]::WriteAllText($path, $text, $utf8)
        Write-Host '[Scarlet PC] Hotspot trace: repaired preserved SP access to x[31].' -ForegroundColor Green
    } else {
        Write-Host '[Scarlet PC] Hotspot trace already applied.' -ForegroundColor DarkGray
    }
    exit 0
}

# Global diagnostic switch/state next to the existing sampling controls.
$anchorPattern = '(const bool kSamplePc\s*=\s*\[\]\s*\{.*?\}\(\);)'
$anchorRegex = [regex]::new(
    $anchorPattern,
    [Text.RegularExpressions.RegexOptions]::Singleline
)
if (-not $anchorRegex.IsMatch($text)) {
    throw 'Scarlet hotspot trace: sampling-control anchor missing'
}

$globals = @'
$1

// [Scarlet.HotspotTrace] targeted ARM64 dump
// Diagnostic-only: dump the hot nnSdk blocks and their dominant Scarlet
// callers after the normal sampling window opens. This gives us the real
// AArch64 instruction fingerprints without touching generated AOT modules.
const bool kScarletHotspotTrace = [] {
    const char* e = std::getenv("SCARLET_HOTSPOT_TRACE");
    return e && *e && *e != '0';
}();
std::atomic<bool> g_scarlet_hotspot_sites_dumped{false};
std::array<std::atomic<u32>, 12> g_scarlet_hotspot_hit_counts{};
'@
$text = $anchorRegex.Replace($text, $globals.TrimEnd(), 1)

# Scope the insertion to RunThread, immediately before the static block call.
$runStart = $text.IndexOf('HaltReason ArmRecomp::RunThread')
$stepStart = $text.IndexOf('HaltReason ArmRecomp::StepThread', $runStart)
if ($runStart -lt 0 -or $stepStart -le $runStart) {
    throw 'Scarlet hotspot trace: RunThread boundaries missing'
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
    throw 'Scarlet hotspot trace: static block call missing in RunThread'
}

$trace = @'
$1if (kScarletHotspotTrace && SampleWindowOpen() && impl->modules_read) {
$1    u64 scarlet_main_base = 0;
$1    u64 scarlet_sdk_base = 0;
$1    for (const auto& [base, name] : impl->modules) {
$1        if (name == "main_app.nss") {
$1            scarlet_main_base = base;
$1        } else if (name == "nnSdk") {
$1            scarlet_sdk_base = base;
$1        }
$1    }
$1
$1    if (scarlet_main_base != 0 && scarlet_sdk_base != 0) {
$1        const auto resolve_offset = [&](u64 address) -> std::string {
$1            if (address >= scarlet_sdk_base) {
$1                return fmt::format("nnSdk+{:#x}", address - scarlet_sdk_base);
$1            }
$1            if (address >= scarlet_main_base && address < scarlet_sdk_base) {
$1                return fmt::format("main_app.nss+{:#x}", address - scarlet_main_base);
$1            }
$1            return fmt::format("{:#x}", address);
$1        };
$1
$1        const auto dump_window = [&](std::string_view label, u64 center) {
$1            LOG_INFO(Core_ARM,
$1                     "[Scarlet.HotspotTrace] site {} center={} abs={:#x}",
$1                     label, resolve_offset(center), center);
$1            // 8 instructions before through 15 after. Enough to identify a
$1            // loop/function prologue and the call sequence around LR.
$1            for (int i = -8; i <= 15; ++i) {
$1                const u64 address = center + static_cast<s64>(i) * 4;
$1                const u32 insn =
$1                    static_cast<u32>(Impl::HostLoad(impl.get(), address, 4));
$1                LOG_INFO(Core_ARM,
$1                         "[Scarlet.HotspotTrace]   {} {:+03d} {:#x}: {:08X}",
$1                         label, i, address, insn);
$1            }
$1        };
$1
$1        if (!g_scarlet_hotspot_sites_dumped.exchange(
$1                true, std::memory_order_relaxed)) {
$1            struct TraceSite {
$1                const char* label;
$1                bool sdk;
$1                u64 offset;
$1            };
$1            static constexpr std::array<TraceSite, 22> sites{{
$1                {"sdk_377320", true,  0x377320},
$1                {"sdk_3726f0", true,  0x3726f0},
$1                {"sdk_379fc0", true,  0x379fc0},
$1                {"sdk_581bd0", true,  0x581bd0},
$1                {"sdk_59a720", true,  0x59a720},
$1                {"sdk_5821c8", true,  0x5821c8},
$1                {"sdk_375be0", true,  0x375be0},
$1                {"sdk_5ade00", true,  0x5ade00},
$1                {"sdk_325224", true,  0x325224},
$1                {"sdk_325320", true,  0x325320},
$1                {"sdk_49c120", true,  0x49c120},
$1                {"sdk_59a958", true,  0x59a958},
$1                {"main_b47a60", false, 0xb47a60},
$1                {"main_2ce6a50", false, 0x2ce6a50},
$1                {"main_b43b7c", false, 0xb43b7c},
$1                {"main_b45d18", false, 0xb45d18},
$1                {"main_b40f10", false, 0xb40f10},
$1                {"main_b71dc4", false, 0xb71dc4},
$1                {"main_2a64c54", false, 0x2a64c54},
$1                {"main_bb7fa0", false, 0xbb7fa0},
$1                {"main_208b274", false, 0x208b274},
$1                {"main_b4423c", false, 0xb4423c},
$1            }};
$1            for (const auto& site : sites) {
$1                dump_window(site.label,
$1                            (site.sdk ? scarlet_sdk_base : scarlet_main_base) +
$1                                site.offset);
$1            }
$1        }
$1
$1        if (impl->ctx.pc >= scarlet_sdk_base) {
$1            const u64 off = impl->ctx.pc - scarlet_sdk_base;
$1            int target = -1;
$1            switch (off) {
$1            case 0x377320: target = 0; break;
$1            case 0x3726f0: target = 1; break;
$1            case 0x379fc0: target = 2; break;
$1            case 0x581bd0: target = 3; break;
$1            case 0x59a720: target = 4; break;
$1            case 0x5821c8: target = 5; break;
$1            case 0x375be0: target = 6; break;
$1            case 0x5ade00: target = 7; break;
$1            case 0x325224: target = 8; break;
$1            case 0x325320: target = 9; break;
$1            case 0x49c120: target = 10; break;
$1            case 0x59a958: target = 11; break;
$1            default: break;
$1            }
$1
$1            if (target >= 0) {
$1                const u32 hit = g_scarlet_hotspot_hit_counts[
$1                    static_cast<size_t>(target)].fetch_add(
$1                        1, std::memory_order_relaxed);
$1                if (hit < 6) {
$1                    LOG_INFO(
$1                        Core_ARM,
$1                        "[Scarlet.HotspotTrace] hit sdk+{:#x} #{} lr={} "
$1                        "x0={:#x} x1={:#x} x2={:#x} x3={:#x} "
$1                        "x4={:#x} x5={:#x} x6={:#x} x7={:#x} sp={:#x}",
$1                        off, hit + 1, resolve_offset(impl->ctx.x[30]),
$1                        impl->ctx.x[0], impl->ctx.x[1], impl->ctx.x[2],
$1                        impl->ctx.x[3], impl->ctx.x[4], impl->ctx.x[5],
$1                        impl->ctx.x[6], impl->ctx.x[7], impl->ctx.x[31]);
$1                }
$1            }
$1        }
$1    }
$1}
$1
$1block(&impl->ctx);
'@

$runText = $blockRegex.Replace($runText, $trace.TrimEnd(), 1)
$text = $prefix + $runText + $suffix

[IO.File]::WriteAllText($path, $text, $utf8)
Write-Host '[Scarlet PC] Hotspot trace: targeted ARM64 dump enabled via SCARLET_HOTSPOT_TRACE=1.' -ForegroundColor Green
