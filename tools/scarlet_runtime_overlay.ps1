param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$provenancePath = Join-Path $repoRoot 'licenses\third-party\runtime-provenance.json'
if (-not (Test-Path $provenancePath -PathType Leaf)) {
    throw "Missing runtime provenance file: $provenancePath"
}
$provenance = Get-Content $provenancePath -Raw | ConvertFrom-Json
$legacyProduct = [string]$provenance.legacy.product
$legacyPascal = [string]$provenance.legacy.pascal
$legacyUpper = [string]$provenance.legacy.upper
$legacyCommandName = [string]$provenance.legacy.command_target
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Expand-Legacy([string]$Text) {
    return $Text.Replace('__PRODUCT__', $legacyProduct).
        Replace('__PASCAL__', $legacyPascal).
        Replace('__UPPER__', $legacyUpper).
        Replace('__COMMAND__', $legacyCommandName)
}

function Replace-Literal([string]$RelativePath, [string]$Old, [string]$New) {
    $RelativePath = Expand-Legacy $RelativePath
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Scarlet runtime overlay: missing file $RelativePath"
    }
    $Old = Expand-Legacy $Old
    $New = Expand-Legacy $New
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
    $RelativePath = Expand-Legacy $RelativePath
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Scarlet runtime overlay: missing file $RelativePath"
    }
    $Pattern = Expand-Legacy $Pattern
    $Replacement = Expand-Legacy $Replacement
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
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_INFO(Frontend, "__COMMAND__ starting up...");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Scarlet PC starting");'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_INFO(Frontend, "__COMMAND__: Initializing system...");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Initializing runtime");'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_INFO(Frontend, "__COMMAND__: System initialized.");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Runtime initialized");'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_INFO(Frontend, "__COMMAND__: Window created, loading game...");' 'LOG_INFO(Frontend, "[Scarlet.Runtime] Window ready");'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_INFO(Frontend, "__COMMAND__: system.Load returned: {}", static_cast<int>(load_result));' 'LOG_INFO(Frontend, "[Scarlet.Game] Load status: {}", static_cast<int>(load_result));'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_CRITICAL(Frontend, "Unhandled fatal exception in __COMMAND__: {}", e.what());' 'LOG_CRITICAL(Frontend, "[Scarlet.Runtime] Unhandled fatal exception: {}", e.what());'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_CRITICAL(Frontend, "Unhandled unknown fatal exception in __COMMAND__");' 'LOG_CRITICAL(Frontend, "[Scarlet.Runtime] Unhandled unknown fatal exception");'

$old = @'
LOG_INFO(Frontend, "__COMMAND__: Calling system.Load for '{}'...", filepath);
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.Game] Loading Pokémon Scarlet from '{}'", filepath);
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
case Core::SystemResultStatus::Success:
        break; // Expected case
'@
$new = @'
case Core::SystemResultStatus::Success:
        LOG_INFO(Frontend, "[Scarlet.Game] Pokémon Scarlet loaded");
        break;
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Native recompiled module [{}] loaded from {} — ArmRecomp active",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Module [{}] {} loaded",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Recompiled instruction guard-v2: {}",
                 recomp_guard_ready ? "ready" : "not negotiated");
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Instruction guard: {}",
                 recomp_guard_ready ? "ready" : "not negotiated");
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Recompiled image ABI {}; page-table fastmem: {}", recomp_bundle_abi,
                 recomp_bundle_abi == 6 ? "negotiated" : "not used");
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Fastmem ABI {}: {}", recomp_bundle_abi,
                 recomp_bundle_abi == 6 ? "ready" : "not used");
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "Recompiled FPX1 native FP: {} (handshake {:#x})",
                 recomp_fpx ? "negotiated" : "not used", recomp_fpx);
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.AOT] Native floating-point: {} (handshake {:#x})",
                 recomp_fpx ? "ready" : "not used", recomp_fpx);
#ifndef __UPPER___NO_JIT
        LOG_INFO(Frontend, "[Scarlet.AOT] Dynamic fallback available");
#else
        LOG_INFO(Frontend, "[Scarlet.AOT] Dynamic fallback unavailable");
#endif
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
if (!s_recomp_modules.empty()) {
        // Combined lookup:
'@
$new = @'
if (!s_recomp_modules.empty()) {
        LOG_INFO(Frontend, "[Scarlet.AOT] {} native modules loaded", s_recomp_modules.size());
        // Combined lookup:
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

$old = @'
void(system.Run());
'@
$new = @'
void(system.Run());
    LOG_INFO(Frontend, "[Scarlet.Runtime] Game execution started");
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

# Remove legacy product wording from player-facing setup/errors.
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"Ignoring a recorded __PRODUCT__ inside this package; keys and firmware "' '"Ignoring a recorded external runtime inside this package; keys and firmware "'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"in __PRODUCT__ (Tools > Install Decryption Keys), then start the game "' '"for Scarlet PC, then start the game "'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"Exported games read firmware only from the installed __PRODUCT__. Remove "' '"Scarlet PC could not use firmware embedded in this package. Remove "'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"__PRODUCT__ (Tools > Install Firmware), then start the game again.\\n\\nYou can "' '"Scarlet PC, then start the game again.\\n\\nYou can "'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"Install current keys in __PRODUCT__ (Tools > Install Decryption "' '"Install current keys for Scarlet PC. "'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"refer to the __PRODUCT__ wiki for more information or the __PRODUCT__ discord for "' '"check the Scarlet PC log for more information. "'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"host does not implement; update __PRODUCT__ or re-export with this build",' '"host does not implement; rebuild the Scarlet PC runtime",'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"implement; update __PRODUCT__ or re-export with this build",' '"implement; rebuild the Scarlet PC runtime",'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' '"Open __PRODUCT__"' '"Open runtime"'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'LOG_INFO(Frontend, "__COMMAND__: reporting application version {} ({})",' 'LOG_INFO(Frontend, "[Scarlet.Game] Application version: {} ({})",'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'std::cout << "__PRODUCT__" << Common::g_scm_branch << " " << Common::g_scm_desc << std::endl;' 'std::cout << "Scarlet PC" << std::endl;'

# Internal Win32 class names are project-owned too.
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'static const wchar_t* kClass = L"__PASCAL__GameDebugPanel";' 'static const wchar_t* kClass = L"ScarletGameDebugPanel";'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'static const wchar_t* const kClass = L"__PASCAL__BuildProgress";' 'static const wchar_t* const kClass = L"ScarletBuildProgress";'

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
Replace-RegexOnce 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $pattern $replacement.Trim()

Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'const auto p = _exe_dir / dll_name;' 'const auto p = _exe_dir / "aot" / dll_name;'

$pattern = 'std::string module_name = Common::UTF16ToUTF8\(dll_name\);\s*module_name = module_name\.substr\(11, module_name\.size\(\) - 15\); // recompiled_\*\.dll\s*s_recomp_modules\.push_back\(\{lkp, sbf, run_slice, abi, guard,\s*module_name == "image" \? "main" : module_name\}\);'
$replacement = @'
std::string module_name = Common::UTF16ToUTF8(dll_name);
                if (module_name == "scarlet-main.dll") module_name = "main";
                else if (module_name == "scarlet-rtld.dll") module_name = "rtld";
                else if (module_name == "scarlet-multimedia.dll") module_name = "subsdk0";
                else if (module_name == "scarlet-sdk.dll") module_name = "sdk";
                s_recomp_modules.push_back({lkp, sbf, run_slice, abi, guard, module_name});
'@
Replace-RegexOnce 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $pattern $replacement.Trim()

# Fixed game window title.
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' 'return exe.empty() ? std::string{"__PRODUCT__"} : exe.stem().string();' 'return std::string{"Pokémon Scarlet"};'

$old = @'
const std::string window_title = fmt::format("__PRODUCT__ {} | {}-{} (Vulkan)", Common::g_build_name,
                                                 Common::g_scm_branch, Common::g_scm_desc);
'@
$new = @'
const std::string window_title = "Pokémon Scarlet";
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

$old = @'
LOG_INFO(Frontend, "__PRODUCT__ Version: {} | {}-{} (Vulkan)", Common::g_build_name,
             Common::g_scm_branch, Common::g_scm_desc);
'@
$new = @'
LOG_INFO(Frontend, "[Scarlet.Runtime] Vulkan backend ready");
'@
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2_vk.cpp' $old.Trim() $new.Trim()

Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'const std::string title = __PASCAL__Cmd::FormatNativeTitle(display);' 'const std::string title = "Pokémon Scarlet";'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Preparing shaders...");' 'std::snprintf(title, sizeof(title), "Pokémon Scarlet");'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'std::snprintf(title, sizeof(title), "Building shaders %zu/%zu", built, total);' 'std::snprintf(title, sizeof(title), "Pokémon Scarlet");'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' '"falling back to __PRODUCT__ icon.");' '"falling back to the built-in Scarlet PC icon.");'
Replace-Literal 'src\__PRODUCT___cmd\emu_window\emu_window_sdl2.cpp' 'LOG_WARNING(Frontend, "Failed to create __PRODUCT__ icon stream.");' 'LOG_WARNING(Frontend, "Failed to create built-in runtime icon stream.");'

# Use the project icon in the Windows PE resource instead of the upstream icon.
$projectIcon = Join-Path $repoRoot 'docs\icon\icon.ico'
$runtimeIcon = Join-Path $SourceDir 'dist\__PRODUCT__.ico'
if (Test-Path $projectIcon -PathType Leaf) {
    Copy-Item $projectIcon $runtimeIcon -Force
}

# Project-owned config and cache names.
Replace-Literal 'src\frontend_common\config.cpp' 'FS::Get__PASCAL__Path(FS::__PASCAL__Path::ConfigDir) / "sdl2-config.ini";' 'FS::Get__PASCAL__Path(FS::__PASCAL__Path::ConfigDir) / "scarlet.ini";'

$old = @'
for (const char* sub : {"config", "cache", "cache/shader", "log", "nand", "sdmc", "dump",
                                     "load", "screenshots", "play_time", "crash_dumps", "amiibo", "tas",
                                     "icons", "themes"}) {
'@
$new = @'
for (const char* sub : {"config", "cache", "cache/scarlet", "cache/scarlet/shader", "log", "nand", "sdmc", "dump",
                                     "load", "screenshots", "play_time", "crash_dumps", "amiibo", "tas",
                                     "icons", "themes"}) {
'@
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' $old.Trim() $new.Trim()

Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'FS::Set__PASCAL__Path(FS::__PASCAL__Path::CacheDir, user_root / "cache");' 'FS::Set__PASCAL__Path(FS::__PASCAL__Path::CacheDir, user_root / "cache" / "scarlet");'
Replace-Literal 'src\__PRODUCT___cmd\__PRODUCT__.cpp' 'FS::Set__PASCAL__Path(FS::__PASCAL__Path::ShaderDir, user_root / "cache" / "shader");' 'FS::Set__PASCAL__Path(FS::__PASCAL__Path::ShaderDir, user_root / "cache" / "scarlet" / "shader");'

# Public nswitch fallback contract; the old variable remains a developer compatibility alias.
$old = @'
bool StrictNoFallback() {
    const char* e = std::getenv("__UPPER___RECOMP_STRICT");
    return e && *e && *e != '0';
}
'@
$new = @'
bool StrictNoFallback() {
    if (const char* nswitch = std::getenv("NSWITCH_AOT_FALLBACK"); nswitch && *nswitch) {
        return *nswitch == '0';
    }
    const char* legacy = std::getenv("__UPPER___RECOMP_STRICT");
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
