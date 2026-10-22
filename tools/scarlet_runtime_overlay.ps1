param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Replace-Literal([string]$RelativePath, [string]$Old, [string]$New) {
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Scarlet runtime overlay: missing file $RelativePath"
    }
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains($Old)) {
        $text = $text.Replace($Old, $New)
        [IO.File]::WriteAllText($path, $text, $utf8)
        return
    }
    if (-not $text.Contains($New)) {
        throw "Scarlet runtime overlay: expected source text not found in $RelativePath"
    }
}

function Replace-RegexOnce([string]$RelativePath, [string]$Pattern, [string]$Replacement) {
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Scarlet runtime overlay: missing file $RelativePath"
    }
    $text = [IO.File]::ReadAllText($path)
    $rx = New-Object System.Text.RegularExpressions.Regex(
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Singleline
    )
    if ($rx.IsMatch($text)) {
        $text = $rx.Replace($text, $Replacement, 1)
        [IO.File]::WriteAllText($path, $text, $utf8)
        return
    }
    if (-not $text.Contains($Replacement.Trim())) {
        throw "Scarlet runtime overlay: expected pattern not found in $RelativePath"
    }
}

# Player-facing Scarlet PC identity.
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_INFO(Frontend, "suyu-cmd starting up...");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Scarlet PC 0.1 starting");'
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_INFO(Frontend, "suyu-cmd: Initializing system...");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Initializing runtime");'
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_INFO(Frontend, "suyu-cmd: System initialized.");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Runtime initialized");'
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_INFO(Frontend, "suyu-cmd: Window created, loading game...");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Window ready");'
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_INFO(Frontend, "suyu-cmd: system.Load returned: {}", static_cast<int>(load_result));' 'LOG_INFO(Frontend, "[Scarlet.Game] Load status: {}", static_cast<int>(load_result));'
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_CRITICAL(Frontend, "Unhandled fatal exception in suyu-cmd: {}", e.what());' 'LOG_CRITICAL(Frontend, "[Scarlet.Runtime] Unhandled fatal exception: {}", e.what());'
Replace-Literal 'src\suyu_cmd\suyu.cpp' 'LOG_CRITICAL(Frontend, "Unhandled unknown fatal exception in suyu-cmd");' 'LOG_CRITICAL(Frontend, "[Scarlet.Runtime] Unhandled unknown fatal exception");'

$old = @'
LOG_INFO(Frontend, "suyu-cmd: Calling system.Load for '{}'...", filepath);
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.Game] Loading Pokémon Scarlet from '{}'", filepath);
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
case Core::SystemResultStatus::Success:
        break; // Expected case
'@
$new = @'
case Core::SystemResultStatus::Success:
        LOG_INFO(Frontend, "[Scarlet.Game] Pokémon Scarlet loaded");
        break;
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Native recompiled module [{}] loaded from {} — ArmRecomp active",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Module [{}] {} loaded",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Recompiled instruction guard-v2: {}",
                 recomp_guard_ready ? "ready" : "not negotiated");
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Instruction guard: {}",
                 recomp_guard_ready ? "ready" : "not negotiated");
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Recompiled image ABI {}; page-table fastmem: {}", recomp_bundle_abi,
                 recomp_bundle_abi == 6 ? "negotiated" : "not used");
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Fastmem ABI {}: {}", recomp_bundle_abi,
                 recomp_bundle_abi == 6 ? "ready" : "not used");
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Recompiled FPX1 native FP: {} (handshake {:#x})",
                 recomp_fpx ? "negotiated" : "not used", recomp_fpx);
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Native floating-point: {} (handshake {:#x})",
                 recomp_fpx ? "ready" : "not used", recomp_fpx);
#ifndef SUYU_NO_JIT
        LOG_INFO(Frontend, "[Scarlet.AOT] Dynamic fallback available");
#else
        LOG_INFO(Frontend, "[Scarlet.AOT] Dynamic fallback unavailable");
#endif
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
if (!s_recomp_modules.empty()) {
        // Combined lookup:
'@
$new = @'
if (!s_recomp_modules.empty()) {
        LOG_INFO(Frontend, "[Scarlet.AOT] {} native modules loaded", s_recomp_modules.size());
        // Combined lookup:
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

$old = @'
void(system.Run());
'@
$new = @'
void(system.Run());
    LOG_INFO(Frontend, "[Scarlet.Runtime] Game execution started");
'@
Replace-Literal 'src\suyu_cmd\suyu.cpp' $old.Trim() $new.Trim()

# Stable product-facing AOT filenames.
$pattern = 'std::vector<std::wstring> dll_order = \{\s*L"recompiled_rtld\.dll",\s*L"recompiled_image\.dll",\s*// main\s*L"recompiled_subsdk0\.dll", L"recompiled_subsdk1\.dll", L"recompiled_subsdk2\.dll",\s*L"recompiled_subsdk3\.dll", L"recompiled_subsdk4\.dll", L"recompiled_subsdk5\.dll",\s*L"recompiled_subsdk6\.dll", L"recompiled_subsdk7\.dll", L"recompiled_subsdk8\.dll",\s*L"recompiled_subsdk9\.dll",\s*L"recompiled_sdk\.dll",\s*\};'
$replacement = @'
std::vector<std::wstring> dll_order = {
            L"scarlet-rtld.dll",
            L"scarlet-main.dll",
            L"scarlet-multimedia.dll",
            L"scarlet-sdk.dll",
        };
'@
Replace-RegexOnce 'src\suyu_cmd\suyu.cpp' $pattern $replacement.Trim()

Replace-Literal 'src\suyu_cmd\suyu.cpp' 'const auto p = _exe_dir / dll_name;' 'const auto p = _exe_dir / "aot" / dll_name;'

$pattern = 'std::string module_name = Common::UTF16ToUTF8\(dll_name\);\s*module_name = module_name\.substr\(11, module_name\.size\(\) - 15\); // recompiled_\*\.dll\s*s_recomp_modules\.push_back\(\{lkp, sbf, run_slice, abi, guard,\s*module_name == "image" \? "main" : module_name\}\);'
$replacement = @'
std::string module_name = Common::UTF16ToUTF8(dll_name);
                if (module_name == "scarlet-main.dll") module_name = "main";
                else if (module_name == "scarlet-rtld.dll") module_name = "rtld";
                else if (module_name == "scarlet-multimedia.dll") module_name = "subsdk0";
                else if (module_name == "scarlet-sdk.dll") module_name = "sdk";
                s_recomp_modules.push_back({lkp, sbf, run_slice, abi, guard, module_name});
'@
Replace-RegexOnce 'src\suyu_cmd\suyu.cpp' $pattern $replacement.Trim()

# Fixed game window title.
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2_vk.cpp' 'return exe.empty() ? std::string{"suyu"} : exe.stem().string();' 'return std::string{"Pokémon Scarlet"};'

$old = @'
const std::string window_title = fmt::format("suyu {} | {}-{} (Vulkan)", Common::g_build_name,
                                                 Common::g_scm_branch, Common::g_scm_desc);
'@
$new = @'
const std::string window_title = "Pokémon Scarlet";
'@
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "suyu Version: {} | {}-{} (Vulkan)", Common::g_build_name,
             Common::g_scm_branch, Common::g_scm_desc);
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.Runtime] Scarlet PC 0.1 | Vulkan");
'@
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2.cpp' 'const std::string title = SuyuCmd::FormatNativeTitle(display);' 'const std::string title = "Pokémon Scarlet";'
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Preparing shaders...");' 'std::snprintf(title, sizeof(title), "Pokémon Scarlet");'
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Building shaders %zu/%zu", built, total);' 'std::snprintf(title, sizeof(title), "Pokémon Scarlet");'
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2.cpp' '"falling back to suyu icon.");' '"falling back to the built-in Scarlet PC icon.");'
Replace-Literal 'src\suyu_cmd\emu_window\emu_window_sdl2.cpp' 'LOG_WARNING(Frontend, "Failed to create suyu icon stream.");' 'LOG_WARNING(Frontend, "Failed to create built-in runtime icon stream.");'

# Use the project icon in the Windows PE resource instead of the upstream icon.
$projectIcon = Join-Path $repoRoot 'docs\icon\icon.ico'
$runtimeIcon = Join-Path $SourceDir 'dist\suyu.ico'
if (Test-Path $projectIcon -PathType Leaf) {
    Copy-Item $projectIcon $runtimeIcon -Force
}

# Project-owned config name.
Replace-Literal 'src\frontend_common\config.cpp' 'FS::GetSuyuPath(FS::SuyuPath::ConfigDir) / "sdl2-config.ini";' 'FS::GetSuyuPath(FS::SuyuPath::ConfigDir) / "scarlet.ini";'

# Public nswitch fallback contract; the old variable remains a developer compatibility alias.
$old = @'
bool StrictNoFallback() {
    const char* e = std::getenv("SUYU_RECOMP_STRICT");
    return e && *e && *e != '0';
}
'@
$new = @'
bool StrictNoFallback() {
    if (const char* nswitch = std::getenv("NSWITCH_AOT_FALLBACK"); nswitch && *nswitch) {
        return *nswitch == '0';
    }
    const char* legacy = std::getenv("SUYU_RECOMP_STRICT");
    return legacy && *legacy && *legacy != '0';
}
'@
Replace-Literal 'src\core\arm\recomp\arm_recomp.cpp' $old.Trim() $new.Trim()

# Scarlet graphics identity in the Vulkan bring-up log.
$old = @'
CollectPhysicalMemoryInfo();
    CollectToolingInfo();
'@
$new = @'
CollectPhysicalMemoryInfo();
    CollectToolingInfo();
    LOG_INFO(Render_Vulkan, "[Scarlet.Graphics] Vulkan device: {}",
             properties.properties.deviceName);
'@
Replace-Literal 'src\video_core\vulkan_common\vulkan_device.cpp' $old.Trim() $new.Trim()

Write-Host '[Scarlet PC] Runtime overlay applied.' -ForegroundColor Green
